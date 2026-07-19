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

template<typename SendPayload>
BreakpointMutationIoResult mutateBreakpointTracked(
    unsigned char command, PortType port, SendPayload&& sendPayload) {
    BreakpointMutationIoResult result;
    (void)SocketCommand::execute(
        port, [&](WindowsSocketClient* client, int handle) -> bool {
            if (!SocketCommand::sendCommandWithHandle(
                    client, command, handle)) {
                return false;
            }
            result.requestStarted = true;
            if (!sendPayload(client))
                return false;
            int serverResult = 0;
            if (!client->Receive(&serverResult, sizeof(serverResult)))
                return false;
            result.responseReceived = true;
            result.applied = serverResult > 0;
            return true;
        });
    return result;
}
} // namespace

BreakpointMutationIoResult SetKernelBreakpointTracked(
    uint64_t address, uint32_t bpType, uint32_t bpSize, PortType port) {
    if (!isValidBreakpointType(bpType) || !isValidBreakpointSize(bpSize))
        return {};

    BreakpointMutationIoResult result = mutateBreakpointTracked(
        CMD_KERNEL_SETBREAKPOINT, port,
        [&](WindowsSocketClient* client) -> bool {
            // 合并三次 Send 为一次连续字节发送。
            unsigned char buf[
                sizeof(address) + sizeof(bpType) + sizeof(bpSize)];
            memcpy(buf, &address, sizeof(address));
            memcpy(buf + sizeof(address), &bpType, sizeof(bpType));
            memcpy(buf + sizeof(address) + sizeof(bpType),
                   &bpSize, sizeof(bpSize));
            return client->Send(buf, sizeof(buf));
        });
    if (result.responseReceived && result.applied)
        trackBreakpointAddress(address);
    return result;
}

BreakpointMutationIoResult RemoveKernelBreakpointTracked(
    uint64_t address, PortType port) {
    BreakpointMutationIoResult result = mutateBreakpointTracked(
        CMD_KERNEL_REMOVEBREAKPOINT, port,
        [&](WindowsSocketClient* client) {
            return client->Send(&address, sizeof(address));
        });
    if (result.responseReceived && result.applied)
        untrackBreakpointAddress(address);
    return result;
}

BreakpointMutationIoResult SuspendKernelBreakpointTracked(
    uint64_t address, PortType port) {
    return mutateBreakpointTracked(
        CMD_KERNEL_SUSPENDBREAKPOINT, port,
        [&](WindowsSocketClient* client) {
            return client->Send(&address, sizeof(address));
        });
}

BreakpointMutationIoResult ResumeKernelBreakpointTracked(
    uint64_t address, PortType port) {
    return mutateBreakpointTracked(
        CMD_KERNEL_RESUMEBREAKPOINT, port,
        [&](WindowsSocketClient* client) {
            return client->Send(&address, sizeof(address));
        });
}

bool SetKernelBreakpoint(uint64_t address, uint32_t bpType,
                         uint32_t bpSize, PortType port) {
    const BreakpointMutationIoResult result =
        SetKernelBreakpointTracked(address, bpType, bpSize, port);
    return result.responseReceived && result.applied;
}

bool RemoveKernelBreakpoint(uint64_t address, PortType port) {
    const BreakpointMutationIoResult result =
        RemoveKernelBreakpointTracked(address, port);
    return result.responseReceived && result.applied;
}

bool SuspendKernelBreakpoint(uint64_t address, PortType port) {
    const BreakpointMutationIoResult result =
        SuspendKernelBreakpointTracked(address, port);
    return result.responseReceived && result.applied;
}

bool ResumeKernelBreakpoint(uint64_t address, PortType port) {
    const BreakpointMutationIoResult result =
        ResumeKernelBreakpointTracked(address, port);
    return result.responseReceived && result.applied;
}

bool ReadKernelBreakpointInfo(uint64_t address, std::vector<HW_HIT_INFO> &infos, PortType port,
                              uint64_t *outTotalHits) {
    infos.clear();
    if (outTotalHits)
        *outTotalHits = 0;

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
            static_cast<uint64_t>(result) > TotalCount)
            return SocketCommand::rejectMalformedResponse(client);
        if (outTotalHits)
            *outTotalHits = TotalCount;
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
    SocketCommand::TransactionLease transaction(port);
    if (!transaction) {
        clearTrackedBreakpointAddresses();
        return false;
    }
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

void ResetTrackedKernelBreakpoints() {
    clearTrackedBreakpointAddresses();
}
