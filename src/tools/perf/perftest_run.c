/**
* Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2021-2026. ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "perftest.h"

#include <ucs/debug/log.h>
#include <ucs/debug/table.h>
#include <ucs/debug/memtrack_int.h>
#include <ucs/sys/sock.h>
#include <ucs/sys/string.h>
#include <ucs/sys/sys.h>

#include <getopt.h>
#include <locale.h>
#include <string.h>


/* Shared with read_batch_file (originally local to that function); hoisted
 * so perftest_max_test_name_width can reuse it. */
#define PERFTEST_MAX_ARG_SIZE 2048


/* Single source of truth for the results-table column widths. Col 0 is
 * the "Stage/Test" prefix; cols 1-8 mirror the data row format below.
 * Cols 1, 5 are widened from the old header (14, 10) to match the data
 * widths (18, 11), eliminating the overflow that today's hand-drawn
 * separator silently allowed. */
#define PERFTEST_RESULTS_N_COLS 9
static const int results_col_widths[PERFTEST_RESULTS_N_COLS] = {14, 18, 10,
                                                                9,  9,  11,
                                                                10, 11, 11};


/* Visible pixel width of body columns [col, col + col_span). Mirrors
 * ucs_table_cell_pixel_width() so the printf %* width passed to
 * stream-row cells exactly matches the cell width the table will draw. */
static int perftest_results_col_width(unsigned col, unsigned col_span)
{
    int w = 0;
    unsigned i;

    for (i = 0; i < col_span; ++i) {
        w += results_col_widths[col + i];
    }
    return w + 3 * ((int)col_span - 1);
}


/* Pre-walk all batch files to find the maximum width of any joined
 * test_name string. Per-leaf test_names are joined with "/" so the
 * worst case is sum_of_per_file_max_token + (num_batch_files - 1) for
 * the separators. Used in FINAL mode to widen col 0 of the results
 * table so the prefix cell carrying the joined test name never
 * overflows; in !FINAL mode the test_name lives in a col_span=9
 * preamble row that auto-fits the existing total width, so col 0 is
 * not widened. Returns 0 when there are no batch files. */
static size_t perftest_max_test_name_width(const struct perftest_context *ctx)
{
    size_t total = 0;
    unsigned i;

    for (i = 0; i < ctx->num_batch_files; ++i) {
        char line[PERFTEST_MAX_ARG_SIZE];
        size_t max_in_file = 0;
        FILE *f;

        f = fopen(ctx->batch_files[i], "r");
        if (f == NULL) {
            continue; /* will be diagnosed by run_test_recurs */
        }

        while (fgets(line, sizeof(line), f) != NULL) {
            char *tok = strtok(line, " \t\n\r");
            if ((tok == NULL) || (tok[0] == '#')) {
                continue;
            }
            max_in_file = ucs_max(max_in_file, strlen(tok));
        }
        fclose(f);

        total += max_in_file + ((i > 0) ? 1 : 0); /* +1 for "/" separator */
    }

    return total;
}


/* Forward declared; defined alongside the other meta helpers further
 * down in this file. */
static void
get_accel_device_str(const ucx_perf_accel_dev_t *dev, char *str, size_t size);


/* Append the meta rows (API, Test, Data layout, Send/Recv memory,
 * optional Send/Recv device, Message size, Window size, optional
 * AM header size) to `table`. Each row is a single LEFT cell of width
 * `col_span`. The caller has already verified that test->api is UCT
 * or UCP. */
static void perftest_add_meta_rows(ucs_table_t *table,
                                   const struct perftest_context *ctx,
                                   const test_type_t *test, unsigned col_span)
{
    /* Label width chosen to cover the longest meta key ("AM header size:"
     * = 15 chars) so values stay column-aligned across all rows. */
    static const char *meta_row_fmt_s  = "%-13s %s";
    static const char *meta_row_fmt_zu = "%-13s %zu";
    static const char *meta_row_fmt_u  = "%-13s %u";
    const char *test_api_str;
    const char *test_data_str;
    char mem_dev_str[16];
    ucs_table_row_t *row;

    if (test->api == UCX_PERF_API_UCT) {
        test_api_str = "transport layer";
        switch (ctx->params.super.uct.data_layout) {
        case UCT_PERF_DATA_LAYOUT_SHORT:
            test_data_str = "short";
            break;
        case UCT_PERF_DATA_LAYOUT_SHORT_IOV:
            test_data_str = "short iov";
            break;
        case UCT_PERF_DATA_LAYOUT_BCOPY:
            test_data_str = "bcopy";
            break;
        case UCT_PERF_DATA_LAYOUT_ZCOPY:
            test_data_str = "zcopy";
            break;
        default:
            test_data_str = "(undefined)";
            break;
        }
    } else {
        ucs_assert(test->api == UCX_PERF_API_UCP);
        test_api_str  = "protocol layer";
        test_data_str = "(automatic)"; /* TODO contig/stride/stream */
    }

    row = ucs_table_add_row(table);
    ucs_table_row_add_cell_fmt(row, col_span, UCS_TABLE_ALIGN_LEFT,
                               meta_row_fmt_s, "API:", test_api_str);

    row = ucs_table_add_row(table);
    ucs_table_row_add_cell_fmt(row, col_span, UCS_TABLE_ALIGN_LEFT,
                               meta_row_fmt_s, "Test:", test->desc);

    row = ucs_table_add_row(table);
    ucs_table_row_add_cell_fmt(row, col_span, UCS_TABLE_ALIGN_LEFT,
                               meta_row_fmt_s, "Data layout:", test_data_str);

    row = ucs_table_add_row(table);
    ucs_table_row_add_cell_fmt(
            row, col_span, UCS_TABLE_ALIGN_LEFT, meta_row_fmt_s, "Send memory:",
            ucs_memory_type_names[ctx->params.super.send_mem_type]);

    row = ucs_table_add_row(table);
    ucs_table_row_add_cell_fmt(
            row, col_span, UCS_TABLE_ALIGN_LEFT, meta_row_fmt_s, "Recv memory:",
            ucs_memory_type_names[ctx->params.super.recv_mem_type]);

    if (ctx->params.super.send_device.mem_type != UCS_MEMORY_TYPE_LAST) {
        get_accel_device_str(&ctx->params.super.send_device, mem_dev_str,
                             sizeof(mem_dev_str));
        row = ucs_table_add_row(table);
        ucs_table_row_add_cell_fmt(row, col_span, UCS_TABLE_ALIGN_LEFT,
                                   meta_row_fmt_s, "Send device:", mem_dev_str);
    }

    if (ctx->params.super.recv_device.mem_type != UCS_MEMORY_TYPE_LAST) {
        get_accel_device_str(&ctx->params.super.recv_device, mem_dev_str,
                             sizeof(mem_dev_str));
        row = ucs_table_add_row(table);
        ucs_table_row_add_cell_fmt(row, col_span, UCS_TABLE_ALIGN_LEFT,
                                   meta_row_fmt_s, "Recv device:", mem_dev_str);
    }

    row = ucs_table_add_row(table);
    ucs_table_row_add_cell_fmt(row, col_span, UCS_TABLE_ALIGN_LEFT,
                               meta_row_fmt_zu, "Message size:",
                               ucx_perf_get_message_size(&ctx->params.super));

    row = ucs_table_add_row(table);
    ucs_table_row_add_cell_fmt(row, col_span, UCS_TABLE_ALIGN_LEFT,
                               meta_row_fmt_u, "Window size:",
                               ctx->params.super.max_outstanding);

    if ((test->api == UCX_PERF_API_UCP) && (test->command == UCX_PERF_CMD_AM)) {
        row = ucs_table_add_row(table);
        ucs_table_row_add_cell_fmt(row, col_span, UCS_TABLE_ALIGN_LEFT,
                                   meta_row_fmt_zu, "AM header size:",
                                   ctx->params.super.ucp.am_hdr_size);
    }
}


/* Open the results-printing table. Chooses one of three layouts:
 *
 *   - has_headers:  9-col unified table. Holds optional meta rows
 *                   (col_span=9, when has_meta), an inner separator,
 *                   the merged-span row, an inner separator, and the
 *                   col-header row. Followed by stream rows for the
 *                   per-iteration data path.
 *   - has_meta only (server-side meta-only or CSV+meta): 1-col table
 *                   with content-derived width (no min_widths set;
 *                   the compact width keeps the meta box right-sized
 *                   to its own content rather than the would-be
 *                   results-table outer width). No stream rows.
 *   - neither:      no-op (e.g. !PRINT_TEST+PRINT_RESULTS+CSV, where
 *                   only print_header's CSV header line is emitted).
 *
 * Col 0 of the 9-col table is widened to max_test_name_width only in
 * FINAL mode (the prefix cell carries the joined test name there).
 * In !FINAL mode the test_name lives in a col_span=9 preamble row that
 * auto-fits the existing total width, so col 0 keeps its default 14
 * chars.
 *
 * In the !FINAL+batch+!CSV+PRINT_RESULTS case, the per-test preamble
 * is emitted from the leaf in run_test_recurs and matched by an
 * after-data separator there. The first test's leading divider is
 * this table's natural bottom frame; the last test's after-data
 * separator doubles as the table's closing line. */
static void perftest_results_table_open(struct perftest_context *ctx,
                                        const test_type_t *test)
{
    const int is_final         = !!(ctx->flags & TEST_FLAG_PRINT_FINAL);
    const int print_test       = !!(ctx->flags & TEST_FLAG_PRINT_TEST);
    const int print_csv        = !!(ctx->flags & TEST_FLAG_PRINT_CSV);
    const int has_meta         = print_test && (test != NULL) &&
                                 ((test->api == UCX_PERF_API_UCT) ||
                                  (test->api == UCX_PERF_API_UCP));
    const int has_headers      = (ctx->flags & TEST_FLAG_PRINT_RESULTS) &&
                                 !print_csv;
    ucs_table_config_t cfg     = {};
    int min_widths[PERFTEST_RESULTS_N_COLS];
    const char *overhead_lat_str;
    ucs_table_row_t *row;
    unsigned i;

    if (!has_meta && !has_headers) {
        return;
    }

    cfg.n_body_cols = has_headers ? PERFTEST_RESULTS_N_COLS : 1;

    if (has_headers) {
        memcpy(min_widths, results_col_widths, sizeof(min_widths));
        if (is_final) {
            min_widths[0] = ucs_max(min_widths[0],
                                    (int)ctx->max_test_name_width);
        }
        cfg.min_widths = min_widths;
    }

    /* CSV+meta and server-side meta-only paths leave cfg.min_widths
     * unset; the 1-col meta box renders at content-derived width
     * (compact). */
    ucs_table_init(&ctx->results_table, &cfg);

    if (has_meta) {
        perftest_add_meta_rows(&ctx->results_table, ctx, test, cfg.n_body_cols);
    }

    if (has_headers) {
        if (has_meta) {
            /* Inner separator between the meta rows and the merged-
             * span row. Without meta, the merged-span row sits
             * directly under the table's auto top frame. */
            ucs_table_add_separator(&ctx->results_table);
        }

        overhead_lat_str = (test == NULL) ? "overhead" : test->overhead_lat;

        /* Merged top header: empty (2) | overhead span 3 |
         *                    bandwidth span 2 | msgrate span 2.
         * Labels centered over their column groups. */
        row = ucs_table_add_row(&ctx->results_table);
        ucs_table_row_add_cell(row, 2, UCS_TABLE_ALIGN_LEFT);
        ucs_table_row_add_cell_fmt(row, 3, UCS_TABLE_ALIGN_CENTER, "%s (usec)",
                                   overhead_lat_str);
        ucs_table_row_add_cell_fmt(row, 2, UCS_TABLE_ALIGN_CENTER,
                                   "bandwidth (MB/s)");
        ucs_table_row_add_cell_fmt(row, 2, UCS_TABLE_ALIGN_CENTER,
                                   "message rate (msg/s)");
        ucs_table_add_separator(&ctx->results_table);

        /* Bottom header: 9 cells, all centered over their columns. */
        row = ucs_table_add_row(&ctx->results_table);
        ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_CENTER, "%s",
                                   is_final ? "Test" : "Stage");
        ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_CENTER,
                                   "# iterations");
        ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_CENTER, "%.1f%%ile",
                                   ctx->params.super.percentile_rank);
        ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_CENTER, "average");
        ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_CENTER, "overall");
        ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_CENTER, "average");
        ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_CENTER, "overall");
        ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_CENTER, "average");
        ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_CENTER, "overall");
    }

    ucs_table_print(&ctx->results_table);

    if (!has_headers) {
        /* Meta-only path: nothing else to write to the table. */
        ucs_table_cleanup(&ctx->results_table);
        return;
    }

    /* Per-thread stream rows. The per-iteration report_func fires
     * concurrently from all OMP threads in MT mode (libperf_thread.c),
     * so each thread needs its own row to avoid racing on reset/add_cell. */
    ctx->n_stream_rows = ucs_max(1u, ctx->params.super.thread_count);
    ctx->results_stream_rows =
            ucs_malloc(ctx->n_stream_rows * sizeof(*ctx->results_stream_rows),
                       "perftest_stream_rows");
    if (ctx->results_stream_rows == NULL) {
        ucs_fatal("failed to allocate perftest stream rows");
    }
    for (i = 0; i < ctx->n_stream_rows; ++i) {
        ctx->results_stream_rows[i] = ucs_table_stream_row_create(
                &ctx->results_table);
    }
}


/* Close the streamed region and release all stream rows + the table.
 * No-op when the table was never opened (CSV mode or server-side
 * meta-only runs).
 *
 * In !FINAL+batch mode the leaf in run_test_recurs already emits an
 * after-data separator that doubles as the closing line for the box,
 * so we skip the closing separator here. In all other modes that
 * opened stream rows (single-test, FINAL+batch) the data stream has
 * no per-test trailing separator, and we emit the closing separator
 * to seal the box. */
static void perftest_results_table_close(struct perftest_context *ctx)
{
    unsigned i;

    if (ctx->results_stream_rows == NULL) {
        return;
    }

    if ((ctx->flags & TEST_FLAG_PRINT_FINAL) || (ctx->num_batch_files == 0)) {
        ucs_table_print_separator(&ctx->results_table);
    }
    for (i = 0; i < ctx->n_stream_rows; ++i) {
        ucs_table_stream_row_destroy(ctx->results_stream_rows[i]);
    }
    ucs_free(ctx->results_stream_rows);
    ucs_table_cleanup(&ctx->results_table);

    ctx->results_stream_rows = NULL;
    ctx->n_stream_rows       = 0;
}


void print_progress(void *UCS_V_UNUSED rte_group,
                    const ucx_perf_result_t *result, void *arg,
                    const char *extra_info, int final, int is_multi_thread)
{
    struct perftest_context *ctx = arg;
    unsigned ti                  = 0;
    ucs_table_row_t *row;
    const char *fmt_int, *fmt_lat, *fmt_bw;

    /* Preserve today's early-return shape: skip non-final lines when the
     * user only wants the FINAL summary, and skip everything when results
     * are disabled. This also guarantees ctx->results_stream_rows is
     * non-NULL by the time the streaming code below runs (the table is
     * opened iff PRINT_RESULTS is set). */
    if (!(ctx->flags & TEST_FLAG_PRINT_RESULTS) ||
        (!final && (ctx->flags & TEST_FLAG_PRINT_FINAL))) {
        return;
    }

    /* CSV path: keep the existing raw printf behavior verbatim. The
     * table is not initialized in CSV mode, so the streaming path
     * below cannot be used. */
    if (ctx->flags & TEST_FLAG_PRINT_CSV) {
        UCS_STRING_BUFFER_ONSTACK(strb, 256);
        unsigned i;
        const char *fmt_csv;

        for (i = 0; i < ctx->num_batch_files; ++i) {
            ucs_string_buffer_appendf(&strb, "%s,", ctx->test_names[i]);
        }

        if (!final) {
#if _OPENMP
            ucs_string_buffer_appendf(&strb, "[thread %d]",
                                      omp_get_thread_num());
#endif
        }

        if (is_multi_thread && final) {
            fmt_csv = "%4.0f,%.3f,%.2f,%.0f";
            ucs_string_buffer_appendf(&strb, fmt_csv, (double)result->iters,
                                      result->latency.total_average * 1e6,
                                      result->bandwidth.total_average /
                                              (1024.0 * 1024.0),
                                      result->msgrate.total_average);
        } else {
            fmt_csv = "%4.0f,%.3f,%.3f,%.3f,%.2f,%.2f,%.0f,%.0f";
            ucs_string_buffer_appendf(&strb, fmt_csv, (double)result->iters,
                                      result->latency.percentile * 1e6,
                                      result->latency.moment_average * 1e6,
                                      result->latency.total_average * 1e6,
                                      result->bandwidth.moment_average /
                                              (1024.0 * 1024.0),
                                      result->bandwidth.total_average /
                                              (1024.0 * 1024.0),
                                      result->msgrate.moment_average,
                                      result->msgrate.total_average);
        }

        fprintf(stdout, "%s\n", ucs_string_buffer_cstr(&strb));
        fflush(stdout);
        return;
    }

    /* Non-CSV path: stream a row into the per-thread scratch row, then
     * print it against the table's locked widths. */
#if _OPENMP
    ti = (unsigned)omp_get_thread_num();
#endif
    ucs_assert(ti < ctx->n_stream_rows);
    row = ctx->results_stream_rows[ti];
    ucs_table_stream_row_reset(row);

    /* Cell 0: prefix cell. The [thread N] label is emitted for EVERY
     * non-final progress callback when compiled with _OPENMP (today's
     * behavior shows "[thread 0]" even in single-thread runs), not
     * gated on is_multi_thread. is_multi_thread is set only for the
     * aggregated final summary from ucx_perf_thread_report_aggregated. */
    if (!final) {
#if _OPENMP
        ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "[thread %d]",
                                   omp_get_thread_num());
#else
        ucs_table_row_add_cell(row, 1, UCS_TABLE_ALIGN_LEFT); /* empty prefix */
#endif
    } else if (ctx->flags & TEST_FLAG_PRINT_FINAL) {
        /* FINAL mode: prefix carries the test name (or "Final:" when no
         * batch). This replaces the per-test banner from print_test_name. */
        if (ctx->num_batch_files > 0) {
            UCS_STRING_BUFFER_ONSTACK(name_buf, 256);
            ucs_string_buffer_append_array(&name_buf, "/", "%s",
                                           ctx->test_names,
                                           ctx->num_batch_files);
            ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "%s",
                                       ucs_string_buffer_cstr(&name_buf));
        } else {
            ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "Final:");
        }
    } else {
        /* final && !PRINT_FINAL: per-stage end-of-run summary line. */
        ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_LEFT, "Final:");
    }

    /* Cells 1..8: data. Widths come from results_col_widths via %*, so
     * they cannot drift from the table's column widths. The %' thousands-
     * separator flag is selected per-cell based on TEST_FLAG_NUMERIC_FMT,
     * applied only to iters and msgrate cells (matches today's fmt_numeric). */
    fmt_int = (ctx->flags & TEST_FLAG_NUMERIC_FMT) ? "%'*.0f" : "%*.0f";
    fmt_lat = "%*.3f"; /* latency: never grouped */
    fmt_bw  = "%*.2f"; /* bandwidth: never grouped */

    if (is_multi_thread && final) {
        /* MT-final: merged spans (1, 1, 3, 2, 2). The %* widths here
         * are derived from the same results_col_widths array, so they
         * automatically match the merged-cell pixel widths of the table
         * (the bug today's hand-coded 29/22/23 widths exhibited). */
        ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_RIGHT, fmt_int,
                                   perftest_results_col_width(1, 1),
                                   (double)result->iters);
        ucs_table_row_add_cell_fmt(row, 3, UCS_TABLE_ALIGN_RIGHT, fmt_lat,
                                   perftest_results_col_width(2, 3),
                                   result->latency.total_average * 1e6);
        ucs_table_row_add_cell_fmt(row, 2, UCS_TABLE_ALIGN_RIGHT, fmt_bw,
                                   perftest_results_col_width(5, 2),
                                   result->bandwidth.total_average /
                                           (1024.0 * 1024.0));
        ucs_table_row_add_cell_fmt(row, 2, UCS_TABLE_ALIGN_RIGHT, fmt_int,
                                   perftest_results_col_width(7, 2),
                                   result->msgrate.total_average);
    } else {
        /* Normal: 8 cells of col_span 1. iters + msgrate (avg, overall)
         * use fmt_int; the others are plain. */
        ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_RIGHT, fmt_int,
                                   results_col_widths[1],
                                   (double)result->iters);
        ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_RIGHT, fmt_lat,
                                   results_col_widths[2],
                                   result->latency.percentile * 1e6);
        ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_RIGHT, fmt_lat,
                                   results_col_widths[3],
                                   result->latency.moment_average * 1e6);
        ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_RIGHT, fmt_lat,
                                   results_col_widths[4],
                                   result->latency.total_average * 1e6);
        ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_RIGHT, fmt_bw,
                                   results_col_widths[5],
                                   result->bandwidth.moment_average /
                                           (1024.0 * 1024.0));
        ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_RIGHT, fmt_bw,
                                   results_col_widths[6],
                                   result->bandwidth.total_average /
                                           (1024.0 * 1024.0));
        ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_RIGHT, fmt_int,
                                   results_col_widths[7],
                                   result->msgrate.moment_average);
        ucs_table_row_add_cell_fmt(row, 1, UCS_TABLE_ALIGN_RIGHT, fmt_int,
                                   results_col_widths[8],
                                   result->msgrate.total_average);
    }

    if ((ctx->flags & TEST_FLAG_PRINT_EXTRA_INFO) && (extra_info != NULL)) {
        /* -X mode: render the row WITHOUT a trailing newline so we can
         * splice "  <extra_info>" before the line break. Matches today's
         * perftest_run.c:100 behavior: the trailing "  " is emitted even
         * when extra_info is the empty string. */
        UCS_STRING_BUFFER_ONSTACK(line_strb, 512);
        ucs_table_render_row(row, &line_strb);
        ucs_string_buffer_appendf(&line_strb, "  %s\n", extra_info);
        printf("%s", ucs_string_buffer_cstr(&line_strb));
    } else {
        ucs_table_print_row(row);
    }

    /* Match today's fflush after each progress line so output stays live
     * when stdout is piped/redirected (fully buffered). */
    fflush(stdout);
}


static void
get_accel_device_str(const ucx_perf_accel_dev_t *dev, char *str, size_t size)
{
    // TODO: retrieve runtime device id
    ucs_snprintf_safe(str, size,
                      (dev->device_id == UCX_PERF_MEM_DEV_DEFAULT) ? "%s" :
                                                                     "%s:%d",
                      ucs_memory_type_names[dev->mem_type], dev->device_id);
}


static void print_header(struct perftest_context *ctx)
{
    test_type_t *test;
    unsigned i;

    test = (ctx->params.test_id == TEST_ID_UNDEFINED) ?
                   NULL :
                   &tests[ctx->params.test_id];

    /* Open one shared results table for the whole run. Builds either
     * a unified 9-col meta+headers table (PRINT_TEST+PRINT_RESULTS+
     * !CSV), a 9-col headers-only table (PRINT_RESULTS+!CSV without
     * meta), a 1-col meta box (CSV+meta or server-side meta-only),
     * or nothing at all (no PRINT_TEST and no PRINT_RESULTS, or
     * PRINT_RESULTS+CSV without meta).
     *
     * In !FINAL mode each test in run_test_recurs emits a col_span=9
     * test_name row + after-name separator into the shared table,
     * with an after-data separator emitted once ucx_perf_run returns.
     * The table is closed at the end of run_test. */
    perftest_results_table_open(ctx, test);

    if ((ctx->flags & TEST_FLAG_PRINT_CSV) &&
        (ctx->flags & TEST_FLAG_PRINT_RESULTS)) {
        /* CSV header: prefix the batch-file basenames (if any) and
         * then the fixed CSV column list. Printed after the meta box
         * (if any) but is not part of it. */
        for (i = 0; i < ctx->num_batch_files; ++i) {
            printf("%s,", ucs_basename(ctx->batch_files[i]));
        }
        printf("iterations,%.1f_percentile_lat,avg_lat,overall_lat,"
               "avg_bw,overall_bw,avg_mr,overall_mr\n",
               ctx->params.super.percentile_rank);
    }
}


static ucs_status_t read_batch_file(FILE *batch_file, const char *file_name,
                                    int *line_num, perftest_params_t *params,
                                    char **test_name_p)
{
#define MAX_SIZE 256
    ucs_status_t status;
    char buf[PERFTEST_MAX_ARG_SIZE];
    char error_prefix[PERFTEST_MAX_ARG_SIZE];
    int argc;
    char *argv[MAX_SIZE + 1];
    int c;
    char *p;

    do {
        if (fgets(buf, sizeof(buf) - 1, batch_file) == NULL) {
            return UCS_ERR_NO_ELEM;
        }
        ++(*line_num);

        argc = 0;
        p    = strtok(buf, " \t\n\r");
        while (p && (argc < MAX_SIZE)) {
            argv[argc++] = p;
            p            = strtok(NULL, " \t\n\r");
        }
        argv[argc] = NULL;
    } while ((argc == 0) || (argv[0][0] == '#'));

    ucs_snprintf_safe(error_prefix, sizeof(error_prefix),
                      "in batch file '%s' line %d: ", file_name, *line_num);

    optind = 1;
    while ((c = getopt_long(argc, argv, TEST_PARAMS_ARGS, TEST_PARAMS_ARGS_LONG,
                            NULL)) != -1) {
        status = parse_test_params(params, c, optarg);
        if (status != UCS_OK) {
            ucs_error("%s-%c %s: %s", error_prefix, c, optarg,
                      ucs_status_string(status));
            return status;
        }
    }

    status = adjust_test_params(params, error_prefix);
    if (status != UCS_OK) {
        return status;
    }

    *test_name_p = strdup(argv[0]);
    return UCS_OK;
}


static ucs_status_t run_test_recurs(struct perftest_context *ctx,
                                    const perftest_params_t *parent_params,
                                    unsigned depth)
{
    perftest_params_t params;
    ucx_perf_result_t result;
    ucs_status_t status;
    FILE *batch_file;
    int line_num;

    ucs_trace_func("depth=%u, num_files=%u", depth, ctx->num_batch_files);

    if (depth >= ctx->num_batch_files) {
        status = check_params(parent_params);
        if (status != UCS_OK) {
            return status;
        }

        /* !FINAL + batch + non-CSV: bracket each test's data rows
         * with "name row + after-name separator" before ucx_perf_run
         * and a matching "after-data separator" once it returns.
         * Together these form a symmetric box for each test.
         *
         * Boundary alignment with the unified opener:
         *   - The first test's leading divider is the unified table's
         *     auto bottom frame (emitted by perftest_results_table_open).
         *   - Subsequent tests' leading divider is the previous test's
         *     after-data separator.
         *   - The last test's after-data separator doubles as the
         *     table's closing line (perftest_results_table_close
         *     skips its closing separator in this mode).
         *
         * Reusing results_stream_rows[0] for the name row is safe
         * because both the name emission and the after-data sep run
         * single-threaded — the per-iteration callbacks fire only
         * inside ucx_perf_run, between them. */
        if ((ctx->flags & TEST_FLAG_PRINT_RESULTS) &&
            !(ctx->flags & TEST_FLAG_PRINT_CSV) &&
            !(ctx->flags & TEST_FLAG_PRINT_FINAL) &&
            (ctx->num_batch_files > 0)) {
            UCS_STRING_BUFFER_ONSTACK(name_buf, 256);
            ucs_table_row_t *name_row = ctx->results_stream_rows[0];

            ucs_string_buffer_append_array(&name_buf, "/", "%s",
                                           ctx->test_names,
                                           ctx->num_batch_files);
            ucs_table_stream_row_reset(name_row);
            ucs_table_row_add_cell_fmt(name_row,
                                       /*col_span=*/PERFTEST_RESULTS_N_COLS,
                                       UCS_TABLE_ALIGN_LEFT, "%s",
                                       ucs_string_buffer_cstr(&name_buf));
            ucs_table_print_row(name_row);
            ucs_table_print_separator(&ctx->results_table);
        }

        status = ucx_perf_run(&parent_params->super, &result);

        if ((ctx->flags & TEST_FLAG_PRINT_RESULTS) &&
            !(ctx->flags & TEST_FLAG_PRINT_CSV) &&
            !(ctx->flags & TEST_FLAG_PRINT_FINAL) &&
            (ctx->num_batch_files > 0)) {
            ucs_table_print_separator(&ctx->results_table);
        }

        return status;
    }

    batch_file = fopen(ctx->batch_files[depth], "r");
    if (batch_file == NULL) {
        ucs_error("Failed to open batch file '%s': %m",
                  ctx->batch_files[depth]);
        return UCS_ERR_IO_ERROR;
    }

    line_num = 0;
    do {
        status = clone_params(&params, parent_params);
        if (status != UCS_OK) {
            goto out;
        }

        status = read_batch_file(batch_file, ctx->batch_files[depth], &line_num,
                                 &params, &ctx->test_names[depth]);
        if (status == UCS_OK) {
            run_test_recurs(ctx, &params, depth + 1);
            free(ctx->test_names[depth]);
            ctx->test_names[depth] = NULL;
        }

        free(params.super.msg_size_list);
        params.super.msg_size_list = NULL;
    } while (status == UCS_OK);

    if (status == UCS_ERR_NO_ELEM) {
        status = UCS_OK;
    }

out:
    fclose(batch_file);
    return status;
}


ucs_status_t run_test(struct perftest_context *ctx)
{
    const char *error_prefix;
    ucs_status_t status;

    ucs_trace_func("");

    setlocale(LC_ALL, "en_US");

    ctx->params.super.report_func = print_progress;
    ctx->params.super.report_arg  = ctx;

    /* no batch files, only command line params */
    if (ctx->num_batch_files == 0) {
        error_prefix = (ctx->flags & TEST_FLAG_PRINT_RESULTS) ?
                               "command line: " :
                               "";
        status       = adjust_test_params(&ctx->params, error_prefix);
        if (status != UCS_OK) {
            return status;
        }
    }

    /* Pre-walk batch files once to compute the maximum joined-test-name
     * width. In FINAL mode it's used as the col 0 minimum for the
     * results table so the prefix cell carrying the joined test name
     * never overflows. In !FINAL mode it's only consulted to keep the
     * outer-width calculation FINAL-aware; the !FINAL preamble row
     * auto-fits the default 131-char table. Cheap: just fgets + strtok,
     * no parameter parsing. */
    ctx->max_test_name_width = perftest_max_test_name_width(ctx);

    print_header(ctx);

    status = run_test_recurs(ctx, &ctx->params, 0);
    if (status != UCS_OK) {
        ucs_error("Failed to run test: %s", ucs_status_string(status));
    }

    /* Close the shared results table that print_header opened. The
     * close is unconditional because results_table_close itself is a
     * no-op when the table was never opened (e.g. CSV mode or server-
     * side meta-only runs). */
    perftest_results_table_close(ctx);

    return status;
}
