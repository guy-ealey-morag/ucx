/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2026. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include <common/test.h>
extern "C" {
#include <ucs/datastruct/string_buffer.h>
#include <ucs/debug/table.h>
}

#include <cstdio>
#include <fcntl.h>
#include <unistd.h>


class test_table : public ucs::test {
protected:
    class table_t {
    public:
        explicit table_t(unsigned n_body_cols)
        {
            ucs_table_config_t cfg = {};
            cfg.n_body_cols        = n_body_cols;
            ucs_table_init(&m_table, &cfg);
        }

        explicit table_t(const ucs_table_config_t &cfg)
        {
            ucs_table_init(&m_table, &cfg);
        }

        ~table_t()
        {
            ucs_table_cleanup(&m_table);
        }

        ucs_table_t *get()
        {
            return &m_table;
        }

        std::string render()
        {
            ucs_string_buffer_t strb = UCS_STRING_BUFFER_INITIALIZER;
            ucs_table_render(&m_table, &strb);
            const std::string out(ucs_string_buffer_cstr(&strb));
            ucs_string_buffer_cleanup(&strb);
            return out;
        }

    private:
        ucs_table_t m_table;
    };

    template<typename F> static std::string capture_stdout(F &&fn)
    {
        FILE *tmp = tmpfile();
        EXPECT_TRUE(tmp != NULL);
        if (tmp == NULL) {
            return std::string();
        }

        fflush(stdout);
        const int saved_fd = dup(fileno(stdout));
        EXPECT_GE(saved_fd, 0);
        EXPECT_GE(dup2(fileno(tmp), fileno(stdout)), 0);

        fn();

        fflush(stdout);
        EXPECT_GE(dup2(saved_fd, fileno(stdout)), 0);
        close(saved_fd);

        rewind(tmp);
        std::string out;
        char buf[256];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), tmp)) > 0) {
            out.append(buf, n);
        }
        fclose(tmp);
        return out;
    }
};


UCS_TEST_F(test_table, empty_table) {
    table_t table(2);
    EXPECT_EQ("+--+--+\n+--+--+\n", table.render());
}

UCS_TEST_F(test_table, single_cell_left) {
    table_t table(1);
    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "abc");

    EXPECT_EQ("+-----+\n"
              "| abc |\n"
              "+-----+\n",
              table.render());
}

UCS_TEST_F(test_table, single_cell_right) {
    table_t table(1);
    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_RIGHT, "%s",
                               "right-anchored");

    EXPECT_EQ("+----------------+\n"
              "| right-anchored |\n"
              "+----------------+\n",
              table.render());
}

UCS_TEST_F(test_table, per_column_max_width) {
    table_t table(2);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "short");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s",
                               "even longer cell");

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s",
                               "a much wider value");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "x");

    EXPECT_EQ("+--------------------+------------------+\n"
              "| short              | even longer cell |\n"
              "| a much wider value | x                |\n"
              "+--------------------+------------------+\n",
              table.render());
}

UCS_TEST_F(test_table, right_align_with_wider_neighbor) {
    table_t table(1);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s",
                               "long left value");

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_RIGHT, "%s", "R");

    EXPECT_EQ("+-----------------+\n"
              "| long left value |\n"
              "|               R |\n"
              "+-----------------+\n",
              table.render());
}

UCS_TEST_F(test_table, separator_plain) {
    table_t table(2);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "a");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "b");
    ucs_table_add_separator(table.get());

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "c");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "d");

    EXPECT_EQ("+---+---+\n"
              "| a | b |\n"
              "+---+---+\n"
              "| c | d |\n"
              "+---+---+\n",
              table.render());
}

UCS_TEST_F(test_table, separator_merged_1_of_2) {
    const int min_widths[2] = {3, 3};
    ucs_table_config_t cfg  = {};
    cfg.n_body_cols         = 2;
    cfg.min_widths          = min_widths;
    table_t table(cfg);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "x");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "y");
    ucs_table_add_separator_with_merged_cols(table.get(), 1);

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell(row, 1, UCS_TABLE_ALIGN_LEFT);
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "z");

    EXPECT_EQ("+-----+-----+\n"
              "| x   | y   |\n"
              "|     +-----+\n"
              "|     | z   |\n"
              "+-----+-----+\n",
              table.render());
}

UCS_TEST_F(test_table, separator_merged_1_of_3) {
    const int min_widths[3] = {3, 3, 3};
    ucs_table_config_t cfg  = {};
    cfg.n_body_cols         = 3;
    cfg.min_widths          = min_widths;
    table_t table(cfg);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "a");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "b");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "c");
    ucs_table_add_separator_with_merged_cols(table.get(), 1);

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell(row, 1, UCS_TABLE_ALIGN_LEFT);
    ucs_table_row_add_cell(row, 1, UCS_TABLE_ALIGN_LEFT);
    ucs_table_row_add_cell(row, 1, UCS_TABLE_ALIGN_LEFT);

    EXPECT_EQ("+-----+-----+-----+\n"
              "| a   | b   | c   |\n"
              "|     +-----+-----+\n"
              "|     |     |     |\n"
              "+-----+-----+-----+\n",
              table.render());
}

UCS_TEST_F(test_table, separator_merged_2_of_3) {
    const int min_widths[3] = {3, 3, 3};
    ucs_table_config_t cfg  = {};
    cfg.n_body_cols         = 3;
    cfg.min_widths          = min_widths;
    table_t table(cfg);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 2, UCS_TABLE_ALIGN_LEFT, "%s", "ab");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "x");
    ucs_table_add_separator_with_merged_cols(table.get(), 2);

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell(row, 2, UCS_TABLE_ALIGN_LEFT);
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "y");

    EXPECT_EQ("+-----+-----+-----+\n"
              "| ab        | x   |\n"
              "|     |     +-----+\n"
              "|           | y   |\n"
              "+-----+-----+-----+\n",
              table.render());
}

UCS_TEST_F(test_table, separator_merged_captured_at_add_time) {
    /* Each separator captures its own merged_cols at add time; row content
     * added later must not retroactively change how the separator renders. */
    const int min_widths[2] = {3, 3};
    ucs_table_config_t cfg  = {};
    cfg.n_body_cols         = 2;
    cfg.min_widths          = min_widths;
    table_t table(cfg);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "a");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "b");

    /* merged_cols=1 even though the next row has a non-empty leading cell. */
    ucs_table_add_separator_with_merged_cols(table.get(), 1);

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "c");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "d");

    /* Plain even though the next row has an empty leading cell. */
    ucs_table_add_separator(table.get());

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell(row, 1, UCS_TABLE_ALIGN_LEFT);
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "e");

    EXPECT_EQ("+-----+-----+\n"
              "| a   | b   |\n"
              "|     +-----+\n"
              "| c   | d   |\n"
              "+-----+-----+\n"
              "|     | e   |\n"
              "+-----+-----+\n",
              table.render());
}

UCS_TEST_F(test_table, trailing_separator_suppresses_bottom_frame) {
    table_t table(2);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "x");
    ucs_table_row_add_cell(row, 1, UCS_TABLE_ALIGN_LEFT);
    ucs_table_add_separator(table.get());

    EXPECT_EQ("+---+--+\n"
              "| x |  |\n"
              "+---+--+\n",
              table.render());
}

UCS_TEST_F(test_table, trailing_merged_separator_suppresses_bottom_frame) {
    /* Bottom-frame suppression keys off kind only, not merged_cols. */
    table_t table(2);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "x");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "y");
    ucs_table_add_separator_with_merged_cols(table.get(), 1);

    EXPECT_EQ("+---+---+\n"
              "| x | y |\n"
              "|   +---+\n",
              table.render());
}

UCS_TEST_F(test_table, consecutive_separators) {
    const int min_widths[1] = {3};
    ucs_table_config_t cfg  = {};
    cfg.n_body_cols         = 1;
    cfg.min_widths          = min_widths;
    table_t table(cfg);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "a");
    ucs_table_add_separator(table.get());
    ucs_table_add_separator(table.get());
    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "b");

    EXPECT_EQ("+-----+\n"
              "| a   |\n"
              "+-----+\n"
              "+-----+\n"
              "| b   |\n"
              "+-----+\n",
              table.render());
}

UCS_TEST_F(test_table, col_span_fits_in_base_widths) {
    table_t table(2);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 2, UCS_TABLE_ALIGN_LEFT, "%s", "hdr");

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "abcdef");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "ghijklm");

    EXPECT_EQ("+--------+---------+\n"
              "| hdr              |\n"
              "| abcdef | ghijklm |\n"
              "+--------+---------+\n",
              table.render());
}

UCS_TEST_F(test_table, col_span_grows_rightmost) {
    /* base widths {2, 2}; merged pixel width 2 + 2 + 3 = 7; content 23
     * adds 16 to widths[1]. */
    table_t table(2);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 2, UCS_TABLE_ALIGN_LEFT, "%s",
                               "this header is too wide");

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "ab");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "cd");

    EXPECT_EQ("+----+--------------------+\n"
              "| this header is too wide |\n"
              "| ab | cd                 |\n"
              "+----+--------------------+\n",
              table.render());
}

UCS_TEST_F(test_table, printf_format_all_alignments) {
    table_t table(2);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_CENTER, "%d %s..%s", 42,
                               "lo", "hi");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s=%u", "k", 7u);

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s",
                               "padding row.");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_RIGHT, "%s", "x");

    EXPECT_EQ("+--------------+-----+\n"
              "|  42 lo..hi   | k=7 |\n"
              "| padding row. |   x |\n"
              "+--------------+-----+\n",
              table.render());
}

UCS_TEST_F(test_table, integration_tl_info) {
    /* Mirrors ucp_tl_info: Type | Component | Transport | Device, using
     * plain, merged_cols=1, and merged_cols=2 separators. */
    table_t table(4);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "Type");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "Component");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "Transport");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "Device");
    ucs_table_add_separator(table.get());

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "network");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "tcp");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "+ tcp");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "dev_a");

    /* New component within network: Type carries over. */
    ucs_table_add_separator_with_merged_cols(table.get(), 1);

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell(row, 1, UCS_TABLE_ALIGN_LEFT);
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "ib");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s",
                               "- rc_verbs");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "dev_b");

    /* New TL within ib: Type and Component carry over. */
    ucs_table_add_separator_with_merged_cols(table.get(), 2);

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell(row, 1, UCS_TABLE_ALIGN_LEFT);
    ucs_table_row_add_cell(row, 1, UCS_TABLE_ALIGN_LEFT);
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s",
                               "- ud_verbs");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "dev_c");

    /* New dev_type. */
    ucs_table_add_separator(table.get());

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s",
                               "intra-node");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "sysv");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "- sysv");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "dev_d");

    EXPECT_EQ("+------------+-----------+------------+--------+\n"
              "| Type       | Component | Transport  | Device |\n"
              "+------------+-----------+------------+--------+\n"
              "| network    | tcp       | + tcp      | dev_a  |\n"
              "|            +-----------+------------+--------+\n"
              "|            | ib        | - rc_verbs | dev_b  |\n"
              "|            |           +------------+--------+\n"
              "|            |           | - ud_verbs | dev_c  |\n"
              "+------------+-----------+------------+--------+\n"
              "| intra-node | sysv      | - sysv     | dev_d  |\n"
              "+------------+-----------+------------+--------+\n",
              table.render());
}

UCS_TEST_F(test_table, row_prefix_default_null) {
    table_t table(1);
    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "abc");

    EXPECT_EQ("+-----+\n| abc |\n+-----+\n", table.render());
}

UCS_TEST_F(test_table, row_prefix_empty_string) {
    /* empty prefix == NULL prefix */
    ucs_table_config_t cfg = {};
    cfg.n_body_cols        = 1;
    cfg.row_prefix         = "";
    table_t table(cfg);
    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "abc");

    EXPECT_EQ("+-----+\n| abc |\n+-----+\n", table.render());
}

UCS_TEST_F(test_table, row_prefix_applies_to_all_lines) {
    /* Prefix is prepended to frames, body rows, and inner separators. */
    ucs_table_config_t cfg = {};
    cfg.n_body_cols        = 1;
    cfg.row_prefix         = "# ";
    table_t table(cfg);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "a");
    ucs_table_add_separator(table.get());
    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "b");

    EXPECT_EQ("# +---+\n"
              "# | a |\n"
              "# +---+\n"
              "# | b |\n"
              "# +---+\n",
              table.render());
}

UCS_TEST_F(test_table, equal_widths_default_disabled) {
    table_t table(3);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "a");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "longer");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "xy");

    EXPECT_EQ("+---+--------+----+\n"
              "| a | longer | xy |\n"
              "+---+--------+----+\n",
              table.render());
}

UCS_TEST_F(test_table, equal_widths_normalizes_to_max) {
    ucs_table_config_t cfg = {};
    cfg.n_body_cols        = 3;
    cfg.equal_widths       = 1;
    table_t table(cfg);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "a");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "longer");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "xy");

    EXPECT_EQ("+--------+--------+--------+\n"
              "| a      | longer | xy     |\n"
              "+--------+--------+--------+\n",
              table.render());
}

UCS_TEST_F(test_table, equal_widths_after_col_span_expansion) {
    /* equal_widths runs after the col_span deficit pass, so the widened
     * rightmost column propagates to its neighbors. */
    ucs_table_config_t cfg = {};
    cfg.n_body_cols        = 2;
    cfg.equal_widths       = 1;
    table_t table(cfg);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 2, UCS_TABLE_ALIGN_LEFT, "%s",
                               "this header is too wide");

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "ab");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "cd");

    EXPECT_EQ("+--------------------+--------------------+\n"
              "| this header is too wide                 |\n"
              "| ab                 | cd                 |\n"
              "+--------------------+--------------------+\n",
              table.render());
}

UCS_TEST_F(test_table, min_widths_widens) {
    const int min_widths[2] = {10, 10};
    ucs_table_config_t cfg  = {};
    cfg.n_body_cols         = 2;
    cfg.min_widths          = min_widths;
    table_t table(cfg);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "a");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "b");

    EXPECT_EQ("+------------+------------+\n"
              "| a          | b          |\n"
              "+------------+------------+\n",
              table.render());
}

UCS_TEST_F(test_table, min_widths_does_not_shrink) {
    /* min_widths is a floor, not a ceiling. */
    const int min_widths[2] = {3, 3};
    ucs_table_config_t cfg  = {};
    cfg.n_body_cols         = 2;
    cfg.min_widths          = min_widths;
    table_t table(cfg);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s",
                               "abcdefghij");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "xy");

    EXPECT_EQ("+------------+-----+\n"
              "| abcdefghij | xy  |\n"
              "+------------+-----+\n",
              table.render());
}

UCS_TEST_F(test_table, stream_print_matches_inline) {
    /* ucs_table_print emits its own bottom frame after the header, which
     * doubles as the divider before the streamed rows. */
    const int min_widths[2] = {4, 4};
    ucs_table_config_t cfg  = {};
    cfg.n_body_cols         = 2;
    cfg.min_widths          = min_widths;
    table_t table(cfg);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "h1");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "h2");

    const std::string out = capture_stdout([&]() {
        ucs_table_print(table.get());

        auto *stream = ucs_table_stream_row_create(table.get());
        ucs_table_row_add_cell_fmt(stream, 1, UCS_TABLE_ALIGN_LEFT, "%s", "a1");
        ucs_table_row_add_cell_fmt(stream, 1, UCS_TABLE_ALIGN_LEFT, "%s", "b1");
        ucs_table_print_row(stream);

        ucs_table_stream_row_reset(stream);
        ucs_table_row_add_cell_fmt(stream, 1, UCS_TABLE_ALIGN_LEFT, "%s", "a2");
        ucs_table_row_add_cell_fmt(stream, 1, UCS_TABLE_ALIGN_LEFT, "%s", "b2");
        ucs_table_print_row(stream);

        ucs_table_print_separator(table.get());
        ucs_table_stream_row_destroy(stream);
    });

    EXPECT_EQ("+------+------+\n"
              "| h1   | h2   |\n"
              "+------+------+\n"
              "| a1   | b1   |\n"
              "| a2   | b2   |\n"
              "+------+------+\n",
              out);
}

UCS_TEST_F(test_table, stream_row_reset) {
    const int min_widths[2] = {5, 5};
    ucs_table_config_t cfg  = {};
    cfg.n_body_cols         = 2;
    cfg.min_widths          = min_widths;
    table_t table(cfg);

    auto *header = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(header, 1, UCS_TABLE_ALIGN_LEFT, "%s", "h1");
    ucs_table_row_add_cell_fmt(header, 1, UCS_TABLE_ALIGN_LEFT, "%s", "h2");

    const std::string out = capture_stdout([&]() {
        ucs_table_print(table.get());

        auto *stream = ucs_table_stream_row_create(table.get());
        ucs_table_row_add_cell_fmt(stream, 1, UCS_TABLE_ALIGN_LEFT, "%s", "x");
        ucs_table_row_add_cell_fmt(stream, 1, UCS_TABLE_ALIGN_LEFT, "%s", "y");
        ucs_table_print_row(stream);

        ucs_table_stream_row_reset(stream);
        ucs_table_row_add_cell_fmt(stream, 1, UCS_TABLE_ALIGN_LEFT, "%s",
                                   "xxxxx");
        ucs_table_row_add_cell_fmt(stream, 1, UCS_TABLE_ALIGN_LEFT, "%s", "y");
        ucs_table_print_row(stream);

        ucs_table_stream_row_destroy(stream);
    });

    EXPECT_EQ("+-------+-------+\n"
              "| h1    | h2    |\n"
              "+-------+-------+\n"
              "| x     | y     |\n"
              "| xxxxx | y     |\n",
              out);
}

UCS_TEST_F(test_table, stream_render_row_no_newline) {
    /* render_row omits the trailing newline so callers can splice extra
     * content before the line break. */
    const int min_widths[2] = {3, 3};
    ucs_table_config_t cfg  = {};
    cfg.n_body_cols         = 2;
    cfg.min_widths          = min_widths;
    table_t table(cfg);

    auto *header = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(header, 1, UCS_TABLE_ALIGN_LEFT, "%s", "h");
    ucs_table_row_add_cell_fmt(header, 1, UCS_TABLE_ALIGN_LEFT, "%s", "i");
    ucs_table_print(table.get());

    auto *stream = ucs_table_stream_row_create(table.get());
    ucs_table_row_add_cell_fmt(stream, 1, UCS_TABLE_ALIGN_LEFT, "%s", "v1");
    ucs_table_row_add_cell_fmt(stream, 1, UCS_TABLE_ALIGN_LEFT, "%s", "v2");

    ucs_string_buffer_t strb = UCS_STRING_BUFFER_INITIALIZER;
    ucs_table_render_row(stream, &strb);
    EXPECT_EQ(std::string("| v1  | v2  |"), ucs_string_buffer_cstr(&strb));

    ucs_string_buffer_appendf(&strb, "  extra\n");
    EXPECT_EQ(std::string("| v1  | v2  |  extra\n"),
              ucs_string_buffer_cstr(&strb));
    ucs_string_buffer_cleanup(&strb);

    ucs_table_stream_row_destroy(stream);
}

UCS_TEST_F(test_table, stream_row_all_alignments) {
    const int min_widths[2] = {4, 4};
    ucs_table_config_t cfg  = {};
    cfg.n_body_cols         = 2;
    cfg.min_widths          = min_widths;
    table_t table(cfg);

    auto *header = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(header, 1, UCS_TABLE_ALIGN_LEFT, "%s", "L");
    ucs_table_row_add_cell_fmt(header, 1, UCS_TABLE_ALIGN_RIGHT, "%s", "R");
    ucs_table_print(table.get());

    auto *stream = ucs_table_stream_row_create(table.get());
    ucs_table_row_add_cell_fmt(stream, 1, UCS_TABLE_ALIGN_CENTER, "%s", "a");
    ucs_table_row_add_cell_fmt(stream, 1, UCS_TABLE_ALIGN_RIGHT, "%s", "z");

    ucs_string_buffer_t strb = UCS_STRING_BUFFER_INITIALIZER;
    ucs_table_render_row(stream, &strb);
    EXPECT_EQ(std::string("|  a   |    z |"), ucs_string_buffer_cstr(&strb));
    ucs_string_buffer_cleanup(&strb);

    ucs_table_stream_row_destroy(stream);
}

UCS_TEST_F(test_table, stream_row_printf_widths_flush) {
    /* Matching %* widths in printf and min_widths produce flush-aligned
     * cells. */
    const int min_widths[3] = {5, 7, 4};
    ucs_table_config_t cfg  = {};
    cfg.n_body_cols         = 3;
    cfg.min_widths          = min_widths;
    table_t table(cfg);

    auto *header = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(header, 1, UCS_TABLE_ALIGN_LEFT, "%s", "A");
    ucs_table_row_add_cell_fmt(header, 1, UCS_TABLE_ALIGN_LEFT, "%s", "B");
    ucs_table_row_add_cell_fmt(header, 1, UCS_TABLE_ALIGN_LEFT, "%s", "C");

    const std::string out = capture_stdout([&]() {
        ucs_table_print(table.get());

        auto *stream = ucs_table_stream_row_create(table.get());
        ucs_table_row_add_cell_fmt(stream, 1, UCS_TABLE_ALIGN_RIGHT, "%*.0f",
                                   min_widths[0], 12.0);
        ucs_table_row_add_cell_fmt(stream, 1, UCS_TABLE_ALIGN_RIGHT, "%*.2f",
                                   min_widths[1], 1.5);
        ucs_table_row_add_cell_fmt(stream, 1, UCS_TABLE_ALIGN_RIGHT, "%*d",
                                   min_widths[2], 7);
        ucs_table_print_row(stream);
        ucs_table_stream_row_destroy(stream);
    });

    EXPECT_EQ("+-------+---------+------+\n"
              "| A     | B       | C    |\n"
              "+-------+---------+------+\n"
              "|    12 |    1.50 |    7 |\n",
              out);
}

UCS_TEST_F(test_table, center_pads_evenly) {
    table_t table(1);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_CENTER, "%s", "abc");

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s",
                               "wider value");

    EXPECT_EQ("+-------------+\n"
              "|     abc     |\n"
              "| wider value |\n"
              "+-------------+\n",
              table.render());
}

UCS_TEST_F(test_table, center_with_col_span) {
    table_t table(2);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 2, UCS_TABLE_ALIGN_CENTER, "%s", "hi");

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "abcd");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "efgh");

    EXPECT_EQ("+------+------+\n"
              "|     hi      |\n"
              "| abcd | efgh |\n"
              "+------+------+\n",
              table.render());
}

UCS_TEST_F(test_table, center_odd_padding_biases_right) {
    table_t table(1);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_CENTER, "%s", "abc");

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "12345678");

    EXPECT_EQ("+----------+\n"
              "|   abc    |\n"
              "| 12345678 |\n"
              "+----------+\n",
              table.render());
}

UCS_TEST_F(test_table, center_with_min_widths) {
    const int min_widths[1] = {7};
    ucs_table_config_t cfg  = {};
    cfg.n_body_cols         = 1;
    cfg.min_widths          = min_widths;
    table_t table(cfg);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_CENTER, "%s", "ab");

    EXPECT_EQ("+---------+\n"
              "|   ab    |\n"
              "+---------+\n",
              table.render());
}

UCS_TEST_F(test_table, render_twice) {
    /* Widths are recomputed on every render: a wider row added between
     * renders must widen the columns on the second render. */
    table_t table(2);
    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "a");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "b");

    EXPECT_EQ("+---+---+\n"
              "| a | b |\n"
              "+---+---+\n",
              table.render());

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s",
                               "long-cell-1");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s",
                               "long-cell-2");

    EXPECT_EQ("+-------------+-------------+\n"
              "| a           | b           |\n"
              "| long-cell-1 | long-cell-2 |\n"
              "+-------------+-------------+\n",
              table.render());
}
