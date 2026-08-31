/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2026. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "topo_groups.h"

#include <ucs/algorithm/qsort_r.h>
#include <ucs/arch/cpu.h>
#include <ucs/debug/assert.h>
#include <ucs/debug/log.h>
#include <ucs/debug/memtrack_int.h>
#include <ucs/debug/table.h>
#include <ucs/sys/string.h>
#include <ucs/sys/sys.h>

#include <dirent.h>
#include <string.h>


#define UCS_TOPO_GROUPS_MELLANOX_VENDOR_ID 0x15b3
#define UCS_TOPO_GROUPS_CX9_DEVICE_ID      0x1025
#define UCS_TOPO_GROUPS_MLX5_VF_DEVICE_ID  0x101e
#define UCS_TOPO_GROUPS_FW_VER_MAX         64


UCS_ARRAY_DECLARE_TYPE(ucs_topo_groups_sys_dev_array_t, size_t,
                       ucs_sys_device_t);
UCS_ARRAY_DECLARE_TYPE(ucs_topo_groups_numa_node_array_t, size_t,
                       ucs_numa_node_t);


static int
ucs_topo_groups_sys_dev_cmp(const void *elem1, const void *elem2, void *arg)
{
    const ucs_topo_sys_device_info_t *devices = arg;
    ucs_sys_device_t sys_dev1 = *(const ucs_sys_device_t*)elem1;
    ucs_sys_device_t sys_dev2 = *(const ucs_sys_device_t*)elem2;
    ucs_bus_id_bit_rep_t bus_id1, bus_id2;
    uintptr_t user_value1, user_value2;

    bus_id1 = ucs_topo_get_bus_id_bit_repr(&devices[sys_dev1].bus_id);
    bus_id2 = ucs_topo_get_bus_id_bit_repr(&devices[sys_dev2].bus_id);

    if (bus_id1 != bus_id2) {
        return (bus_id1 > bus_id2) - (bus_id1 < bus_id2);
    }

    user_value1 = devices[sys_dev1].user_value;
    user_value2 = devices[sys_dev2].user_value;

    return (user_value1 > user_value2) - (user_value1 < user_value2);
}

static void
ucs_topo_groups_sys_dev_sort(ucs_topo_groups_sys_dev_array_t *sys_devs,
                             const ucs_topo_sys_device_info_t *devices)
{
    if (ucs_array_is_empty(sys_devs)) {
        return;
    }

    ucs_qsort_r(ucs_array_begin(sys_devs), ucs_array_length(sys_devs),
                sizeof(*ucs_array_begin(sys_devs)), ucs_topo_groups_sys_dev_cmp,
                (void*)devices);
}

/* Compact the array by removing unknown devices. */
static void
ucs_topo_groups_sys_dev_compact(ucs_topo_groups_sys_dev_array_t *sys_devs)
{
    size_t dst = 0;
    size_t src;

    for (src = 0; src < ucs_array_length(sys_devs); ++src) {
        if (ucs_array_elem(sys_devs, src) != UCS_SYS_DEVICE_ID_UNKNOWN) {
            ucs_array_elem(sys_devs, dst++) = ucs_array_elem(sys_devs, src);
        }
    }

    for (src = dst; src < ucs_array_length(sys_devs); ++src) {
        ucs_array_elem(sys_devs, src) = UCS_SYS_DEVICE_ID_UNKNOWN;
    }

    ucs_array_set_length(sys_devs, dst);
}

/* This filter is required because currently CUDA gpus may have duplicates in
 * the devices array due to duplicate insertion by NVML and the CUDA driver. */
static void
ucs_topo_groups_gpu_aliases_filter(ucs_topo_groups_sys_dev_array_t *gpus,
                                   const ucs_topo_sys_device_info_t *devices)
{
    ucs_bus_id_bit_rep_t bus_id1, bus_id2;
    ucs_sys_device_t sys_dev1, sys_dev2;
    size_t i;

    if (ucs_array_length(gpus) < 2) {
        return;
    }

    i = 0;
    while (i < ucs_array_length(gpus) - 1) {
        sys_dev1 = ucs_array_elem(gpus, i);
        sys_dev2 = ucs_array_elem(gpus, i + 1);

        bus_id1 = ucs_topo_get_bus_id_bit_repr(&devices[sys_dev1].bus_id);
        bus_id2 = ucs_topo_get_bus_id_bit_repr(&devices[sys_dev2].bus_id);

        if ((bus_id1 == bus_id2) &&
            (devices[sys_dev2].user_value == UCS_SYS_DEVICE_USER_VALUE_EMPTY)) {
            ucs_array_elem(gpus, i + 1) = UCS_SYS_DEVICE_ID_UNKNOWN;

            /* Promised by sorting. */
            ucs_assert(devices[sys_dev1].user_value !=
                       UCS_SYS_DEVICE_USER_VALUE_EMPTY);

            i += 2;
        } else {
            i++;
        }
    }

    ucs_topo_groups_sys_dev_compact(gpus);
}

static ucs_status_t
ucs_topo_groups_read_ib_fw_ver(const ucs_sys_bus_id_t *bus_id, char *fw_ver,
                               size_t max)
{
    char *sysfs_path;
    struct dirent *entry;
    ucs_status_t status;
    size_t path_len;
    DIR *dir;

    status = ucs_string_alloc_path_buffer(&sysfs_path, "sysfs_path");
    if (status != UCS_OK) {
        return status;
    }

    status = ucs_topo_bus_id_to_sysfs_path(bus_id, sysfs_path, PATH_MAX);
    if (status != UCS_OK) {
        goto out_free_sysfs_path;
    }

    path_len = strlen(sysfs_path);
    ucs_strncpy_safe(sysfs_path + path_len, "/infiniband", PATH_MAX - path_len);

    dir = opendir(sysfs_path);
    if (dir == NULL) {
        status = UCS_ERR_NO_ELEM;
        goto out_free_sysfs_path;
    }

    /* Find the device name directory (e.g. mlx5_0) */
    do {
        entry = readdir(dir);
    } while ((entry != NULL) && (entry->d_name[0] == '.'));

    if (entry == NULL) {
        status = UCS_ERR_NO_ELEM;
        goto out_close_dir;
    }

    if (ucs_read_file_str(fw_ver, max, 1, "%s/%s/fw_ver", sysfs_path,
                          entry->d_name) < 0) {
        status = UCS_ERR_IO_ERROR;
        goto out_close_dir;
    }

    ucs_strtrim(fw_ver);
    status = UCS_OK;

out_close_dir:
    closedir(dir);
out_free_sysfs_path:
    ucs_free(sysfs_path);
    return status;
}

static void
ucs_topo_groups_cx9_filter(const ucs_topo_sys_device_info_t *devices,
                           ucs_topo_groups_sys_dev_array_t *nics)
{
    char fw_ver[UCS_TOPO_GROUPS_FW_VER_MAX];
    const ucs_topo_sys_device_info_t *device;
    ucs_sys_device_t *sys_dev;
    const ucs_sys_pci_id_t *pci_id;
    ucs_status_t status;

    ucs_log_indent(1);

    ucs_array_for_each(sys_dev, nics) {
        device = &devices[*sys_dev];
        pci_id = &device->pci_id;

        ucs_log_indent(-1);

        ucs_trace("cx9_filter: processing network device " UCS_SYS_BUS_ID_FMT,
                  UCS_SYS_BUS_ID_ARG(&device->bus_id));

        ucs_log_indent(1);

        if (pci_id->vendor == UCS_TOPO_GROUPS_MELLANOX_VENDOR_ID) {
            if (pci_id->device == UCS_TOPO_GROUPS_CX9_DEVICE_ID) {
                ucs_trace("cx9 device found (device id)");
                continue;
            } else if (pci_id->device == UCS_TOPO_GROUPS_MLX5_VF_DEVICE_ID) {
                ucs_trace("mlx5 VF device found");
                status = ucs_topo_groups_read_ib_fw_ver(&device->bus_id, fw_ver,
                                                        sizeof(fw_ver));
                if (status == UCS_OK) {
                    if (strncmp(fw_ver, "82.", 3) == 0) {
                        ucs_trace("cx9 device found (firmware version)");
                        continue;
                    } else {
                        ucs_trace("firmware version mismatch: %s", fw_ver);
                    }
                } else {
                    ucs_trace("could not read firmware version (error: %s)",
                              ucs_status_string(status));
                }
            }
        }

        ucs_trace("ignoring network device " UCS_SYS_BUS_ID_FMT
                  " (pci id " UCS_SYS_PCI_ID_FMT ")",
                  UCS_SYS_BUS_ID_ARG(&device->bus_id),
                  UCS_SYS_PCI_ID_ARG(pci_id));
        *sys_dev = UCS_SYS_DEVICE_ID_UNKNOWN;
    }

    ucs_log_indent(-1);

    ucs_topo_groups_sys_dev_compact(nics);
}

static ucs_status_t
ucs_topo_groups_devices_collect(const ucs_topo_sys_device_info_t *devices,
                                unsigned num_devices,
                                ucs_topo_groups_sys_dev_array_t *acc_devices,
                                ucs_topo_groups_sys_dev_array_t *net_devices)
{
    ucs_topo_groups_sys_dev_array_t *target_array;
    unsigned i;

    for (i = 0; i < num_devices; ++i) {
        if (devices[i].device_class == UCS_TOPO_DEVICE_CLASS_ACC) {
            target_array = acc_devices;
        } else if (devices[i].device_class == UCS_TOPO_DEVICE_CLASS_NET) {
            target_array = net_devices;
        } else {
            continue;
        }

        *ucs_array_append(target_array,
                          ucs_error("failed to append device to target_array");
                          return UCS_ERR_NO_MEMORY) = (ucs_sys_device_t)i;
    }

    return UCS_OK;
}

static int ucs_topo_groups_bus_id_same_slot(const ucs_sys_bus_id_t *bus_id1,
                                            const ucs_sys_bus_id_t *bus_id2)
{
    return (bus_id1->domain == bus_id2->domain) &&
           (bus_id1->bus == bus_id2->bus) && (bus_id1->slot == bus_id2->slot);
}

static int ucs_topo_groups_bus_id_equal(const ucs_sys_bus_id_t *bus_id1,
                                        const ucs_sys_bus_id_t *bus_id2)
{
    return ucs_topo_groups_bus_id_same_slot(bus_id1, bus_id2) &&
           (bus_id1->function == bus_id2->function);
}

static ucs_status_t
ucs_topo_groups_gpus_build(const ucs_topo_sys_device_info_t *devices,
                           const ucs_topo_groups_sys_dev_array_t *acc_devices,
                           ucs_topo_gpu_array_t *gpus)
{
    const ucs_sys_bus_id_t *gpu_bus_id = NULL;
    const ucs_sys_bus_id_t *dev_bus_id;
    const ucs_sys_device_t *sys_dev;
    ucs_topo_gpu_t *gpu;

    ucs_array_for_each(sys_dev, acc_devices) {
        dev_bus_id = &devices[*sys_dev].bus_id;

        /* Start a new GPU if the bus ids differ. */
        if ((gpu_bus_id == NULL) ||
            !ucs_topo_groups_bus_id_equal(gpu_bus_id, dev_bus_id)) {
            gpu = ucs_array_append(gpus,
                                   ucs_error("failed to append to gpus array");
                                   return UCS_ERR_NO_MEMORY);
            memset(gpu, 0, sizeof(*gpu));
            gpu_bus_id = dev_bus_id;
        }

        if (gpu->num_devices >= UCS_TOPO_MAX_DEVICES_PER_GPU) {
            ucs_error("too many accelerator devices (%zu) with bus "
                      "id " UCS_SYS_BUS_ID_FMT,
                      gpu->num_devices, UCS_SYS_BUS_ID_ARG(dev_bus_id));
            return UCS_ERR_EXCEEDS_LIMIT;
        }

        gpu->devices[gpu->num_devices++] = *sys_dev;
    }

    return UCS_OK;
}

static ucs_status_t
ucs_topo_groups_nics_build(const ucs_topo_sys_device_info_t *devices,
                           const ucs_topo_groups_sys_dev_array_t *net_devices,
                           ucs_topo_nic_array_t *nics)
{
    const ucs_sys_bus_id_t *nic_bus_id = NULL;
    const ucs_sys_bus_id_t *dev_bus_id;
    const ucs_sys_device_t *sys_dev;
    ucs_topo_nic_t *nic;

    ucs_array_for_each(sys_dev, net_devices) {
        dev_bus_id = &devices[*sys_dev].bus_id;

        /* Start a new NIC if the bus ids differ (excluding the function). */
        if ((nic_bus_id == NULL) ||
            !ucs_topo_groups_bus_id_same_slot(nic_bus_id, dev_bus_id)) {
            nic = ucs_array_append(nics,
                                   ucs_error("failed to append to nics array");
                                   return UCS_ERR_NO_MEMORY);
            memset(nic, 0, sizeof(*nic));
            nic_bus_id = dev_bus_id;
        }

        if (nic->num_ports >= UCS_TOPO_MAX_PORTS_PER_NIC) {
            ucs_error(
                    "too many network devices (%zu) in pci slot %04x:%02x:%02x",
                    nic->num_ports, (unsigned)dev_bus_id->domain,
                    (unsigned)dev_bus_id->bus, (unsigned)dev_bus_id->slot);
            return UCS_ERR_EXCEEDS_LIMIT;
        }

        nic->ports[nic->num_ports++] = *sys_dev;
    }

    return UCS_OK;
}

void ucs_topo_init_group(ucs_topo_group_t *group)
{
    ucs_array_init_dynamic(&group->gpus);
    ucs_array_init_dynamic(&group->nics);
}

static void ucs_topo_init_groups(ucs_topo_groups_t *groups)
{
    groups->type = UCS_TOPO_GROUPS_TYPE_UNKNOWN;
    ucs_array_init_dynamic(&groups->groups);
}

static ucs_status_t
ucs_topo_groups_inventory_build(const ucs_topo_sys_device_info_t *devices,
                                unsigned num_devices,
                                ucs_topo_groups_type_t type,
                                ucs_topo_group_t *inventory_p)
{
    ucs_topo_groups_sys_dev_array_t acc_devices = UCS_ARRAY_DYNAMIC_INITIALIZER;
    ucs_topo_groups_sys_dev_array_t net_devices = UCS_ARRAY_DYNAMIC_INITIALIZER;
    ucs_topo_group_t inventory;
    ucs_status_t status;

    ucs_topo_init_group(&inventory);

    status = ucs_topo_groups_devices_collect(devices, num_devices, &acc_devices,
                                             &net_devices);
    if (status != UCS_OK) {
        goto err_free_arrays;
    }

    ucs_topo_groups_sys_dev_sort(&acc_devices, devices);
    ucs_topo_groups_sys_dev_sort(&net_devices, devices);

    /* TODO: Remove this filter when NVML duplicates issue is fixed. */
    ucs_topo_groups_gpu_aliases_filter(&acc_devices, devices);

    if (type == UCS_TOPO_GROUPS_TYPE_VERA_RUBIN) {
        ucs_topo_groups_cx9_filter(devices, &net_devices);
    }

    status = ucs_topo_groups_gpus_build(devices, &acc_devices, &inventory.gpus);
    if (status != UCS_OK) {
        goto err_free_arrays;
    }

    status = ucs_topo_groups_nics_build(devices, &net_devices, &inventory.nics);
    if (status != UCS_OK) {
        goto err_free_arrays;
    }

    ucs_debug("built inventory with %zu physical gpus (%zu devices) and %zu "
              "physical nics (%zu devices)",
              (size_t)ucs_array_length(&inventory.gpus),
              (size_t)ucs_array_length(&acc_devices),
              (size_t)ucs_array_length(&inventory.nics),
              (size_t)ucs_array_length(&net_devices));

    ucs_array_cleanup_dynamic(&net_devices);
    ucs_array_cleanup_dynamic(&acc_devices);

    *inventory_p = inventory;
    return UCS_OK;

err_free_arrays:
    ucs_topo_release_group(&inventory);
    ucs_array_cleanup_dynamic(&net_devices);
    ucs_array_cleanup_dynamic(&acc_devices);
    return status;
}

static ucs_status_t
ucs_topo_groups_get_or_add_group(ucs_numa_node_t numa_node,
                                 ucs_topo_groups_numa_node_array_t *numa_nodes,
                                 ucs_topo_groups_t *groups,
                                 ucs_topo_group_t **group_p)
{
    ucs_topo_group_t *group;
    size_t i;

    /* Check if the group already exists for the given NUMA node. */
    for (i = 0; i < ucs_array_length(numa_nodes); ++i) {
        if (ucs_array_elem(numa_nodes, i) == numa_node) {
            *group_p = &ucs_array_elem(&groups->groups, i);
            return UCS_OK;
        }
    }

    *ucs_array_append(numa_nodes,
                      ucs_error("failed to append to numa nodes array");
                      return UCS_ERR_NO_MEMORY) = numa_node;

    group = ucs_array_append(&groups->groups,
                             ucs_error("failed to append to groups array");
                             return UCS_ERR_NO_MEMORY);
    ucs_topo_init_group(group);

    *group_p = group;
    return UCS_OK;
}

static ucs_status_t ucs_topo_groups_build_groups_by_numa_node(
        const ucs_topo_sys_device_info_t *devices,
        const ucs_topo_group_t *inventory, ucs_topo_groups_type_t groups_type,
        ucs_topo_groups_t *groups)
{
    ucs_topo_groups_numa_node_array_t numa_nodes = UCS_ARRAY_DYNAMIC_INITIALIZER;
    const ucs_topo_gpu_t *gpu;
    const ucs_topo_nic_t *nic;
    ucs_topo_group_t *group;
    ucs_numa_node_t numa_node;
    ucs_status_t status;

    groups->type = groups_type;

    ucs_array_for_each(gpu, &inventory->gpus) {
        numa_node = devices[gpu->devices[0]].numa_node;
        if (numa_node == UCS_NUMA_NODE_UNDEFINED) {
            ucs_error("system device %u has undefined numa node",
                      gpu->devices[0]);
            status = UCS_ERR_NO_ELEM;
            goto out_cleanup_numa_nodes;
        }

        status = ucs_topo_groups_get_or_add_group(numa_node, &numa_nodes,
                                                  groups, &group);
        if (status != UCS_OK) {
            goto out_cleanup_numa_nodes;
        }

        *ucs_array_append(&group->gpus, {
            ucs_error("failed to append to group gpus array");
            status = UCS_ERR_NO_MEMORY;
            goto out_cleanup_numa_nodes;
        }) = *gpu;
    }

    ucs_array_for_each(nic, &inventory->nics) {
        numa_node = devices[nic->ports[0]].numa_node;
        if (numa_node == UCS_NUMA_NODE_UNDEFINED) {
            ucs_error("system device %u has undefined numa node",
                      nic->ports[0]);
            status = UCS_ERR_NO_ELEM;
            goto out_cleanup_numa_nodes;
        }

        status = ucs_topo_groups_get_or_add_group(numa_node, &numa_nodes,
                                                  groups, &group);
        if (status != UCS_OK) {
            goto out_cleanup_numa_nodes;
        }

        *ucs_array_append(&group->nics, {
            ucs_error("failed to append to group nics array");
            status = UCS_ERR_NO_MEMORY;
            goto out_cleanup_numa_nodes;
        }) = *nic;
    }

    status = UCS_OK;

out_cleanup_numa_nodes:
    ucs_array_cleanup_dynamic(&numa_nodes);
    return status;
}

void ucs_topo_init_group(ucs_topo_group_t *group)
{
    ucs_array_init_dynamic(&group->gpus);
    ucs_array_init_dynamic(&group->nics);
}

static const char *ucs_topo_groups_type_str(ucs_topo_groups_type_t type)
{
    switch (type) {
    case UCS_TOPO_GROUPS_TYPE_UNKNOWN:
        return "unknown";
    case UCS_TOPO_GROUPS_TYPE_VERA_RUBIN:
        return "vera-rubin";
    default:
        return "<invalid>";
    }
}

static void
ucs_topo_groups_append_device_names(const ucs_topo_sys_device_info_t *devices,
                                    const ucs_sys_device_t *sys_devs,
                                    size_t num_devices,
                                    ucs_string_buffer_t *strb)
{
    size_t i;

    if (num_devices > 1) {
        ucs_string_buffer_appendf(strb, "[");
    }

    for (i = 0; i < num_devices; ++i) {
        ucs_string_buffer_appendf(strb, "%s%s", (i == 0) ? "" : ";",
                                  devices[sys_devs[i]].name);
    }

    if (num_devices > 1) {
        ucs_string_buffer_appendf(strb, "]");
    }
}

static void
ucs_topo_groups_format_group(const ucs_topo_sys_device_info_t *devices,
                             const ucs_topo_group_t *group,
                             ucs_string_buffer_t *gpus_strb,
                             ucs_string_buffer_t *nics_strb)
{
    const ucs_topo_gpu_t *gpu;
    const ucs_topo_nic_t *nic;
    size_t i;

    i = 0;
    ucs_array_for_each(gpu, &group->gpus) {
        if (i++ > 0) {
            ucs_string_buffer_appendf(gpus_strb, " ");
        }

        ucs_topo_groups_append_device_names(devices, gpu->devices,
                                            gpu->num_devices, gpus_strb);
    }

    i = 0;
    ucs_array_for_each(nic, &group->nics) {
        if (i++ > 0) {
            ucs_string_buffer_appendf(nics_strb, " ");
        }

        ucs_topo_groups_append_device_names(devices, nic->ports, nic->num_ports,
                                            nics_strb);
    }
}

static void ucs_topo_groups_log(const ucs_topo_sys_device_info_t *devices,
                                const ucs_topo_groups_t *groups)
{
    const ucs_table_config_t table_config = {
        .n_cols = 3
    };
    ucs_string_buffer_t gpus_strb         = UCS_STRING_BUFFER_INITIALIZER;
    ucs_string_buffer_t nics_strb         = UCS_STRING_BUFFER_INITIALIZER;
    ucs_string_buffer_t table_strb        = UCS_STRING_BUFFER_INITIALIZER;
    const ucs_topo_group_t *group;
    ucs_table_row_h row;
    ucs_status_t status;
    ucs_table_t table;
    size_t group_idx;

    ucs_table_init(&table, &table_config);

    ucs_table_add_row(&table, &row);
    ucs_table_row_add_cell_fmt(&table, row, table_config.n_cols,
                               UCS_TABLE_ALIGN_LEFT, "Topology groups type: %s",
                               ucs_topo_groups_type_str(groups->type));
    ucs_table_add_separator(&table);

    ucs_table_add_row(&table, &row);
    ucs_table_row_add_cell_fmt(&table, row, 1, UCS_TABLE_ALIGN_LEFT, "Group #");
    ucs_table_row_add_cell_fmt(&table, row, 1, UCS_TABLE_ALIGN_LEFT, "GPUs");
    ucs_table_row_add_cell_fmt(&table, row, 1, UCS_TABLE_ALIGN_LEFT, "NICs");
    ucs_table_add_separator(&table);

    group_idx = 0;
    ucs_array_for_each(group, &groups->groups) {
        ucs_string_buffer_reset(&gpus_strb);
        ucs_string_buffer_reset(&nics_strb);
        ucs_topo_groups_format_group(devices, group, &gpus_strb, &nics_strb);

        ucs_table_add_row(&table, &row);
        ucs_table_row_add_cell_fmt(&table, row, 1, UCS_TABLE_ALIGN_RIGHT, "%zu",
                                   group_idx++);
        ucs_table_row_add_cell_fmt(&table, row, 1, UCS_TABLE_ALIGN_LEFT, "%s",
                                   ucs_string_buffer_cstr(&gpus_strb));
        ucs_table_row_add_cell_fmt(&table, row, 1, UCS_TABLE_ALIGN_LEFT, "%s",
                                   ucs_string_buffer_cstr(&nics_strb));
    }

    ucs_table_render(&table, &table_strb);
    status = ucs_table_get_status(&table);
    if (status != UCS_OK) {
        ucs_warn("topology groups table render incomplete: %s",
                 ucs_status_string(status));
    }

    ucs_log_print_compact(ucs_string_buffer_cstr(&table_strb));

    ucs_table_cleanup(&table);
    ucs_string_buffer_cleanup(&table_strb);
    ucs_string_buffer_cleanup(&nics_strb);
    ucs_string_buffer_cleanup(&gpus_strb);
}

ucs_status_t
ucs_topo_build_groups_inner(const ucs_topo_sys_device_info_t *devices,
                            unsigned num_devices, ucs_topo_groups_t *groups_p)
{
    ucs_cpu_model_t cpu_model = ucs_arch_get_cpu_model();
    ucs_topo_groups_type_t groups_type;
    ucs_topo_group_t inventory;
    ucs_topo_groups_t groups;
    ucs_status_t status;

    ucs_topo_init_groups(&groups);

    if (cpu_model != UCS_CPU_MODEL_NVIDIA_VERA) {
        /* Currently only Vera Rubin is supported. */
        goto out;
    }

    groups_type = UCS_TOPO_GROUPS_TYPE_VERA_RUBIN;

    status = ucs_topo_groups_inventory_build(devices, num_devices, groups_type,
                                             &inventory);
    if (status != UCS_OK) {
        return status;
    }

    status = ucs_topo_groups_build_groups_by_numa_node(devices, &inventory,
                                                       groups_type, &groups);
    if (status != UCS_OK) {
        goto err_cleanup_groups;
    }

    if (ucs_log_is_enabled(UCS_LOG_LEVEL_DEBUG)) {
        ucs_topo_groups_log(devices, &groups);
    }

    /* TODO: Validate Vera-Rubin groups: 2 GPUs and 4 NICs per group. */

    ucs_topo_release_group(&inventory);

out:
    ucs_debug("initialized topo groups of type %s with %zu groups",
              ucs_topo_groups_type_str(groups.type),
              ucs_array_length(&groups.groups));

    *groups_p = groups;
    return UCS_OK;

err_cleanup_groups:
    ucs_topo_release_groups(&groups);
    ucs_topo_release_group(&inventory);
    return status;
}
