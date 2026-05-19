/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2026. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "ucp_tl_info.h"

#include <ucs/datastruct/string_buffer.h>
#include <ucs/debug/log.h>
#include <ucs/debug/log_table.h>
#include <ucs/sys/math.h>
#include <ucs/sys/string.h>
#include <ucs/sys/topo/base/topo.h>
#include <string.h>


#define UCP_TL_INFO_DEVS_PER_LINE 3
/* Visual width of mark character + separator space */
#define UCP_TL_INFO_MARK_VISUAL   2
#define UCP_TL_INFO_MARK_ENABLED  "+"
#define UCP_TL_INFO_MARK_DISABLED "-"
#define UCP_TL_INFO_HDR_TYPE      "Type"
#define UCP_TL_INFO_HDR_TRANSPORT "Transport"
#define UCP_TL_INFO_HDR_DEVICE    "Device (System device)"
#define UCP_TL_INFO_HDR_COMPONENT "Component"
#define UCP_TL_INFO_UNAVAILABLE   "<unavailable>"

/* Column indices in the widths[] / cells[] arrays */
enum {
    UCP_TL_INFO_COL_TYPE,
    UCP_TL_INFO_COL_CMPT,
    UCP_TL_INFO_COL_TL,
    UCP_TL_INFO_COL_DEV,
    UCP_TL_INFO_NUM_COLS
};

static int ucp_tl_info_is_same_group(const ucp_tl_info_entry_t *entries,
                                     unsigned a, unsigned b)
{
    return (entries[a].cmpt_index == entries[b].cmpt_index) &&
           (strcmp(entries[a].rsc.tl_name, entries[b].rsc.tl_name) == 0);
}

static int
ucp_tl_info_is_group_leader(const ucp_tl_info_entry_t *entries, unsigned idx)
{
    unsigned j;

    for (j = 0; j < idx; ++j) {
        if (ucp_tl_info_is_same_group(entries, j, idx)) {
            return 0;
        }
    }
    return 1;
}

static int ucp_tl_info_cmpt_has_rscs(const ucp_tl_info_entry_t *all_rscs,
                                     unsigned num_all_rscs,
                                     ucp_rsc_index_t cmpt_idx)
{
    unsigned i;

    for (i = 0; i < num_all_rscs; ++i) {
        if (all_rscs[i].cmpt_index == cmpt_idx) {
            return 1;
        }
    }
    return 0;
}

static uct_device_type_t
ucp_tl_info_cmpt_dev_type(const ucp_tl_info_entry_t *all_rscs,
                          unsigned num_all_rscs, ucp_rsc_index_t cmpt_idx)
{
    unsigned i;

    for (i = 0; i < num_all_rscs; ++i) {
        if (all_rscs[i].cmpt_index == cmpt_idx) {
            return all_rscs[i].rsc.dev_type;
        }
    }
    return UCT_DEVICE_TYPE_LAST;
}

/*
 * Emit one data row and toggle the per-(type, cmpt, tl) "first" flags so that
 * subsequent rows in the same group leave those columns blank.
 */
static void ucp_tl_info_emit_row(ucs_string_buffer_t *strb, const int *widths,
                                 const char *type_str, const char *cmpt_str,
                                 const char *tl_str, const char *dev_str,
                                 int *first_type, int *first_cmpt,
                                 int *first_tl, int *printed_any)
{
    const char *cells[UCP_TL_INFO_NUM_COLS];

    cells[UCP_TL_INFO_COL_TYPE] = *first_type ? type_str : "";
    cells[UCP_TL_INFO_COL_CMPT] = *first_cmpt ? cmpt_str : "";
    cells[UCP_TL_INFO_COL_TL]   = *first_tl ? tl_str : "";
    cells[UCP_TL_INFO_COL_DEV]  = dev_str;

    ucs_log_table_append_row(strb, cells, widths, UCP_TL_INFO_NUM_COLS);
    *first_tl    = 0;
    *first_cmpt  = 0;
    *first_type  = 0;
    *printed_any = 1;
}

void ucp_context_log_tl_info(ucp_context_h context,
                             const ucp_tl_info_entry_t *all_rscs,
                             unsigned num_all_rscs)
{
    ucs_string_buffer_t strb = UCS_STRING_BUFFER_INITIALIZER;
    ucp_rsc_index_t cmpt_idx;
    uct_device_type_t dev_type, cmpt_dev_type;
    unsigned i, j;
    size_t type_width, tl_width, dev_width, cmpt_width, len, line_width;
    size_t dev_buf_len;
    int widths[UCP_TL_INFO_NUM_COLS];
    int printed_any, first_type, first_cmpt, first_tl, first_unavail;
    int dev_count, tl_enabled;
    char dev_buf[512];
    char title_buf[96];
    char tl_buf[UCT_TL_NAME_MAX + 8];

    // TODO: replace with env var check
    if (!ucs_log_is_enabled(UCS_LOG_LEVEL_INFO)) {
        return;
    }

    type_width = ucs_max(strlen(UCP_TL_INFO_HDR_TYPE),
                         strlen(UCP_TL_INFO_UNAVAILABLE));
    tl_width   = strlen(UCP_TL_INFO_HDR_TRANSPORT);
    dev_width  = strlen(UCP_TL_INFO_HDR_DEVICE);
    cmpt_width = strlen(UCP_TL_INFO_HDR_COMPONENT);

    for (dev_type = UCT_DEVICE_TYPE_NET; dev_type < UCT_DEVICE_TYPE_LAST;
         ++dev_type) {
        len = strlen(uct_device_type_names[dev_type]);
        if (len > type_width) {
            type_width = len;
        }
    }

    for (i = 0; i < num_all_rscs; ++i) {
        len = UCP_TL_INFO_MARK_VISUAL + strlen(all_rscs[i].rsc.tl_name);
        if (len > tl_width) {
            tl_width = len;
        }
    }

    for (cmpt_idx = 0; cmpt_idx < context->num_cmpts; ++cmpt_idx) {
        len = strlen(context->tl_cmpts[cmpt_idx].attr.name);
        if (len > cmpt_width) {
            cmpt_width = len;
        }
    }

    for (cmpt_idx = 0; cmpt_idx < context->num_cmpts; ++cmpt_idx) {
        for (i = 0; i < num_all_rscs; ++i) {
            if ((all_rscs[i].cmpt_index != cmpt_idx) ||
                !ucp_tl_info_is_group_leader(all_rscs, i)) {
                continue;
            }

            line_width = 0;
            dev_count  = 0;
            for (j = i; j < num_all_rscs; ++j) {
                if (!ucp_tl_info_is_same_group(all_rscs, j, i)) {
                    continue;
                }
                if ((dev_count > 0) &&
                    (dev_count % UCP_TL_INFO_DEVS_PER_LINE == 0)) {
                    if (line_width > dev_width) {
                        dev_width = line_width;
                    }
                    line_width = 0;
                }
                if (dev_count % UCP_TL_INFO_DEVS_PER_LINE > 0) {
                    line_width += 2;
                }
                line_width += UCP_TL_INFO_MARK_VISUAL +
                              strlen(all_rscs[j].rsc.dev_name);
                if (all_rscs[j].rsc.sys_device != UCS_SYS_DEVICE_ID_UNKNOWN) {
                    line_width += 2 +
                                  strlen(ucs_topo_sys_device_get_name(
                                          all_rscs[j].rsc.sys_device)) +
                                  1;
                }
                dev_count++;
            }
            if (line_width > dev_width) {
                dev_width = line_width;
            }
        }
    }

    widths[UCP_TL_INFO_COL_TYPE] = (int)type_width;
    widths[UCP_TL_INFO_COL_CMPT] = (int)cmpt_width;
    widths[UCP_TL_INFO_COL_TL]   = (int)tl_width;
    widths[UCP_TL_INFO_COL_DEV]  = (int)dev_width;

    if (!ucs_string_is_empty(context->name)) {
        snprintf(title_buf, sizeof(title_buf),
                 "Available Transports and Devices (ctx: %s)", context->name);
    } else {
        snprintf(title_buf, sizeof(title_buf),
                 "Available Transports and Devices");
    }

    ucs_log_table_append_title(&strb, title_buf, widths, UCP_TL_INFO_NUM_COLS);
    ucs_log_table_append_separator(&strb, widths, UCP_TL_INFO_NUM_COLS, 0,
                                   UCS_LOG_TABLE_LCORNER_SEP);
    {
        const char *hdr[UCP_TL_INFO_NUM_COLS];

        hdr[UCP_TL_INFO_COL_TYPE] = UCP_TL_INFO_HDR_TYPE;
        hdr[UCP_TL_INFO_COL_CMPT] = UCP_TL_INFO_HDR_COMPONENT;
        hdr[UCP_TL_INFO_COL_TL]   = UCP_TL_INFO_HDR_TRANSPORT;
        hdr[UCP_TL_INFO_COL_DEV]  = UCP_TL_INFO_HDR_DEVICE;
        ucs_log_table_append_row(&strb, hdr, widths, UCP_TL_INFO_NUM_COLS);
    }
    ucs_log_table_append_separator(&strb, widths, UCP_TL_INFO_NUM_COLS, 0,
                                   UCS_LOG_TABLE_LCORNER_SEP);

    printed_any = 0;
    for (dev_type = UCT_DEVICE_TYPE_NET; dev_type < UCT_DEVICE_TYPE_LAST;
         ++dev_type) {
        first_type = 1;
        for (cmpt_idx = 0; cmpt_idx < context->num_cmpts; ++cmpt_idx) {
            /* All resources from a single component are assumed to share the
             * same device type, so the first match determines the type */
            cmpt_dev_type = ucp_tl_info_cmpt_dev_type(all_rscs, num_all_rscs,
                                                      cmpt_idx);
            if (cmpt_dev_type != dev_type) {
                continue;
            }

            first_cmpt = 1;
            for (i = 0; i < num_all_rscs; ++i) {
                if ((all_rscs[i].cmpt_index != cmpt_idx) ||
                    !ucp_tl_info_is_group_leader(all_rscs, i)) {
                    continue;
                }

                if (first_cmpt && printed_any) {
                    if (first_type) {
                        ucs_log_table_append_separator(
                                &strb, widths, UCP_TL_INFO_NUM_COLS, 0,
                                UCS_LOG_TABLE_LCORNER_SEP);
                    } else {
                        ucs_log_table_append_separator(
                                &strb, widths, UCP_TL_INFO_NUM_COLS, 1,
                                UCS_LOG_TABLE_LCORNER_ROW);
                    }
                }

                tl_enabled = 0;
                for (j = i; j < num_all_rscs; ++j) {
                    if (ucp_tl_info_is_same_group(all_rscs, j, i) &&
                        all_rscs[j].enabled) {
                        tl_enabled = 1;
                        break;
                    }
                }

                snprintf(tl_buf, sizeof(tl_buf), "%s %s",
                         tl_enabled ? UCP_TL_INFO_MARK_ENABLED :
                                      UCP_TL_INFO_MARK_DISABLED,
                         all_rscs[i].rsc.tl_name);

                first_tl    = 1;
                dev_count   = 0;
                dev_buf[0]  = '\0';
                dev_buf_len = 0;
                for (j = i; j < num_all_rscs; ++j) {
                    if (!ucp_tl_info_is_same_group(all_rscs, j, i)) {
                        continue;
                    }

                    if ((dev_count > 0) &&
                        (dev_count % UCP_TL_INFO_DEVS_PER_LINE == 0)) {
                        ucp_tl_info_emit_row(
                                &strb, widths, uct_device_type_names[dev_type],
                                context->tl_cmpts[cmpt_idx].attr.name, tl_buf,
                                dev_buf, &first_type, &first_cmpt, &first_tl,
                                &printed_any);
                        dev_buf[0]  = '\0';
                        dev_buf_len = 0;
                    }

                    if (dev_count % UCP_TL_INFO_DEVS_PER_LINE > 0) {
                        dev_buf_len += snprintf(dev_buf + dev_buf_len,
                                                sizeof(dev_buf) - dev_buf_len,
                                                "  ");
                        if (dev_buf_len >= sizeof(dev_buf)) {
                            dev_buf_len = sizeof(dev_buf) - 1;
                        }
                    }
                    if (all_rscs[j].rsc.sys_device !=
                        UCS_SYS_DEVICE_ID_UNKNOWN) {
                        dev_buf_len += snprintf(
                                dev_buf + dev_buf_len,
                                sizeof(dev_buf) - dev_buf_len, "%s %s (%s)",
                                all_rscs[j].enabled ? UCP_TL_INFO_MARK_ENABLED :
                                                      UCP_TL_INFO_MARK_DISABLED,
                                all_rscs[j].rsc.dev_name,
                                ucs_topo_sys_device_get_name(
                                        all_rscs[j].rsc.sys_device));
                    } else {
                        dev_buf_len += snprintf(
                                dev_buf + dev_buf_len,
                                sizeof(dev_buf) - dev_buf_len, "%s %s",
                                all_rscs[j].enabled ? UCP_TL_INFO_MARK_ENABLED :
                                                      UCP_TL_INFO_MARK_DISABLED,
                                all_rscs[j].rsc.dev_name);
                    }
                    if (dev_buf_len >= sizeof(dev_buf)) {
                        dev_buf_len = sizeof(dev_buf) - 1;
                    }
                    dev_count++;
                }

                if (dev_buf[0] != '\0') {
                    ucp_tl_info_emit_row(&strb, widths,
                                         uct_device_type_names[dev_type],
                                         context->tl_cmpts[cmpt_idx].attr.name,
                                         tl_buf, dev_buf, &first_type,
                                         &first_cmpt, &first_tl, &printed_any);
                }
            }
        }
    }

    first_unavail = 1;
    for (cmpt_idx = 0; cmpt_idx < context->num_cmpts; ++cmpt_idx) {
        if (!ucp_tl_info_cmpt_has_rscs(all_rscs, num_all_rscs, cmpt_idx)) {
            const char *row[UCP_TL_INFO_NUM_COLS];

            row[UCP_TL_INFO_COL_TL]  = "";
            row[UCP_TL_INFO_COL_DEV] = "";

            if (first_unavail) {
                if (printed_any) {
                    ucs_log_table_append_separator(&strb, widths,
                                                   UCP_TL_INFO_NUM_COLS, 0,
                                                   UCS_LOG_TABLE_LCORNER_SEP);
                }
                row[UCP_TL_INFO_COL_TYPE] = UCP_TL_INFO_UNAVAILABLE;
                row[UCP_TL_INFO_COL_CMPT] =
                        context->tl_cmpts[cmpt_idx].attr.name;
                ucs_log_table_append_row(&strb, row, widths,
                                         UCP_TL_INFO_NUM_COLS);
                first_unavail = 0;
            } else {
                ucs_log_table_append_separator(&strb, widths,
                                               UCP_TL_INFO_NUM_COLS, 1,
                                               UCS_LOG_TABLE_LCORNER_SEP);
                row[UCP_TL_INFO_COL_TYPE] = "";
                row[UCP_TL_INFO_COL_CMPT] =
                        context->tl_cmpts[cmpt_idx].attr.name;
                ucs_log_table_append_row(&strb, row, widths,
                                         UCP_TL_INFO_NUM_COLS);
            }
            printed_any = 1;
        }
    }

    ucs_log_table_append_separator(&strb, widths, UCP_TL_INFO_NUM_COLS, 0,
                                   UCS_LOG_TABLE_LCORNER_SEP);
    ucs_log_print_compact_lines(&strb);
    ucs_string_buffer_cleanup(&strb);
}
