#include "host/ccu_execution_registry.h"

#include <limits>

bool CcuOwnerLeaseV1::Retain() {
    if (retained) return true;
    if (owner == nullptr || ops.abi_version != CCU_EXECUTION_ABI_V1 || ops.struct_size != sizeof(ops) ||
        ops.retain == nullptr || ops.release == nullptr || ops.retain(owner) != 0) return false;
    retained = true;
    return true;
}

bool CcuOwnerLeaseV1::Release() {
    if (!retained) return true;
    if (ops.release == nullptr || ops.release(owner) != 0) return false;
    retained = false;
    return true;
}

CcuOwnerLeaseV1::~CcuOwnerLeaseV1() {
    // Failed releases are quarantined by the registry owner before destruction.
    if (retained) (void)Release();
}

CcuExecutionRegistryV1 &CcuExecutionRegistryV1::Instance() {
    static CcuExecutionRegistryV1 registry;
    return registry;
}

uint64_t CcuExecutionRegistryV1::NextId() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (next_id_ == 0 || next_id_ == std::numeric_limits<uint64_t>::max()) return 0;
    return next_id_++;
}

bool CcuExecutionRegistryV1::AddContext(const std::shared_ptr<CcuContextEntryV1> &context) {
    if (!context || context->id == 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return contexts_.emplace(context->id, context).second;
}

std::shared_ptr<CcuContextEntryV1> CcuExecutionRegistryV1::GetContext(CcuContextId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = contexts_.find(id);
    return it == contexts_.end() ? nullptr : it->second;
}

std::shared_ptr<CcuRequestEntryV1> CcuExecutionRegistryV1::GetRequest(CcuRequestId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = requests_.find(id);
    return it == requests_.end() ? nullptr : it->second;
}

bool CcuExecutionRegistryV1::AddRequest(const std::shared_ptr<CcuRequestEntryV1> &request) {
    if (!request || request->id == 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_.emplace(request->id, request).second;
}

void CcuExecutionRegistryV1::RemoveRequest(CcuRequestId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    requests_.erase(id);
}

void CcuExecutionRegistryV1::RemoveContext(CcuContextId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    contexts_.erase(id);
}

