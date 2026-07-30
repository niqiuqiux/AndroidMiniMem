#include "client_singleton.h"
#include "SocketCommand.h"

#include <chrono>
#include <cstring>

namespace {

bool receiveResult(WindowsSocketClient* client,
                   UxnOperationIoResult& output) {
    CeUxnResult wire{};
    if (!client->Receive(&wire, sizeof(wire))) {
        return false;
    }
    output.responseReceived = true;
    if ((wire.result != 0 && wire.result != 1) ||
        (wire.result == 1 && wire.errorCode != 0) ||
        (wire.result == 0 && wire.errorCode <= 0)) {
        return SocketCommand::rejectMalformedResponse(client);
    }
    output.applied = wire.result == 1;
    output.errorCode = wire.errorCode;
    return true;
}

template<typename Result, typename Payload>
bool receiveResultWithPayload(WindowsSocketClient* client,
                              Result& output,
                              Payload& payload) {
    if (!receiveResult(client, output)) {
        return false;
    }
    return client->Receive(&payload, sizeof(payload));
}

} // namespace

UxnInstallIoResult InstallUxnBreakpointTracked(
    uint64_t address, uint32_t flags, PortType type) {
    UxnInstallIoResult output;
    (void)SocketCommand::execute(
        type, [&](WindowsSocketClient* client, int handle) -> bool {
            const uint8_t command = CMD_KERNEL_UXN_INSTALL;
            if (!SocketCommand::sendCommandWithHandle(client, command, handle)) {
                return false;
            }
            output.requestStarted = true;
            unsigned char payload[sizeof(address) + sizeof(flags)];
            std::memcpy(payload, &address, sizeof(address));
            std::memcpy(payload + sizeof(address), &flags, sizeof(flags));
            if (!client->Send(payload, sizeof(payload))) {
                return false;
            }
            return receiveResultWithPayload(client, output, output.install);
        });
    return output;
}

UxnOperationIoResult RemoveUxnBreakpointTracked(
    uint64_t address, PortType type) {
    UxnOperationIoResult output;
    (void)SocketCommand::execute(
        type, [&](WindowsSocketClient* client, int handle) -> bool {
            const uint8_t command = CMD_KERNEL_UXN_REMOVE;
            if (!SocketCommand::sendCommandWithHandle(client, command, handle)) {
                return false;
            }
            output.requestStarted = true;
            return client->Send(&address, sizeof(address)) &&
                   receiveResult(client, output);
        });
    return output;
}

UxnWaitIoResult WaitUxnBreakpointTracked(
    uint32_t slot, uint32_t timeoutMs, uint64_t lastSequence,
    PortType type) {
    UxnWaitIoResult output;
    if (timeoutMs == 0 || timeoutMs > 60000 ||
        (slot != CE_UXN_WAIT_ANY_SLOT && slot >= CE_UXN_MAX_SLOTS)) {
        return output;
    }

    const auto ioDeadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutMs + 5000u);
    SocketIoTimeout::ScopedTimeout waitTimeout(ioDeadline);
    (void)SocketCommand::execute(
        type, [&](WindowsSocketClient* client, int) -> bool {
            const uint8_t command = CMD_KERNEL_UXN_WAIT;
            if (!client->Send(&command, sizeof(command))) {
                return false;
            }
            output.requestStarted = true;
            unsigned char payload[
                sizeof(slot) + sizeof(timeoutMs) + sizeof(lastSequence)];
            std::memcpy(payload, &slot, sizeof(slot));
            std::memcpy(payload + sizeof(slot), &timeoutMs,
                        sizeof(timeoutMs));
            std::memcpy(payload + sizeof(slot) + sizeof(timeoutMs),
                        &lastSequence, sizeof(lastSequence));
            if (!client->Send(payload, sizeof(payload))) {
                return false;
            }
            return receiveResultWithPayload(client, output, output.event);
        });
    return output;
}

UxnOperationIoResult ResumeUxnBreakpointTracked(
    const CeUxnResume& request, PortType type) {
    UxnOperationIoResult output;
    (void)SocketCommand::execute(
        type, [&](WindowsSocketClient* client, int) -> bool {
            const uint8_t command = CMD_KERNEL_UXN_RESUME;
            if (!client->Send(&command, sizeof(command))) {
                return false;
            }
            output.requestStarted = true;
            return client->Send(&request, sizeof(request)) &&
                   receiveResult(client, output);
        });
    return output;
}

UxnStatusIoResult QueryUxnBreakpointStatusTracked(
    uint32_t slot, PortType type) {
    UxnStatusIoResult output;
    (void)SocketCommand::execute(
        type, [&](WindowsSocketClient* client, int) -> bool {
            const uint8_t command = CMD_KERNEL_UXN_STATUS;
            if (!client->Send(&command, sizeof(command))) {
                return false;
            }
            output.requestStarted = true;
            if (!client->Send(&slot, sizeof(slot))) {
                return false;
            }
            return receiveResultWithPayload(client, output, output.status);
        });
    return output;
}

UxnOperationIoResult ClearUxnBreakpointsTracked(PortType type) {
    UxnOperationIoResult output;
    (void)SocketCommand::executeNoHandle(
        type, [&](WindowsSocketClient* client) -> bool {
            const uint8_t command = CMD_KERNEL_UXN_CLEAR;
            output.requestStarted = true;
            return client->Send(&command, sizeof(command)) &&
                   receiveResult(client, output);
        });
    return output;
}
