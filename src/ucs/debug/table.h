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

/*
 * Buffered ASCII-table builder used by UCX log output.
 *
 * The caller appends rows and separators to a ucs_table_t, then calls
 * ucs_table_render() to produce the ASCII drawing in a
 * ucs_string_buffer_t. Column widths, separator corner style, and
 * carry-over rendering (where a leading cell visually "continues" across a
 * separator) are computed from the buffered content during render.
 *
 *   Concepts:
 *
 *   - Body columns: the table is laid out over a fixed number of equal-
 *     resolution "body" columns chosen at init time. The actual visible
 *     cells per row may be fewer if some cells use col_span > 1 to merge
 *     adjacent body columns.
 *
 *   - Cell: a value in one row. Has an explicit alignment selected at
 *     add-cell time:
 *
 *       - LEFT:   "| content                 |"
 *       - RIGHT:  "|                 content |"
 *       - CENTER: "|         content         |"
 *
 *   - Separator: a horizontal "+---+---+" rule between rows. Frame
 *     separators at the top and bottom of the table are inserted
 *     automatically by render().
 *
 *   - Carry-over: opt-in per separator via the `merged_cols` argument to
 *     ucs_table_add_separator_with_merged_cells(). When non-zero, the
 *     leading `merged_cols` body columns of that separator render as blank
 *     "|     |" segments rather than dashes, and the leftmost corner is
 *     '|' instead of '+'. This is used to visually continue a column from
 *     the row above the separator into the row below. The remaining body
 *     columns render as the regular dashed segments. The plain
 *     ucs_table_add_separator() helper is shorthand for `merged_cols == 0`.
 *
 * All cell content is owned by the table; ucs_table_cleanup() releases
 * everything.
 *
 *
 *   Lifecycle:
 *
 *   A table starts in a "building" state where rows, separators, and
 *   cells can be added. Calling ucs_table_render() or ucs_table_print()
 *   computes the per-column widths and emits the table; the widths are
 *   then kept alive on the table until ucs_table_cleanup() runs. While
 *   widths are alive, the table accepts streaming rows and additional
 *   separators printed directly to stdout (see "Streamed rows" below),
 *   but it rejects new builder calls (add_row / add_separator / cell
 *   appends on regular rows / set_min_col_widths) via assertion. This
 *   prevents silent column overflow that would otherwise happen if rows
 *   were added after the widths were computed.
 *
 *   Existing one-shot callers (init + add rows + render + cleanup) see
 *   no behavior change: their widths are freed by cleanup as before.
 *
 *
 *   Streamed rows:
 *
 *   After ucs_table_render() (or ucs_table_print()), the caller can
 *   open ucs_table_stream_row_create() handles and use the regular
 *   ucs_table_row_add_cell() / ucs_table_cell_appendf() API to
 *   populate them, then call ucs_table_print_row() to emit each row
 *   using the table's already-computed widths. Streamed rows are not stored in the table's
 *   entries; they are owned by the caller and must be released with
 *   ucs_table_stream_row_destroy(). To close a streamed region, the
 *   caller calls ucs_table_print_separator() once. This pattern is
 *   used for "live" progress output where rows arrive incrementally
 *   and must align to a header rendered once at the top.
 *
 *   Streamed rows alone cannot guarantee they fit in their columns
 *   (the table did not see their content when computing widths). The
 *   caller should call ucs_table_set_min_col_widths() before render
 *   to lock in column widths that match the printf widths it will use
 *   to format streamed-row cells.
 */


typedef struct ucs_table_row ucs_table_row_t;
typedef struct ucs_table_cell ucs_table_cell_t;


/*
 * Cell alignment selector. Set at ucs_table_row_add_cell() time and
 * fixed for the lifetime of the cell.
 *
 * - LEFT:   single string, padded on the right.
 * - RIGHT:  single string, padded on the left.
 * - CENTER: single string, padded equally on both sides (right side
 *           gets the extra space when padding is odd).
 */
typedef enum {
    UCS_TABLE_ALIGN_LEFT,
    UCS_TABLE_ALIGN_RIGHT,
    UCS_TABLE_ALIGN_CENTER
} ucs_table_align_t;


/*
 * Internal type. Each cell holds a single text buffer plus an explicit
 * alignment. The buffer is managed by the table API; callers must not
 * access it directly.
 */
struct ucs_table_cell {
    unsigned            col_span;
    ucs_table_align_t   align;
    ucs_string_buffer_t text;
};


/* Internal type. */
typedef enum {
    UCS_TABLE_ENTRY_ROW,
    UCS_TABLE_ENTRY_SEPARATOR
} ucs_table_entry_kind_t;


/* Internal type. Dynamic array of cells (declared as a named type so it
 * can be referenced from both ucs_table_entry_t and stream-row storage
 * without each use creating a distinct anonymous struct). */
UCS_ARRAY_DECLARE_TYPE(ucs_table_cells_t, unsigned, ucs_table_cell_t);


/* Internal type: one entry in the table is either a row (a vector of
 * cells) or a separator marker. For separator entries, `merged_cols`
 * is the number of leading body columns to render as blank carry-over
 * segments instead of dashed segments (zero means a regular dashed
 * separator). The `cells` array is unused for separator entries. */
typedef struct {
    ucs_table_entry_kind_t kind;
    unsigned               merged_cols;
    ucs_table_cells_t      cells;
} ucs_table_entry_t;


UCS_ARRAY_DECLARE_TYPE(ucs_table_entries_t, unsigned, ucs_table_entry_t);
UCS_ARRAY_DECLARE_TYPE(ucs_table_row_handles_t, unsigned, ucs_table_row_t*);


/*
 * Buffered ASCII table. Callers should treat this as opaque: declare a
 * local of this type, pass its address to ucs_table_init(), and
 * manipulate it through the API functions below.
 */
typedef struct ucs_table {
    unsigned                n_body_cols;
    ucs_table_entries_t     entries;
    ucs_table_row_handles_t row_handles;
    /* Prepended to every rendered line (both body rows and separators).
     * NULL means no prefix. The table does not own the string; the
     * caller must keep it alive until render/cleanup. */
    const char              *row_prefix;
    /* Per-body-column widths. Lazily allocated on the first
     * ucs_table_render() / ucs_table_print() call and kept alive on
     * the table until ucs_table_cleanup(). NULL while in the
     * "building" state, non-NULL once the table has been rendered. */
    int                     *widths;
    /* Heap-copied minimum per-body-column widths, or NULL. When set,
     * ucs_table_compute_widths() initializes each column to
     * min_widths[i] before measuring content, guaranteeing column
     * widths >= the caller's mins. Owned by the table, freed in
     * ucs_table_cleanup(). */
    int                     *min_widths;
    /* When non-zero, ucs_table_render() normalizes every body-column
     * width to the maximum computed width, producing uniformly-wide
     * columns. Default: 0 (per-column widths). */
    int                     equal_widths;
    /* Number of currently-live stream rows allocated against this
     * table via ucs_table_stream_row_create(). Asserted to be zero
     * by ucs_table_cleanup() to catch caller leaks. */
    unsigned                n_stream_rows;
} ucs_table_t;


/*
 * Initialize a buffered table with n_body_cols body columns. Every row
 * subsequently added to this table must contain cells whose col_spans sum
 * exactly to n_body_cols.
 */
void ucs_table_init(ucs_table_t *table, unsigned n_body_cols);


/*
 * Release all storage owned by the table (rows, cells, per-cell string
 * buffers). After this call the table is unusable.
 */
void ucs_table_cleanup(ucs_table_t *table);


/*
 * Set a string that will be prepended to every line emitted by
 * ucs_table_render(), including both body rows and separator lines.
 * Pass NULL to disable the prefix. The table stores the pointer as-is
 * and does not copy or free it; the caller must keep the string alive
 * until the table is rendered or cleaned up.
 *
 * Example: set the prefix to "# " to render the entire table as a
 * sequence of comment lines.
 */
void ucs_table_set_row_prefix(ucs_table_t *table, const char *prefix);


/*
 * Enable or disable equal-width rendering. When `equal_widths` is
 * non-zero, ucs_table_render() expands every body column to the
 * maximum computed width so all columns render at the same width.
 * Default: disabled.
 */
void ucs_table_set_equal_widths(ucs_table_t *table, int equal_widths);


/*
 * Lock per-body-column minimum widths. ucs_table_compute_widths()
 * starts each column at min_widths[i] (or 0 when unset) before
 * measuring content, so the resulting widths are >= the caller's
 * minimums. Useful when the caller plans to populate streamed rows
 * with fixed-width printf formats and needs the table columns to
 * accommodate them.
 *
 * The widths array is copied; the caller does not need to keep it
 * alive. Pass NULL to clear any previously-set minimums.
 *
 * Must be called in the "building" state (before ucs_table_render() /
 * ucs_table_print()); asserts when the widths are already alive.
 */
void ucs_table_set_min_col_widths(ucs_table_t *table, const int *min_widths);


/*
 * Append a plain horizontal separator between rows. All body columns
 * render as the regular dashed "+----" segments. Equivalent to
 * ucs_table_add_separator_with_merged_cells(table, 0).
 *
 * Frame separators at the very top and bottom of the table are
 * inserted automatically by render(); do not add them explicitly.
 */
void ucs_table_add_separator(ucs_table_t *table);


/*
 * Append a horizontal separator with carry-over over the leading
 * `merged_cols` body columns. Those positions render as blank
 * "|     " segments (shifting the leftmost corner from '+' to '|');
 * the remaining body columns render as the regular dashed "+----"
 * segments.
 *
 * `merged_cols` must be <= the table's n_body_cols. Pass
 * `merged_cols == 0` to add a plain dashed separator (or use the
 * shorter ucs_table_add_separator()).
 *
 * Frame separators at the very top and bottom of the table are
 * inserted automatically by render(); do not add them explicitly.
 */
void ucs_table_add_separator_with_merged_cells(ucs_table_t *table,
                                               unsigned merged_cols);


/*
 * Begin a new row in the table. Subsequent add-cell calls on the returned
 * row handle populate the row left-to-right. The sum of col_spans must
 * equal the table's n_body_cols.
 *
 * The row handle is valid for use with add-cell functions until the next
 * add_row() or add_separator() call on the same table.
 */
ucs_table_row_t *ucs_table_add_row(ucs_table_t *table);


/*
 * Add an empty cell with the given alignment and return a handle that
 * can be passed to ucs_table_cell_appendf() to populate the cell, or
 * used as-is as an empty/carry-over cell. The handle is valid for the
 * lifetime of the table.
 */
ucs_table_cell_t *ucs_table_row_add_cell(ucs_table_row_t *row,
                                         unsigned col_span,
                                         ucs_table_align_t align);


/*
 * One-shot: add a cell with the given alignment and printf-style
 * content. The cell is owned by the table; no handle is returned.
 * Preferred form for cells whose content is fully known at the call
 * site. Equivalent to ucs_table_row_add_cell + ucs_table_cell_appendf.
 *
 * The same '\n' / '\t' policy as ucs_table_cell_appendf() applies to
 * the formatted result.
 */
void ucs_table_row_add_cell_fmt(ucs_table_row_t *row, unsigned col_span,
                                ucs_table_align_t align, const char *fmt, ...)
        UCS_F_PRINTF(4, 5);


/*
 * Append printf-style content to a cell handle returned by
 * ucs_table_row_add_cell(). Multiple calls concatenate.
 *
 * Asserts that the resulting buffer never contains '\n' or '\t'.
 */
void ucs_table_cell_appendf(ucs_table_cell_t *cell, const char *fmt, ...)
        UCS_F_PRINTF(2, 3);


/*
 * Render the buffered table into strb. Frame separators are prepended
 * and appended automatically. Body column widths are derived from the
 * maximum cell content per column; merged cells expand the rightmost
 * body column they span if their content does not fit in the existing
 * widths. Separator corner and carry-over rendering are derived from
 * the `merged_cols` argument passed at separator creation time (see
 * ucs_table_add_separator_with_merged_cells()).
 */
void ucs_table_render(ucs_table_t *table, ucs_string_buffer_t *strb);


/*
 * Render the buffered table and write the result directly to stdout.
 * The caller still owns `table` and must call ucs_table_cleanup() when
 * done; this function only manages the temporary render buffer.
 */
void ucs_table_print(ucs_table_t *table);


/*
 * Create a new streaming row against `table`. The row is NOT stored in
 * the table's entries; the caller owns it and must release it via
 * ucs_table_stream_row_destroy().
 *
 * Cells are populated via the regular ucs_table_row_add_cell() /
 * ucs_table_cell_appendf() API. The row is then printed with
 * ucs_table_print_row() (or ucs_table_render_row()) against the
 * table's column widths.
 *
 * Must be called after ucs_table_render() / ucs_table_print(), i.e.
 * once `table->widths` is alive. Asserts otherwise.
 *
 * Multiple stream rows can co-exist on the same table; each thread
 * that emits progress concurrently should hold its own stream row.
 */
ucs_table_row_t *ucs_table_stream_row_create(ucs_table_t *table);


/*
 * Reset a stream row so it can be re-populated with different cells.
 * Frees the cells' string buffers and clears the cell count. The row
 * is reusable across multiple ucs_table_print_row() calls.
 *
 * Cell layout (col_spans, count) may differ between resets — the row
 * always re-grows up to n_body_cols, no more.
 */
void ucs_table_stream_row_reset(ucs_table_row_t *row);


/*
 * Release a stream row. The caller must not use the row after this
 * call; ucs_table_cleanup() asserts that all stream rows have been
 * destroyed.
 */
void ucs_table_stream_row_destroy(ucs_table_row_t *row);


/*
 * Render a stream row into `strb` using the table's column widths,
 * WITHOUT a trailing newline. Lets callers splice extra content (e.g.
 * debug strings) between the row's closing `|` and the line break.
 *
 * The row must be a stream row (created via ucs_table_stream_row_create),
 * and the table must have been rendered at least once (widths alive).
 */
void ucs_table_render_row(const ucs_table_row_t *row,
                          ucs_string_buffer_t *strb);


/*
 * Render a stream row and write it to stdout with a trailing newline.
 * Thin wrapper around ucs_table_render_row().
 */
void ucs_table_print_row(const ucs_table_row_t *row);


/*
 * Write a single horizontal separator (corner-level-0, matches the
 * bottom-frame line shape) to stdout using the table's column widths.
 * Used to close a streamed region after one or more ucs_table_print_row()
 * calls.
 *
 * Asserts that the table has been rendered (widths alive).
 */
void ucs_table_print_separator(ucs_table_t *table);


/*
 * Tokenize strb by '\n' and emit each line via ucs_log_print_compact.
 * Does NOT clean up the buffer; the caller owns it.
 *
 * TODO: Remove after PR #11435 is merged.
 */
void ucs_log_print_compact_lines(ucs_string_buffer_t *strb);

END_C_DECLS

#endif
