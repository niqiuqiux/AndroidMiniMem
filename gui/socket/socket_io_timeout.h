#pragma once

#include <winsock2.h>
#include <windows.h>

#include <chrono>

namespace SocketIoTimeout {

inline thread_local DWORD g_threadTimeoutMs = 0;
inline thread_local std::chrono::steady_clock::time_point g_threadDeadline;

inline constexpr DWORD kMaxTimeoutMs = 300000;

// 单次 socket I/O 的最小超时下限。
//
// 调用级的"剩余总预算"会被当作单次 recv/send 的 SO_RCVTIMEO/SO_SNDTIMEO。
// 一旦命令已经发出，就必须给设备至少一个网络往返的时间把响应读回来，否则
// 会出现：命令已送达设备、设备也正常回包，但客户端因为剩余预算（可能只有
// 几毫秒）小于往返时延而误判 recv 超时（WSAETIMEDOUT/10060）。
// 总预算仍由各请求入口在"两次往返之间"通过 IsThreadTimeoutExpired() 把关，
// 所以这里给单次 I/O 设下限不会让超时整体失效，最多多放行一个在途往返。
inline constexpr DWORD kMinIoTimeoutMs = 5000;

inline DWORD ClampTimeoutMs(unsigned long long requestedMs) {
    return requestedMs > kMaxTimeoutMs
               ? kMaxTimeoutMs
               : static_cast<DWORD>(requestedMs);
}

inline bool HasThreadTimeout() {
    return g_threadTimeoutMs > 0;
}

inline DWORD GetThreadTimeoutMs() {
    return g_threadTimeoutMs;
}

inline bool IsThreadTimeoutExpired() {
    return HasThreadTimeout() &&
           std::chrono::steady_clock::now() >= g_threadDeadline;
}

inline DWORD GetRemainingTimeoutMs() {
    if (!HasThreadTimeout()) {
        return 0;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= g_threadDeadline) {
        return 1;
    }

    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            g_threadDeadline - now).count();
    if (remaining <= 0) {
        return 1;
    }

    return ClampTimeoutMs(static_cast<unsigned long long>(remaining));
}

class ScopedTimeout {
public:
    explicit ScopedTimeout(int seconds)
        : previous_(g_threadTimeoutMs),
          previousDeadline_(g_threadDeadline) {
        if (seconds <= 0) {
            g_threadTimeoutMs = 0;
            g_threadDeadline = {};
            return;
        }

        const unsigned long long requested =
            static_cast<unsigned long long>(seconds) * 1000ull;
        g_threadTimeoutMs = ClampTimeoutMs(requested);
        g_threadDeadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(g_threadTimeoutMs);
    }

    ~ScopedTimeout() {
        g_threadTimeoutMs = previous_;
        g_threadDeadline = previousDeadline_;
    }

    ScopedTimeout(const ScopedTimeout&) = delete;
    ScopedTimeout& operator=(const ScopedTimeout&) = delete;

private:
    DWORD previous_ = 0;
    std::chrono::steady_clock::time_point previousDeadline_;
};

class SocketOptionTimeoutGuard {
public:
    SocketOptionTimeoutGuard(SOCKET sock, int option)
        : sock_(sock), option_(option) {
        if (!HasThreadTimeout() || sock_ == INVALID_SOCKET) {
            return;
        }

        // 用剩余预算，但不低于单次 I/O 下限：命令一旦发出就要给足时间读回
        // 响应，避免在途响应被预算耗尽掐断（见 kMinIoTimeoutMs 注释）。
        const DWORD remainingMs = GetRemainingTimeoutMs();
        const DWORD timeoutMs =
            remainingMs < kMinIoTimeoutMs ? kMinIoTimeoutMs : remainingMs;
        int optLen = sizeof(previous_);
        restore_ = ::getsockopt(sock_, SOL_SOCKET, option_,
                                reinterpret_cast<char*>(&previous_),
                                &optLen) != SOCKET_ERROR;
        (void)::setsockopt(sock_, SOL_SOCKET, option_,
                           reinterpret_cast<const char*>(&timeoutMs),
                           sizeof(timeoutMs));
    }

    ~SocketOptionTimeoutGuard() {
        if (restore_ && sock_ != INVALID_SOCKET) {
            (void)::setsockopt(sock_, SOL_SOCKET, option_,
                               reinterpret_cast<const char*>(&previous_),
                               sizeof(previous_));
        }
    }

    void dismissRestore() {
        restore_ = false;
    }

    SocketOptionTimeoutGuard(const SocketOptionTimeoutGuard&) = delete;
    SocketOptionTimeoutGuard& operator=(const SocketOptionTimeoutGuard&) = delete;

private:
    SOCKET sock_ = INVALID_SOCKET;
    int option_ = 0;
    DWORD previous_ = 0;
    bool restore_ = false;
};

} // namespace SocketIoTimeout
