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


/* Row handles are heap-allocated separately so they remain valid even
 * if 'entries' is later reallocated. */
struct ucs_table_row {
    ucs_table_t *table;
    unsigned    entry_idx;
};


/* Side selector for cell setters. */
typedef enum {
    UCS_TABLE_SIDE_LEFT,
    UCS_TABLE_SIDE_RIGHT
} ucs_table_side_t;


void ucs_table_init(ucs_table_t *table, unsigned n_body_cols)
{
    table->n_body_cols  = n_body_cols;
    table->row_prefix   = NULL;
    table->widths       = NULL;
    table->equal_widths = 0;
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


void ucs_table_cleanup(ucs_table_t *table)
{
    ucs_table_entry_t *entry;
    ucs_table_cell_t *cell;
    ucs_table_row_t **row_p;

    /* widths is only set while a render call is in progress. */
    ucs_assert(table->widths == NULL);

    ucs_array_for_each(entry, &table->entries) {
        if (entry->kind != UCS_TABLE_ENTRY_ROW) {
            continue;
        }
        ucs_array_for_each(cell, &entry->cells) {
            ucs_string_buffer_cleanup(&cell->left);
            ucs_string_buffer_cleanup(&cell->right);
        }
        ucs_array_cleanup_dynamic(&entry->cells);
    }
    ucs_array_cleanup_dynamic(&table->entries);

    ucs_array_for_each(row_p, &table->row_handles) {
        ucs_free(*row_p);
    }
    ucs_array_cleanup_dynamic(&table->row_handles);
}


void ucs_table_add_separator(ucs_table_t *table)
{
    ucs_table_entry_t *entry;
    unsigned len;

    /* Reject consecutive separators */
    len = ucs_array_length(&table->entries);
    ucs_assert((len == 0) || (ucs_array_elem(&table->entries, len - 1).kind !=
                              UCS_TABLE_ENTRY_SEPARATOR));

    entry       = ucs_array_append(&table->entries,
                                   ucs_fatal("failed to grow table entries"));
    entry->kind = UCS_TABLE_ENTRY_SEPARATOR;
    /* `cells` is unused for separators; leave the array in zero-init state
     * so cleanup can safely call ucs_array_cleanup_dynamic on it. */
    ucs_array_init_dynamic(&entry->cells);
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

    /* Pre-reserve cells to n_body_cols so subsequent add_cell calls do not
     * reallocate the cells buffer. Cell pointers handed out by add_cell
     * then remain valid for the lifetime of the table. */
    status = ucs_array_reserve(&entry->cells, table->n_body_cols);
    if (status != UCS_OK) {
        ucs_fatal("failed to reserve table row cells");
    }

    row->table     = table;
    row->entry_idx = ucs_array_length(&table->entries) - 1;

    row_slot  = ucs_array_append(&table->row_handles,
                                 ucs_fatal("failed to grow table row handles"));
    *row_slot = row;

    return row;
}


static ucs_table_entry_t *ucs_table_row_entry(ucs_table_row_t *row)
{
    ucs_table_entry_t *entry;

    entry = &ucs_array_elem(&row->table->entries, row->entry_idx);
    ucs_assert(entry->kind == UCS_TABLE_ENTRY_ROW);
    return entry;
}


ucs_table_cell_t *
ucs_table_row_add_cell(ucs_table_row_t *row, unsigned col_span)
{
    ucs_table_entry_t *entry = ucs_table_row_entry(row);
    ucs_table_cell_t *cell;

    /* Pre-reservation in add_row guarantees this append cannot grow the
     * cells buffer, so the returned pointer is stable. */
    cell           = ucs_array_append(&entry->cells,
                                      ucs_fatal("table row exceeded body column "
                                                "count"));
    cell->col_span = col_span;
    ucs_string_buffer_init(&cell->left);
    ucs_string_buffer_init(&cell->right);
    return cell;
}


static void ucs_table_cell_vappendf(ucs_table_cell_t *cell,
                                    ucs_table_side_t side, const char *fmt,
                                    va_list ap)
{
    ucs_string_buffer_t *strb = (side == UCS_TABLE_SIDE_LEFT) ? &cell->left :
                                                                &cell->right;

    ucs_string_buffer_vappendf(strb, fmt, ap);

    ucs_assertv(strchr(ucs_string_buffer_cstr(strb), '\n') == NULL,
                "table cell content must not contain '\\n': '%s'",
                ucs_string_buffer_cstr(strb));
}


void ucs_table_cell_appendf_left(ucs_table_cell_t *cell, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    ucs_table_cell_vappendf(cell, UCS_TABLE_SIDE_LEFT, fmt, ap);
    va_end(ap);
}


void ucs_table_cell_appendf_right(ucs_table_cell_t *cell, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    ucs_table_cell_vappendf(cell, UCS_TABLE_SIDE_RIGHT, fmt, ap);
    va_end(ap);
}


ucs_table_cell_t *ucs_table_row_add_cell_left(ucs_table_row_t *row,
                                              unsigned col_span,
                                              const char *fmt, ...)
{
    ucs_table_cell_t *cell = ucs_table_row_add_cell(row, col_span);
    va_list ap;

    va_start(ap, fmt);
    ucs_table_cell_vappendf(cell, UCS_TABLE_SIDE_LEFT, fmt, ap);
    va_end(ap);

    return cell;
}


ucs_table_cell_t *ucs_table_row_add_cell_right(ucs_table_row_t *row,
                                               unsigned col_span,
                                               const char *fmt, ...)
{
    ucs_table_cell_t *cell = ucs_table_row_add_cell(row, col_span);
    va_list ap;

    va_start(ap, fmt);
    ucs_table_cell_vappendf(cell, UCS_TABLE_SIDE_RIGHT, fmt, ap);
    va_end(ap);

    return cell;
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


/* Compute per-body-column widths needed to fit every cell. Two passes:
 *   Pass 1: derive per-column widths from col_span=1 cells.
 *   Pass 2: expand the rightmost spanned column of each merged cell to
 *           absorb any remaining deficit.
 *
 * Splitting the passes keeps merged cells from over-expanding the
 * rightmost spanned column when they happen to be added before the body
 * rows that would naturally establish the column widths. */
static void ucs_table_compute_widths(const ucs_table_t *table, int *widths)
{
    ucs_table_entry_t *entry;
    ucs_table_cell_t *cell;
    unsigned i, body_col, content_len;
    int existing;

    for (i = 0; i < table->n_body_cols; ++i) {
        widths[i] = 0;
    }

    /* Pass 1: col_span == 1 cells only. */
    ucs_array_for_each(entry, &table->entries) {
        if (entry->kind != UCS_TABLE_ENTRY_ROW) {
            continue;
        }
        body_col = 0;
        ucs_array_for_each(cell, &entry->cells) {
            if (cell->col_span == 1) {
                content_len = ucs_string_buffer_length(&cell->left) +
                              ucs_string_buffer_length(&cell->right);

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
                content_len = ucs_string_buffer_length(&cell->left) +
                              ucs_string_buffer_length(&cell->right);

                existing = ucs_table_cell_pixel_width(widths, body_col,
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
}


/* Format a single cell at the given pixel width. Three cases:
 *  - right is empty -> left-aligned
 *  - left is empty  -> right-aligned
 *  - both set       -> "left<spaces>right" with the gap padding the
 *                      cell to its full width.
 */
static void ucs_table_render_cell(ucs_string_buffer_t *strb,
                                  const ucs_table_cell_t *cell, int pixel_width)
{
    const char *left_cstr  = ucs_string_buffer_cstr(&cell->left);
    const char *right_cstr = ucs_string_buffer_cstr(&cell->right);
    int left_len, right_width;

    if (ucs_string_is_empty(right_cstr)) {
        /* left-only */
        ucs_string_buffer_appendf(strb, "| %-*s ", pixel_width, left_cstr);
    } else if (ucs_string_is_empty(left_cstr)) {
        /* right-only */
        ucs_string_buffer_appendf(strb, "| %*s ", pixel_width, right_cstr);
    } else {
        /* both sides */
        left_len    = (int)strlen(left_cstr);
        right_width = ucs_max(pixel_width - left_len, 0);
        ucs_assertv(right_width > 0,
                    "cell content '%s' + '%s' exceeds width %d", left_cstr,
                    right_cstr, pixel_width);
        ucs_string_buffer_appendf(strb, "| %s%*s ", left_cstr, right_width,
                                  right_cstr);
    }
}


static void ucs_table_render_row(const ucs_table_t *table,
                                 ucs_string_buffer_t *strb,
                                 const ucs_table_entry_t *entry)
{
    const ucs_table_cell_t *cell;
    unsigned body_col = 0;

    ucs_assert(table->widths != NULL);

    if (table->row_prefix != NULL) {
        ucs_string_buffer_appendf(strb, "%s", table->row_prefix);
    }

    ucs_array_for_each(cell, &entry->cells) {
        ucs_table_render_cell(strb, cell,
                              ucs_table_cell_pixel_width(table->widths,
                                                         body_col,
                                                         cell->col_span));
        body_col += cell->col_span;
    }
    ucs_string_buffer_appendf(strb, "|\n");
}


/*
 * Render a horizontal separator over the body-column widths. The first
 * `merged_cols` body columns render as blank "|     " segments
 * (the cell below is merged with the cell above); the rest render as
 * "+-----" dashed segments. The very leftmost corner is '|' iff
 * merged_cols > 0; intermediate corners between merged cells
 * stay '+'.
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

        if ((i == 0) && (merged_cols > 0)) {
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


/* Locate the nearest ROW entry strictly after entry_idx, or NULL if
 * the separator is at (or only followed by) the bottom of the table. */
static const ucs_table_entry_t *
ucs_table_row_after(const ucs_table_t *table, unsigned entry_idx)
{
    unsigned i;

    for (i = entry_idx + 1; i < ucs_array_length(&table->entries); ++i) {
        const ucs_table_entry_t *entry = &ucs_array_elem(&table->entries, i);
        if (entry->kind == UCS_TABLE_ENTRY_ROW) {
            return entry;
        }
    }
    return NULL;
}


/* Locate the nearest ROW entry strictly before entry_idx. */
static const ucs_table_entry_t *
ucs_table_row_before(const ucs_table_t *table, unsigned entry_idx)
{
    unsigned i;

    for (i = entry_idx; i > 0; --i) {
        const ucs_table_entry_t *entry = &ucs_array_elem(&table->entries,
                                                         i - 1);
        if (entry->kind == UCS_TABLE_ENTRY_ROW) {
            return entry;
        }
    }
    return NULL;
}


/* Number of leading body columns to render as blank segments in the
 * separator at entry_idx. A column is "merged" when the cell directly
 * below has empty left and right content, which means it visually
 * extends the cell above through the separator. Returns 0 if the
 * separator has no row on one of its sides (top or bottom of table). */
static unsigned
ucs_table_separator_merged_cols(const ucs_table_t *table, unsigned entry_idx)
{
    const ucs_table_entry_t *row_above, *row_below;
    ucs_table_cell_t *cell;
    unsigned merged = 0;

    row_above = ucs_table_row_before(table, entry_idx);
    row_below = ucs_table_row_after(table, entry_idx);
    if ((row_above == NULL) || (row_below == NULL)) {
        return 0;
    }

    ucs_array_for_each(cell, &row_below->cells) {
        if ((ucs_string_buffer_length(&cell->left) != 0) ||
            (ucs_string_buffer_length(&cell->right) != 0)) {
            break;
        }
        merged += cell->col_span;
    }

    return merged;
}


void ucs_table_render(ucs_table_t *table, ucs_string_buffer_t *strb)
{
    const ucs_table_entry_t *entry;
    unsigned i;

    /* Allocate per-body-column widths on the table for the duration of
     * this render call. The render_* helpers assert the array is set. */
    ucs_assert(table->widths == NULL);
    table->widths = ucs_malloc(table->n_body_cols * sizeof(*table->widths),
                               "ucs_table_widths");
    if (table->widths == NULL) {
        ucs_fatal("failed to allocate table widths");
    }
    ucs_table_compute_widths(table, table->widths);

    /* Top frame */
    ucs_table_render_separator(table, strb, 0);

    for (i = 0; i < ucs_array_length(&table->entries); ++i) {
        entry = &ucs_array_elem(&table->entries, i);
        if (entry->kind == UCS_TABLE_ENTRY_ROW) {
            ucs_table_render_row(table, strb, entry);
            continue;
        }

        ucs_table_render_separator(table, strb,
                                   ucs_table_separator_merged_cols(table, i));
    }

    /* Bottom frame, skip when the last entry is already a separator */
    if (ucs_array_is_empty(&table->entries) ||
        (ucs_array_last(&table->entries)->kind != UCS_TABLE_ENTRY_SEPARATOR)) {
        ucs_table_render_separator(table, strb, 0);
    }

    ucs_free(table->widths);
    table->widths = NULL;
}


void ucs_table_print(ucs_table_t *table)
{
    ucs_string_buffer_t strb = UCS_STRING_BUFFER_INITIALIZER;

    ucs_table_render(table, &strb);
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
