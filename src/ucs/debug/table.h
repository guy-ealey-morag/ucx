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
 *   - Cell: a value in one row. Has two independently-anchored "sides":
 *     a left-anchored part and a right-anchored part. Either or both may
 *     be empty.
 *
 *       - Left only:  "| content                 |"
 *       - Right only: "|                 content |"
 *       - Both:       "| left            content |"
 *
 *   - Separator: a horizontal "+---+---+" rule between rows. Frame
 *     separators at the top and bottom of the table are inserted
 *     automatically by render().
 *
 *   - Carry-over: when the row immediately following a separator has one
 *     or more empty leading cells, those positions render as blank
 *     "|     |" segments rather than dashes, and the leftmost corner is
 *     '|' instead of '+'. This is used to visually continue a column from
 *     the row above the separator into the row below.
 *
 * All cell content is owned by the table; ucs_table_cleanup() releases
 * everything.
 */


typedef struct ucs_table_row ucs_table_row_t;
typedef struct ucs_table_cell ucs_table_cell_t;


/*
 * Internal type. Each cell holds a left-anchored and a right-anchored
 * string buffer. Both buffers are managed by the table API; callers
 * must not access them directly.
 */
struct ucs_table_cell {
    unsigned            col_span;
    ucs_string_buffer_t left;
    ucs_string_buffer_t right;
};


/* Internal type. */
typedef enum {
    UCS_TABLE_ENTRY_ROW,
    UCS_TABLE_ENTRY_SEPARATOR
} ucs_table_entry_kind_t;


/* Internal type: one entry in the table is either a row (a vector of
 * cells) or a separator marker. */
typedef struct {
    ucs_table_entry_kind_t                  kind;
    ucs_array_s(unsigned, ucs_table_cell_t) cells;
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
    /* Internal scratch: per-body-column widths, allocated for the
     * duration of a ucs_table_render() call and NULL otherwise. */
    int                     *widths;
    /* When non-zero, ucs_table_render() normalizes every body-column
     * width to the maximum computed width, producing uniformly-wide
     * columns. Default: 0 (per-column widths). */
    int                     equal_widths;
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
 * Append a manual horizontal separator between rows. The renderer decides
 * the separator's exact appearance (carry-over leading cells, leftmost
 * corner) based on the row directly below the separator. Frame separators
 * at the very top and bottom of the table are inserted automatically by
 * render(); do not add them explicitly.
 */
void ucs_table_add_separator(ucs_table_t *table);


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
 * Convenience: add a cell whose content is left-anchored, built printf-
 * style. Equivalent to add_cell + cell_appendf_left. The 'f' suffix is
 * omitted to keep all row-level cell setters short; the format-string
 * argument signals printf-style. Returns the newly-added cell so the
 * caller can append additional content (e.g. a right-anchored side).
 */
ucs_table_cell_t *
ucs_table_row_add_cell_left(ucs_table_row_t *row, unsigned col_span,
                            const char *fmt, ...) UCS_F_PRINTF(3, 4);


/*
 * Convenience: add a cell whose content is right-anchored, built printf-
 * style. Equivalent to add_cell + cell_appendf_right. Returns the newly-
 * added cell so the caller can append additional content (e.g. a
 * left-anchored side).
 */
ucs_table_cell_t *
ucs_table_row_add_cell_right(ucs_table_row_t *row, unsigned col_span,
                             const char *fmt, ...) UCS_F_PRINTF(3, 4);


/*
 * Add an empty cell (both anchors blank) and return a handle that can
 * be passed to cell_appendf_left and cell_appendf_right. Used for
 * split cells that carry independently-anchored content on both sides
 * and for cells whose content is built incrementally.
 *
 * The cell handle is valid for the lifetime of the table.
 */
ucs_table_cell_t *
ucs_table_row_add_cell(ucs_table_row_t *row, unsigned col_span);


/*
 * Append printf-style content to the left-anchored side of a cell.
 * Multiple calls concatenate into a single left-anchored string.
 */
void ucs_table_cell_appendf_left(ucs_table_cell_t *cell, const char *fmt, ...)
        UCS_F_PRINTF(2, 3);


/*
 * Append printf-style content to the right-anchored side of a cell.
 * Multiple calls concatenate into a single right-anchored string.
 */
void ucs_table_cell_appendf_right(ucs_table_cell_t *cell, const char *fmt, ...)
        UCS_F_PRINTF(2, 3);


/*
 * Render the buffered table into strb. Frame separators are prepended
 * and appended automatically. Body column widths are derived from the
 * maximum cell content per column; merged cells expand the rightmost
 * body column they span if their content does not fit in the existing
 * widths. Separator corner and carry-over rendering are derived from
 * the empty leading cells of the row that follows each separator.
 */
void ucs_table_render(ucs_table_t *table, ucs_string_buffer_t *strb);


/*
 * Render the buffered table and write the result directly to stdout.
 * The caller still owns `table` and must call ucs_table_cleanup() when
 * done; this function only manages the temporary render buffer.
 */
void ucs_table_print(ucs_table_t *table);


/*
 * Tokenize strb by '\n' and emit each line via ucs_log_print_compact.
 * Does NOT clean up the buffer; the caller owns it.
 *
 * TODO: Remove after PR #11435 is merged.
 */
void ucs_log_print_compact_lines(ucs_string_buffer_t *strb);

END_C_DECLS

#endif
