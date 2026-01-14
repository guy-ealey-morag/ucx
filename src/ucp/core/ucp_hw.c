/**
 * Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <ucp/api/ucp.h>
#include <ucs/debug/log.h>
#include <ucs/sys/sys.h>

#include <stdlib.h>


#define UCP_PCI_DEVICES_PATH      "/sys/bus/pci/devices"

#define UCP_PCI_VENDOR_MELLANOX   0x15b3
#define UCP_PCI_VENDOR_NVIDIA     0x10de
#define UCP_PCI_VENDOR_AMD        0x1002

#define UCP_PCI_CLASS_IB          0x0207
#define UCP_PCI_CLASS_GPU_DISPLAY 0x0300
#define UCP_PCI_CLASS_GPU_3D      0x0302


typedef struct {
    unsigned num_ib_devices;
    unsigned num_gpus;
} ucp_hardware_query_ctx_t;


static ucs_status_t
ucp_hardware_query_scan_cb(const struct dirent *entry, void *arg)
{
    ucp_hardware_query_ctx_t *ctx = arg;
    char sysfs_path[PATH_MAX];
    char vendor_str[16];
    char class_str[16];
    ucs_status_t status;
    unsigned long vendor_id;
    unsigned long class_id;

    /* Skip '.' and '..' entries */
    if (entry->d_name[0] == '.') {
        return UCS_OK;
    }

    snprintf(sysfs_path, sizeof(sysfs_path), "%s/%s", UCP_PCI_DEVICES_PATH,
             entry->d_name);

    /* Read vendor ID */
    status = ucs_sys_read_sysfs_file(entry->d_name, sysfs_path, "vendor",
                                     vendor_str, sizeof(vendor_str),
                                     UCS_LOG_LEVEL_TRACE);
    if (status != UCS_OK) {
        ucs_trace("failed to read vendor ID for device %s", entry->d_name);
        return UCS_OK; /* Skip this device, continue scanning */
    }

    vendor_id = strtoul(vendor_str, NULL, 0);

    /* Read class ID */
    status = ucs_sys_read_sysfs_file(entry->d_name, sysfs_path, "class",
                                     class_str, sizeof(class_str),
                                     UCS_LOG_LEVEL_TRACE);
    if (status != UCS_OK) {
        ucs_trace("failed to read class ID for device %s", entry->d_name);
        return UCS_OK; /* Skip this device, continue scanning */
    }

    class_id = strtoul(class_str, NULL, 0) >> 8; /* Class is upper 16 bits */

    /* Check for InfiniBand device */
    if ((vendor_id == UCP_PCI_VENDOR_MELLANOX) &&
        (class_id == UCP_PCI_CLASS_IB)) {
        ctx->num_ib_devices++;
        ucs_debug("found IB device #%d: %s vendor=0x%lx class=0x%lx",
                  ctx->num_ib_devices, entry->d_name, vendor_id, class_id);
    }

    /* Check for GPU */
    if (((vendor_id == UCP_PCI_VENDOR_NVIDIA) ||
         (vendor_id == UCP_PCI_VENDOR_AMD)) &&
        ((class_id == UCP_PCI_CLASS_GPU_DISPLAY) ||
         (class_id == UCP_PCI_CLASS_GPU_3D))) {
        ctx->num_gpus++;
        ucs_debug("found GPU #%d: %s vendor=0x%lx class=0x%lx",
                  ctx->num_gpus, entry->d_name, vendor_id, class_id);
    }

    return UCS_OK;
}


ucs_status_t
ucp_hardware_query(ucp_hardware_attrs_t *attr)
{
    ucp_hardware_query_ctx_t ctx = {0, 0};
    ucs_status_t status;

    if (attr->field_mask & UCP_HARDWARE_ATTR_FIELD_HARDWARE_SUPPORT) {
        attr->hardware_support = 0;
#if HAVE_IB
        attr->hardware_support |= UCP_HARDWARE_SUPPORT_IB;
#endif
#if HAVE_CUDA
        attr->hardware_support |= UCP_HARDWARE_SUPPORT_CUDA;
#endif
    }

    status = ucs_sys_readdir(UCP_PCI_DEVICES_PATH, ucp_hardware_query_scan_cb,
                             &ctx);
    if (status != UCS_OK) {
        ucs_debug("failed to scan PCI devices directory");
        return status;
    }

    if (attr->field_mask & UCP_HARDWARE_ATTR_FIELD_NUM_IB_DEVICES) {
        attr->num_ib_devices = ctx.num_ib_devices;
    }

    if (attr->field_mask & UCP_HARDWARE_ATTR_FIELD_NUM_GPUS) {
        attr->num_gpus = ctx.num_gpus;
    }

    return UCS_OK;
}
