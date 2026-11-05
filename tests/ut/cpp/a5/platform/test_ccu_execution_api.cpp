#include <gtest/gtest.h>

#include <array>
#include <cstring>

#include "host/ccu_execution.h"

namespace {
struct Owner { int refs{}; };
int retain(void *p) { ++static_cast<Owner *>(p)->refs; return 0; }
int release(void *p) { --static_cast<Owner *>(p)->refs; return 0; }
int exchange(void *, uint64_t, uint64_t, uint32_t, const void *local, uint32_t local_size,
             void *ranked, uint32_t stride, uint32_t ranks, uint32_t) {
    if (local_size > stride || ranks != 2 || (local_size != 0 && (local == nullptr || ranked == nullptr))) return -1;
    if (local_size == 0) return 0;
    for (uint32_t i = 0; i < ranks; ++i)
        if (local_size != 0) std::memcpy(static_cast<uint8_t *>(ranked) + i * stride, local, local_size);
    return 0;
}
int abort_exchange(void *, uint64_t, uint64_t, uint32_t, int32_t) { return 0; }

struct Fixture {
    Owner bootstrap_owner;
    Owner input_owner;
    Owner output_owner;
    CcuOwnerOpsV1 owner_ops{CCU_EXECUTION_ABI_V1, sizeof(CcuOwnerOpsV1), retain, release};
    CcuBootstrapOpsV1 bootstrap_ops{CCU_EXECUTION_ABI_V1, sizeof(CcuBootstrapOpsV1), exchange, abort_exchange};
    std::array<uint8_t, 8> root_info{};
    CcuContextId context{};
    CcuWindowId input{};
    CcuWindowId output{};
    CcuHostResultV1 result{};

    void Create() {
        CcuCreateOptionsV1 options{};
        options.abi_version = CCU_EXECUTION_ABI_V1;
        options.struct_size = sizeof(options);
        options.device_id = 0;
        options.rank_count = 2;
        options.root_info_size = root_info.size();
        options.group_id = 19;
        options.root_info = root_info.data();
        options.bootstrap_ops = &bootstrap_ops;
        options.bootstrap_owner = &bootstrap_owner;
        options.bootstrap_owner_ops = &owner_ops;
        ASSERT_EQ(ccu_context_create(&options, &context, &result), CCU_STATUS_SUCCESS);
        EXPECT_EQ(bootstrap_owner.refs, 1);
    }
    void Register(CcuWindowId *id, Owner *owner, uint64_t allocation, uint64_t base) {
        CcuWindowImportV1 input{};
        input.abi_version = CCU_EXECUTION_ABI_V1;
        input.struct_size = sizeof(input);
        input.device_id = 0;
        input.allocation_id = allocation;
        input.allocation_generation = 1;
        input.allocation_base_va = base;
        input.allocation_size = 4096;
        input.window_size = 4096;
        input.allocation_owner = owner;
        input.allocation_owner_ops = &owner_ops;
        ASSERT_EQ(ccu_window_register(context, &input, id, &result), CCU_STATUS_SUCCESS);
        EXPECT_EQ(owner->refs, 1);
    }
};
}

TEST(CcuExecutionApi, RetainsWindowOwnersAcrossBindingAndPreparesValidatedRequest) {
    Fixture f;
    f.Create();
    f.Register(&f.input, &f.input_owner, 11, 0x10000);
    f.Register(&f.output, &f.output_owner, 12, 0x20000);
    CcuWindowBindingV1 binding{};
    binding.abi_version = CCU_EXECUTION_ABI_V1;
    binding.struct_size = sizeof(binding);
    binding.input_window = f.input;
    binding.output_window = f.output;
    binding.binding_generation = 1;
    ASSERT_EQ(ccu_bind_windows(f.context, &binding, &f.result), CCU_STATUS_SUCCESS);

    CcuRequestSpecV1 spec{};
    spec.abi_version = CCU_EXECUTION_ABI_V1;
    spec.struct_size = sizeof(spec);
    spec.operation = CCU_OPERATION_BROADCAST;
    spec.dtype = CCU_DATA_TYPE_FLOAT32;
    spec.collective_id = 1;
    spec.element_count = 64;
    CcuRequestId request{};
    ASSERT_EQ(ccu_prepare(f.context, &spec, &request, &f.result), CCU_STATUS_SUCCESS);
    EXPECT_NE(request, 0U);
    EXPECT_EQ(f.result.expected_sequence, 1U);
    EXPECT_EQ(ccu_context_close(f.context, 1, &f.result), CCU_STATUS_RETAINED);
    EXPECT_EQ(f.input_owner.refs, 1);
    EXPECT_EQ(f.output_owner.refs, 1);
    ASSERT_EQ(ccu_cancel_prepared(request, &f.result), CCU_STATUS_SUCCESS);
    ASSERT_EQ(ccu_context_close(f.context, 1, &f.result), CCU_STATUS_SUCCESS);
    EXPECT_EQ(f.input_owner.refs, 0);
    EXPECT_EQ(f.output_owner.refs, 0);
    EXPECT_EQ(f.bootstrap_owner.refs, 0);
}

TEST(CcuExecutionApi, RejectsInvalidOwnerAndWindowBoundsWithoutPublishingHandles) {
    Fixture f;
    f.Create();
    CcuWindowImportV1 input{};
    input.abi_version = CCU_EXECUTION_ABI_V1;
    input.struct_size = sizeof(input);
    input.device_id = 0;
    input.allocation_id = 1;
    input.allocation_generation = 1;
    input.allocation_base_va = 0x10000;
    input.allocation_size = 32;
    input.window_offset = 16;
    input.window_size = 32;
    input.allocation_owner = &f.input_owner;
    input.allocation_owner_ops = &f.owner_ops;
    CcuWindowId window = 123;
    EXPECT_EQ(ccu_window_register(f.context, &input, &window, &f.result), CCU_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(window, 0U);
    EXPECT_EQ(f.input_owner.refs, 0);
    EXPECT_EQ(ccu_context_close(f.context, 1, &f.result), CCU_STATUS_SUCCESS);
    EXPECT_EQ(f.bootstrap_owner.refs, 0);
}

TEST(CcuExecutionApi, ArmRefusesToClaimSubmissionWithoutAnSdkAdapter) {
    Fixture f;
    f.Create();
    f.Register(&f.input, &f.input_owner, 11, 0x10000);
    f.Register(&f.output, &f.output_owner, 12, 0x20000);
    CcuWindowBindingV1 binding{};
    binding.abi_version = CCU_EXECUTION_ABI_V1;
    binding.struct_size = sizeof(binding);
    binding.input_window = f.input;
    binding.output_window = f.output;
    binding.binding_generation = 1;
    ASSERT_EQ(ccu_bind_windows(f.context, &binding, &f.result), CCU_STATUS_SUCCESS);
    CcuRequestSpecV1 spec{};
    spec.abi_version = CCU_EXECUTION_ABI_V1;
    spec.struct_size = sizeof(spec);
    spec.operation = CCU_OPERATION_BROADCAST;
    spec.dtype = CCU_DATA_TYPE_FLOAT32;
    spec.collective_id = 1;
    spec.element_count = 1;
    CcuRequestId request{};
    ASSERT_EQ(ccu_prepare(f.context, &spec, &request, &f.result), CCU_STATUS_SUCCESS);
    CcuTriggerInfoV1 trigger{};
    EXPECT_EQ(ccu_arm(request, &trigger, &f.result), CCU_STATUS_UNSUPPORTED);
    EXPECT_EQ(trigger.gate_va, 0U);
    EXPECT_EQ(ccu_cancel_prepared(request, &f.result), CCU_STATUS_SUCCESS);
    EXPECT_EQ(ccu_context_close(f.context, 1, &f.result), CCU_STATUS_SUCCESS);
    EXPECT_EQ(f.input_owner.refs, 0);
    EXPECT_EQ(f.output_owner.refs, 0);
    EXPECT_EQ(f.bootstrap_owner.refs, 0);
}

