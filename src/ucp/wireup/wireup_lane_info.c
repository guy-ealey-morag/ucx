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
#include <ucs/sys/topo/base/topo.h>
#include <string.h>


#define UCP_EP_LANE_INFO_DASHES \
    "------------------------------------------------------------------------" \
    "------------------------------------------------------------------------"

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

static int ucp_ep_lane_is_same_tl(const ucp_ep_config_key_t *key,
                                  ucp_context_h context,
                                  ucp_rsc_index_t cm_index,
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

static void ucp_wireup_format_lane_dev(const ucp_ep_config_key_t *key,
                                       ucp_context_h context,
                                       ucp_lane_index_t lane,
                                       ucp_rsc_index_t cm_index,
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

static void ucp_wireup_format_lane_types(const ucp_ep_config_key_t *key,
                                         ucp_lane_index_t lane,
                                         ucp_lane_type_mask_t types_union,
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

void ucp_wireup_log_ep_lanes(ucp_worker_h worker,
                             const ucp_ep_config_key_t *key,
                             ucp_rsc_index_t cm_index,
                             ucp_ep_h ep)
{
    ucp_context_h context = worker->context;
    ucp_lane_index_t lane, j, k;
    ucp_rsc_index_t rsc_index;
    uct_tl_resource_desc_t *rsc;
    ucp_lane_type_mask_t types_union;
    size_t tl_width, dev_width, count_width, types_width;
    size_t len, total_width;
    const char *tl_name, *dev_name, *ep_type;
    char title_buf[96];
    char types_buf[128];
    char dev_buf[128];
    int count, already_seen, first_tl, printed_any, j_is_leader;

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

    tl_width    = strlen("Transport");
    dev_width   = strlen("Device (Sys. dev.)");
    count_width = strlen("# Lanes");
    types_width = strlen("Lane Types");

    /* Pass 1: compute column widths */
    for (lane = 0; lane < key->num_lanes; ++lane) {
        already_seen = 0;
        for (j = 0; j < lane; ++j) {
            if (ucp_ep_lane_is_same_dev(key, j, lane)) {
                already_seen = 1;
                break;
            }
        }
        if (already_seen) {
            continue;
        }

        if (lane == key->cm_lane) {
            tl_name  = "cm";
            dev_name = (cm_index != UCP_NULL_RESOURCE) ?
                       ucp_context_cm_name(context, cm_index) : "<unknown>";
        } else {
            rsc_index = key->lanes[lane].rsc_index;
            rsc       = &context->tl_rscs[rsc_index].tl_rsc;
            tl_name   = rsc->tl_name;
            dev_name  = rsc->dev_name;
        }

        len = strlen(tl_name);
        if (len > tl_width) {
            tl_width = len;
        }

        ucp_wireup_format_lane_dev(key, context, lane, cm_index,
                                   dev_name, dev_buf, sizeof(dev_buf));

        len = strlen(dev_buf);
        if (len > dev_width) {
            dev_width = len;
        }

        types_union = 0;
        for (j = lane; j < key->num_lanes; ++j) {
            if (ucp_ep_lane_is_same_dev(key, lane, j)) {
                types_union |= key->lanes[j].lane_types;
            }
        }

        ucp_wireup_format_lane_types(key, lane, types_union,
                                     types_buf, sizeof(types_buf));

        len = strlen(types_buf);
        if (len > types_width) {
            types_width = len;
        }
    }

    snprintf(title_buf, sizeof(title_buf), "Endpoint Lanes: ep %p (%s)",
             ep, ep_type);
    total_width = tl_width + dev_width + count_width + types_width + 9;

    ucs_info("+-%.*s-+", (int)total_width, UCP_EP_LANE_INFO_DASHES);
    ucs_info("| %-*s |", (int)total_width, title_buf);
    ucs_info("+-%.*s-+-%.*s-+-%.*s-+-%.*s-+",
             (int)tl_width, UCP_EP_LANE_INFO_DASHES,
             (int)dev_width, UCP_EP_LANE_INFO_DASHES,
             (int)count_width, UCP_EP_LANE_INFO_DASHES,
             (int)types_width, UCP_EP_LANE_INFO_DASHES);
    ucs_info("| %-*s | %-*s | %-*s | %-*s |",
             (int)tl_width, "Transport",
             (int)dev_width, "Device (Sys. dev.)",
             (int)count_width, "# Lanes",
             (int)types_width, "Lane Types");
    ucs_info("+-%.*s-+-%.*s-+-%.*s-+-%.*s-+",
             (int)tl_width, UCP_EP_LANE_INFO_DASHES,
             (int)dev_width, UCP_EP_LANE_INFO_DASHES,
             (int)count_width, UCP_EP_LANE_INFO_DASHES,
             (int)types_width, UCP_EP_LANE_INFO_DASHES);

    /* Pass 2: print rows grouped by transport */
    printed_any = 0;
    for (lane = 0; lane < key->num_lanes; ++lane) {
        already_seen = 0;
        for (j = 0; j < lane; ++j) {
            if (ucp_ep_lane_is_same_dev(key, j, lane)) {
                already_seen = 1;
                break;
            }
        }
        if (already_seen) {
            continue;
        }

        first_tl = 1;
        for (j = 0; j < lane; ++j) {
            j_is_leader = 1;
            for (k = 0; k < j; ++k) {
                if (ucp_ep_lane_is_same_dev(key, k, j)) {
                    j_is_leader = 0;
                    break;
                }
            }
            if (!j_is_leader) {
                continue;
            }
            if (ucp_ep_lane_is_same_tl(key, context, cm_index, j, lane)) {
                first_tl = 0;
                break;
            }
        }

        if (first_tl && printed_any) {
            ucs_info("+-%.*s-+-%.*s-+-%.*s-+-%.*s-+",
                     (int)tl_width, UCP_EP_LANE_INFO_DASHES,
                     (int)dev_width, UCP_EP_LANE_INFO_DASHES,
                     (int)count_width, UCP_EP_LANE_INFO_DASHES,
                     (int)types_width, UCP_EP_LANE_INFO_DASHES);
        }

        if (lane == key->cm_lane) {
            tl_name  = "cm";
            dev_name = (cm_index != UCP_NULL_RESOURCE) ?
                       ucp_context_cm_name(context, cm_index) : "<unknown>";
        } else {
            rsc_index = key->lanes[lane].rsc_index;
            rsc       = &context->tl_rscs[rsc_index].tl_rsc;
            tl_name   = rsc->tl_name;
            dev_name  = rsc->dev_name;
        }

        ucp_wireup_format_lane_dev(key, context, lane, cm_index,
                                   dev_name, dev_buf, sizeof(dev_buf));

        count       = 0;
        types_union = 0;
        for (j = 0; j < key->num_lanes; ++j) {
            if (ucp_ep_lane_is_same_dev(key, lane, j)) {
                count++;
                types_union |= key->lanes[j].lane_types;
            }
        }

        ucp_wireup_format_lane_types(key, lane, types_union,
                                     types_buf, sizeof(types_buf));

        ucs_info("| %-*s | %-*s | %*d | %-*s |",
                 (int)tl_width, first_tl ? tl_name : "",
                 (int)dev_width, dev_buf,
                 (int)count_width, count,
                 (int)types_width, types_buf);

        printed_any = 1;
    }

    ucs_info("+-%.*s-+-%.*s-+-%.*s-+-%.*s-+",
             (int)tl_width, UCP_EP_LANE_INFO_DASHES,
             (int)dev_width, UCP_EP_LANE_INFO_DASHES,
             (int)count_width, UCP_EP_LANE_INFO_DASHES,
             (int)types_width, UCP_EP_LANE_INFO_DASHES);
}
