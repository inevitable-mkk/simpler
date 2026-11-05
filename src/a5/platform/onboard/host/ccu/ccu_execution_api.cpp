#include "host/ccu_execution.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>

#include "host/ccu_execution_registry.h"

namespace {
constexpr uint32_t kPhaseBind = 1;
constexpr uint32_t kPhasePrepare = 2;
constexpr uint32_t kPhaseCancel = 4;

void SetResult(CcuHostResultV1 *out, int status, CcuStageV1 stage, const char *message,
               uint64_t expected = 0, uint64_t observed = 0, bool retained = false) {
    if (out == nullptr) return;
    std::memset(out, 0, sizeof(*out));
    out->abi_version = CCU_EXECUTION_ABI_V1;
    out->struct_size = sizeof(*out);
    out->status = status;
    out->stage = stage;
    out->expected_sequence = expected;
    out->observed_sequence = observed;
    out->retained = retained ? 1U : 0U;
    if (message != nullptr) std::strncpy(out->message, message, sizeof(out->message) - 1);
}

bool ValidOwnerOps(const CcuOwnerOpsV1 *ops) {
    return ops != nullptr && ops->abi_version == CCU_EXECUTION_ABI_V1 && ops->struct_size == sizeof(*ops) &&
           ops->retain != nullptr && ops->release != nullptr;
}

int Fail(CcuHostResultV1 *result, int status, CcuStageV1 stage, const char *message) {
    SetResult(result, status, stage, message);
    return status;
}

int Unsupported(CcuHostResultV1 *result, CcuStageV1 stage, const char *detail) {
    SetResult(result, CCU_STATUS_UNSUPPORTED, stage, detail);
    return CCU_STATUS_UNSUPPORTED;
}

int Exchange(CcuContextEntryV1 &ctx, uint64_t collective, uint32_t phase, const void *local,
             uint32_t local_size, void *ordered, uint32_t stride, uint32_t timeout_ms) {
    if (ctx.bootstrap.exchange == nullptr) return CCU_STATUS_INVALID_ARGUMENT;
    return ctx.bootstrap.exchange(ctx.bootstrap_owner, ctx.options.group_id, collective, phase, local, local_size,
                                  ordered, stride, CCU_EXECUTION_RANK_COUNT_V1, timeout_ms);
}

std::shared_ptr<CcuWindowLeaseV1> FindWindow(CcuContextEntryV1 &ctx, CcuWindowId id) {
    const auto it = ctx.windows.find(id);
    return it == ctx.windows.end() ? nullptr : it->second;
}

int MapValidation(CcuRequestValidationError error) {
    if (error == CcuRequestValidationError::NONE) return CCU_STATUS_SUCCESS;
    if (error == CcuRequestValidationError::WINDOW_OUT_OF_RANGE ||
        error == CcuRequestValidationError::OVERLAPPING_WINDOWS) return CCU_STATUS_INVALID_ARGUMENT;
    return CCU_STATUS_PROTOCOL_ERROR;
}
}

extern "C" int ccu_query_capabilities(CcuCapabilitiesV1 *out) {
    if (out == nullptr || out->struct_size < sizeof(*out)) return CCU_STATUS_INVALID_ARGUMENT;
    const uint32_t capacity = out->struct_size;
    std::memset(out, 0, sizeof(*out));
    out->abi_version = CCU_EXECUTION_ABI_V1;
    out->struct_size = std::min<uint32_t>(capacity, sizeof(*out));
    out->max_rank_count = CCU_EXECUTION_RANK_COUNT_V1;
    out->max_element_count = CCU_EXECUTION_MAX_COUNT_V1;
    out->max_inflight = 1;
    // CCU capability is withheld until the SDK adapter and completion Store are linked.
    return CCU_STATUS_SUCCESS;
}

extern "C" int ccu_get_root_info(int32_t, void *, uint32_t, uint32_t *size_out) {
    if (size_out != nullptr) *size_out = 0;
    return CCU_STATUS_UNSUPPORTED;
}

extern "C" int ccu_context_create(const CcuCreateOptionsV1 *options, CcuContextId *context_out,
                                   CcuHostResultV1 *result) {
    if (context_out != nullptr) *context_out = 0;
    if (options == nullptr || context_out == nullptr || options->abi_version != CCU_EXECUTION_ABI_V1 ||
        options->struct_size != sizeof(*options) || options->device_id < 0 || options->rank_count != 2 ||
        options->rank >= options->rank_count || options->group_id == 0 || options->root_info == nullptr ||
        options->root_info_size == 0 || options->root_info_size > 65536 || options->bootstrap_ops == nullptr ||
        options->bootstrap_ops->abi_version != CCU_EXECUTION_ABI_V1 ||
        options->bootstrap_ops->struct_size != sizeof(CcuBootstrapOpsV1) ||
        options->bootstrap_ops->exchange == nullptr || !ValidOwnerOps(options->bootstrap_owner_ops) ||
        options->bootstrap_owner == nullptr) {
        return Fail(result, CCU_STATUS_INVALID_ARGUMENT, CCU_STAGE_CREATE, "Invalid context options or owner contract");
    }
    try {
        auto context = std::make_shared<CcuContextEntryV1>();
        context->id = CcuExecutionRegistryV1::Instance().NextId();
        if (context->id == 0) return Fail(result, CCU_STATUS_INTERNAL_ERROR, CCU_STAGE_CREATE, "Handle space exhausted");
        context->options = *options;
        context->bootstrap = *options->bootstrap_ops;
        context->bootstrap_owner = options->bootstrap_owner;
        context->root_info.resize(options->root_info_size);
        std::memcpy(context->root_info.data(), options->root_info, options->root_info_size);
        context->options.root_info = nullptr;
        context->options.bootstrap_ops = nullptr;
        context->options.bootstrap_owner_ops = nullptr;
        context->bootstrap_lease.owner = options->bootstrap_owner;
        context->bootstrap_lease.ops = *options->bootstrap_owner_ops;
        if (!context->bootstrap_lease.Retain())
            return Fail(result, CCU_STATUS_INVALID_ARGUMENT, CCU_STAGE_CREATE, "Bootstrap owner retain failed");
        if (!CcuExecutionRegistryV1::Instance().AddContext(context))
            return Fail(result, CCU_STATUS_INTERNAL_ERROR, CCU_STAGE_CREATE, "Could not register context handle");
        *context_out = context->id;
        SetResult(result, CCU_STATUS_SUCCESS, CCU_STAGE_CREATE, "Host context registered; SDK resources not initialized");
        return CCU_STATUS_SUCCESS;
    } catch (...) {
        return Fail(result, CCU_STATUS_INTERNAL_ERROR, CCU_STAGE_CREATE, "Context allocation failed");
    }
}

extern "C" int ccu_window_register(CcuContextId id, const CcuWindowImportV1 *input,
                                    CcuWindowId *window_out, CcuHostResultV1 *result) {
    if (window_out != nullptr) *window_out = 0;
    auto ctx = CcuExecutionRegistryV1::Instance().GetContext(id);
    if (!ctx) return Fail(result, CCU_STATUS_STALE_HANDLE, CCU_STAGE_WINDOW_REGISTER, "Unknown context handle");
    if (input == nullptr || window_out == nullptr || input->abi_version != CCU_EXECUTION_ABI_V1 ||
        input->struct_size != sizeof(*input) || input->device_id != ctx->options.device_id || input->allocation_id == 0 ||
        input->allocation_generation == 0 || input->allocation_base_va == 0 || input->allocation_size == 0 ||
        input->window_size == 0 || input->window_offset > input->allocation_size ||
        input->window_size > input->allocation_size - input->window_offset || input->allocation_owner == nullptr ||
        input->window_offset > std::numeric_limits<uint64_t>::max() - input->allocation_base_va ||
        !ValidOwnerOps(input->allocation_owner_ops))
        return Fail(result, CCU_STATUS_INVALID_ARGUMENT, CCU_STAGE_WINDOW_REGISTER, "Invalid window or owner lease");
    try {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        if (ctx->closing || ctx->bound || ctx->request)
            return Fail(result, CCU_STATUS_INVALID_STATE, CCU_STAGE_WINDOW_REGISTER, "Window registration is closed");
        auto window = std::make_shared<CcuWindowLeaseV1>();
        window->id = CcuExecutionRegistryV1::Instance().NextId();
        window->import = *input;
        window->import.allocation_owner_ops = nullptr;
        window->owner.owner = input->allocation_owner;
        window->owner.ops = *input->allocation_owner_ops;
        if (window->id == 0 || !window->owner.Retain())
            return Fail(result, CCU_STATUS_INVALID_ARGUMENT, CCU_STAGE_WINDOW_REGISTER, "Window owner retain failed");
        ctx->windows.emplace(window->id, window);
        *window_out = window->id;
        SetResult(result, CCU_STATUS_SUCCESS, CCU_STAGE_WINDOW_REGISTER, "Window owner retained");
        return CCU_STATUS_SUCCESS;
    } catch (...) {
        return Fail(result, CCU_STATUS_INTERNAL_ERROR, CCU_STAGE_WINDOW_REGISTER, "Window registration failed");
    }
}

extern "C" int ccu_bind_windows(CcuContextId id, const CcuWindowBindingV1 *binding,
                                  CcuHostResultV1 *result) try {
    auto ctx = CcuExecutionRegistryV1::Instance().GetContext(id);
    if (!ctx) return Fail(result, CCU_STATUS_STALE_HANDLE, CCU_STAGE_BIND, "Unknown context handle");
    if (binding == nullptr || binding->abi_version != CCU_EXECUTION_ABI_V1 || binding->struct_size != sizeof(*binding) ||
        binding->binding_generation == 0 || binding->input_window == 0 || binding->output_window == 0 ||
        std::any_of(std::begin(binding->reserved), std::end(binding->reserved), [](uint64_t v) { return v != 0; }))
        return Fail(result, CCU_STATUS_INVALID_ARGUMENT, CCU_STAGE_BIND, "Invalid window binding");
    std::lock_guard<std::mutex> lock(ctx->mutex);
    auto input = FindWindow(*ctx, binding->input_window);
    auto output = FindWindow(*ctx, binding->output_window);
    if (ctx->closing || ctx->request || !input || !output)
        return Fail(result, CCU_STATUS_INVALID_STATE, CCU_STAGE_BIND, "Binding requires two idle registered windows");
    struct WireWindow { CcuPeerWindowV1 input; CcuPeerWindowV1 output; } local{};
    const auto wire = [](const CcuWindowLeaseV1 &w) {
        return CcuPeerWindowV1{w.import.allocation_id, w.import.allocation_generation,
                               w.import.allocation_base_va, w.import.allocation_size, w.import.window_offset,
                               w.import.window_size, w.import.device_id};
    };
    local.input = wire(*input);
    local.output = wire(*output);
    std::array<WireWindow, 2> peers{};
    const int rc = Exchange(*ctx, binding->binding_generation, kPhaseBind, &local, sizeof(local), peers.data(),
                            sizeof(WireWindow), 5000);
    if (rc != 0) {
        if (ctx->bootstrap.report_abort != nullptr)
            (void)ctx->bootstrap.report_abort(ctx->bootstrap_owner, ctx->options.group_id,
                                             binding->binding_generation, kPhaseBind, rc);
        return Fail(result, CCU_STATUS_PROTOCOL_ERROR, CCU_STAGE_BIND, "Peer window exchange failed");
    }
    for (uint32_t rank = 0; rank < CCU_EXECUTION_RANK_COUNT_V1; ++rank) {
        const auto &peer = peers[rank];
        if (peer.input.allocation_id == 0 || peer.output.allocation_id == 0 ||
            peer.input.allocation_generation == 0 || peer.output.allocation_generation == 0 ||
            peer.input.base_va == 0 || peer.output.base_va == 0 || peer.input.window_size == 0 ||
            peer.output.window_size == 0 || peer.input.device_id < 0 || peer.output.device_id < 0)
            return Fail(result, CCU_STATUS_PROTOCOL_ERROR, CCU_STAGE_BIND, "Peer supplied invalid window metadata");
    }
    const auto &local_peer = peers[ctx->options.rank];
    if (local_peer.input.allocation_id != local.input.allocation_id ||
        local_peer.input.allocation_generation != local.input.allocation_generation ||
        local_peer.output.allocation_id != local.output.allocation_id ||
        local_peer.output.allocation_generation != local.output.allocation_generation)
        return Fail(result, CCU_STATUS_PROTOCOL_ERROR, CCU_STAGE_BIND, "Bootstrap returned mismatched local windows");
    ctx->peer_windows[ctx->options.rank] = peers[ctx->options.rank].output;
    ctx->peer_windows[1U - ctx->options.rank] = peers[1U - ctx->options.rank].output;
    ctx->input_id = binding->input_window;
    ctx->output_id = binding->output_window;
    ctx->binding_generation = binding->binding_generation;
    ctx->bound = true;
    SetResult(result, CCU_STATUS_SUCCESS, CCU_STAGE_BIND, "Peer window metadata exchanged; HCOMM registration pending");
    return CCU_STATUS_SUCCESS;
} catch (...) {
    return Fail(result, CCU_STATUS_INTERNAL_ERROR, CCU_STAGE_BIND, "Unexpected bind failure");
}

extern "C" int ccu_prepare(CcuContextId id, const CcuRequestSpecV1 *spec, CcuRequestId *request_out,
                            CcuHostResultV1 *result) try {
    if (request_out != nullptr) *request_out = 0;
    auto ctx = CcuExecutionRegistryV1::Instance().GetContext(id);
    if (!ctx) return Fail(result, CCU_STATUS_STALE_HANDLE, CCU_STAGE_PREPARE, "Unknown context handle");
    if (spec == nullptr || request_out == nullptr || spec->abi_version != CCU_EXECUTION_ABI_V1 ||
        spec->struct_size != sizeof(*spec) || spec->collective_id == 0 || spec->reserved0 != 0)
        return Fail(result, CCU_STATUS_INVALID_ARGUMENT, CCU_STAGE_PREPARE, "Invalid request specification");
    std::lock_guard<std::mutex> lock(ctx->mutex);
    if (!ctx->bound || ctx->closing || ctx->request)
        return Fail(result, CCU_STATUS_BUSY, CCU_STAGE_PREPARE, "Context is unbound, closing, or has an active request");
    if (ctx->next_sequence == 0 || ctx->next_sequence == std::numeric_limits<uint64_t>::max())
        return Fail(result, CCU_STATUS_INVALID_STATE, CCU_STAGE_PREPARE, "Request sequence exhausted");
    auto input = FindWindow(*ctx, ctx->input_id);
    auto output = FindWindow(*ctx, ctx->output_id);
    if (!input || !output) return Fail(result, CCU_STATUS_STALE_HANDLE, CCU_STAGE_PREPARE, "Window lease expired");
    auto request = std::make_shared<CcuRequestEntryV1>();
    request->id = CcuExecutionRegistryV1::Instance().NextId();
    request->input_id = input->id;
    request->output_id = output->id;
    auto &d = request->descriptor;
    d.abi_version = CCU_EXECUTION_ABI_V1;
    d.struct_size = sizeof(d);
    d.operation = spec->operation;
    d.dtype = spec->dtype;
    d.context_id = id;
    d.context_generation = ctx->generation;
    d.request_seq = ctx->next_sequence;
    d.program_id = 1;
    d.input_window_id = input->id;
    d.output_window_id = output->id;
    d.input_offset = spec->input_offset;
    d.output_offset = spec->output_offset;
    d.element_count = spec->element_count;
    d.root = spec->root;
    d.rank_count = CCU_EXECUTION_RANK_COUNT_V1;
    d.collective_id = spec->collective_id;
    const auto range = [](const CcuWindowLeaseV1 &w) {
        return CcuWindowRangeV1{w.id, w.import.allocation_id, w.import.allocation_generation,
                                w.import.window_offset, w.import.window_size, w.import.device_id};
    };
    const auto validation = ccu_validate_window_ranges_v1(d, range(*input), range(*output));
    const int validation_status = MapValidation(validation);
    if (validation_status != CCU_STATUS_SUCCESS)
        return Fail(result, validation_status, CCU_STAGE_PREPARE, "Request does not fit registered window leases");
    struct Contract { uint64_t collective; uint64_t count; uint32_t operation; uint32_t dtype; uint32_t root; };
    const Contract local{spec->collective_id, spec->element_count, spec->operation, spec->dtype, spec->root};
    std::array<Contract, 2> peers{};
    const int rc = Exchange(*ctx, spec->collective_id, kPhasePrepare, &local, sizeof(local), peers.data(),
                            sizeof(Contract), 5000);
    if (rc != 0 || std::memcmp(&peers[0], &peers[1], sizeof(Contract)) != 0)
        return Fail(result, CCU_STATUS_PROTOCOL_ERROR, CCU_STAGE_PREPARE, "Peer request contract mismatch");
    if (request->id == 0 || request->state.Prepare(d.request_seq) != CcuStateResultV1::OK ||
        !CcuExecutionRegistryV1::Instance().AddRequest(request))
        return Fail(result, CCU_STATUS_INTERNAL_ERROR, CCU_STAGE_PREPARE, "Could not reserve request handle");
    ++ctx->next_sequence;
    ctx->request = request;
    *request_out = request->id;
    SetResult(result, CCU_STATUS_SUCCESS, CCU_STAGE_PREPARE, "Request prepared; HCOMM program registration pending",
              d.request_seq);
    return CCU_STATUS_SUCCESS;
} catch (...) {
    if (request_out != nullptr) *request_out = 0;
    return Fail(result, CCU_STATUS_INTERNAL_ERROR, CCU_STAGE_PREPARE, "Unexpected request preparation failure");
}

extern "C" int ccu_arm(CcuRequestId id, CcuTriggerInfoV1 *trigger_out, CcuHostResultV1 *result) {
    auto request = CcuExecutionRegistryV1::Instance().GetRequest(id);
    if (!request) return Fail(result, CCU_STATUS_STALE_HANDLE, CCU_STAGE_ARM, "Unknown request handle");
    if (trigger_out != nullptr) std::memset(trigger_out, 0, sizeof(*trigger_out));
    return Unsupported(result, CCU_STAGE_ARM, "HCOMM program registration and CCU Launch adapter are not linked");
}

extern "C" int ccu_trigger_host(CcuRequestId, CcuHostResultV1 *result) {
    return Unsupported(result, CCU_STAGE_TRIGGER, "AIV trigger trampoline is not linked");
}

extern "C" int ccu_poll(CcuRequestId id, CcuHostResultV1 *result) {
    auto request = CcuExecutionRegistryV1::Instance().GetRequest(id);
    if (!request) return Fail(result, CCU_STATUS_STALE_HANDLE, CCU_STAGE_POLL, "Unknown request handle");
    SetResult(result, CCU_STATUS_PENDING, CCU_STAGE_POLL, "CCU stream query adapter is not linked",
              request->descriptor.request_seq, 0);
    return CCU_STATUS_PENDING;
}

extern "C" int ccu_wait(CcuRequestId id, uint32_t, CcuHostResultV1 *result) {
    auto request = CcuExecutionRegistryV1::Instance().GetRequest(id);
    if (!request) return Fail(result, CCU_STATUS_STALE_HANDLE, CCU_STAGE_WAIT, "Unknown request handle");
    request->state.RetainPending();
    SetResult(result, CCU_STATUS_RETAINED, CCU_STAGE_WAIT, "Wait cannot prove CCU completion without SDK adapter",
              request->descriptor.request_seq, 0, true);
    return CCU_STATUS_RETAINED;
}

extern "C" int ccu_cancel_prepared(CcuRequestId id, CcuHostResultV1 *result) {
    auto request = CcuExecutionRegistryV1::Instance().GetRequest(id);
    if (!request) return Fail(result, CCU_STATUS_STALE_HANDLE, CCU_STAGE_CANCEL, "Unknown request handle");
    auto ctx = CcuExecutionRegistryV1::Instance().GetContext(request->descriptor.context_id);
    if (!ctx) return Fail(result, CCU_STATUS_STALE_HANDLE, CCU_STAGE_CANCEL, "Unknown context handle");
    const int rc = Exchange(*ctx, request->descriptor.collective_id, kPhaseCancel, nullptr, 0, nullptr, 0, 5000);
    const auto state = request->state.CancelPrepared(rc == 0);
    if (state != CcuStateResultV1::OK) {
        const int status = state == CcuStateResultV1::RETAINED ? CCU_STATUS_RETAINED : CCU_STATUS_INVALID_STATE;
        SetResult(result, status, CCU_STAGE_CANCEL, "Prepared request cancellation was not confirmed",
                  request->descriptor.request_seq, 0, state == CcuStateResultV1::RETAINED);
        return status;
    }
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->request.reset();
    }
    CcuExecutionRegistryV1::Instance().RemoveRequest(id);
    SetResult(result, CCU_STATUS_SUCCESS, CCU_STAGE_CANCEL, "Prepared request cancelled",
              request->descriptor.request_seq);
    return CCU_STATUS_SUCCESS;
}

extern "C" int ccu_retire(CcuRequestId id, uint32_t, CcuHostResultV1 *result) {
    auto request = CcuExecutionRegistryV1::Instance().GetRequest(id);
    if (!request) return Fail(result, CCU_STATUS_STALE_HANDLE, CCU_STAGE_RETIRE, "Unknown request handle");
    return Unsupported(result, CCU_STAGE_RETIRE, "Retire requires verified stream drain and completion Store");
}

extern "C" int ccu_context_close(CcuContextId id, uint32_t, CcuHostResultV1 *result) try {
    auto ctx = CcuExecutionRegistryV1::Instance().GetContext(id);
    if (!ctx) return Fail(result, CCU_STATUS_STALE_HANDLE, CCU_STAGE_CLOSE, "Unknown context handle");
    std::lock_guard<std::mutex> lock(ctx->mutex);
    ctx->closing = true;
    if (ctx->request)
        return Fail(result, CCU_STATUS_RETAINED, CCU_STAGE_CLOSE, "Context still owns an unretired request or SDK work");
    for (auto it = ctx->windows.begin(); it != ctx->windows.end();) {
        if (!it->second->owner.Release()) {
            ctx->retained = true;
            return Fail(result, CCU_STATUS_RETAINED, CCU_STAGE_CLOSE, "Window owner release failed");
        }
        it = ctx->windows.erase(it);
    }
    if (!ctx->bootstrap_lease.Release()) {
        ctx->retained = true;
        return Fail(result, CCU_STATUS_RETAINED, CCU_STAGE_CLOSE, "Bootstrap owner release failed");
    }
    CcuExecutionRegistryV1::Instance().RemoveContext(id);
    SetResult(result, CCU_STATUS_SUCCESS, CCU_STAGE_CLOSE, "Host owners released");
    return CCU_STATUS_SUCCESS;
} catch (...) {
    return Fail(result, CCU_STATUS_INTERNAL_ERROR, CCU_STAGE_CLOSE, "Unexpected context close failure");
}

