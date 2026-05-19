/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2026. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCS_LOG_TABLE_H_
#define UCS_LOG_TABLE_H_

#include <ucs/datastruct/string_buffer.h>
#include <ucs/sys/compiler_def.h>


BEGIN_C_DECLS

/*
 * Build and emit ASCII tables to the UCS log using the compact-line
 * formatter. Helpers append onto a ucs_string_buffer_t so callers can
 * interleave their own domain-specific rows when needed.
 *
 * Column widths are expressed as int because the underlying %-*s expects
 * int. Width N produces a cell of "N characters of content + 1 space
 * padding on each side" (total cell width = N + 2, excluding the '|'
 * separators).
 */


/* Maximum supported column width for the dashes constant. Picking a value
 * comfortably above any realistic UCX column width avoids silent truncation
 * of the separator line. */
#define UCS_LOG_TABLE_DASH_MAX 256


/* Long string of '-' characters used to draw separators via "%.*s". */
extern const char ucs_log_table_dashes[UCS_LOG_TABLE_DASH_MAX + 1];


/* Leftmost corner character to draw when first_intact_cols > 0. */
typedef enum {
    UCS_LOG_TABLE_LCORNER_SEP, /* '+' separator-style (default) */
    UCS_LOG_TABLE_LCORNER_ROW  /* '|' row-style (carries cell 0 down from a
                                  row above) */
} ucs_log_table_lcorner_t;


/*
 * Horizontal separator across all columns.
 *
 *   first_intact_cols == 0:
 *     +-w0-+-w1-+ ... +-wn-+
 *
 *   first_intact_cols == K > 0 (leading K cells render as blank content
 *   with no dashes, e.g. the leftmost columns "continue" from the row
 *   above):
 *     <L> b0 + b1 + ... + b(K-1) +-wK-+-...-+-wn-+
 *
 *   where <L> is '+' when lcorner == UCS_LOG_TABLE_LCORNER_SEP and '|' when
 *   lcorner == UCS_LOG_TABLE_LCORNER_ROW. The corner between intact and
 *   dashed cells is always '+'. `lcorner` is ignored when
 *   first_intact_cols == 0 (leftmost corner is always '+').
 */
void ucs_log_table_append_separator(ucs_string_buffer_t *strb,
                                    const int *widths, unsigned n_cols,
                                    unsigned first_intact_cols,
                                    ucs_log_table_lcorner_t lcorner);


/*
 * Top-and-title bar spanning all columns merged into a single cell:
 *   +-W-+
 *   | title |
 * where W = sum(widths) + 3 * (n_cols - 1).
 */
void ucs_log_table_append_title(ucs_string_buffer_t *strb, const char *title,
                                const int *widths, unsigned n_cols);


/*
 * One data/header row "| c0 | c1 | ... | cn |". A NULL cell renders as "".
 */
void ucs_log_table_append_row(ucs_string_buffer_t *strb,
                              const char *const *cells, const int *widths,
                              unsigned n_cols);


/*
 * Tokenize strb by '\n' and emit each line via ucs_log_print_compact.
 * Does NOT cleanup the buffer; the caller owns it.
 */
void ucs_log_print_compact_lines(ucs_string_buffer_t *strb);

END_C_DECLS

#endif
