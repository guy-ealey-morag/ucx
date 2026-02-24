/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2026. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "ucp_tl_info.h"

#include <ucs/debug/log.h>
#include <ucs/datastruct/string_buffer.h>
#include <ucs/sys/topo/base/topo.h>
#include <ucs/sys/math.h>
#include <string.h>


#define UCP_TL_INFO_DASHES \
    "------------------------------------------------------------------------" \
    "------------------------------------------------------------------------"
#define UCP_TL_INFO_DEVS_PER_LINE  3
/* Visual width of mark character + separator space */
#define UCP_TL_INFO_MARK_VISUAL    2
#define UCP_TL_INFO_MARK_ENABLED   "+"
#define UCP_TL_INFO_MARK_DISABLED  "-"
#define UCP_TL_INFO_ROW_FMT        "| %-*s | %-*s | %-*s | %-*s |"

static int ucp_tl_info_is_same_group(const ucp_tl_info_entry_t *entries,
                                     unsigned a, unsigned b)
{
    return (entries[a].cmpt_index == entries[b].cmpt_index) &&
           (strcmp(entries[a].rsc.tl_name, entries[b].rsc.tl_name) == 0);
}

static int ucp_tl_info_is_group_leader(const ucp_tl_info_entry_t *entries,
                                       unsigned idx)
{
    unsigned j;

    for (j = 0; j < idx; ++j) {
        if (ucp_tl_info_is_same_group(entries, j, idx)) {
            return 0;
        }
    }
    return 1;
}

void ucp_context_log_tl_info(ucp_context_h context,
                             const ucp_tl_info_entry_t *all_rscs,
                             unsigned num_all_rscs)
{
    ucp_rsc_index_t cmpt_idx;
    uct_device_type_t dev_type, cmpt_dev_type;
    unsigned i, j;
    size_t type_width, tl_width, dev_width, cmpt_width, len, line_width;
    int printed_any, first_type, first_cmpt, first_tl;
    int dev_count;
    int has_rscs, tl_enabled;
    char dev_buf[512];
    char tl_buf[UCT_TL_NAME_MAX + 8];
    size_t dev_buf_len;
    ucs_string_buffer_t strb = UCS_STRING_BUFFER_INITIALIZER;
    char *line;

    if (!ucs_log_is_enabled(UCS_LOG_LEVEL_INFO)) {
        return;
    }

    type_width = ucs_max(strlen("Type"), strlen("<unavailable>"));
    tl_width   = strlen("Transport");
    dev_width  = strlen("Device (System device)");
    cmpt_width = strlen("Component");

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

/*
 * Scoped macro: emits a 4-column separator line using the local
 * column-width variables. Defined here and #undef'd at end of function.
 */
#define UCP_TL_INFO_LOG_SEP() \
    ucs_string_buffer_appendf(&strb, "+-%.*s-+-%.*s-+-%.*s-+-%.*s-+\n", \
                              (int)type_width, UCP_TL_INFO_DASHES, \
                              (int)cmpt_width, UCP_TL_INFO_DASHES, \
                              (int)tl_width, UCP_TL_INFO_DASHES, \
                              (int)dev_width, UCP_TL_INFO_DASHES)

    ucs_string_buffer_appendf(&strb, "+-%.*s-+\n",
                              (int)(type_width + cmpt_width + tl_width +
                                    dev_width + 9),
                              UCP_TL_INFO_DASHES);
    ucs_string_buffer_appendf(&strb, "| %-*s |\n",
                              (int)(type_width + cmpt_width + tl_width +
                                    dev_width + 9),
                              "Available Transports and Devices");
    UCP_TL_INFO_LOG_SEP();
    ucs_string_buffer_appendf(&strb, UCP_TL_INFO_ROW_FMT "\n",
                              (int)type_width, "Type",
                              (int)cmpt_width, "Component",
                              (int)tl_width, "Transport",
                              (int)dev_width, "Device (System device)");
    UCP_TL_INFO_LOG_SEP();

    printed_any = 0;
    for (dev_type = UCT_DEVICE_TYPE_NET; dev_type < UCT_DEVICE_TYPE_LAST;
         ++dev_type) {
        first_type = 1;
        for (cmpt_idx = 0; cmpt_idx < context->num_cmpts; ++cmpt_idx) {
            /* All resources from a single component are assumed to share the
             * same device type, so the first match determines the type */
            cmpt_dev_type = UCT_DEVICE_TYPE_LAST;
            for (i = 0; i < num_all_rscs; ++i) {
                if (all_rscs[i].cmpt_index == cmpt_idx) {
                    cmpt_dev_type = all_rscs[i].rsc.dev_type;
                    break;
                }
            }
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
                        UCP_TL_INFO_LOG_SEP();
                    } else {
                        ucs_string_buffer_appendf(
                                &strb,
                                "| %-*s +-%.*s-+-%.*s-+-%.*s-+\n",
                                (int)type_width, "",
                                (int)cmpt_width, UCP_TL_INFO_DASHES,
                                (int)tl_width, UCP_TL_INFO_DASHES,
                                (int)dev_width, UCP_TL_INFO_DASHES);
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
                        ucs_string_buffer_appendf(
                                &strb, UCP_TL_INFO_ROW_FMT "\n",
                                (int)type_width,
                                first_type ?
                                        uct_device_type_names[dev_type] : "",
                                (int)cmpt_width,
                                first_cmpt ?
                                        context->tl_cmpts[cmpt_idx].attr.name :
                                        "",
                                (int)tl_width,
                                first_tl ? tl_buf : "",
                                (int)dev_width,
                                dev_buf);
                        first_tl    = 0;
                        first_cmpt  = 0;
                        first_type  = 0;
                        printed_any = 1;
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
                        dev_buf_len += snprintf(dev_buf + dev_buf_len,
                                                sizeof(dev_buf) - dev_buf_len,
                                                "%s %s",
                                                all_rscs[j].enabled ?
                                                        UCP_TL_INFO_MARK_ENABLED :
                                                        UCP_TL_INFO_MARK_DISABLED,
                                                all_rscs[j].rsc.dev_name);
                    }
                    if (dev_buf_len >= sizeof(dev_buf)) {
                        dev_buf_len = sizeof(dev_buf) - 1;
                    }
                    dev_count++;
                }

                if (dev_buf[0] != '\0') {
                    ucs_string_buffer_appendf(
                            &strb, UCP_TL_INFO_ROW_FMT "\n",
                            (int)type_width,
                            first_type ?
                                    uct_device_type_names[dev_type] : "",
                            (int)cmpt_width,
                            first_cmpt ?
                                    context->tl_cmpts[cmpt_idx].attr.name :
                                    "",
                            (int)tl_width,
                            first_tl ? tl_buf : "",
                            (int)dev_width,
                            dev_buf);
                    first_tl    = 0;
                    first_cmpt  = 0;
                    first_type  = 0;
                    printed_any = 1;
                }
            }
        }
    }

    for (cmpt_idx = 0; cmpt_idx < context->num_cmpts; ++cmpt_idx) {
        has_rscs = 0;
        for (i = 0; i < num_all_rscs; ++i) {
            if (all_rscs[i].cmpt_index == cmpt_idx) {
                has_rscs = 1;
                break;
            }
        }
        if (!has_rscs) {
            if (printed_any) {
                UCP_TL_INFO_LOG_SEP();
            }
            ucs_string_buffer_appendf(&strb, UCP_TL_INFO_ROW_FMT "\n",
                                      (int)type_width, "<unavailable>",
                                      (int)cmpt_width,
                                      context->tl_cmpts[cmpt_idx].attr.name,
                                      (int)tl_width, "",
                                      (int)dev_width, "");
            printed_any = 1;
        }
    }

    UCP_TL_INFO_LOG_SEP();

    ucs_string_buffer_for_each_token(line, &strb, "\n") {
        ucs_log_print_compact(line);
    }
    ucs_string_buffer_cleanup(&strb);
#undef UCP_TL_INFO_LOG_SEP
}
