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
    /* RAII wrapper around the buffered builder: init in ctor, cleanup in
     * dtor so each test fixture is concise and exception-safe. */
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

    /* Capture stdout while running the given callable. Used to test the
     * streaming print_row / print_separator functions, which write
     * directly to stdout. */
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


/* Group A: structural rendering */

UCS_TEST_F(test_table, empty_table_renders_top_and_bottom_frames) {
    /* Zero rows: render still emits the top and bottom frame separators.
     * With n_body_cols=2 and zero recorded content, both columns are 0
     * pixels wide and the separator collapses to "+--" per column. */
    table_t table(2);
    EXPECT_EQ("+--+--+\n+--+--+\n", table.render());
}

UCS_TEST_F(test_table, single_cell_row_left_aligned) {
    table_t table(1);
    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "abc");

    EXPECT_EQ("+-----+\n"
              "| abc |\n"
              "+-----+\n",
              table.render());
}

UCS_TEST_F(test_table, multi_row_uses_per_column_max_width) {
    /* The builder picks the maximum content per body column so that
     * shorter cells in other rows are padded out to that width. */
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


/* Group B: alignment */

UCS_TEST_F(test_table, right_aligned_cell) {
    table_t table(1);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_RIGHT, "%s",
                               "right-anchored");

    EXPECT_EQ("+----------------+\n"
              "| right-anchored |\n"
              "+----------------+\n",
              table.render());
}

UCS_TEST_F(test_table, right_anchor_pads_against_wider_left_row) {
    /* The body column width comes from the longest row; the right-
     * anchored cell pads on the left to reach that width. */
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


/* Group D: separator carry-over via explicit merged_cols */

UCS_TEST_F(test_table,
           separator_with_merged_cols_renders_blank_leading_segment) {
    /* Row above has both cells set. Separator with merged_cols=1
     * follows. Row below leaves the leading cell empty so the visual
     * column-merge effect lines up. Expected: leftmost separator
     * segment is a blank "|     " (carry-over), the rest is a dashed
     * "+-----" segment. */
    table_t table(2);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "type");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "data1");
    ucs_table_add_separator_with_merged_cols(table.get(), 1);

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell(row, 1, UCS_TABLE_ALIGN_LEFT);
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "data2");

    EXPECT_EQ("+------+-------+\n"
              "| type | data1 |\n"
              "|      +-------+\n"
              "|      | data2 |\n"
              "+------+-------+\n",
              table.render());
}

UCS_TEST_F(test_table, separator_without_merged_cols_uses_plus_corner) {
    /* Plain separator (no merged_cols) renders fully dashed
     * "+-----+" regardless of whether the row below has empty
     * leading cells. */
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

UCS_TEST_F(test_table, separator_at_table_edges_is_always_plain_frame) {
    /* A plain separator immediately after the last row has no row
     * below; the bottom-frame line is byte-identical to it, so
     * render() suppresses the auto bottom frame and the table ends
     * with a single "+---+--+" line rather than two duplicated
     * frames. */
    table_t table(2);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "x");
    ucs_table_row_add_cell(row, 1, UCS_TABLE_ALIGN_LEFT); /* empty */
    ucs_table_add_separator(table.get());

    EXPECT_EQ("+---+--+\n"
              "| x |  |\n"
              "+---+--+\n",
              table.render());
}

UCS_TEST_F(test_table, separator_with_merged_cols_one_blanks_first_column) {
    /* Positive test: merged_cols=1 makes the leftmost separator
     * segment render as a blank "|     " carry-over and shifts the
     * leftmost corner from '+' to '|'. The remaining segments stay
     * dashed. */
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

UCS_TEST_F(test_table,
           separator_with_zero_merged_cols_renders_plain_when_row_below_blank) {
    /* Regression: an empty leading cell on the row below must NOT
     * influence the separator. Carry-over is decided purely by the
     * separator's merged_cols value. */
    const int min_widths[2] = {3, 3};
    ucs_table_config_t cfg  = {};
    cfg.n_body_cols         = 2;
    cfg.min_widths          = min_widths;
    table_t table(cfg);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "x");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "y");
    ucs_table_add_separator(table.get());

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell(row, 1, UCS_TABLE_ALIGN_LEFT); /* empty */
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "z");

    EXPECT_EQ("+-----+-----+\n"
              "| x   | y   |\n"
              "+-----+-----+\n"
              "|     | z   |\n"
              "+-----+-----+\n",
              table.render());
}

UCS_TEST_F(test_table, separator_with_merged_cols_only_blanks_leading_columns) {
    /* merged_cols=1 in a 3-column table only blanks the leading
     * column; the trailing two columns render as dashed segments
     * regardless of what content (or lack thereof) the row below
     * has in those columns. */
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

UCS_TEST_F(test_table,
           separator_with_merged_cols_two_blanks_two_leading_columns) {
    /* merged_cols=2 in a 3-column table blanks the leading two body
     * columns. The leftmost corner is '|', the corner between the
     * two merged segments is also '|', and the trailing dashed
     * segment closes with a regular '+'. Mirrors the canonical use
     * case of a leading "category" cell that spans two body
     * columns. */
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

UCS_TEST_F(test_table,
           separator_merged_cols_captured_at_add_time_not_inferred_from_row) {
    /* Regression: each separator captures its own merged_cols when
     * it is added; later changes to the row that follows must NOT
     * retroactively change how that separator renders. Two
     * separators with different merged_cols values appear in the
     * same table and render independently. */
    const int min_widths[2] = {3, 3};
    ucs_table_config_t cfg  = {};
    cfg.n_body_cols         = 2;
    cfg.min_widths          = min_widths;
    table_t table(cfg);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "a");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "b");

    /* First separator: merged_cols=1 even though the row below has
     * non-empty leading cells. */
    ucs_table_add_separator_with_merged_cols(table.get(), 1);

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "c");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "d");

    /* Second separator: plain even though the row below has an
     * empty leading cell. */
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


/* Group E: col_span */

UCS_TEST_F(test_table, col_span_merged_cell_short_content_inherits_widths) {
    /* The merged cell content fits within the sum of base column widths
     * (computed from the body row's per-column cells), so no expansion is
     * needed. */
    table_t table(2);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 2, UCS_TABLE_ALIGN_LEFT, "%s", "hdr");

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "abcdef");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "ghijklm");

    /* Body cols are 6 and 7 wide; the merged cell sits over both,
     * total pixel width 6 + 7 + 3 = 16. */
    EXPECT_EQ("+--------+---------+\n"
              "| hdr              |\n"
              "| abcdef | ghijklm |\n"
              "+--------+---------+\n",
              table.render());
}

UCS_TEST_F(test_table, col_span_grows_rightmost_body_column_when_needed) {
    /* The merged cell's content is wider than the sum of its base body
     * column widths plus the merge join padding: the rightmost spanned
     * body column expands to absorb the excess. */
    table_t table(2);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 2, UCS_TABLE_ALIGN_LEFT, "%s",
                               "this header is too wide");

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "ab");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "cd");

    /* base widths {2, 2}; merged pixel width = 2 + 2 + 3 = 7; content =
     * 23. deficit 23 - 7 = 16 added to widths[1]. New widths {2, 18}. */
    EXPECT_EQ("+----+--------------------+\n"
              "| this header is too wide |\n"
              "| ab | cd                 |\n"
              "+----+--------------------+\n",
              table.render());
}


/* Group G: printf formatting goes through unchanged */

UCS_TEST_F(test_table, printf_formatting_supported_by_all_alignments) {
    /* Exercises printf formatting through every alignment branch
     * (LEFT, RIGHT, CENTER). A wider row pins body col 0 to 12,
     * which leaves visible padding around the centered cell. */
    table_t table(2);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_CENTER, "%d %s..%s", 42,
                               "lo", "hi");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s=%u", "k", 7u);

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s",
                               "padding row.");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_RIGHT, "%s", "x");

    /* Body col 0 width = 12 (driven by 'padding row.'). The CENTER
     * cell '42 lo..hi' (9 chars) gets 1 space on the left and 2 on
     * the right inside the 12-wide cell. */
    EXPECT_EQ("+--------------+-----+\n"
              "|  42 lo..hi   | k=7 |\n"
              "| padding row. |   x |\n"
              "+--------------+-----+\n",
              table.render());
}


/* Group H: integration - mirrors the proto_debug layout */

UCS_TEST_F(test_table, integration_tl_info_like_layout) {
    /* Mirrors the ucp_tl_info table shape: 4 body columns
     * (Type | Component | Transport | Device) with three flavors of
     * inter-row separator:
     *  - plain (merged_cols=0) between dev_types,
     *  - merged_cols=1 between components within the same dev_type
     *    (Type column carries over),
     *  - merged_cols=2 between TLs within the same component
     *    (Type and Component columns carry over).
     * Columns below the carry-over are left empty so the "merged"
     * cell visually continues across the divider. */
    table_t table(4);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "Type");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "Component");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "Transport");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "Device");
    ucs_table_add_separator(table.get());

    /* network / tcp / + tcp */
    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "network");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "tcp");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "+ tcp");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "dev_a");

    /* New component within network: type carries over (1 col). */
    ucs_table_add_separator_with_merged_cols(table.get(), 1);

    /* network / ib / - rc_verbs */
    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell(row, 1, UCS_TABLE_ALIGN_LEFT);
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "ib");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s",
                               "- rc_verbs");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "dev_b");

    /* New TL within ib: type AND component carry over (2 cols). */
    ucs_table_add_separator_with_merged_cols(table.get(), 2);

    /* network / ib / - ud_verbs */
    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell(row, 1, UCS_TABLE_ALIGN_LEFT);
    ucs_table_row_add_cell(row, 1, UCS_TABLE_ALIGN_LEFT);
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s",
                               "- ud_verbs");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "dev_c");

    /* New dev_type: plain dashed separator. */
    ucs_table_add_separator(table.get());

    /* intra-node / sysv / - sysv */
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


/* Group I: row_prefix */

UCS_TEST_F(test_table, row_prefix_default_is_null_no_prefix) {
    /* Default value of row_prefix is NULL; rendering matches a table
     * that never touched the prefix. */
    table_t table(1);
    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "abc");

    EXPECT_EQ("+-----+\n"
              "| abc |\n"
              "+-----+\n",
              table.render());
}

UCS_TEST_F(test_table, row_prefix_prepended_to_every_line) {
    /* "# " is prepended to body rows AND to the top/bottom frame
     * separators, so the entire table reads as a comment block. */
    ucs_table_config_t cfg = {};
    cfg.n_body_cols        = 1;
    cfg.row_prefix         = "# ";
    table_t table(cfg);
    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "abc");

    EXPECT_EQ("# +-----+\n"
              "# | abc |\n"
              "# +-----+\n",
              table.render());
}

UCS_TEST_F(test_table, row_prefix_prepended_to_inner_separator) {
    /* Verify an explicit add_separator line between rows also receives
     * the prefix. */
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

UCS_TEST_F(test_table, row_prefix_empty_string_renders_unchanged) {
    /* Empty-string prefix is a no-op but must not crash and must
     * produce output byte-identical to the no-prefix case. */
    ucs_table_config_t cfg = {};
    cfg.n_body_cols        = 1;
    cfg.row_prefix         = "";
    table_t table(cfg);
    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "abc");

    EXPECT_EQ("+-----+\n"
              "| abc |\n"
              "+-----+\n",
              table.render());
}

/* Group J: equal_widths */

UCS_TEST_F(test_table, equal_widths_default_disabled_uses_per_column_widths) {
    /* With equal_widths left at its default of 0, each body column
     * keeps its own per-column width. Regression guard against
     * accidental normalization. */
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

UCS_TEST_F(test_table, equal_widths_normalizes_all_columns_to_max) {
    /* Per-column widths from the row are {1, 6, 2}. With equal_widths
     * enabled, render() picks max=6 and widens every column to 6 so
     * the short cells pad out to match the widest one. */
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

UCS_TEST_F(test_table, equal_widths_with_col_span_propagates_post_expansion) {
    /* Pass 1 picks widths {2, 2} from the second row. Pass 2 expands
     * widths[1] to absorb the merged 23-char header => widths become
     * {2, 18}. Equal-widths then normalizes both to 18, so the second
     * row's two cells render at the wider column width. */
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


/* Group K: set_min_col_widths */

UCS_TEST_F(test_table, set_min_col_widths_widens_narrow_columns) {
    /* min_widths {10, 10} forces each column to be at least 10 chars,
     * even though the actual content is much shorter. */
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

UCS_TEST_F(test_table,
           set_min_col_widths_does_not_shrink_columns_wider_than_min) {
    /* When the row's content exceeds the minimum, the computed column
     * width wins and the table grows past the minimum. */
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

/* Group L: streaming API (stream_row / print_row / print_separator) */

UCS_TEST_F(test_table,
           streaming_print_plus_rows_plus_separator_matches_inline_render) {
    /* Streaming flow: build a header with a couple of rows, render +
     * print it, then stream two additional rows and close with a
     * separator. The captured stdout must byte-match a fully-inline
     * rendering of the same shape — note that ucs_table_print emits
     * its own bottom frame after the header row, which doubles as the
     * dividing separator between the header and the streamed data;
     * the inline comparison must therefore include an explicit
     * add_separator between the header and the data rows. */
    /* Lock widths so the streamed cells have predictable widths. */
    const int min_widths[2] = {4, 4};
    ucs_table_config_t cfg  = {};
    cfg.n_body_cols         = 2;
    cfg.min_widths          = min_widths;
    table_t streamed(cfg);
    auto *row = ucs_table_add_row(streamed.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "h1");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "h2");

    const std::string streamed_out = capture_stdout([&]() {
        ucs_table_print(streamed.get());

        auto *stream = ucs_table_stream_row_create(streamed.get());
        ucs_table_row_add_cell_fmt(stream, 1, UCS_TABLE_ALIGN_LEFT, "%s", "a1");
        ucs_table_row_add_cell_fmt(stream, 1, UCS_TABLE_ALIGN_LEFT, "%s", "b1");
        ucs_table_print_row(stream);

        ucs_table_stream_row_reset(stream);
        ucs_table_row_add_cell_fmt(stream, 1, UCS_TABLE_ALIGN_LEFT, "%s", "a2");
        ucs_table_row_add_cell_fmt(stream, 1, UCS_TABLE_ALIGN_LEFT, "%s", "b2");
        ucs_table_print_row(stream);

        ucs_table_print_separator(streamed.get());
        ucs_table_stream_row_destroy(stream);
    });

    /* Equivalent fully-inline table: header row, explicit separator
     * (matches the streamed flow's bottom-frame-doubles-as-divider
     * boundary), then the two data rows. */
    table_t inline_table(cfg);
    row = ucs_table_add_row(inline_table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "h1");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "h2");
    ucs_table_add_separator(inline_table.get());
    row = ucs_table_add_row(inline_table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "a1");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "b1");
    row = ucs_table_add_row(inline_table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "a2");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "b2");

    EXPECT_EQ(inline_table.render(), streamed_out);
}

UCS_TEST_F(test_table, stream_row_reset_repopulate_uses_same_widths) {
    /* A stream row reset between two prints produces a different
     * rendered line, but the column widths (from the table) stay
     * the same. */
    const int min_widths[2] = {5, 5};
    ucs_table_config_t cfg  = {};
    cfg.n_body_cols         = 2;
    cfg.min_widths          = min_widths;
    table_t table(cfg);

    /* Need a real row in the table so the header gets a meaningful width. */
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

    /* Both streamed rows render at the locked 5-char widths. The
     * bottom frame after the header row is emitted by ucs_table_print
     * and serves as the visual divider before the streamed data. */
    EXPECT_EQ("+-------+-------+\n"
              "| h1    | h2    |\n"
              "+-------+-------+\n"
              "| x     | y     |\n"
              "| xxxxx | y     |\n",
              out);
}

UCS_TEST_F(test_table, render_row_omits_trailing_newline_for_extra_content) {
    /* render_row writes "| ... |" without a trailing newline so the
     * caller can splice extra content before the final '\n'. */
    const int min_widths[2] = {3, 3};
    ucs_table_config_t cfg  = {};
    cfg.n_body_cols         = 2;
    cfg.min_widths          = min_widths;
    table_t table(cfg);

    auto *header = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(header, 1, UCS_TABLE_ALIGN_LEFT, "%s", "h");
    ucs_table_row_add_cell_fmt(header, 1, UCS_TABLE_ALIGN_LEFT, "%s", "i");
    ucs_table_print(table.get()); /* compute widths */

    auto *stream = ucs_table_stream_row_create(table.get());
    ucs_table_row_add_cell_fmt(stream, 1, UCS_TABLE_ALIGN_LEFT, "%s", "v1");
    ucs_table_row_add_cell_fmt(stream, 1, UCS_TABLE_ALIGN_LEFT, "%s", "v2");

    ucs_string_buffer_t strb = UCS_STRING_BUFFER_INITIALIZER;
    ucs_table_render_row(stream, &strb);
    /* No trailing newline. */
    EXPECT_EQ(std::string("| v1  | v2  |"), ucs_string_buffer_cstr(&strb));

    /* Caller can append more, then a newline. */
    ucs_string_buffer_appendf(&strb, "  extra\n");
    EXPECT_EQ(std::string("| v1  | v2  |  extra\n"),
              ucs_string_buffer_cstr(&strb));
    ucs_string_buffer_cleanup(&strb);

    ucs_table_stream_row_destroy(stream);
}

UCS_TEST_F(test_table, stream_row_supports_all_alignments) {
    /* Stream rows accept the same alignment selector as regular rows. */
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
    /* CENTER-aligned single string. */
    ucs_table_row_add_cell_fmt(stream, 1, UCS_TABLE_ALIGN_CENTER, "%s", "a");
    /* RIGHT-aligned single string. */
    ucs_table_row_add_cell_fmt(stream, 1, UCS_TABLE_ALIGN_RIGHT, "%s", "z");

    ucs_string_buffer_t strb = UCS_STRING_BUFFER_INITIALIZER;
    ucs_table_render_row(stream, &strb);
    EXPECT_EQ(std::string("|  a   |    z |"), ucs_string_buffer_cstr(&strb));
    ucs_string_buffer_cleanup(&strb);

    ucs_table_stream_row_destroy(stream);
}

UCS_TEST_F(test_table,
           streamed_row_with_constant_printf_widths_lays_out_flush) {
    /* Caller-supplied min_widths used with matching constant %* printf
     * widths produces cells that fill the column exactly. */
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

    /* "12" right-padded to 5 chars; "1.50" right-padded to 7;
     * "   7" right-padded to 4. The data row aligns flush with the
     * header cell borders. The bottom frame after the header row is
     * emitted by ucs_table_print. */
    EXPECT_EQ("+-------+---------+------+\n"
              "| A     | B       | C    |\n"
              "+-------+---------+------+\n"
              "|    12 |    1.50 |    7 |\n",
              out);
}


/* Group N: centered cells */

UCS_TEST_F(test_table, centered_cell_pads_evenly_around_text) {
    /* Single body column. Wider second row pins col 0 to 11. CENTER
     * 'abc' in width 11: pad=8, left_pad=4, right_pad=4. */
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

UCS_TEST_F(test_table,
           centered_cell_with_col_span_centers_over_merged_columns) {
    /* Merged CENTER cell spans both body cols. The body row pins each
     * col to width 4, so the merged pixel width is 4+4+3=11. CENTER
     * 'hi' in 11: pad=9, left_pad=4, right_pad=5 (right gets the odd
     * extra). */
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

UCS_TEST_F(test_table, centered_cell_with_odd_padding_biases_right_pad) {
    /* When pad is odd, the right side gets the extra space (left_pad =
     * pad/2, right_pad = pad - left_pad). With width 8 and content 3,
     * pad=5 -> left_pad=2, right_pad=3. */
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

UCS_TEST_F(test_table, centered_cell_with_equal_widths_uses_normalized_width) {
    /* Per-column widths from the row are {1, 6}. equal_widths
     * normalizes both to 6. CENTER 'x' in width 6: pad=5,
     * left_pad=2, right_pad=3. */
    ucs_table_config_t cfg = {};
    cfg.n_body_cols        = 2;
    cfg.equal_widths       = 1;
    table_t table(cfg);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_CENTER, "%s", "x");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "longer");

    EXPECT_EQ("+--------+--------+\n"
              "|   x    | longer |\n"
              "+--------+--------+\n",
              table.render());
}

UCS_TEST_F(test_table, centered_cell_with_min_col_widths_respects_minimum) {
    /* min_widths {7} forces col 0 to 7 even though the content is only
     * 2 chars. CENTER 'ab' in 7: pad=5, left_pad=2, right_pad=3. */
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

UCS_TEST_F(test_table, centered_cell_followed_by_merged_cols_keeps_corner) {
    /* CENTER cell with non-empty content above; the separator below
     * uses merged_cols=1 to blank the leading column. The leading
     * cell on the row below is left empty so the visual merge lines
     * up. CENTER vs LEFT alignment is orthogonal to the
     * separator-driven carry-over. */
    table_t table(2);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_CENTER, "%s", "ctr");
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "data1");
    ucs_table_add_separator_with_merged_cols(table.get(), 1);

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell(row, 1, UCS_TABLE_ALIGN_LEFT);
    ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s", "data2");

    EXPECT_EQ("+-----+-------+\n"
              "| ctr | data1 |\n"
              "|     +-------+\n"
              "|     | data2 |\n"
              "+-----+-------+\n",
              table.render());
}
