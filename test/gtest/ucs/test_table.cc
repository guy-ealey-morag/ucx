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


class test_table : public ucs::test {
protected:
    /* RAII wrapper around the buffered builder: init in ctor, cleanup in
     * dtor so each test fixture is concise and exception-safe. */
    class table_t {
    public:
        explicit table_t(unsigned n_body_cols)
        {
            ucs_table_init(&m_table, n_body_cols);
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
    ucs_table_row_add_cell_left(row, 1, "%s", "abc");

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
    ucs_table_row_add_cell_left(row, 1, "%s", "short");
    ucs_table_row_add_cell_left(row, 1, "%s", "even longer cell");

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_left(row, 1, "%s", "a much wider value");
    ucs_table_row_add_cell_left(row, 1, "%s", "x");

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
    ucs_table_row_add_cell_right(row, 1, "%s", "right-anchored");

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
    ucs_table_row_add_cell_left(row, 1, "%s", "long left value");

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_right(row, 1, "%s", "R");

    EXPECT_EQ("+-----------------+\n"
              "| long left value |\n"
              "|               R |\n"
              "+-----------------+\n",
              table.render());
}


/* Group C: split cells (cell_appendf_left + cell_appendf_right on the same
 * cell) */

UCS_TEST_F(test_table, split_cell_fills_gap_with_spaces) {
    /* Two body columns with one row that pins the body-col-0 width via
     * 'a much wider value' (18 chars). The split cell has 'count' on the
     * left and 'range' on the right; the gap between them is spaces. */
    table_t table(2);

    auto *row  = ucs_table_add_row(table.get());
    auto *cell = ucs_table_row_add_cell(row, 1);
    ucs_table_cell_appendf_left(cell, "%s", "count");
    ucs_table_cell_appendf_right(cell, "%s", "range");
    ucs_table_row_add_cell_left(row, 1, "%s", "filler");

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_left(row, 1, "%s", "a much wider value");
    ucs_table_row_add_cell_left(row, 1, "%s", "filler2");

    /* Body col 0 width = 18, the split cell rendered there has L=5+R=5
     * with (18-5-5)=8 spaces between. */
    EXPECT_EQ("+--------------------+---------+\n"
              "| count        range | filler  |\n"
              "| a much wider value | filler2 |\n"
              "+--------------------+---------+\n",
              table.render());
}

UCS_TEST_F(test_table, split_cell_left_only_renders_as_left_aligned) {
    /* When only set_left is called, the cell still has both buffers
     * available but the right side is empty: the renderer falls back to
     * the left-only branch. */
    table_t table(1);

    auto *row  = ucs_table_add_row(table.get());
    auto *cell = ucs_table_row_add_cell(row, 1);
    ucs_table_cell_appendf_left(cell, "%s", "L only");

    EXPECT_EQ("+--------+\n"
              "| L only |\n"
              "+--------+\n",
              table.render());
}


/* Group D: auto carry-over separators */

UCS_TEST_F(test_table,
           separator_with_empty_leading_cell_below_uses_carry_over) {
    /* Row above has both cells set. Separator follows. Row below leaves
     * the leading cell empty. Expected: leftmost separator segment is a
     * blank "|     " (carry-over), the rest is a dashed "+-----" segment. */
    table_t table(2);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_left(row, 1, "%s", "type");
    ucs_table_row_add_cell_left(row, 1, "%s", "data1");
    ucs_table_add_separator(table.get());

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_left(row, 1, "%s", "");
    ucs_table_row_add_cell_left(row, 1, "%s", "data2");

    EXPECT_EQ("+------+-------+\n"
              "| type | data1 |\n"
              "|      +-------+\n"
              "|      | data2 |\n"
              "+------+-------+\n",
              table.render());
}

UCS_TEST_F(test_table, separator_without_empty_leading_cells_uses_plus_corner) {
    /* All leading cells of row below are non-empty: regular "+-----+"
     * separator with no carry-over. */
    table_t table(2);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_left(row, 1, "%s", "a");
    ucs_table_row_add_cell_left(row, 1, "%s", "b");
    ucs_table_add_separator(table.get());

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_left(row, 1, "%s", "c");
    ucs_table_row_add_cell_left(row, 1, "%s", "d");

    EXPECT_EQ("+---+---+\n"
              "| a | b |\n"
              "+---+---+\n"
              "| c | d |\n"
              "+---+---+\n",
              table.render());
}

UCS_TEST_F(test_table, separator_at_table_edges_is_always_plain_frame) {
    /* A separator immediately after the last row has no row below; it
     * collapses to the regular bottom-frame style with no carry-over.
     * Since that line is byte-identical to the auto-appended bottom
     * frame, render() suppresses the auto bottom frame so the table
     * ends with a single "+---+--+" line rather than two duplicated
     * frames. */
    table_t table(2);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_left(row, 1, "%s", "x");
    ucs_table_row_add_cell_left(row, 1, "%s", "");
    ucs_table_add_separator(table.get());

    EXPECT_EQ("+---+--+\n"
              "| x |  |\n"
              "+---+--+\n",
              table.render());
}


/* Group E: col_span */

UCS_TEST_F(test_table, col_span_merged_cell_short_content_inherits_widths) {
    /* The merged cell content fits within the sum of base column widths
     * (computed from the body row's per-column cells), so no expansion is
     * needed. */
    table_t table(2);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_left(row, 2, "%s", "hdr");

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_left(row, 1, "%s", "abcdef");
    ucs_table_row_add_cell_left(row, 1, "%s", "ghijklm");

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
    ucs_table_row_add_cell_left(row, 2, "%s", "this header is too wide");

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_left(row, 1, "%s", "ab");
    ucs_table_row_add_cell_left(row, 1, "%s", "cd");

    /* base widths {2, 2}; merged pixel width = 2 + 2 + 3 = 7; content =
     * 23. deficit 23 - 7 = 16 added to widths[1]. New widths {2, 18}. */
    EXPECT_EQ("+----+--------------------+\n"
              "| this header is too wide |\n"
              "| ab | cd                 |\n"
              "+----+--------------------+\n",
              table.render());
}


/* Group F: row_add_cell_left/_right equivalence with the handle setters */

UCS_TEST_F(test_table, row_add_cell_left_equals_add_cell_plus_appendf_left) {
    table_t table_a(2);
    auto *row_a = ucs_table_add_row(table_a.get());
    ucs_table_row_add_cell_left(row_a, 1, "%s", "hello");
    ucs_table_row_add_cell_left(row_a, 1, "%s", "world");

    table_t table_b(2);
    auto *row_b   = ucs_table_add_row(table_b.get());
    auto *cell_b1 = ucs_table_row_add_cell(row_b, 1);
    ucs_table_cell_appendf_left(cell_b1, "%s", "hello");
    auto *cell_b2 = ucs_table_row_add_cell(row_b, 1);
    ucs_table_cell_appendf_left(cell_b2, "%s", "world");

    EXPECT_EQ(table_a.render(), table_b.render());
}

UCS_TEST_F(test_table, row_add_cell_right_equals_add_cell_plus_appendf_right) {
    table_t table_a(2);
    auto *row_a = ucs_table_add_row(table_a.get());
    ucs_table_row_add_cell_right(row_a, 1, "%s", "hello");
    ucs_table_row_add_cell_right(row_a, 1, "%s", "world");

    table_t table_b(2);
    auto *row_b   = ucs_table_add_row(table_b.get());
    auto *cell_b1 = ucs_table_row_add_cell(row_b, 1);
    ucs_table_cell_appendf_right(cell_b1, "%s", "hello");
    auto *cell_b2 = ucs_table_row_add_cell(row_b, 1);
    ucs_table_cell_appendf_right(cell_b2, "%s", "world");

    EXPECT_EQ(table_a.render(), table_b.render());
}

UCS_TEST_F(test_table, cell_appendf_left_concatenates_multiple_calls) {
    /* The plain rename intent: cell_appendf_* supports calling more than
     * once to build the cell content incrementally. */
    table_t table(1);

    auto *row  = ucs_table_add_row(table.get());
    auto *cell = ucs_table_row_add_cell(row, 1);
    ucs_table_cell_appendf_left(cell, "%s", "hello");
    ucs_table_cell_appendf_left(cell, "-%s", "world");

    EXPECT_EQ("+-------------+\n"
              "| hello-world |\n"
              "+-------------+\n",
              table.render());
}


/* Group G: printf formatting goes through unchanged */

UCS_TEST_F(test_table, printf_formatting_supported_by_all_setters) {
    /* Exercises printf formatting through every setter variant. A wider
     * row pins body col 0 to 12, which leaves a visible gap between the
     * split cell's left and right halves. */
    table_t table(2);

    auto *row  = ucs_table_add_row(table.get());
    auto *cell = ucs_table_row_add_cell(row, 1);
    ucs_table_cell_appendf_left(cell, "%d", 42);
    ucs_table_cell_appendf_right(cell, "%s..%s", "lo", "hi");
    ucs_table_row_add_cell_left(row, 1, "%s=%u", "k", 7u);

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_left(row, 1, "%s", "padding row.");
    ucs_table_row_add_cell_left(row, 1, "%s", "");

    /* Body col 0 width = 12, the split cell renders as L=2 + 4 spaces +
     * R=6. */
    EXPECT_EQ("+--------------+-----+\n"
              "| 42    lo..hi | k=7 |\n"
              "| padding row. |     |\n"
              "+--------------+-----+\n",
              table.render());
}


/* Group H: integration - mirrors the proto_debug layout */

UCS_TEST_F(test_table, integration_proto_debug_like_layout) {
    /* Three body columns. Header is two cells (right-anchored ep_cfg in
     * col 0, left-anchored sel_param spanning cols 1+2). Body rows have
     * three cells, the first being a split cell with optional left
     * count and right range. */
    table_t table(3);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_right(row, 1, "%s", "cfg#1");
    ucs_table_row_add_cell_left(row, 2, "%s", "op from host memory");
    ucs_table_add_separator(table.get());

    /* Body row 1: no count, range = 0..2038 */
    row        = ucs_table_add_row(table.get());
    auto *cell = ucs_table_row_add_cell(row, 1);
    ucs_table_cell_appendf_right(cell, "%s", "0..2038");
    ucs_table_row_add_cell_left(row, 1, "%s", "short");
    ucs_table_row_add_cell_left(row, 1, "%s", "rc_mlx5");

    /* Body row 2: count 0, range = 2039..8184 */
    row  = ucs_table_add_row(table.get());
    cell = ucs_table_row_add_cell(row, 1);
    ucs_table_cell_appendf_left(cell, "%u  ", 0u);
    ucs_table_cell_appendf_right(cell, "%s", "2039..8184");
    ucs_table_row_add_cell_left(row, 1, "%s", "zero-copy");
    ucs_table_row_add_cell_left(row, 1, "%s", "rc_mlx5");

    const std::string out = table.render();

    /* Body col 0 width is max("0..2038", "0  2039..8184") = 13.
     * Body col 1 width is max("short", "zero-copy")     = 9.
     * Body col 2 width is max("rc_mlx5", "rc_mlx5")     = 7.
     * Header col 1 spans cols 1+2 (= 9+7+3 = 19); content "op from
     * host memory" is 19 chars, exactly fits without expansion. */
    EXPECT_EQ("+---------------+-----------+---------+\n"
              "|         cfg#1 | op from host memory |\n"
              "+---------------+-----------+---------+\n"
              "|       0..2038 | short     | rc_mlx5 |\n"
              "| 0  2039..8184 | zero-copy | rc_mlx5 |\n"
              "+---------------+-----------+---------+\n",
              out);
}


/* Group I: row_prefix */

UCS_TEST_F(test_table, row_prefix_default_is_null_no_prefix) {
    /* Default value of row_prefix is NULL; rendering matches a table
     * that never touched the prefix. */
    table_t table(1);
    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_left(row, 1, "%s", "abc");

    EXPECT_EQ("+-----+\n"
              "| abc |\n"
              "+-----+\n",
              table.render());
}

UCS_TEST_F(test_table, row_prefix_prepended_to_every_line) {
    /* "# " is prepended to body rows AND to the top/bottom frame
     * separators, so the entire table reads as a comment block. */
    table_t table(1);
    ucs_table_set_row_prefix(table.get(), "# ");
    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_left(row, 1, "%s", "abc");

    EXPECT_EQ("# +-----+\n"
              "# | abc |\n"
              "# +-----+\n",
              table.render());
}

UCS_TEST_F(test_table, row_prefix_prepended_to_inner_separator) {
    /* Verify an explicit add_separator line between rows also receives
     * the prefix. */
    table_t table(1);
    ucs_table_set_row_prefix(table.get(), "# ");

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_left(row, 1, "%s", "a");
    ucs_table_add_separator(table.get());
    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_left(row, 1, "%s", "b");

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
    table_t table(1);
    ucs_table_set_row_prefix(table.get(), "");
    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_left(row, 1, "%s", "abc");

    EXPECT_EQ("+-----+\n"
              "| abc |\n"
              "+-----+\n",
              table.render());
}

UCS_TEST_F(test_table, row_prefix_can_be_cleared_back_to_null) {
    /* After a non-NULL prefix is set and then cleared, subsequent
     * renders must drop the prefix on every line. */
    table_t table(1);
    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_left(row, 1, "%s", "abc");

    ucs_table_set_row_prefix(table.get(), "# ");
    EXPECT_EQ("# +-----+\n"
              "# | abc |\n"
              "# +-----+\n",
              table.render());

    ucs_table_set_row_prefix(table.get(), NULL);
    EXPECT_EQ("+-----+\n"
              "| abc |\n"
              "+-----+\n",
              table.render());
}


/* Group J: equal_widths */

UCS_TEST_F(test_table, equal_widths_default_disabled_uses_per_column_widths) {
    /* Without ucs_table_set_equal_widths, each body column keeps its
     * own per-column width. Regression guard against accidental
     * normalization. */
    table_t table(3);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_left(row, 1, "%s", "a");
    ucs_table_row_add_cell_left(row, 1, "%s", "longer");
    ucs_table_row_add_cell_left(row, 1, "%s", "xy");

    EXPECT_EQ("+---+--------+----+\n"
              "| a | longer | xy |\n"
              "+---+--------+----+\n",
              table.render());
}

UCS_TEST_F(test_table, equal_widths_normalizes_all_columns_to_max) {
    /* Per-column widths from the row are {1, 6, 2}. With equal_widths
     * enabled, render() picks max=6 and widens every column to 6 so
     * the short cells pad out to match the widest one. */
    table_t table(3);
    ucs_table_set_equal_widths(table.get(), 1);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_left(row, 1, "%s", "a");
    ucs_table_row_add_cell_left(row, 1, "%s", "longer");
    ucs_table_row_add_cell_left(row, 1, "%s", "xy");

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
    table_t table(2);
    ucs_table_set_equal_widths(table.get(), 1);

    auto *row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_left(row, 2, "%s", "this header is too wide");

    row = ucs_table_add_row(table.get());
    ucs_table_row_add_cell_left(row, 1, "%s", "ab");
    ucs_table_row_add_cell_left(row, 1, "%s", "cd");

    EXPECT_EQ("+--------------------+--------------------+\n"
              "| this header is too wide                 |\n"
              "| ab                 | cd                 |\n"
              "+--------------------+--------------------+\n",
              table.render());
}
