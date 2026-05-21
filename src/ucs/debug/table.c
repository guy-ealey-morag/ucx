/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2026. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include "table.h"

#include <ucs/datastruct/array.h>
#include <ucs/debug/assert.h>
#include <ucs/debug/log_def.h>
#include <ucs/debug/memtrack_int.h>
#include <ucs/sys/compiler.h>
#include <ucs/sys/math.h>
#include <ucs/sys/string.h>
#include <stdio.h>
#include <string.h>


/* Discriminates regular rows (cells in table->entries) from stream rows
 * (cells owned by the row). */
typedef enum {
    UCS_TABLE_ROW_REGULAR,
    UCS_TABLE_ROW_STREAM
} ucs_table_row_kind_t;


struct ucs_table_row {
    ucs_table_t          *table;
    ucs_table_row_kind_t kind;
    union {
        unsigned          entry_idx; /* index into table->entries */
        ucs_table_cells_t cells; /* cells owned by this row */
    } u;
};


void ucs_table_init(ucs_table_t *table, const ucs_table_config_t *config)
{
    unsigned *owned_min_widths;

    table->config        = *config;
    table->n_stream_rows = 0;
    ucs_array_init_dynamic(&table->entries);
    ucs_array_init_dynamic(&table->row_handles);

    table->widths = ucs_calloc(config->n_body_cols, sizeof(*table->widths),
                               "ucs_table_widths");
    if (table->widths == NULL) {
        ucs_fatal("failed to allocate table widths");
    }

    if (config->min_widths != NULL) {
        /* Deep-copy so the caller's array can be transient. */
        owned_min_widths = ucs_malloc(config->n_body_cols *
                                              sizeof(*owned_min_widths),
                                      "ucs_table_min_widths");
        if (owned_min_widths == NULL) {
            ucs_fatal("failed to allocate table min widths");
        }

        memcpy(owned_min_widths, config->min_widths,
               config->n_body_cols * sizeof(*owned_min_widths));
        table->config.min_widths = owned_min_widths;
    }
}


void ucs_table_cleanup(ucs_table_t *table)
{
    ucs_table_entry_t *entry;
    ucs_table_cell_t *cell;
    ucs_table_row_t **row_p;

    ucs_assertv(table->n_stream_rows == 0,
                "table has %u live stream rows at cleanup",
                table->n_stream_rows);

    ucs_array_for_each(entry, &table->entries) {
        if (entry->kind != UCS_TABLE_ENTRY_ROW) {
            continue;
        }
        ucs_array_for_each(cell, &entry->cells) {
            ucs_string_buffer_cleanup(&cell->text);
        }
        ucs_array_cleanup_dynamic(&entry->cells);
    }
    ucs_array_cleanup_dynamic(&table->entries);

    ucs_array_for_each(row_p, &table->row_handles) {
        ucs_free(*row_p);
    }
    ucs_array_cleanup_dynamic(&table->row_handles);

    ucs_free(table->widths);
    table->widths = NULL;

    ucs_free((void*)table->config.min_widths);
    table->config.min_widths = NULL;
}


void ucs_table_add_separator_with_merged_cols(ucs_table_t *table,
                                              unsigned merged_cols)
{
    ucs_table_entry_t *entry;

    ucs_assertv(merged_cols <= table->config.n_body_cols,
                "merged_cols=%u exceeds n_body_cols=%u", merged_cols,
                table->config.n_body_cols);

    entry              = ucs_array_append(&table->entries,
                                          ucs_fatal("failed to grow table entries"));
    entry->kind        = UCS_TABLE_ENTRY_SEPARATOR;
    entry->merged_cols = merged_cols;
}


void ucs_table_add_separator(ucs_table_t *table)
{
    ucs_table_add_separator_with_merged_cols(table, 0);
}


ucs_table_row_t *ucs_table_add_row(ucs_table_t *table)
{
    ucs_table_entry_t *entry;
    ucs_table_row_t *row, **row_slot;
    ucs_status_t status;

    row = ucs_malloc(sizeof(*row), "ucs_table_row");
    if (row == NULL) {
        ucs_fatal("failed to allocate table row");
    }

    entry       = ucs_array_append(&table->entries,
                                   ucs_fatal("failed to grow table entries"));
    entry->kind = UCS_TABLE_ENTRY_ROW;
    ucs_array_init_dynamic(&entry->cells);

    /* Pre-reserve so cell pointers stay stable across add_cell. */
    status = ucs_array_reserve(&entry->cells, table->config.n_body_cols);
    if (status != UCS_OK) {
        ucs_fatal("failed to reserve table row cells");
    }

    row->table       = table;
    row->kind        = UCS_TABLE_ROW_REGULAR;
    row->u.entry_idx = ucs_array_length(&table->entries) - 1;

    row_slot  = ucs_array_append(&table->row_handles,
                                 ucs_fatal("failed to grow table row handles"));
    *row_slot = row;

    return row;
}


static ucs_table_cells_t *ucs_table_row_cells(ucs_table_row_t *row)
{
    ucs_table_entry_t *entry;

    if (row->kind == UCS_TABLE_ROW_STREAM) {
        return &row->u.cells;
    }

    entry = &ucs_array_elem(&row->table->entries, row->u.entry_idx);
    ucs_assert(entry->kind == UCS_TABLE_ENTRY_ROW);
    return &entry->cells;
}


ucs_table_cell_t *ucs_table_row_add_cell(ucs_table_row_t *row,
                                         unsigned col_span,
                                         ucs_table_align_t align)
{
    ucs_table_cells_t *cells = ucs_table_row_cells(row);
    ucs_table_cell_t *cell =
            ucs_array_append(cells, ucs_fatal("table row exceeded body "
                                              "column count"));
    cell->col_span = col_span;
    cell->align    = align;
    ucs_string_buffer_init(&cell->text);
    return cell;
}


void ucs_table_row_add_cell_fmt(ucs_table_row_t *row, unsigned col_span,
                                ucs_table_align_t align, const char *fmt, ...)
{
    ucs_table_cell_t *cell = ucs_table_row_add_cell(row, col_span, align);
    const char *cstr;
    va_list ap;

    va_start(ap, fmt);
    ucs_string_buffer_vappendf(&cell->text, fmt, ap);
    va_end(ap);

    cstr = ucs_string_buffer_cstr(&cell->text);

    ucs_assertv(strchr(cstr, '\n') == NULL,
                "table cell content must not contain '\\n': '%s'", cstr);
}


/* Total visible width of a cell spanning `col_span` body columns. Each
 * merged column adds its own width plus the " | " (3 chars) it would have
 * used as a separator. */
static unsigned ucs_table_cell_pixel_width(const unsigned *body_widths,
                                           unsigned start, unsigned col_span)
{
    unsigned width = 0;
    unsigned i;

    for (i = 0; i < col_span; ++i) {
        width += body_widths[start + i];
    }
    width += 3 * (col_span - 1);
    return width;
}


static unsigned ucs_table_cell_content_len(ucs_table_cell_t *cell)
{
    return ucs_string_buffer_length(&cell->text);
}


/* Compute per-body-column widths needed to fit every cell. Recomputed on
 * every call so callers may add rows or separators between renders and
 * have widths adapt to the new content. Two passes: pass 1 sizes columns
 * from col_span == 1 cells; pass 2 expands the rightmost spanned column
 * of each merged cell to absorb any remaining deficit. Splitting them
 * prevents merged cells added before the body rows from over-expanding
 * the rightmost spanned column. Each column starts at config.min_widths[i]
 * (or 0 when unset) so the caller can lock in a lower bound for stream-row
 * content the table does not see. */
static void ucs_table_compute_widths(ucs_table_t *table)
{
    const unsigned *min_widths = table->config.min_widths;
    unsigned *widths           = table->widths;
    ucs_table_entry_t *entry;
    ucs_table_cell_t *cell;
    unsigned i, body_col, content_len;
    unsigned existing;

    for (i = 0; i < table->config.n_body_cols; ++i) {
        widths[i] = (min_widths != NULL) ? min_widths[i] : 0;
    }

    /* Pass 1: col_span == 1 cells. */
    ucs_array_for_each(entry, &table->entries) {
        if (entry->kind != UCS_TABLE_ENTRY_ROW) {
            continue;
        }
        body_col = 0;
        ucs_array_for_each(cell, &entry->cells) {
            if (cell->col_span == 1) {
                content_len      = ucs_table_cell_content_len(cell);
                widths[body_col] = ucs_max(widths[body_col], content_len);
            }
            body_col += cell->col_span;
        }
    }

    /* Pass 2: merged cells expand the rightmost spanned column. */
    ucs_array_for_each(entry, &table->entries) {
        if (entry->kind != UCS_TABLE_ENTRY_ROW) {
            continue;
        }

        body_col = 0;
        ucs_array_for_each(cell, &entry->cells) {
            if (cell->col_span > 1) {
                content_len = ucs_table_cell_content_len(cell);
                existing    = ucs_table_cell_pixel_width(widths, body_col,
                                                         cell->col_span);

                if (content_len > existing) {
                    widths[body_col + cell->col_span - 1] += content_len -
                                                             existing;
                }
            }
            body_col += cell->col_span;
        }
    }

    /* Equal-widths: widen every column to the max (runs after pass 2 so
     * it can only widen). */
    if (table->config.equal_widths) {
        unsigned max_width = 0;
        for (i = 0; i < table->config.n_body_cols; ++i) {
            max_width = ucs_max(max_width, widths[i]);
        }
        for (i = 0; i < table->config.n_body_cols; ++i) {
            widths[i] = max_width;
        }
    }
}


/* Format a single cell at the given pixel width, branching on alignment. */
static void ucs_table_render_cell(ucs_string_buffer_t *strb,
                                  const ucs_table_cell_t *cell,
                                  unsigned pixel_width)
{
    const char *cstr = ucs_string_buffer_cstr(&cell->text);
    int content_len, pad, left_pad, right_pad;

    switch (cell->align) {
    case UCS_TABLE_ALIGN_LEFT:
        ucs_string_buffer_appendf(strb, "| %-*s ", (int)pixel_width, cstr);
        break;

    case UCS_TABLE_ALIGN_RIGHT:
        ucs_string_buffer_appendf(strb, "| %*s ", (int)pixel_width, cstr);
        break;

    case UCS_TABLE_ALIGN_CENTER:
        content_len = (int)strlen(cstr);
        pad         = ucs_max((int)pixel_width - content_len, 0);
        left_pad    = pad / 2;
        right_pad   = pad - left_pad;
        ucs_string_buffer_appendf(strb, "| %*s%s%*s ", left_pad, "", cstr,
                                  right_pad, "");
        break;
    }
}


/* Render one body row. The closing "|" has no trailing newline so callers
 * can splice extra content before the line break. */
static void ucs_table_render_cells(const ucs_table_t *table,
                                   ucs_string_buffer_t *strb,
                                   const ucs_table_cells_t *cells)
{
    const ucs_table_cell_t *cell;
    unsigned body_col = 0;

    if (table->config.row_prefix != NULL) {
        ucs_string_buffer_appendf(strb, "%s", table->config.row_prefix);
    }

    ucs_array_for_each(cell, cells) {
        ucs_table_render_cell(strb, cell,
                              ucs_table_cell_pixel_width(table->widths,
                                                         body_col,
                                                         cell->col_span));
        body_col += cell->col_span;
    }
    ucs_string_buffer_appendf(strb, "|");
}


/* Render a horizontal separator; carry-over for the leading `merged_cols`
 * body columns is handled inline. */
static void ucs_table_render_separator(const ucs_table_t *table,
                                       ucs_string_buffer_t *strb,
                                       unsigned merged_cols)
{
    static const char dashes[] =
            "----------------------------------------------------------------"
            "----------------------------------------------------------------"
            "----------------------------------------------------------------"
            "----------------------------------------------------------------";
    const unsigned max_width = sizeof(dashes) - 1;
    char left_corner;
    unsigned width;
    unsigned i;

    if (table->config.row_prefix != NULL) {
        ucs_string_buffer_appendf(strb, "%s", table->config.row_prefix);
    }

    for (i = 0; i < table->config.n_body_cols; ++i) {
        width = table->widths[i];
        ucs_assertv(width <= max_width, "widths[%u]=%u out of range [0, %u]", i,
                    width, max_width);

        if (i < merged_cols) {
            left_corner = '|';
        } else {
            left_corner = '+';
        }

        if (i < merged_cols) {
            ucs_string_buffer_appendf(strb, "%c %-*s ", left_corner, (int)width,
                                      "");
        } else {
            ucs_string_buffer_appendf(strb, "%c-%.*s-", left_corner, (int)width,
                                      dashes);
        }
    }

    ucs_string_buffer_appendf(strb, "+\n");
}


void ucs_table_render(ucs_table_t *table, ucs_string_buffer_t *strb)
{
    const ucs_table_entry_t *entry;
    unsigned i;

    ucs_table_compute_widths(table);

    /* Top frame */
    ucs_table_render_separator(table, strb, 0);

    /* Body rows and separators */
    for (i = 0; i < ucs_array_length(&table->entries); ++i) {
        entry = &ucs_array_elem(&table->entries, i);
        switch (entry->kind) {
        case UCS_TABLE_ENTRY_ROW:
            ucs_table_render_cells(table, strb, &entry->cells);
            ucs_string_buffer_appendf(strb, "\n");
            break;

        case UCS_TABLE_ENTRY_SEPARATOR:
            ucs_table_render_separator(table, strb, entry->merged_cols);
            break;
        }
    }

    /* Bottom frame; skip when the last entry is already a separator. */
    if (ucs_array_is_empty(&table->entries) ||
        (ucs_array_last(&table->entries)->kind != UCS_TABLE_ENTRY_SEPARATOR)) {
        ucs_table_render_separator(table, strb, 0);
    }
}


void ucs_table_print(ucs_table_t *table)
{
    ucs_string_buffer_t strb = UCS_STRING_BUFFER_INITIALIZER;

    ucs_table_render(table, &strb);
    printf("%s", ucs_string_buffer_cstr(&strb));
    ucs_string_buffer_cleanup(&strb);
}


ucs_table_row_t *ucs_table_stream_row_create(ucs_table_t *table)
{
    ucs_table_row_t *row, **row_slot;
    ucs_status_t status;

    row = ucs_malloc(sizeof(*row), "ucs_table_stream_row");
    if (row == NULL) {
        ucs_fatal("failed to allocate table stream row");
    }

    row->table = table;
    row->kind  = UCS_TABLE_ROW_STREAM;
    ucs_array_init_dynamic(&row->u.cells);

    status = ucs_array_reserve(&row->u.cells, table->config.n_body_cols);
    if (status != UCS_OK) {
        ucs_fatal("failed to reserve stream row cells");
    }

    row_slot  = ucs_array_append(&table->row_handles,
                                 ucs_fatal("failed to grow table row handles"));
    *row_slot = row;
    ++table->n_stream_rows;

    return row;
}


void ucs_table_stream_row_reset(ucs_table_row_t *row)
{
    ucs_table_cell_t *cell;

    ucs_assert(row->kind == UCS_TABLE_ROW_STREAM);

    /* Full cleanup so the next add_cell doesn't leak previous backing memory. */
    ucs_array_for_each(cell, &row->u.cells) {
        ucs_string_buffer_cleanup(&cell->text);
    }
    ucs_array_set_length(&row->u.cells, 0);
}


void ucs_table_stream_row_destroy(ucs_table_row_t *row)
{
    ucs_table_cell_t *cell;
    ucs_table_row_t **row_slot;
    ucs_table_t *table;
    unsigned i;

    ucs_assert(row->kind == UCS_TABLE_ROW_STREAM);

    ucs_array_for_each(cell, &row->u.cells) {
        ucs_string_buffer_cleanup(&cell->text);
    }
    ucs_array_cleanup_dynamic(&row->u.cells);

    /* Remove from row_handles (unordered; replace-with-last). */
    table = row->table;
    for (i = 0; i < ucs_array_length(&table->row_handles); ++i) {
        row_slot = &ucs_array_elem(&table->row_handles, i);
        if (*row_slot == row) {
            *row_slot = *ucs_array_last(&table->row_handles);
            ucs_array_set_length(&table->row_handles,
                                 ucs_array_length(&table->row_handles) - 1);
            break;
        }
    }

    --table->n_stream_rows;
    ucs_free(row);
}


void ucs_table_render_row(const ucs_table_row_t *row, ucs_string_buffer_t *strb)
{
    ucs_assert(row->kind == UCS_TABLE_ROW_STREAM);
    ucs_table_render_cells(row->table, strb, &row->u.cells);
}


void ucs_table_print_row(const ucs_table_row_t *row)
{
    ucs_string_buffer_t strb = UCS_STRING_BUFFER_INITIALIZER;

    ucs_table_render_row(row, &strb);
    printf("%s\n", ucs_string_buffer_cstr(&strb));
    ucs_string_buffer_cleanup(&strb);
}


void ucs_table_print_separator(ucs_table_t *table)
{
    ucs_string_buffer_t strb = UCS_STRING_BUFFER_INITIALIZER;

    ucs_table_render_separator(table, &strb, 0);
    printf("%s", ucs_string_buffer_cstr(&strb));
    ucs_string_buffer_cleanup(&strb);
}
