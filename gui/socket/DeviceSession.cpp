#include "DeviceSession.h"

thread_local std::weak_ptr<DeviceSession::SharedRequestLease>
    DeviceSession::activeRequestLease_;

DeviceSession& DeviceSession::GetInstance() {
    // 进程级生命周期避免 socket 单例析构顺序导致回调访问已销毁对象。
    static DeviceSession* instance = new DeviceSession();
    return *instance;
}

bool DeviceSession::RequestLease::isCurrent() const {
    return valid_ && owner_ &&
           owner_->GetState() == State::Connected &&
           owner_->GetGeneration() == generation_;
}

DeviceSession::RequestLease DeviceSession::AcquireRequest() {
    if (auto active = activeRequestLease_.lock();
        active && active->owner == this) {
        const uint64_t generation = active->generation;
        const bool valid =
            state_.load(std::memory_order_acquire) == State::Connected &&
            generation_.load(std::memory_order_acquire) == generation;
        return RequestLease(this, std::move(active), generation, valid);
    }

    std::shared_lock<std::shared_mutex> lock(lifecycleGate_);
    const State state = state_.load(std::memory_order_acquire);
    const uint64_t generation = generation_.load(std::memory_order_acquire);
    if (state != State::Connected) {
        return RequestLease(this, nullptr, generation, false);
    }

    auto sharedLease = std::make_shared<SharedRequestLease>(
        this, std::move(lock), generation);
    activeRequestLease_ = sharedLease;
    return RequestLease(this, std::move(sharedLease), generation, true);
}

DeviceSession::LifecycleLease DeviceSession::AcquireLifecycle() {
    return LifecycleLease(lifecycleGate_);
}

void DeviceSession::BeginConnect() {
    generation_.fetch_add(1, std::memory_order_acq_rel);
    state_.store(State::Connecting, std::memory_order_release);
}

void DeviceSession::FinishConnect(bool success) {
    state_.store(success ? State::Connected : State::Disconnected,
                 std::memory_order_release);
}

void DeviceSession::Disconnect() {
    const State oldState = state_.load(std::memory_order_acquire);
    if (oldState != State::Disconnected) {
        generation_.fetch_add(1, std::memory_order_acq_rel);
    }
    state_.store(State::Disconnected, std::memory_order_release);
}

void DeviceSession::MarkPoisoned() {
    State expected = State::Connected;
    if (state_.compare_exchange_strong(expected, State::Poisoned,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
        generation_.fetch_add(1, std::memory_order_acq_rel);
    }
}

DeviceSession::State DeviceSession::GetState() const {
    return state_.load(std::memory_order_acquire);
}

bool DeviceSession::IsConnected() const {
    return GetState() == State::Connected;
}

bool DeviceSession::IsPoisoned() const {
    return GetState() == State::Poisoned;
}

uint64_t DeviceSession::GetGeneration() const {
    return generation_.load(std::memory_order_acquire);
}
