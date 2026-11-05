#include "host/ccu_request_state.h"

CcuStateResultV1 CcuRequestStateV1::RetainLocked(bool poison) {
    phase_ = CcuRequestPhaseV1::RETAINED;
    poisoned_ = poisoned_ || poison;
    return CcuStateResultV1::RETAINED;
}

CcuStateResultV1 CcuRequestStateV1::Prepare(uint64_t sequence) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (poisoned_ || phase_ == CcuRequestPhaseV1::RETAINED) return CcuStateResultV1::RETAINED;
    if (phase_ != CcuRequestPhaseV1::IDLE && phase_ != CcuRequestPhaseV1::RETIRED &&
        phase_ != CcuRequestPhaseV1::CANCELLED) return CcuStateResultV1::BUSY;
    if (sequence == 0 || sequence <= last_sequence_) return CcuStateResultV1::STALE_SEQUENCE;
    last_sequence_ = active_sequence_ = sequence;
    data_ready_ = trigger_drained_ = ccu_drained_ = false;
    phase_ = CcuRequestPhaseV1::PREPARED;
    return CcuStateResultV1::OK;
}

CcuStateResultV1 CcuRequestStateV1::BeginArm() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ != CcuRequestPhaseV1::PREPARED) return CcuStateResultV1::INVALID_STATE;
    phase_ = CcuRequestPhaseV1::ARMING;
    return CcuStateResultV1::OK;
}

CcuStateResultV1 CcuRequestStateV1::FinishArm(CcuSubmissionV1 submission, bool peers_armed) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ != CcuRequestPhaseV1::ARMING) return CcuStateResultV1::INVALID_STATE;
    if (submission == CcuSubmissionV1::UNKNOWN || (submission == CcuSubmissionV1::SUBMITTED && !peers_armed))
        return RetainLocked(true);
    if (submission == CcuSubmissionV1::NOT_SUBMITTED) {
        if (peers_armed) return RetainLocked(true);
        phase_ = CcuRequestPhaseV1::PREPARED;
        return CcuStateResultV1::SUBMISSION_NOT_ACCEPTED;
    }
    phase_ = CcuRequestPhaseV1::ARMED;
    return CcuStateResultV1::OK;
}

CcuStateResultV1 CcuRequestStateV1::BeginTrigger() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ != CcuRequestPhaseV1::ARMED) return CcuStateResultV1::INVALID_STATE;
    phase_ = CcuRequestPhaseV1::TRIGGER_PENDING;
    return CcuStateResultV1::OK;
}

CcuStateResultV1 CcuRequestStateV1::FinishTrigger(CcuSubmissionV1 submission) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ != CcuRequestPhaseV1::TRIGGER_PENDING) return CcuStateResultV1::INVALID_STATE;
    if (submission == CcuSubmissionV1::UNKNOWN) return RetainLocked(true);
    if (submission == CcuSubmissionV1::NOT_SUBMITTED) {
        phase_ = CcuRequestPhaseV1::ARMED;
        return CcuStateResultV1::SUBMISSION_NOT_ACCEPTED;
    }
    phase_ = CcuRequestPhaseV1::IN_FLIGHT;
    return CcuStateResultV1::OK;
}

CcuStateResultV1 CcuRequestStateV1::ObserveCompletion(uint64_t completed_sequence) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ != CcuRequestPhaseV1::IN_FLIGHT && phase_ != CcuRequestPhaseV1::DATA_READY)
        return CcuStateResultV1::INVALID_STATE;
    if (completed_sequence < active_sequence_) return CcuStateResultV1::PENDING;
    if (completed_sequence != active_sequence_) {
        RetainLocked(true);
        return CcuStateResultV1::COMPLETION_MISMATCH;
    }
    data_ready_ = true;
    phase_ = CcuRequestPhaseV1::DATA_READY;
    return CcuStateResultV1::OK;
}

CcuStateResultV1 CcuRequestStateV1::MarkDrained(bool trigger_stream_drained, bool ccu_stream_drained) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ != CcuRequestPhaseV1::DATA_READY && phase_ != CcuRequestPhaseV1::IN_FLIGHT)
        return CcuStateResultV1::INVALID_STATE;
    trigger_drained_ = trigger_drained_ || trigger_stream_drained;
    ccu_drained_ = ccu_drained_ || ccu_stream_drained;
    if (!trigger_drained_ || !ccu_drained_) return CcuStateResultV1::PENDING;
    if (!data_ready_) return RetainLocked(true);
    phase_ = CcuRequestPhaseV1::DRAINED;
    return CcuStateResultV1::OK;
}

CcuStateResultV1 CcuRequestStateV1::RetainPending() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ != CcuRequestPhaseV1::TRIGGER_PENDING && phase_ != CcuRequestPhaseV1::IN_FLIGHT &&
        phase_ != CcuRequestPhaseV1::DATA_READY && phase_ != CcuRequestPhaseV1::RETIRING)
        return CcuStateResultV1::INVALID_STATE;
    return RetainLocked(false);
}

CcuStateResultV1 CcuRequestStateV1::CancelPrepared(bool peers_cancelled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ != CcuRequestPhaseV1::PREPARED) return CcuStateResultV1::INVALID_STATE;
    if (!peers_cancelled) return RetainLocked(false);
    active_sequence_ = 0;
    phase_ = CcuRequestPhaseV1::CANCELLED;
    return CcuStateResultV1::OK;
}

CcuStateResultV1 CcuRequestStateV1::BeginRetire() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ != CcuRequestPhaseV1::DRAINED) return CcuStateResultV1::INVALID_STATE;
    phase_ = CcuRequestPhaseV1::RETIRING;
    return CcuStateResultV1::OK;
}

CcuStateResultV1 CcuRequestStateV1::FinishRetire(bool peers_retired) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ != CcuRequestPhaseV1::RETIRING) return CcuStateResultV1::INVALID_STATE;
    if (!peers_retired) return RetainLocked(false);
    active_sequence_ = 0;
    phase_ = CcuRequestPhaseV1::RETIRED;
    return CcuStateResultV1::OK;
}

CcuStateResultV1 CcuRequestStateV1::RecoverDrained(uint64_t completed_sequence, bool trigger_stream_drained,
                                                    bool ccu_stream_drained) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ != CcuRequestPhaseV1::RETAINED) return CcuStateResultV1::INVALID_STATE;
    if (poisoned_ || completed_sequence != active_sequence_ || !trigger_stream_drained || !ccu_stream_drained)
        return CcuStateResultV1::RETAINED;
    data_ready_ = trigger_drained_ = ccu_drained_ = true;
    phase_ = CcuRequestPhaseV1::DRAINED;
    return CcuStateResultV1::OK;
}

CcuStateResultV1 CcuRequestStateV1::Close(bool quiescence_proven) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ == CcuRequestPhaseV1::CLOSED) return CcuStateResultV1::OK;
    if (phase_ == CcuRequestPhaseV1::IDLE || phase_ == CcuRequestPhaseV1::RETIRED ||
        phase_ == CcuRequestPhaseV1::CANCELLED || (phase_ == CcuRequestPhaseV1::DRAINED && !poisoned_)) {
        phase_ = CcuRequestPhaseV1::CLOSED;
        return CcuStateResultV1::OK;
    }
    if (quiescence_proven) {
        phase_ = CcuRequestPhaseV1::CLOSED;
        return CcuStateResultV1::OK;
    }
    return CcuStateResultV1::RETAINED;
}

CcuRequestPhaseV1 CcuRequestStateV1::Phase() const { std::lock_guard<std::mutex> lock(mutex_); return phase_; }
uint64_t CcuRequestStateV1::Sequence() const { std::lock_guard<std::mutex> lock(mutex_); return active_sequence_; }
bool CcuRequestStateV1::Poisoned() const { std::lock_guard<std::mutex> lock(mutex_); return poisoned_; }

