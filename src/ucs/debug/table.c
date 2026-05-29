/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2026. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

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


/* Streaming row: caller-owned, not stored in table->entries. Prints against
 * the table's already-computed widths. */
struct ucs_table_stream_row {
    ucs_table_t       *table;
    ucs_table_cells_t cells;
};


void ucs_table_init(ucs_table_t *table, const ucs_table_config_t *config)
{
    unsigned *min_widths;

    ucs_assertv(config->n_cols > 0,
                "number of columns must be positive (n_cols: %u)",
                config->n_cols);

    table->config        = *config;
    table->n_stream_rows = 0;
    ucs_array_init_dynamic(&table->entries);

    table->widths = ucs_calloc(config->n_cols, sizeof(*table->widths),
                               "ucs_table_widths");
    if (table->widths == NULL) {
        ucs_fatal("failed to allocate table widths");
    }

    if (config->min_widths != NULL) {
        min_widths = ucs_malloc(config->n_cols * sizeof(*min_widths),
                                "ucs_table_min_widths");
        if (min_widths == NULL) {
            ucs_fatal("failed to allocate table min widths");
        }

        memcpy(min_widths, config->min_widths,
               config->n_cols * sizeof(*min_widths));
        table->config.min_widths = min_widths;
    }
}


void ucs_table_cleanup(ucs_table_t *table)
{
    ucs_table_entry_t *entry;
    ucs_table_cell_t *cell;

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

    ucs_free(table->widths);
    table->widths = NULL;

    ucs_free((void*)table->config.min_widths);
    table->config.min_widths = NULL;
}


void ucs_table_add_separator_with_merged_cols(ucs_table_t *table,
                                              unsigned merged_cols)
{
    ucs_table_entry_t *entry;

    ucs_assertv(merged_cols <= table->config.n_cols,
                "merged_cols=%u exceeds n_cols=%u", merged_cols,
                table->config.n_cols);

    entry              = ucs_array_append_fixed(&table->entries);
    entry->kind        = UCS_TABLE_ENTRY_SEPARATOR;
    entry->merged_cols = merged_cols;
}


void ucs_table_add_separator(ucs_table_t *table)
{
    ucs_table_add_separator_with_merged_cols(table, 0);
}


ucs_table_row_h ucs_table_add_row(ucs_table_t *table)
{
    ucs_table_entry_t *entry;
    ucs_status_t status;

    entry              = ucs_array_append_fixed(&table->entries);
    entry->kind        = UCS_TABLE_ENTRY_ROW;
    entry->merged_cols = 0;
    ucs_array_init_dynamic(&entry->cells);

    /* Pre-reserve so cell pointers stay stable across add_cell. */
    status = ucs_array_reserve(&entry->cells, table->config.n_cols);
    if (status != UCS_OK) {
        ucs_fatal("failed to reserve table row cells");
    }

    return ucs_array_length(&table->entries) - 1;
}


static ucs_table_cells_t *
ucs_table_row_cells(ucs_table_t *table, ucs_table_row_h row)
{
    ucs_table_entry_t *entry = &ucs_array_elem(&table->entries, row);

    ucs_assert(entry->kind == UCS_TABLE_ENTRY_ROW);
    return &entry->cells;
}


static ucs_table_cell_t *ucs_table_cells_add(ucs_table_cells_t *cells,
                                             unsigned col_span,
                                             ucs_table_align_t align)
{
    ucs_table_cell_t *cell;

    ucs_assertv(col_span > 0, "column span must be positive (col_span: %u)",
                col_span);

    cell           = ucs_array_append_fixed(cells);
    cell->col_span = col_span;
    cell->align    = align;
    ucs_string_buffer_init(&cell->text);
    return cell;
}


static void ucs_table_cells_add_fmt(ucs_table_cells_t *cells, unsigned col_span,
                                    ucs_table_align_t align, const char *fmt,
                                    va_list ap)
{
    ucs_table_cell_t *cell = ucs_table_cells_add(cells, col_span, align);
    const char UCS_V_UNUSED *cstr;

    ucs_string_buffer_vappendf(&cell->text, fmt, ap);

    cstr = ucs_string_buffer_cstr(&cell->text);

    ucs_assertv(strchr(cstr, '\n') == NULL,
                "table cell content must not contain '\\n': '%s'", cstr);
}


void ucs_table_row_add_cell_empty(ucs_table_t *table, ucs_table_row_h row,
                                  unsigned col_span)
{
    ucs_table_cells_add(ucs_table_row_cells(table, row), col_span,
                        UCS_TABLE_ALIGN_LEFT);
}


void ucs_table_row_add_cell_fmt(ucs_table_t *table, ucs_table_row_h row,
                                unsigned col_span, ucs_table_align_t align,
                                const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    ucs_table_cells_add_fmt(ucs_table_row_cells(table, row), col_span, align,
                            fmt, ap);
    va_end(ap);
}


static unsigned
ucs_table_cell_character_width(const ucs_table_t *table, const unsigned *widths,
                               unsigned start, unsigned col_span)
{
    unsigned width = 0;
    unsigned i;

    ucs_assertv(col_span > 0, "column span must be positive (col_span: %u)",
                col_span);
    ucs_assertv(
            start + col_span <= table->config.n_cols,
            "column span exceeds number of columns (start: %u, col_span: %u)",
            start, col_span);

    for (i = 0; i < col_span; ++i) {
        width += widths[start + i];
    }
    width += 3 * (col_span - 1);
    return width;
}


static unsigned ucs_table_cell_content_len(ucs_table_cell_t *cell)
{
    return ucs_string_buffer_length(&cell->text);
}


static void ucs_table_compute_widths(ucs_table_t *table)
{
    const unsigned *min_widths = table->config.min_widths;
    unsigned *widths           = table->widths;
    ucs_table_entry_t *entry;
    ucs_table_cell_t *cell;
    unsigned i, col, content_len;
    unsigned existing;

    for (i = 0; i < table->config.n_cols; ++i) {
        widths[i] = (min_widths != NULL) ? min_widths[i] : 0;
    }

    /* Pass 1: col_span == 1 cells. */
    ucs_array_for_each(entry, &table->entries) {
        if (entry->kind != UCS_TABLE_ENTRY_ROW) {
            continue;
        }
        col = 0;
        ucs_array_for_each(cell, &entry->cells) {
            ucs_assertv(col + cell->col_span <= table->config.n_cols,
                        "row column span exceeds number of columns");

            if (cell->col_span == 1) {
                content_len = ucs_table_cell_content_len(cell);
                widths[col] = ucs_max(widths[col], content_len);
            }
            col += cell->col_span;
        }
    }

    /* Pass 2: merged cells expand the rightmost spanned column. */
    ucs_array_for_each(entry, &table->entries) {
        if (entry->kind != UCS_TABLE_ENTRY_ROW) {
            continue;
        }

        col = 0;
        ucs_array_for_each(cell, &entry->cells) {
            ucs_assertv(col + cell->col_span <= table->config.n_cols,
                        "row column span exceeds number of columns");

            if (cell->col_span > 1) {
                content_len = ucs_table_cell_content_len(cell);
                existing = ucs_table_cell_character_width(table, widths, col,
                                                          cell->col_span);
                if (content_len > existing) {
                    widths[col + cell->col_span - 1] += content_len - existing;
                }
            }
            col += cell->col_span;
        }
    }

    /* Equal-widths: widen every column to the max (runs after pass 2 so it
     * can only widen). */
    if (table->config.equal_widths) {
        unsigned max_width = 0;
        for (i = 0; i < table->config.n_cols; ++i) {
            max_width = ucs_max(max_width, widths[i]);
        }
        for (i = 0; i < table->config.n_cols; ++i) {
            widths[i] = max_width;
        }
    }
}


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

    default:
        ucs_fatal("invalid cell alignment %d", cell->align);
    }
}


static void
ucs_table_append_row_prefix(const ucs_table_t *table, ucs_string_buffer_t *strb)
{
    if (table->config.row_prefix != NULL) {
        ucs_string_buffer_appendf(strb, "%s", table->config.row_prefix);
    }
}


static void ucs_table_render_cells(const ucs_table_t *table,
                                   const unsigned *widths,
                                   const ucs_table_cells_t *cells,
                                   ucs_string_buffer_t *strb)
{
    const ucs_table_cell_t *cell;
    unsigned col = 0;

    ucs_table_append_row_prefix(table, strb);

    ucs_array_for_each(cell, cells) {
        ucs_table_render_cell(strb, cell,
                              ucs_table_cell_character_width(table, widths, col,
                                                             cell->col_span));
        col += cell->col_span;
    }
    ucs_string_buffer_appendf(strb, "|");
}


static void
ucs_table_render_separator(const ucs_table_t *table, const unsigned *widths,
                           unsigned merged_cols, ucs_string_buffer_t *strb)
{
    unsigned i;

    ucs_assertv(merged_cols <= table->config.n_cols,
                "merged_cols=%u exceeds n_cols=%u", merged_cols,
                table->config.n_cols);

    ucs_table_append_row_prefix(table, strb);

    /* Blank carry-over: continue the cells above into the cells below. */
    for (i = 0; i < merged_cols; ++i) {
        ucs_string_buffer_appendc(strb, '|', 1);
        ucs_string_buffer_appendc(strb, ' ', widths[i] + 2);
    }

    for (i = merged_cols; i < table->config.n_cols; ++i) {
        ucs_string_buffer_appendc(strb, '+', 1);
        ucs_string_buffer_appendc(strb, '-', widths[i] + 2);
    }

    ucs_string_buffer_appendc(strb, '+', 1);
    ucs_string_buffer_appendc(strb, '\n', 1);
}


void ucs_table_render(ucs_table_t *table, ucs_string_buffer_t *strb)
{
    const ucs_table_entry_t *entry;
    unsigned i;

    ucs_table_compute_widths(table);

    /* Top frame */
    ucs_table_render_separator(table, table->widths, 0, strb);

    /* Body rows and separators */
    for (i = 0; i < ucs_array_length(&table->entries); ++i) {
        entry = &ucs_array_elem(&table->entries, i);
        switch (entry->kind) {
        case UCS_TABLE_ENTRY_ROW:
            ucs_table_render_cells(table, table->widths, &entry->cells, strb);
            ucs_string_buffer_appendf(strb, "\n");
            break;

        case UCS_TABLE_ENTRY_SEPARATOR:
            ucs_table_render_separator(table, table->widths, entry->merged_cols,
                                       strb);
            break;

        default:
            ucs_fatal("invalid table entry kind %d", entry->kind);
        }
    }

    /* Bottom frame; skip when the last entry is already a separator. */
    if (ucs_array_is_empty(&table->entries) ||
        (ucs_array_last(&table->entries)->kind != UCS_TABLE_ENTRY_SEPARATOR)) {
        ucs_table_render_separator(table, table->widths, 0, strb);
    }
}


void ucs_table_print(ucs_table_t *table)
{
    ucs_string_buffer_t strb = UCS_STRING_BUFFER_INITIALIZER;

    ucs_table_render(table, &strb);
    printf("%s", ucs_string_buffer_cstr(&strb));
    ucs_string_buffer_cleanup(&strb);
}


ucs_table_stream_row_t *ucs_table_stream_row_create(ucs_table_t *table)
{
    ucs_table_stream_row_t *row;
    ucs_status_t status;

    row = ucs_malloc(sizeof(*row), "ucs_table_stream_row");
    if (row == NULL) {
        ucs_fatal("failed to allocate table stream row");
    }

    row->table = table;
    ucs_array_init_dynamic(&row->cells);

    status = ucs_array_reserve(&row->cells, table->config.n_cols);
    if (status != UCS_OK) {
        ucs_fatal("failed to reserve stream row cells");
    }

    ++table->n_stream_rows;
    return row;
}


void ucs_table_stream_row_reset(ucs_table_stream_row_t *row)
{
    ucs_table_cell_t *cell;

    /* Full cleanup so the next add_cell doesn't leak previous backing
     * memory. */
    ucs_array_for_each(cell, &row->cells) {
        ucs_string_buffer_cleanup(&cell->text);
    }
    ucs_array_set_length(&row->cells, 0);
}


void ucs_table_stream_row_destroy(ucs_table_stream_row_t *row)
{
    ucs_table_cell_t *cell;

    ucs_array_for_each(cell, &row->cells) {
        ucs_string_buffer_cleanup(&cell->text);
    }
    ucs_array_cleanup_dynamic(&row->cells);

    --row->table->n_stream_rows;
    ucs_free(row);
}


void ucs_table_stream_row_add_cell_empty(ucs_table_stream_row_t *row,
                                         unsigned col_span)
{
    ucs_table_cells_add(&row->cells, col_span, UCS_TABLE_ALIGN_LEFT);
}


void ucs_table_stream_row_add_cell_fmt(ucs_table_stream_row_t *row,
                                       unsigned col_span,
                                       ucs_table_align_t align, const char *fmt,
                                       ...)
{
    va_list ap;

    va_start(ap, fmt);
    ucs_table_cells_add_fmt(&row->cells, col_span, align, fmt, ap);
    va_end(ap);
}


void ucs_table_render_row(const ucs_table_stream_row_t *row,
                          ucs_string_buffer_t *strb)
{
    ucs_table_render_cells(row->table, row->table->widths, &row->cells, strb);
}


void ucs_table_print_row(const ucs_table_stream_row_t *row)
{
    ucs_string_buffer_t strb = UCS_STRING_BUFFER_INITIALIZER;

    ucs_table_render_row(row, &strb);
    printf("%s\n", ucs_string_buffer_cstr(&strb));
    ucs_string_buffer_cleanup(&strb);
}


void ucs_table_print_separator(ucs_table_t *table)
{
    ucs_string_buffer_t strb = UCS_STRING_BUFFER_INITIALIZER;

    ucs_table_render_separator(table, table->widths, 0, &strb);
    printf("%s", ucs_string_buffer_cstr(&strb));
    ucs_string_buffer_cleanup(&strb);
}
