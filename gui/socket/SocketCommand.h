#pragma once

#include "client_singleton.h"
#include "client.hpp"
#include "socket_request_manager.h"
#include <functional>
#include <chrono>

namespace SocketCommand {

// 需要进程句柄的命令（大多数）
// fn(WindowsSocketClient* client, int handle) -> bool
template<typename Func>
bool execute(PortType port, Func&& fn) {
    auto* client = GetSocketMgr().GetClient(port);
    if (!client || !client->IsConnected())
        return false;
    int handle = 0;
    if (!EnsureOpenHandle(handle, port))
        return false;
    auto* portMutex = GetSocketMgr().GetMutex(port);
    return SocketRequestManager::GetInstance().ExecuteRequestWithLock(
        portMutex, [&]() -> bool {
            client->DrainPending();
            return fn(client, handle);
        });
}

// 不需要句柄的命令（GetVersion, GetProcessList, InitDriver）
// fn(WindowsSocketClient* client) -> bool
template<typename Func>
bool executeNoHandle(PortType port, Func&& fn) {
    auto* client = GetSocketMgr().GetClient(port);
    if (!client || !client->IsConnected())
        return false;
    auto* portMutex = GetSocketMgr().GetMutex(port);
    return SocketRequestManager::GetInstance().ExecuteRequestWithLock(
        portMutex, [&]() -> bool {
            client->DrainPending();
            return fn(client);
        });
}

// 需要句柄的命令，返回 int 结果（扫描类）
template<typename Func>
int executeWithResult(PortType port, Func&& fn, int failureValue = 0) {
    auto* client = GetSocketMgr().GetClient(port);
    if (!client || !client->IsConnected())
        return failureValue;
    int handle = 0;
    if (!EnsureOpenHandle(handle, port))
        return failureValue;
    int result = 0;
    auto* portMutex = GetSocketMgr().GetMutex(port);
    bool success = SocketRequestManager::GetInstance().ExecuteRequestWithLock(
        portMutex, [&]() -> bool {
            client->DrainPending();
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
