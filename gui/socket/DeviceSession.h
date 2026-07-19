#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>

class DeviceSession {
    struct SharedRequestLease;

public:
    enum class State {
        Disconnected,
        Connecting,
        Connected,
        Poisoned,
    };

    class RequestLease {
    public:
        RequestLease(RequestLease&&) noexcept = default;
        RequestLease& operator=(RequestLease&&) noexcept = default;
        RequestLease(const RequestLease&) = delete;
        RequestLease& operator=(const RequestLease&) = delete;

        explicit operator bool() const { return valid_; }
        uint64_t generation() const { return generation_; }
        bool isCurrent() const;

    private:
        friend class DeviceSession;
        RequestLease(DeviceSession* owner,
                     std::shared_ptr<SharedRequestLease> sharedLease,
                     uint64_t generation,
                     bool valid)
            : owner_(owner),
              sharedLease_(std::move(sharedLease)),
              generation_(generation),
              valid_(valid) {}

        DeviceSession* owner_ = nullptr;
        std::shared_ptr<SharedRequestLease> sharedLease_;
        uint64_t generation_ = 0;
        bool valid_ = false;
    };

    using LifecycleLease = std::unique_lock<std::shared_mutex>;

    static DeviceSession& GetInstance();

    RequestLease AcquireRequest();
    LifecycleLease AcquireLifecycle();

    // 以下状态切换要求调用方持有 LifecycleLease。
    void BeginConnect();
    void FinishConnect(bool success);
    void Disconnect();

    // I/O 失败时可在共享请求租约内调用，并立即使当前代际失效。
    void MarkPoisoned();

    State GetState() const;
    bool IsConnected() const;
    bool IsPoisoned() const;
    uint64_t GetGeneration() const;

private:
    struct SharedRequestLease {
        SharedRequestLease(DeviceSession* ownerValue,
                           std::shared_lock<std::shared_mutex> lockValue,
                           uint64_t generationValue)
            : owner(ownerValue),
              lock(std::move(lockValue)),
              generation(generationValue) {}

        DeviceSession* owner = nullptr;
        std::shared_lock<std::shared_mutex> lock;
        uint64_t generation = 0;
    };

    static thread_local std::weak_ptr<SharedRequestLease> activeRequestLease_;

    mutable std::shared_mutex lifecycleGate_;
    std::atomic<State> state_{State::Disconnected};
    std::atomic<uint64_t> generation_{0};
};
