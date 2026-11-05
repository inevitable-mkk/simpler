#include <gtest/gtest.h>

#include "common/ccu_execution_types.h"

TEST(CcuExecutionAbi, RecordsHaveStableVersionsAndExpectedDeviceLayouts) {
    EXPECT_EQ(CCU_EXECUTION_ABI_V1, 1U);
    EXPECT_EQ(sizeof(CcuRequestDescriptorV1), 128U);
    EXPECT_EQ(alignof(CcuRequestDescriptorV1), 128U);
    EXPECT_EQ(offsetof(CcuRequestDescriptorV1, request_seq), 32U);
    EXPECT_EQ(sizeof(CcuTriggerInfoV1), 128U);
    EXPECT_EQ(offsetof(CcuTriggerInfoV1, gate_va), 32U);
    EXPECT_EQ(sizeof(CcuCompletionRecordV1), 256U);
    EXPECT_EQ(offsetof(CcuCompletionRecordV1, completed_seq), 128U);
}

