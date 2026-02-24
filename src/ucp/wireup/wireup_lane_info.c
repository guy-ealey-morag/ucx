/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2026. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "wireup_lane_info.h"

#include <ucp/proto/lane_type.h>
#include <ucs/debug/log.h>
#include <ucs/datastruct/string_buffer.h>
#include <ucs/sys/topo/base/topo.h>
#include <ucs/sys/string.h>
#include <string.h>


#define UCP_EP_LANE_INFO_DASHES \
    "------------------------------------------------------------------------" \
    "------------------------------------------------------------------------"
#define UCP_EP_LANE_INFO_ROW_FMT   "| %-*s | %-*s | %-*s | %-*s |"
#define UCP_EP_LANE_INFO_HDR_TL    "Transport"
#define UCP_EP_LANE_INFO_HDR_DEV   "Device (Sys. dev.)"
#define UCP_EP_LANE_INFO_HDR_COUNT "# Lanes"
#define UCP_EP_LANE_INFO_HDR_TYPES "Lane Types"

static int ucp_ep_lane_is_same_dev(const ucp_ep_config_key_t *key,
                                   ucp_lane_index_t a, ucp_lane_index_t b)
{
    if ((a == key->cm_lane) && (b == key->cm_lane)) {
        return 1;
    }
    if ((a == key->cm_lane) || (b == key->cm_lane)) {
        return 0;
    }
    return key->lanes[a].rsc_index == key->lanes[b].rsc_index;
}

static int ucp_ep_lane_is_dev_leader(const ucp_ep_config_key_t *key,
                                     ucp_lane_index_t lane)
{
    ucp_lane_index_t j;

    for (j = 0; j < lane; ++j) {
        if (ucp_ep_lane_is_same_dev(key, j, lane)) {
            return 0;
        }
    }
    return 1;
}

static int ucp_ep_lane_is_same_tl(const ucp_ep_config_key_t *key,
                                  ucp_context_h context,
                                  ucp_lane_index_t a, ucp_lane_index_t b)
{
    if ((a == key->cm_lane) && (b == key->cm_lane)) {
        return 1;
    }
    if ((a == key->cm_lane) || (b == key->cm_lane)) {
        return 0;
    }
    return strcmp(
            context->tl_rscs[key->lanes[a].rsc_index].tl_rsc.tl_name,
            context->tl_rscs[key->lanes[b].rsc_index].tl_rsc.tl_name) == 0;
}

static void ucp_wireup_get_lane_names(const ucp_ep_config_key_t *key,
                                      ucp_context_h context,
                                      ucp_lane_index_t lane,
                                      const char **tl_name_p,
                                      const char **dev_name_p)
{
    if (lane == key->cm_lane) {
        *tl_name_p  = "cm";
        *dev_name_p = "cm";
    } else {
        *tl_name_p  = context->tl_rscs[key->lanes[lane].rsc_index].tl_rsc.tl_name;
        *dev_name_p = context->tl_rscs[key->lanes[lane].rsc_index].tl_rsc.dev_name;
    }
}

static void ucp_wireup_format_lane_dev(const ucp_ep_config_key_t *key,
                                       ucp_context_h context,
                                       ucp_lane_index_t lane,
                                       const char *dev_name,
                                       char *buf, size_t buf_size)
{
    const char *sysdev_name;

    if ((lane != key->cm_lane) &&
        (context->tl_rscs[key->lanes[lane].rsc_index].tl_rsc.sys_device !=
         UCS_SYS_DEVICE_ID_UNKNOWN)) {
        sysdev_name = ucs_topo_sys_device_get_name(
                context->tl_rscs[key->lanes[lane].rsc_index]
                        .tl_rsc.sys_device);
        if (sysdev_name != NULL) {
            snprintf(buf, buf_size, "%s (%s)", dev_name, sysdev_name);
        } else {
            snprintf(buf, buf_size, "%s", dev_name);
        }
    } else {
        snprintf(buf, buf_size, "%s", dev_name);
    }
}

static void ucp_wireup_format_lane_types(ucp_lane_type_mask_t types_union,
                                         char *buf, size_t buf_size)
{
    ucp_lane_type_t lt;
    size_t len = 0;

    buf[0] = '\0';
    for (lt = UCP_LANE_TYPE_FIRST; lt < UCP_LANE_TYPE_LAST; ++lt) {
        if (types_union & UCS_BIT(lt)) {
            if (len > 0) {
                len += snprintf(buf + len, buf_size - len, ", ");
                if (len >= buf_size) {
                    len = buf_size - 1;
                }
            }
            len += snprintf(buf + len, buf_size - len, "%s",
                            ucp_lane_type_info[lt].short_name);
            if (len >= buf_size) {
                len = buf_size - 1;
            }
        }
    }
}

static ucp_lane_type_mask_t
ucp_wireup_collect_lane_types(const ucp_ep_config_key_t *key,
                              ucp_lane_index_t leader, int *count_p)
{
    ucp_lane_type_mask_t types_union = 0;
    ucp_lane_index_t j;
    int count = 0;

    for (j = 0; j < key->num_lanes; ++j) {
        if (ucp_ep_lane_is_same_dev(key, leader, j)) {
            count++;
            types_union |= key->lanes[j].lane_types;
        }
    }

    if (count_p != NULL) {
        *count_p = count;
    }
    return types_union;
}

void ucp_wireup_log_ep_lanes(ucp_worker_h worker,
                             const ucp_ep_config_key_t *key,
                             ucp_worker_cfg_index_t cfg_index)
{
    ucp_context_h context = worker->context;
    ucp_lane_index_t lane, j;
    ucp_lane_type_mask_t types_union;
    size_t tl_width, dev_width, count_width, types_width;
    size_t len, total_width;
    const char *tl_name, *dev_name, *ep_type;
    char title_buf[96];
    char types_buf[128];
    char dev_buf[128];
    int count, first_tl, printed_any;
    ucs_string_buffer_t strb = UCS_STRING_BUFFER_INITIALIZER;
    char *line;

    if (!ucs_log_is_enabled(UCS_LOG_LEVEL_INFO)) {
        return;
    }

    if (key->flags & UCP_EP_CONFIG_KEY_FLAG_SELF) {
        ep_type = "self";
    } else if (key->flags & UCP_EP_CONFIG_KEY_FLAG_INTRA_NODE) {
        ep_type = "intra-node";
    } else {
        ep_type = "inter-node";
    }

    tl_width    = strlen(UCP_EP_LANE_INFO_HDR_TL);
    dev_width   = strlen(UCP_EP_LANE_INFO_HDR_DEV);
    count_width = strlen(UCP_EP_LANE_INFO_HDR_COUNT);
    types_width = strlen(UCP_EP_LANE_INFO_HDR_TYPES);

/*
 * Scoped macro: emits a 4-column separator line using the local
 * column-width variables. Defined here and #undef'd at end of function.
 */
#define UCP_LANE_INFO_LOG_SEP() \
    ucs_string_buffer_appendf(&strb, "+-%.*s-+-%.*s-+-%.*s-+-%.*s-+\n", \
                              (int)tl_width, UCP_EP_LANE_INFO_DASHES, \
                              (int)dev_width, UCP_EP_LANE_INFO_DASHES, \
                              (int)count_width, UCP_EP_LANE_INFO_DASHES, \
                              (int)types_width, UCP_EP_LANE_INFO_DASHES)

    /* Pass 1: compute column widths */
    for (lane = 0; lane < key->num_lanes; ++lane) {
        if (!ucp_ep_lane_is_dev_leader(key, lane)) {
            continue;
        }

        ucp_wireup_get_lane_names(key, context, lane,
                                  &tl_name, &dev_name);

        len = strlen(tl_name);
        if (len > tl_width) {
            tl_width = len;
        }

        ucp_wireup_format_lane_dev(key, context, lane,
                                   dev_name, dev_buf, sizeof(dev_buf));

        len = strlen(dev_buf);
        if (len > dev_width) {
            dev_width = len;
        }

        types_union = ucp_wireup_collect_lane_types(key, lane, NULL);
        ucp_wireup_format_lane_types(types_union, types_buf, sizeof(types_buf));

        len = strlen(types_buf);
        if (len > types_width) {
            types_width = len;
        }
    }

    if (!ucs_string_is_empty(context->name)) {
        snprintf(title_buf, sizeof(title_buf),
                 "Endpoint Config #%d (ctx: %s, type: %s)",
                 cfg_index, context->name, ep_type);
    } else {
        snprintf(title_buf, sizeof(title_buf),
                 "Endpoint Config #%d (type: %s)", cfg_index, ep_type);
    }
    total_width = tl_width + dev_width + count_width + types_width + 9;

    ucs_string_buffer_appendf(&strb, "+-%.*s-+\n",
                              (int)total_width, UCP_EP_LANE_INFO_DASHES);
    ucs_string_buffer_appendf(&strb, "| %-*s |\n",
                              (int)total_width, title_buf);
    UCP_LANE_INFO_LOG_SEP();
    ucs_string_buffer_appendf(&strb, UCP_EP_LANE_INFO_ROW_FMT "\n",
                              (int)tl_width, UCP_EP_LANE_INFO_HDR_TL,
                              (int)dev_width, UCP_EP_LANE_INFO_HDR_DEV,
                              (int)count_width, UCP_EP_LANE_INFO_HDR_COUNT,
                              (int)types_width, UCP_EP_LANE_INFO_HDR_TYPES);
    UCP_LANE_INFO_LOG_SEP();

    /* Pass 2: print rows grouped by transport */
    printed_any = 0;
    for (lane = 0; lane < key->num_lanes; ++lane) {
        if (!ucp_ep_lane_is_dev_leader(key, lane)) {
            continue;
        }

        first_tl = 1;
        for (j = 0; j < lane; ++j) {
            if (!ucp_ep_lane_is_dev_leader(key, j)) {
                continue;
            }
            if (ucp_ep_lane_is_same_tl(key, context, j, lane)) {
                first_tl = 0;
                break;
            }
        }

        if (first_tl && printed_any) {
            UCP_LANE_INFO_LOG_SEP();
        }

        ucp_wireup_get_lane_names(key, context, lane,
                                  &tl_name, &dev_name);
        ucp_wireup_format_lane_dev(key, context, lane,
                                   dev_name, dev_buf, sizeof(dev_buf));

        types_union = ucp_wireup_collect_lane_types(key, lane, &count);
        ucp_wireup_format_lane_types(types_union, types_buf, sizeof(types_buf));

        ucs_string_buffer_appendf(&strb, "| %-*s | %-*s | %*d | %-*s |\n",
                                  (int)tl_width, first_tl ? tl_name : "",
                                  (int)dev_width, dev_buf,
                                  (int)count_width, count,
                                  (int)types_width, types_buf);

        printed_any = 1;
    }

    UCP_LANE_INFO_LOG_SEP();

    ucs_string_buffer_for_each_token(line, &strb, "\n") {
        ucs_log_print_compact(line);
    }
    ucs_string_buffer_cleanup(&strb);
#undef UCP_LANE_INFO_LOG_SEP
}
