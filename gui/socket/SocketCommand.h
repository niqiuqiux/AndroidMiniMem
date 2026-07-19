#pragma once

#include "client_singleton.h"
#include "client.hpp"
#include "socket_request_manager.h"
#include <functional>
#include <chrono>
#include <mutex>

namespace SocketCommand {

class TransactionLease {
public:
    explicit TransactionLease(PortType port)
        : requestLease_(GetSocketMgr().AcquireRequestLease()) {
        if (!requestLease_)
            return;

        auto* mutex = GetSocketMgr().GetTransactionMutex(port);
        if (!mutex)
            return;

        transactionLock_ =
            std::unique_lock<std::recursive_timed_mutex>(*mutex,
                                                         std::defer_lock);
        if (SocketIoTimeout::HasThreadTimeout()) {
            const auto remaining = std::chrono::milliseconds(
                SocketIoTimeout::GetRemainingTimeoutMs());
            if (!transactionLock_.try_lock_for(remaining))
                return;
        } else {
            transactionLock_.lock();
        }
        valid_ = requestLease_.isCurrent();
    }

    explicit operator bool() const {
        return valid_ && requestLease_.isCurrent();
    }

    uint64_t generation() const { return requestLease_.generation(); }

    TransactionLease(TransactionLease&&) = delete;
    TransactionLease& operator=(TransactionLease&&) = delete;
    TransactionLease(const TransactionLease&) = delete;
    TransactionLease& operator=(const TransactionLease&) = delete;

private:
    DeviceSession::RequestLease requestLease_;
    std::unique_lock<std::recursive_timed_mutex> transactionLock_;
    bool valid_ = false;
};

inline bool rejectMalformedResponse(WindowsSocketClient* client) {
    if (client) {
        client->InvalidateProtocol();
    }
    return false;
}

// 需要进程句柄的命令（大多数）
// fn(WindowsSocketClient* client, int handle) -> bool
template<typename Func>
bool execute(PortType port, Func&& fn) {
    auto& socketMgr = GetSocketMgr();
    TransactionLease transaction(port);
    if (!transaction)
        return false;
    auto* client = socketMgr.GetClient(port);
    if (!client)
        return false;
    int handle = 0;
    if (!EnsureOpenHandle(handle, port))
        return false;
    auto* portMutex = socketMgr.GetMutex(port);
    return SocketRequestManager::GetInstance().ExecuteRequestWithLock(
        portMutex, [&]() -> bool {
            if (!transaction || !client->IsConnected())
                return false;
            return fn(client, handle);
        });
}

// 不需要句柄的命令（GetVersion, GetProcessList, InitDriver）
// fn(WindowsSocketClient* client) -> bool
template<typename Func>
bool executeNoHandle(PortType port, Func&& fn) {
    auto& socketMgr = GetSocketMgr();
    TransactionLease transaction(port);
    if (!transaction)
        return false;
    auto* client = socketMgr.GetClient(port);
    if (!client)
        return false;
    auto* portMutex = socketMgr.GetMutex(port);
    return SocketRequestManager::GetInstance().ExecuteRequestWithLock(
        portMutex, [&]() -> bool {
            if (!transaction || !client->IsConnected())
                return false;
            return fn(client);
        });
}

// 需要句柄的命令，返回 int 结果（扫描类）
template<typename Func>
int executeWithResult(PortType port, Func&& fn, int failureValue = 0) {
    auto& socketMgr = GetSocketMgr();
    TransactionLease transaction(port);
    if (!transaction)
        return failureValue;
    auto* client = socketMgr.GetClient(port);
    if (!client)
        return failureValue;
    int handle = 0;
    if (!EnsureOpenHandle(handle, port))
        return failureValue;
    int result = 0;
    auto* portMutex = socketMgr.GetMutex(port);
    bool success = SocketRequestManager::GetInstance().ExecuteRequestWithLock(
        portMutex, [&]() -> bool {
            if (!transaction || !client->IsConnected())
                return false;
            return fn(client, handle, result);
        });
    return success ? result : failureValue;
}

// 发送命令字节+句柄的 helper
inline bool sendCommandWithHandle(WindowsSocketClient* client, uint8_t cmd, int handle) {
    if (!client->Send(&cmd, sizeof(cmd)))
        return false;
    if (!client->Send(&handle, sizeof(handle)))
        return false;
    return true;
}

} // namespace SocketCommand
