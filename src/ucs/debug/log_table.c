/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2026. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "log_table.h"

#include <ucs/debug/assert.h>
#include <ucs/debug/log_def.h>


/* 256 '-' characters (4 chunks of 64) + implicit NUL */
const char ucs_log_table_dashes[UCS_LOG_TABLE_DASH_MAX + 1] =
        "----------------------------------------------------------------"
        "----------------------------------------------------------------"
        "----------------------------------------------------------------"
        "----------------------------------------------------------------";


void ucs_log_table_append_separator(ucs_string_buffer_t *strb,
                                    const int *widths, unsigned n_cols,
                                    unsigned first_intact_cols,
                                    ucs_log_table_lcorner_t lcorner)
{
    char left_corner;
    unsigned i;

    for (i = 0; i < n_cols; ++i) {
        ucs_assertv((widths[i] >= 0) && (widths[i] <= UCS_LOG_TABLE_DASH_MAX),
                    "widths[%u]=%d out of range [0, %d]", i, widths[i],
                    UCS_LOG_TABLE_DASH_MAX);

        if ((i == 0) && (first_intact_cols > 0) &&
            (lcorner == UCS_LOG_TABLE_LCORNER_ROW)) {
            left_corner = '|';
        } else {
            left_corner = '+';
        }

        if (i < first_intact_cols) {
            ucs_string_buffer_appendf(strb, "%c %-*s ", left_corner, widths[i],
                                      "");
        } else {
            ucs_string_buffer_appendf(strb, "%c-%.*s-", left_corner, widths[i],
                                      ucs_log_table_dashes);
        }
    }

    ucs_string_buffer_appendf(strb, "+\n");
}

void ucs_log_table_append_title(ucs_string_buffer_t *strb, const char *title,
                                const int *widths, unsigned n_cols)
{
    int total = 0;
    unsigned i;

    for (i = 0; i < n_cols; ++i) {
        ucs_assertv((widths[i] >= 0) && (widths[i] <= UCS_LOG_TABLE_DASH_MAX),
                    "widths[%u]=%d out of range [0, %d]", i, widths[i],
                    UCS_LOG_TABLE_DASH_MAX);
        total += widths[i];
    }

    /* "3 * (n_cols - 1)" accounts for the " | " separators between cells
     * being merged into a single wide cell. */
    if (n_cols > 0) {
        total += 3 * (int)(n_cols - 1);
    }

    ucs_string_buffer_appendf(strb, "+-%.*s-+\n", total, ucs_log_table_dashes);
    ucs_string_buffer_appendf(strb, "| %-*s |\n", total, title);
}

void ucs_log_table_append_row(ucs_string_buffer_t *strb,
                              const char *const *cells, const int *widths,
                              unsigned n_cols)
{
    const char *cell;
    unsigned i;

    for (i = 0; i < n_cols; ++i) {
        cell = (cells[i] != NULL) ? cells[i] : "";
        ucs_string_buffer_appendf(strb, "| %-*s ", widths[i], cell);
    }

    ucs_string_buffer_appendf(strb, "|\n");
}

void ucs_log_print_compact_lines(ucs_string_buffer_t *strb)
{
    char *line;

    ucs_string_buffer_for_each_token(line, strb, "\n") {
        ucs_log_print_compact(line);
    }
}
