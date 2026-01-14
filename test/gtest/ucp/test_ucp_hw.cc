/**
 * Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * See file LICENSE for terms.
 */

#include "ucp_test.h"

class test_ucp_hardware_query : public ucs::test {
};

UCS_TEST_F(test_ucp_hardware_query, ib_devices_and_gpus) {
    ucp_hardware_attrs_t hw_attrs;
    ucs_status_t status;

    hw_attrs.field_mask = UCP_HARDWARE_ATTR_FIELD_NUM_IB_DEVICES |
                          UCP_HARDWARE_ATTR_FIELD_NUM_GPUS;
    status              = ucp_hardware_query(&hw_attrs);
    ASSERT_EQ(UCS_OK, status);

    /* Verify that we found at least one device of each type */
    EXPECT_GT(hw_attrs.num_ib_devices, 0u);
    EXPECT_GT(hw_attrs.num_gpus, 0u);
}
