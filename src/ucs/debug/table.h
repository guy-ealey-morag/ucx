/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2026. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCS_TABLE_H_
#define UCS_TABLE_H_

#include <ucs/datastruct/array.h>
#include <ucs/datastruct/string_buffer.h>
#include <ucs/sys/compiler_def.h>


BEGIN_C_DECLS

/**
 * Buffered ASCII table builder. Callers add rows, cells, and separators,
 * then ucs_table_render() emits the drawing into a ucs_string_buffer_t.
 *
 * Layout: a table has a fixed number of columns chosen at init time. A row
 * may have fewer cells if some use col_span > 1.
 *
 * Separators are horizontal "+---+---" rules between rows. The top/bottom
 * frames are added by render(); do not add them manually. A separator can
 * "carry over" its leading columns by passing merged_cols > 0: those
 * positions render as blank "|     " segments, visually continuing the cell
 * above into the cell below.
 *
 * Streaming rows (ucs_table_stream_row_create) are owned by the caller and
 * print against the already-computed widths, so the caller should set
 * config.min_widths to lock in widths large enough for the streamed content
 * the table never measures.
 */


typedef unsigned ucs_table_row_h;


/** Opaque, caller-owned streaming row; see ucs_table_stream_row_create(). */
typedef struct ucs_table_stream_row ucs_table_stream_row_t;


/**
 * Cell alignment, fixed at add-cell time.
 */
typedef enum {
    UCS_TABLE_ALIGN_LEFT, /**< pad on the right */
    UCS_TABLE_ALIGN_RIGHT, /**< pad on the left */
    UCS_TABLE_ALIGN_CENTER /**< pad equally (right side gets the extra
                                 when padding is odd) */
} ucs_table_align_t;


/** Cell entry (internal). */
typedef struct ucs_table_cell {
    unsigned            col_span; /**< body columns spanned */
    ucs_table_align_t   align; /**< alignment selected at add-cell time */
    ucs_string_buffer_t text; /**< cell content, owned by the table */
} ucs_table_cell_t;


typedef enum {
    UCS_TABLE_ENTRY_ROW,
    UCS_TABLE_ENTRY_SEPARATOR
} ucs_table_entry_kind_t;


/** Named dynamic array of cells; referenced by both row and stream rows. */
UCS_ARRAY_DECLARE_TYPE(ucs_table_cells_t, unsigned, ucs_table_cell_t);


/**
 * Table entry: a row (vector of cells) or a separator. `cells` is only
 * populated when `kind == UCS_TABLE_ENTRY_ROW`; `merged_cols` is only used
 * when `kind == UCS_TABLE_ENTRY_SEPARATOR`.
 */
typedef struct {
    ucs_table_entry_kind_t kind;
    unsigned               merged_cols; /**< leading columns rendered as blank
                                             carry-over (separator only) */
    ucs_table_cells_t      cells; /**< row cells (row only) */
} ucs_table_entry_t;


UCS_ARRAY_DECLARE_TYPE(ucs_table_entries_t, unsigned, ucs_table_entry_t);


/**
 * Configuration for ucs_table_init(). Zero-initialize and set the fields you
 * need; only n_cols is required.
 */
typedef struct ucs_table_config {
    /** Total number of columns; per-row cells' col_spans must sum to this. */
    unsigned       n_cols;
    /** Prepended to every rendered line, or NULL. Caller-owned; must outlive
     *  the table. */
    const char     *row_prefix;
    /** Per-column lower bounds for the computed widths, or NULL. Length must
     *  be >= n_cols. Useful for streamed rows whose content the table never
     *  measures. Deep-copied into the table during init. */
    const unsigned *min_widths;
    /** When non-zero, render every column at the maximum computed width so
     *  all columns are equal-width. */
    int            equal_widths;
} ucs_table_config_t;


/**
 * Buffered ASCII table. Manipulate via the API; do not access fields.
 */
typedef struct ucs_table {
    ucs_table_config_t  config; /**< caller config, with min_widths
                                     deep-copied */
    ucs_table_entries_t entries;
    unsigned            *widths; /**< per-column widths; set by render */
    unsigned            n_stream_rows; /**< live stream rows; asserted 0 at
                                            cleanup */
} ucs_table_t;


/**
 * Initialize a buffered table.
 *
 * @param [out] table   Table to initialize.
 * @param [in]  config  Configuration (non-NULL); see ucs_table_config_t.
 *                      Copied into the table; min_widths is deep-copied.
 */
void ucs_table_init(ucs_table_t *table, const ucs_table_config_t *config);


/**
 * Release all storage owned by the table. After this call the table is
 * unusable. All stream rows must be destroyed first.
 *
 * @param [in,out] table  Table to clean up.
 */
void ucs_table_cleanup(ucs_table_t *table);


/**
 * Append a horizontal separator with carry-over over the leading
 * `merged_cols` columns. Those positions render as blank "|     " segments
 * (leftmost corner becomes '|'); the rest render as "+----" dashed segments.
 *
 * The top and bottom frame separators are inserted automatically by
 * ucs_table_render(); do not add them explicitly.
 *
 * @param [in,out] table        Table to append to.
 * @param [in]     merged_cols  Number of leading columns to render as blank
 *                              carry-over (must be <= n_cols).
 */
void ucs_table_add_separator_with_merged_cols(ucs_table_t *table,
                                              unsigned merged_cols);


/**
 * Shorthand for ucs_table_add_separator_with_merged_cols(table, 0).
 *
 * @param [in,out] table  Table to append to.
 */
void ucs_table_add_separator(ucs_table_t *table);


/**
 * Begin a new row. Subsequent ucs_table_row_add_cell_*() calls populate it
 * left-to-right; the sum of col_spans must equal n_cols. The returned handle
 * is valid until the table is cleaned up.
 *
 * @param [in,out] table  Table to append to.
 * @return Row handle for use with add-cell functions.
 */
ucs_table_row_h ucs_table_add_row(ucs_table_t *table);


/**
 * Add an empty cell.
 *
 * @param [in,out] table     Table that owns @a row.
 * @param [in]     row       Row returned by ucs_table_add_row().
 * @param [in]     col_span  Number of columns to span.
 */
void ucs_table_row_add_cell_empty(ucs_table_t *table, ucs_table_row_h row,
                                  unsigned col_span);


/**
 * Add a cell with printf-style content. Asserts the result has no '\n'.
 *
 * @param [in,out] table     Table that owns @a row.
 * @param [in]     row       Row returned by ucs_table_add_row().
 * @param [in]     col_span  Number of columns to span.
 * @param [in]     align     Cell alignment.
 * @param [in]     fmt       printf format string.
 */
void ucs_table_row_add_cell_fmt(ucs_table_t *table, ucs_table_row_h row,
                                unsigned col_span, ucs_table_align_t align,
                                const char *fmt, ...) UCS_F_PRINTF(5, 6);


/**
 * Render the table into @a strb. Recomputes column widths on every call,
 * adapting to any rows or separators added since the previous render, and
 * emits top frame + body rows/separators + bottom frame.
 *
 * @param [in,out] table  Table to render.
 * @param [in,out] strb   Destination string buffer.
 */
void ucs_table_render(ucs_table_t *table, ucs_string_buffer_t *strb);


/**
 * Render the table directly to stdout.
 *
 * @param [in,out] table  Table to print.
 */
void ucs_table_print(ucs_table_t *table);


/**
 * Create a streaming row against @a table. The row is caller-owned (not
 * stored in the table's entries) and must be released with
 * ucs_table_stream_row_destroy(). Populate via
 * ucs_table_stream_row_add_cell_*() and emit via ucs_table_print_row() /
 * ucs_table_render_row().
 *
 * @param [in,out] table  Table (must already have been rendered).
 * @return New stream row.
 *
 * @note Multiple stream rows can co-exist on the same table.
 */
ucs_table_stream_row_t *ucs_table_stream_row_create(ucs_table_t *table);


/**
 * Reset a stream row so it can be re-populated with different cells. Cell
 * layout may differ between resets up to n_cols.
 *
 * @param [in,out] row  Stream row to reset.
 */
void ucs_table_stream_row_reset(ucs_table_stream_row_t *row);


/**
 * Release a stream row. The caller must not use the row afterwards.
 *
 * @param [in,out] row  Stream row to destroy.
 */
void ucs_table_stream_row_destroy(ucs_table_stream_row_t *row);


/**
 * Add an empty cell to a stream row.
 *
 * @param [in,out] row       Stream row.
 * @param [in]     col_span  Number of columns to span.
 */
void ucs_table_stream_row_add_cell_empty(ucs_table_stream_row_t *row,
                                         unsigned col_span);


/**
 * Add a cell with printf-style content to a stream row. Asserts the result
 * has no '\n'.
 *
 * @param [in,out] row       Stream row.
 * @param [in]     col_span  Number of columns to span.
 * @param [in]     align     Cell alignment.
 * @param [in]     fmt       printf format string.
 */
void ucs_table_stream_row_add_cell_fmt(ucs_table_stream_row_t *row,
                                       unsigned col_span,
                                       ucs_table_align_t align, const char *fmt,
                                       ...) UCS_F_PRINTF(4, 5);


/**
 * Render a stream row into @a strb WITHOUT a trailing newline, so callers can
 * splice extra content between the row's closing '|' and the line break.
 *
 * @param [in]     row   Stream row.
 * @param [in,out] strb  Destination string buffer.
 */
void ucs_table_render_row(const ucs_table_stream_row_t *row,
                          ucs_string_buffer_t *strb);


/**
 * Render a stream row to stdout with a trailing newline.
 *
 * @param [in] row  Stream row.
 */
void ucs_table_print_row(const ucs_table_stream_row_t *row);


/**
 * Write a horizontal separator (bottom-frame shape) to stdout. Used to close
 * a streamed region after one or more ucs_table_print_row() calls.
 *
 * @param [in] table  Table (must already have been rendered).
 */
void ucs_table_print_separator(ucs_table_t *table);


END_C_DECLS

#endif
