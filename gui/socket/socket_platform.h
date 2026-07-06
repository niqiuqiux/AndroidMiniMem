#pragma once

#include <cstdint>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

namespace SocketPlatform {

using SocketOptionLength = int;
using SocketTimeoutValue = DWORD;
using AvailableBytes = u_long;

struct StartupState {
    std::mutex mutex;
    uint32_t refCount = 0;
};

inline StartupState& GetStartupState() {
    static StartupState state;
    return state;
}

inline bool Startup() {
    auto& state = GetStartupState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.refCount > 0) {
        ++state.refCount;
        return true;
    }

    WSADATA wsaData{};
    const int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        WSASetLastError(result);
        return false;
    }

    state.refCount = 1;
    return true;
}

inline void Cleanup() {
    auto& state = GetStartupState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.refCount == 0) {
        return;
    }

    --state.refCount;
    if (state.refCount == 0) {
        WSACleanup();
    }
}

inline int LastError() {
    return WSAGetLastError();
}

inline void Close(SOCKET sock) {
    closesocket(sock);
}

inline void Shutdown(SOCKET sock) {
    shutdown(sock, SD_BOTH);
}

inline bool IsConnectionResetError(int error) {
    return error == WSAECONNRESET || error == WSAECONNABORTED;
}

inline int GetAvailableBytes(SOCKET sock, AvailableBytes& pending) {
    return ioctlsocket(sock, FIONREAD, &pending);
}

inline SocketTimeoutValue MakeTimeoutValue(DWORD timeoutMs) {
    return timeoutMs;
}

inline char* MutableOptionData(SocketTimeoutValue& value) {
    return reinterpret_cast<char*>(&value);
}

inline const char* OptionData(const SocketTimeoutValue& value) {
    return reinterpret_cast<const char*>(&value);
}

} // namespace SocketPlatform

#else

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using SOCKET = int;
using DWORD = uint32_t;

inline constexpr SOCKET INVALID_SOCKET = -1;
inline constexpr int SOCKET_ERROR = -1;

namespace SocketPlatform {

using SocketOptionLength = socklen_t;
using SocketTimeoutValue = timeval;
using AvailableBytes = int;

inline bool Startup() {
    static std::once_flag signalOnce;
    std::call_once(signalOnce, [] {
        std::signal(SIGPIPE, SIG_IGN);
    });
    return true;
}

inline void Cleanup() {
}

inline int LastError() {
    return errno;
}

inline void Close(SOCKET sock) {
    close(sock);
}

inline void Shutdown(SOCKET sock) {
    shutdown(sock, SHUT_RDWR);
}

inline bool IsConnectionResetError(int error) {
    return error == ECONNRESET || error == ECONNABORTED ||
           error == EPIPE || error == ENOTCONN;
}

inline int GetAvailableBytes(SOCKET sock, AvailableBytes& pending) {
    return ioctl(sock, FIONREAD, &pending);
}

inline SocketTimeoutValue MakeTimeoutValue(DWORD timeoutMs) {
    SocketTimeoutValue timeout{};
    timeout.tv_sec = static_cast<time_t>(timeoutMs / 1000);
    timeout.tv_usec = static_cast<suseconds_t>((timeoutMs % 1000) * 1000);
    return timeout;
}

inline char* MutableOptionData(SocketTimeoutValue& value) {
    return reinterpret_cast<char*>(&value);
}

inline const char* OptionData(const SocketTimeoutValue& value) {
    return reinterpret_cast<const char*>(&value);
}

} // namespace SocketPlatform

#endif

namespace SocketPlatform {

class ScopedSocket {
public:
    explicit ScopedSocket(SOCKET socket = INVALID_SOCKET) : socket_(socket) {}
    ~ScopedSocket() { reset(); }

    ScopedSocket(const ScopedSocket&) = delete;
    ScopedSocket& operator=(const ScopedSocket&) = delete;

    ScopedSocket(ScopedSocket&& other) noexcept : socket_(other.release()) {}

    ScopedSocket& operator=(ScopedSocket&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    SOCKET get() const { return socket_; }
    bool valid() const { return socket_ != INVALID_SOCKET; }

    SOCKET release() {
        const SOCKET socket = socket_;
        socket_ = INVALID_SOCKET;
        return socket;
    }

    void reset(SOCKET socket = INVALID_SOCKET) {
        if (socket_ != INVALID_SOCKET) {
            Close(socket_);
        }
        socket_ = socket;
    }

private:
    SOCKET socket_ = INVALID_SOCKET;
};

} // namespace SocketPlatform
