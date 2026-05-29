/**
* Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2024-2026. ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#ifndef UCX_PERFTEST_CONTEXT_H
#define UCX_PERFTEST_CONTEXT_H

#include "api/libperf.h"
#include "lib/libperf_int.h"
#include <ucs/debug/table.h>
#include <ucs/sys/string.h>


#define MAX_BATCH_FILES         32
#define MAX_CPUS                1024


enum {
    TEST_FLAG_PRINT_RESULTS    = UCS_BIT(0),
    TEST_FLAG_PRINT_TEST       = UCS_BIT(1),
    TEST_FLAG_SET_AFFINITY     = UCS_BIT(8),
    TEST_FLAG_NUMERIC_FMT      = UCS_BIT(9),
    TEST_FLAG_PRINT_FINAL      = UCS_BIT(10),
    TEST_FLAG_PRINT_CSV        = UCS_BIT(11),
    TEST_FLAG_PRINT_EXTRA_INFO = UCS_BIT(12)
};


typedef struct sock_rte_group {
    int                          sendfd;
    int                          recvfd;
    int                          is_server;
    int                          size;
    int                          peer;
} sock_rte_group_t;


typedef struct perftest_params {
    ucx_perf_params_t            super;
    int                          test_id;
} perftest_params_t;


struct perftest_context {
    perftest_params_t            params;
    const char                   *server_addr;
    uint16_t                     port;
    sa_family_t                  af;
    int                          mpi;
    unsigned                     num_cpus;
    unsigned                     cpus[MAX_CPUS];
    unsigned                     flags;

    unsigned                     num_batch_files;
    char                         *batch_files[MAX_BATCH_FILES];
    char                         *test_names[MAX_BATCH_FILES];
    const char                   *mad_port;

    sock_rte_group_t             sock_rte_group;

    /* Results table for the boxed (non-CSV) output. Streamed rows are
     * one per OMP thread because progress callbacks fire concurrently
     * from multiple threads in MT mode. results_stream_rows == NULL is
     * the "table closed" sentinel. */
    ucs_table_t                  results_table;
    ucs_table_stream_row_t       **results_stream_rows;
    unsigned                     n_stream_rows;

    /* Precomputed in run_test from a pre-walk of all batch files: the
     * maximum length of any joined test_name string. In FINAL mode
     * it's the results_table col 0 minimum so the prefix cell carrying
     * the joined test name never overflows. In !FINAL mode col 0 stays
     * at its default width and the col_span=9 preamble row auto-fits
     * the existing total width. 0 when there are no batch files. */
    size_t                       max_test_name_width;
};


static inline void
perftest_params_release_msg_size_list(perftest_params_t *params)
{
    free(params->super.msg_size_list);
    params->super.msg_size_list = NULL;
}


static inline ucs_status_t
perftest_params_merge(perftest_params_t *dst, const perftest_params_t *src)
{
    char uct_dev_name[UCT_DEVICE_NAME_MAX];
    unsigned needed_flags;

    /* backup required dst parameters */
    ucs_strncpy_safe(uct_dev_name, dst->super.uct.dev_name,
                     UCT_DEVICE_NAME_MAX);
    needed_flags = (dst->super.flags & UCX_PERF_TEST_FLAG_ERR_HANDLING);

    perftest_params_release_msg_size_list(dst);

    memcpy(dst, src, sizeof(*src));

    if (dst->super.msg_size_cnt != 0) {
        dst->super.msg_size_list = calloc(dst->super.msg_size_cnt,
                                          sizeof(*dst->super.msg_size_list));
        if (dst->super.msg_size_list == NULL) {
            return UCS_ERR_NO_MEMORY;
        }

        memcpy(dst->super.msg_size_list, src->super.msg_size_list,
               src->super.msg_size_cnt * sizeof(*src->super.msg_size_list));
    }

    /* restore required overwritten parameters */
    ucs_strncpy_safe(dst->super.uct.dev_name, uct_dev_name,
                     UCT_DEVICE_NAME_MAX);
    dst->super.flags |= needed_flags;
    return UCS_OK;
}


extern ucs_list_link_t rte_list;

#endif /* UCX_PERFTEST_CONTEXT_H */
