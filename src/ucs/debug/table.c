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


/* 256 '-' characters (4 chunks of 64) + implicit NUL. The literal here is the
 * single source of truth for the maximum supported column width; the bound
 * UCS_TABLE_DASH_MAX below is derived from it via sizeof. Picking a value
 * comfortably above any realistic UCX column width avoids silent truncation
 * of separator lines. */
static const char ucs_table_dashes[] =
        "----------------------------------------------------------------"
        "----------------------------------------------------------------"
        "----------------------------------------------------------------"
        "----------------------------------------------------------------";


/* Maximum supported column width for the dashes constant. */
#define UCS_TABLE_DASH_MAX ((int)(sizeof(ucs_table_dashes) - 1))


/* Stream-row cells live in an array embedded in the row itself; regular
 * rows reference an entry index in table->entries. The kind discriminator
 * routes ucs_table_row_add_cell to the right backing storage. */
typedef enum {
    UCS_TABLE_ROW_REGULAR,
    UCS_TABLE_ROW_STREAM
} ucs_table_row_kind_t;


/* Row handles are heap-allocated separately so they remain valid even
 * if 'entries' is later reallocated. */
struct ucs_table_row {
    ucs_table_t          *table;
    ucs_table_row_kind_t kind;
    union {
        /* UCS_TABLE_ROW_REGULAR: index into table->entries. */
        unsigned          entry_idx;
        /* UCS_TABLE_ROW_STREAM: cells owned by this row (not in
         * table->entries). Reset on each ucs_table_stream_row_reset(). */
        ucs_table_cells_t cells;
    } u;
};


void ucs_table_init(ucs_table_t *table, unsigned n_body_cols)
{
    table->n_body_cols   = n_body_cols;
    table->row_prefix    = NULL;
    table->widths        = NULL;
    table->min_widths    = NULL;
    table->equal_widths  = 0;
    table->n_stream_rows = 0;
    ucs_array_init_dynamic(&table->entries);
    ucs_array_init_dynamic(&table->row_handles);
}


void ucs_table_set_row_prefix(ucs_table_t *table, const char *prefix)
{
    table->row_prefix = prefix;
}


void ucs_table_set_equal_widths(ucs_table_t *table, int equal_widths)
{
    table->equal_widths = equal_widths;
}


void ucs_table_set_min_col_widths(ucs_table_t *table, const int *min_widths)
{
    /* Setting min widths after the table has been rendered would have
     * no effect (widths are already computed) and would leave the user
     * with a false sense of layout. Catch this in debug builds. */
    ucs_assert(table->widths == NULL);

    ucs_free(table->min_widths);
    table->min_widths = NULL;

    if (min_widths == NULL) {
        return;
    }

    table->min_widths = ucs_malloc(table->n_body_cols *
                                           sizeof(*table->min_widths),
                                   "ucs_table_min_widths");
    if (table->min_widths == NULL) {
        ucs_fatal("failed to allocate table min widths");
    }
    memcpy(table->min_widths, min_widths,
           table->n_body_cols * sizeof(*table->min_widths));
}


void ucs_table_cleanup(ucs_table_t *table)
{
    ucs_table_entry_t *entry;
    ucs_table_cell_t *cell;
    ucs_table_row_t **row_p;

    /* Stream rows hold references to this table; the caller must
     * destroy them before tearing down the table. */
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

    ucs_free(table->min_widths);
    table->min_widths = NULL;
}


void ucs_table_add_separator_with_merged_cells(ucs_table_t *table,
                                               unsigned merged_cols)
{
    ucs_table_entry_t *entry;
    unsigned len;

    /* Adding entries after the table has been rendered would silently
     * overflow the computed widths. Caught in debug builds. */
    ucs_assert(table->widths == NULL);

    ucs_assertv(merged_cols <= table->n_body_cols,
                "merged_cols=%u exceeds n_body_cols=%u", merged_cols,
                table->n_body_cols);

    /* Reject consecutive separators */
    len = ucs_array_length(&table->entries);
    ucs_assert((len == 0) || (ucs_array_elem(&table->entries, len - 1).kind !=
                              UCS_TABLE_ENTRY_SEPARATOR));

    entry = ucs_array_append(&table->entries, ucs_fatal("failed to grow table "
                                                        "entries"));
    entry->kind        = UCS_TABLE_ENTRY_SEPARATOR;
    entry->merged_cols = merged_cols;
    /* `cells` is unused for separators; leave the array in zero-init state
     * so cleanup can safely call ucs_array_cleanup_dynamic on it. */
    ucs_array_init_dynamic(&entry->cells);
}


void ucs_table_add_separator(ucs_table_t *table)
{
    ucs_table_add_separator_with_merged_cells(table, 0);
}


ucs_table_row_t *ucs_table_add_row(ucs_table_t *table)
{
    ucs_table_entry_t *entry;
    ucs_table_row_t *row, **row_slot;
    ucs_status_t status;

    /* Adding entries after the table has been rendered would silently
     * overflow the computed widths. Caught in debug builds. */
    ucs_assert(table->widths == NULL);

    row = ucs_malloc(sizeof(*row), "ucs_table_row");
    if (row == NULL) {
        ucs_fatal("failed to allocate table row");
    }

    entry       = ucs_array_append(&table->entries,
                                   ucs_fatal("failed to grow table entries"));
    entry->kind = UCS_TABLE_ENTRY_ROW;
    ucs_array_init_dynamic(&entry->cells);

    /* Pre-reserve cells to n_body_cols so subsequent add_cell calls do not
     * reallocate the cells buffer. Cell pointers handed out by add_cell
     * then remain valid for the lifetime of the table. */
    status = ucs_array_reserve(&entry->cells, table->n_body_cols);
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


/* Return the cells array for a row regardless of whether it's a regular
 * row (cells live in table->entries) or a stream row (cells live in
 * row->u.cells). */
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
    ucs_table_cells_t *cells;
    ucs_table_cell_t *cell;

    /* On regular rows: cells must be added before render, otherwise
     * they would silently overflow the computed column widths. Stream
     * rows are already gated by stream_row_create requiring widths to
     * be alive, so they have no equivalent restriction here. */
    if (row->kind == UCS_TABLE_ROW_REGULAR) {
        ucs_assert(row->table->widths == NULL);
    }

    cells = ucs_table_row_cells(row);

    /* Pre-reservation in add_row / stream_row_create guarantees this
     * append cannot grow the cells buffer, so the returned pointer is
     * stable. */
    cell = ucs_array_append(cells, ucs_fatal("table row exceeded body "
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


/* Total visible width of a cell that spans `col_span` body columns,
 * given per-body-column widths. Each merged column contributes its own
 * width plus the 3 characters (" | ") that would have separated it from
 * the previous cell if it had not been merged. */
static int ucs_table_cell_pixel_width(const int *body_widths, unsigned start,
                                      unsigned col_span)
{
    int width = 0;
    unsigned i;

    for (i = 0; i < col_span; ++i) {
        width += body_widths[start + i];
    }
    width += 3 * ((int)col_span - 1);
    return width;
}


/* Visible content length of a cell: the cell text is exactly the bytes
 * that will land inside the rendered cell. */
static unsigned ucs_table_cell_content_len(ucs_table_cell_t *cell)
{
    return ucs_string_buffer_length(&cell->text);
}


/* Compute per-body-column widths needed to fit every cell. Two passes:
 *   Pass 1: derive per-column widths from col_span=1 cells.
 *   Pass 2: expand the rightmost spanned column of each merged cell to
 *           absorb any remaining deficit.
 *
 * Splitting the passes keeps merged cells from over-expanding the
 * rightmost spanned column when they happen to be added before the body
 * rows that would naturally establish the column widths.
 *
 * Each column starts at table->min_widths[i] (or 0 when unset) so the
 * caller can lock in a lower bound for stream-row content the table
 * does not see during measurement. */
/* Allocate and populate table->widths if it has not been computed
 * already. Idempotent — repeated calls are a no-op. Widths live on the
 * table from the first call until ucs_table_cleanup(). */
static void ucs_table_compute_widths(ucs_table_t *table)
{
    ucs_table_entry_t *entry;
    ucs_table_cell_t *cell;
    unsigned i, body_col, content_len;
    int existing;
    int *widths;

    if (table->widths != NULL) {
        return;
    }

    widths = ucs_malloc(table->n_body_cols * sizeof(*widths),
                        "ucs_table_widths");
    if (widths == NULL) {
        ucs_fatal("failed to allocate table widths");
    }

    for (i = 0; i < table->n_body_cols; ++i) {
        widths[i] = (table->min_widths != NULL) ? table->min_widths[i] : 0;
    }

    /* Pass 1: col_span == 1 cells only. */
    ucs_array_for_each(entry, &table->entries) {
        if (entry->kind != UCS_TABLE_ENTRY_ROW) {
            continue;
        }
        body_col = 0;
        ucs_array_for_each(cell, &entry->cells) {
            if (cell->col_span == 1) {
                content_len      = ucs_table_cell_content_len(cell);
                widths[body_col] = ucs_max(widths[body_col], (int)content_len);
            }
            body_col += cell->col_span;
        }
    }

    /* Pass 2: merged cells expand the rightmost spanned body column only
     * when their content does not fit the per-column widths from pass 1. */
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

                if ((int)content_len > existing) {
                    widths[body_col + cell->col_span - 1] += (int)content_len -
                                                             existing;
                }
            }
            body_col += cell->col_span;
        }
    }

    /* Equal-width pass: widen every body column to the maximum so all
     * columns render at the same width. Runs after pass 2 so it only
     * ever widens columns and never invalidates merged-cell fits. */
    if (table->equal_widths) {
        int max_width = 0;
        for (i = 0; i < table->n_body_cols; ++i) {
            max_width = ucs_max(max_width, widths[i]);
        }
        for (i = 0; i < table->n_body_cols; ++i) {
            widths[i] = max_width;
        }
    }

    table->widths = widths;
}


/* Format a single cell at the given pixel width. Branches on the cell's
 * explicit alignment:
 *
 *   LEFT   "| content                 |"
 *   RIGHT  "|                 content |"
 *   CENTER "|        content          |" (right side gets the extra
 *                                          space when padding is odd)
 */
static void ucs_table_render_cell(ucs_string_buffer_t *strb,
                                  const ucs_table_cell_t *cell, int pixel_width)
{
    const char *cstr = ucs_string_buffer_cstr(&cell->text);
    int content_len, pad, left_pad, right_pad;

    switch (cell->align) {
    case UCS_TABLE_ALIGN_LEFT:
        ucs_string_buffer_appendf(strb, "| %-*s ", pixel_width, cstr);
        break;

    case UCS_TABLE_ALIGN_RIGHT:
        ucs_string_buffer_appendf(strb, "| %*s ", pixel_width, cstr);
        break;

    case UCS_TABLE_ALIGN_CENTER:
        content_len = (int)strlen(cstr);
        pad         = ucs_max(pixel_width - content_len, 0);
        left_pad    = pad / 2;
        right_pad   = pad - left_pad;
        ucs_string_buffer_appendf(strb, "| %*s%s%*s ", left_pad, "", cstr,
                                  right_pad, "");
        break;
    }
}


/* Render one body row (line of cells) into strb. Used for both
 * regular and stream rows: the caller passes the appropriate cells
 * array. The closing "|" is emitted without a trailing newline; the
 * caller appends '\n' when needed — stream rows in -X mode skip the
 * newline so the caller can splice trailing content before the line
 * break. */
static void ucs_table_render_cells(const ucs_table_t *table,
                                   ucs_string_buffer_t *strb,
                                   const ucs_table_cells_t *cells)
{
    const ucs_table_cell_t *cell;
    unsigned body_col = 0;

    ucs_assert(table->widths != NULL);

    if (table->row_prefix != NULL) {
        ucs_string_buffer_appendf(strb, "%s", table->row_prefix);
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


/*
 * Render a horizontal separator over the body-column widths. The first
 * `merged_cols` body columns render as blank "|     " segments
 * (the cell below is merged with the cell above); the rest render as
 * "+-----" dashed segments. Corners between merged segments render as '|'
 * so the carry-over region is visually continuous.
 */
static void ucs_table_render_separator(const ucs_table_t *table,
                                       ucs_string_buffer_t *strb,
                                       unsigned merged_cols)
{
    char left_corner;
    int width;
    unsigned i;

    ucs_assert(table->widths != NULL);

    if (table->row_prefix != NULL) {
        ucs_string_buffer_appendf(strb, "%s", table->row_prefix);
    }

    for (i = 0; i < table->n_body_cols; ++i) {
        width = table->widths[i];
        ucs_assertv((width >= 0) && (width <= UCS_TABLE_DASH_MAX),
                    "widths[%u]=%d out of range [0, %d]", i, width,
                    UCS_TABLE_DASH_MAX);

        if (i < merged_cols) {
            left_corner = '|';
        } else {
            left_corner = '+';
        }

        if (i < merged_cols) {
            ucs_string_buffer_appendf(strb, "%c %-*s ", left_corner, width, "");
        } else {
            ucs_string_buffer_appendf(strb, "%c-%.*s-", left_corner, width,
                                      ucs_table_dashes);
        }
    }

    ucs_string_buffer_appendf(strb, "+\n");
}


void ucs_table_render(ucs_table_t *table, ucs_string_buffer_t *strb)
{
    const ucs_table_entry_t *entry;
    unsigned i;

    ucs_table_compute_widths(table);

    ucs_table_render_separator(table, strb, 0);

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

    /* Bottom frame; skip only when the last entry is already a
     * separator (so we don't render two consecutive separator lines). */
    if (ucs_array_is_empty(&table->entries) ||
        (ucs_array_last(&table->entries)->kind != UCS_TABLE_ENTRY_SEPARATOR)) {
        ucs_table_render_separator(table, strb, 0);
    }

    /* Note: widths are intentionally NOT freed here. They stay alive on
     * the table until ucs_table_cleanup() so that subsequent streamed
     * rows can render against the same column layout. */
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

    /* Stream rows render against the table's widths, so the table must
     * have been rendered (or printed) at least once first. */
    ucs_assert(table->widths != NULL);

    row = ucs_malloc(sizeof(*row), "ucs_table_stream_row");
    if (row == NULL) {
        ucs_fatal("failed to allocate table stream row");
    }

    row->table = table;
    row->kind  = UCS_TABLE_ROW_STREAM;
    ucs_array_init_dynamic(&row->u.cells);

    /* Pre-reserve to n_body_cols so add_cell calls never reallocate
     * the cells buffer (cell pointers stay stable). */
    status = ucs_array_reserve(&row->u.cells, table->n_body_cols);
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

    /* Full cleanup of each cell's string buffer (not just `reset`) so
     * the next add_cell call's ucs_string_buffer_init doesn't leak the
     * previous backing memory. */
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

    /* Drop the row from the table's row_handles array so cleanup
     * doesn't double-free. The handle array is unordered for our
     * purposes, so we replace-with-last and shrink. */
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
    ucs_assert(row->table->widths != NULL);

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

    ucs_assert(table->widths != NULL);

    ucs_table_render_separator(table, &strb, 0);
    printf("%s", ucs_string_buffer_cstr(&strb));
    ucs_string_buffer_cleanup(&strb);
}


void ucs_log_print_compact_lines(ucs_string_buffer_t *strb)
{
    char *line;

    ucs_string_buffer_for_each_token(line, strb, "\n") {
        ucs_log_print_compact(line);
    }
}
