#include "client_singleton.h"
#include "SocketCommand.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace {
constexpr int kMaxBreakpointHitCount = 100000;

std::mutex g_trackedBreakpointMutex;
std::vector<uint64_t> g_trackedBreakpointAddresses;

bool isValidBreakpointType(uint32_t bpType) {
    return bpType >= 1 && bpType <= 4;
}

bool isValidBreakpointSize(uint32_t bpSize) {
    return bpSize == 1 || bpSize == 2 || bpSize == 4 || bpSize == 8;
}

void trackBreakpointAddress(uint64_t address) {
    std::lock_guard<std::mutex> lock(g_trackedBreakpointMutex);
    if (std::find(g_trackedBreakpointAddresses.begin(), g_trackedBreakpointAddresses.end(), address) ==
        g_trackedBreakpointAddresses.end()) {
        g_trackedBreakpointAddresses.push_back(address);
    }
}

void untrackBreakpointAddress(uint64_t address) {
    std::lock_guard<std::mutex> lock(g_trackedBreakpointMutex);
    g_trackedBreakpointAddresses.erase(
        std::remove(g_trackedBreakpointAddresses.begin(), g_trackedBreakpointAddresses.end(), address),
        g_trackedBreakpointAddresses.end());
}

std::vector<uint64_t> snapshotTrackedBreakpointAddresses() {
    std::lock_guard<std::mutex> lock(g_trackedBreakpointMutex);
    return g_trackedBreakpointAddresses;
}

void clearTrackedBreakpointAddresses() {
    std::lock_guard<std::mutex> lock(g_trackedBreakpointMutex);
    g_trackedBreakpointAddresses.clear();
}
} // namespace

bool SetKernelBreakpoint(uint64_t address, uint32_t bpType, uint32_t bpSize, PortType port) {
    if (!isValidBreakpointType(bpType) || !isValidBreakpointSize(bpSize))
        return false;

    const bool success = SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_KERNEL_SETBREAKPOINT;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        // 合并三次 Send 为一次连续字节发送
        unsigned char buf[sizeof(address) + sizeof(bpType) + sizeof(bpSize)];
        memcpy(buf, &address, sizeof(address));
        memcpy(buf + sizeof(address), &bpType, sizeof(bpType));
        memcpy(buf + sizeof(address) + sizeof(bpType), &bpSize, sizeof(bpSize));
        if (!client->Send(buf, sizeof(buf)))
            return false;
        int result = 0;
        if (!client->Receive(&result, sizeof(result)))
            return false;
        return result != 0;
    });
    if (success) {
        trackBreakpointAddress(address);
    }
    return success;
}

bool RemoveKernelBreakpoint(uint64_t address, PortType port) {
    const bool success = SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_KERNEL_REMOVEBREAKPOINT;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        if (!client->Send(&address, sizeof(address)))
            return false;
        int result = 0;
        if (!client->Receive(&result, sizeof(result)))
            return false;
        return result != 0;
    });
    if (success) {
        untrackBreakpointAddress(address);
    }
    return success;
}

bool SuspendKernelBreakpoint(uint64_t address, PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_KERNEL_SUSPENDBREAKPOINT;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        if (!client->Send(&address, sizeof(address)))
            return false;
        int result = 0;
        if (!client->Receive(&result, sizeof(result)))
            return false;
        return result != 0;
    });
}

bool ResumeKernelBreakpoint(uint64_t address, PortType port) {
    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_KERNEL_RESUMEBREAKPOINT;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        if (!client->Send(&address, sizeof(address)))
            return false;
        int result = 0;
        if (!client->Receive(&result, sizeof(result)))
            return false;
        return result != 0;
    });
}

bool ReadKernelBreakpointInfo(uint64_t address, std::vector<HW_HIT_INFO> &infos, PortType port) {
    infos.clear();

    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_KERNEL_READHWBPINFO;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        if (!client->Send(&address, sizeof(address)))
            return false;
        int result = 0;
        uint64_t TotalCount = 0;
        if (!client->Receive(&result, sizeof(result)))
            return false;
        if (!client->Receive(&TotalCount, sizeof(TotalCount)))
            return false;
        if (result < 0 ||
            result > kMaxBreakpointHitCount ||
            TotalCount > static_cast<uint64_t>(kMaxBreakpointHitCount) ||
            static_cast<uint64_t>(result) > TotalCount)
            return false;
        if (result > 0) {
            std::vector<HW_HIT_INFO> receivedInfos(static_cast<size_t>(result));
            if (!client->Receive(receivedInfos.data(), static_cast<size_t>(result) * sizeof(HW_HIT_INFO)))
                return false;
            infos.swap(receivedInfos);
        }
        return true;
    });
}

bool ClearTrackedKernelBreakpoints(PortType port) {
    auto addresses = snapshotTrackedBreakpointAddresses();
    bool allRemoved = true;
    for (uint64_t address : addresses) {
        if (!RemoveKernelBreakpoint(address, port)) {
            allRemoved = false;
        }
    }
    clearTrackedBreakpointAddresses();
    return allRemoved;
}
