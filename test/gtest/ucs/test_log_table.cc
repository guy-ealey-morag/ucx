/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2026. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include <common/test.h>
#include <vector>
extern "C" {
#include <ucs/datastruct/string_buffer.h>
#include <ucs/debug/log_table.h>
}


class test_log_table : public ucs::test {
protected:
    /* Build a fresh string buffer, run `body`, then return its contents and
     * clean up. Used directly by tests that compose multiple appends; the
     * per-helper wrappers below cover the common one-append case. */
    template<typename Body>
    std::string render(Body body) {
        ucs_string_buffer_t strb = UCS_STRING_BUFFER_INITIALIZER;
        body(&strb);
        const std::string out(ucs_string_buffer_cstr(&strb));
        ucs_string_buffer_cleanup(&strb);
        return out;
    }

    std::string separator(const std::vector<int> &widths, unsigned intact,
                    ucs_log_table_lcorner_t lcorner) {
        return render([&](ucs_string_buffer_t *strb) {
            ucs_log_table_append_separator(strb, widths.data(), widths.size(),
                                           intact, lcorner);
        });
    }

    std::string title(const std::vector<int> &widths, const char *title_str) {
        return render([&](ucs_string_buffer_t *strb) {
            ucs_log_table_append_title(strb, title_str, widths.data(),
                                       widths.size());
        });
    }

    std::string row(const std::vector<int> &widths,
                    const std::vector<const char*> &cells) {
        return render([&](ucs_string_buffer_t *strb) {
            ucs_log_table_append_row(strb, cells.data(), widths.data(),
                                     widths.size());
        });
    }
};


/* Group A: ucs_log_table_dashes constant */

UCS_TEST_F(test_log_table, dashes_constant) {
    EXPECT_EQ(std::string(UCS_LOG_TABLE_DASH_MAX, '-'),
              std::string(ucs_log_table_dashes));
}


/* Group B: ucs_log_table_append_separator */

UCS_TEST_F(test_log_table, separator_no_intact_two_cols) {
    EXPECT_EQ("+-----+-------+\n",
              separator({3, 5}, 0, UCS_LOG_TABLE_LCORNER_SEP));
}

UCS_TEST_F(test_log_table, separator_no_intact_four_cols) {
    EXPECT_EQ("+------+-----+-----------+--------+\n",
              separator({4, 3, 9, 6}, 0, UCS_LOG_TABLE_LCORNER_SEP));
}

UCS_TEST_F(test_log_table, separator_lcorner_ignored_when_intact_zero) {
    /* Per the contract, lcorner is ignored when first_intact_cols == 0. */
    EXPECT_EQ(separator({4, 3, 9, 6}, 0, UCS_LOG_TABLE_LCORNER_SEP),
              separator({4, 3, 9, 6}, 0, UCS_LOG_TABLE_LCORNER_ROW));
}

UCS_TEST_F(test_log_table, separator_intact_one_sep_corner) {
    EXPECT_EQ("+     +-------+\n",
              separator({3, 5}, 1, UCS_LOG_TABLE_LCORNER_SEP));
}

UCS_TEST_F(test_log_table, separator_intact_one_row_corner) {
    EXPECT_EQ("|     +-------+\n",
              separator({3, 5}, 1, UCS_LOG_TABLE_LCORNER_ROW));
}

UCS_TEST_F(test_log_table, separator_intact_all_cells_row_corner) {
    /* All cells intact, ROW left corner. Non-leftmost corners stay '+'
     * even when their cells are intact. */
    EXPECT_EQ("|    +      +\n",
              separator({2, 4}, 2, UCS_LOG_TABLE_LCORNER_ROW));
}

UCS_TEST_F(test_log_table, separator_single_column_intact_sep) {
    EXPECT_EQ("+     +\n", separator({3}, 1, UCS_LOG_TABLE_LCORNER_SEP));
}


/* Group C: ucs_log_table_append_title */

UCS_TEST_F(test_log_table, title_four_columns) {
    /* total = 4+3+9+6 + 3*(4-1) = 31 */
    const std::string expected = "+-" + std::string(31, '-') + "-+\n" +
                                 "| Hello" + std::string(31 - 5, ' ') +
                                 " |\n";
    EXPECT_EQ(expected, title({4, 3, 9, 6}, "Hello"));
}

UCS_TEST_F(test_log_table, title_single_column) {
    /* Exercises the n_cols == 1 path (the `if (n_cols > 0)` guard
     * ensures we don't underflow on `(n_cols - 1)`). */
    const std::string expected = "+-" + std::string(10, '-') + "-+\n" +
                                 "| Title" + std::string(10 - 5, ' ') +
                                 " |\n";
    EXPECT_EQ(expected, title({10}, "Title"));
}

UCS_TEST_F(test_log_table, title_empty_string) {
    const std::string expected = "+-------+\n| " + std::string(5, ' ') +
                                 " |\n";
    EXPECT_EQ(expected, title({5}, ""));
}


/* Group D: ucs_log_table_append_row */

UCS_TEST_F(test_log_table, row_basic_three_cols) {
    EXPECT_EQ("| a   | bcd   | xy |\n",
              row({3, 5, 2}, {"a", "bcd", "xy"}));
}

UCS_TEST_F(test_log_table, row_null_cell) {
    EXPECT_EQ("| a   |       | xy |\n",
              row({3, 5, 2}, {"a", NULL, "xy"}));
}

UCS_TEST_F(test_log_table, row_empty_string_equivalent_to_null) {
    /* The documented contract: NULL renders as "". */
    EXPECT_EQ(row({3, 5, 2}, {"a", NULL, "xy"}),
              row({3, 5, 2}, {"a", "", "xy"}));
}

UCS_TEST_F(test_log_table, row_cell_longer_than_width) {
    /* `%-*s` pads but never truncates; document that this is intentional. */
    EXPECT_EQ("| overflow |\n", row({2}, {"overflow"}));
}

/* Group E: full integration */

UCS_TEST_F(test_log_table, full_table_2x2) {
    /* Build a complete 2-column, 2-row table and compare the rendered
     * multi-line string against a literal expected value. This proves the
     * helpers compose into the documented table shape. */
    static const int widths[]        = {4, 6};
    static const char *const hdr[]   = {"C1", "Col2"};
    static const char *const row0[]  = {"a1", "b1"};
    static const char *const row1[]  = {"a2", "b2"};

    /* total_width = 4 + 6 + 3 * (2 - 1) = 13 → title bar has 13 dashes
     * between "+-" and "-+". Body separator: "+-" + 4 dashes + "-+-" +
     * 6 dashes + "-+" → "+------+--------+". */
    const std::string expected =
            "+-" + std::string(13, '-') + "-+\n" +
            "| Title" + std::string(13 - 5, ' ') + " |\n" +
            "+------+--------+\n" +
            "| C1   | Col2   |\n" +
            "+------+--------+\n" +
            "| a1   | b1     |\n" +
            "| a2   | b2     |\n" +
            "+------+--------+\n";

    EXPECT_EQ(expected, render([&](ucs_string_buffer_t *strb) {
                  ucs_log_table_append_title(strb, "Title", widths, 2);
                  ucs_log_table_append_separator(strb, widths, 2, 0,
                                                 UCS_LOG_TABLE_LCORNER_SEP);
                  ucs_log_table_append_row(strb, hdr, widths, 2);
                  ucs_log_table_append_separator(strb, widths, 2, 0,
                                                 UCS_LOG_TABLE_LCORNER_SEP);
                  ucs_log_table_append_row(strb, row0, widths, 2);
                  ucs_log_table_append_row(strb, row1, widths, 2);
                  ucs_log_table_append_separator(strb, widths, 2, 0,
                                                 UCS_LOG_TABLE_LCORNER_SEP);
              }));
}
