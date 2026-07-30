#include "../mem/IMemBackend.h"
#include "../mem/MemService.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

template<typename T>
std::vector<unsigned char> bytesOf(T value) {
    std::vector<unsigned char> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

class FakeBackend;

class FakeReadTransaction final : public Mem::IMemReadTransaction {
public:
    explicit FakeReadTransaction(FakeBackend& backend);

    bool valid() const override;
    bool fetchModules(std::vector<Mem::ModuleInfo>& modules) override;
    bool readMemory(uint64_t address,
                    uint32_t size,
                    std::vector<unsigned char>& bytes) override;
    bool readMemoryBatch(
        const std::vector<Mem::MemoryReadRequest>& requests,
        std::vector<Mem::MemoryBlock>& blocks) override;

private:
    FakeBackend& backend_;
};

class FakeSymbolTransaction final : public Mem::IMemSymbolTransaction {
public:
    explicit FakeSymbolTransaction(FakeBackend& backend);

    bool valid() const override;
    bool fetchModules(std::vector<Mem::ModuleInfo>& modules) override;
    bool initialize(uint64_t moduleBase, int& totalCount) override;
    bool fetch(size_t offset,
               size_t limit,
               std::vector<Mem::SymbolInfo>& symbols,
               int& totalCount) override;
    bool find(uint64_t moduleBase,
              const std::string& name,
              uint64_t& address) override;

private:
    FakeBackend& backend_;
};

class FakeBackend final : public Mem::IMemBackend {
public:
    mutable bool connected = true;
    mutable bool poisoned = false;
    mutable uint64_t generation = 1;
    mutable Mem::TargetSnapshot target{42, 7, 2, 1};
    std::string selectedName = "target";
    std::vector<Mem::ProcessInfo> processes{{42, "target"}, {99, "next"}};
    std::vector<Mem::ModuleInfo> modules{
        {0x1000, 0x1000, 1, 5, "/data/app/libfoo.so"},
        {0x2000, 0x1000, 1, 1, "/data/app/libfoo.so"},
        {0x5000, 0x1000, 1, 5, "/system/lib/libbar.so"},
    };
    std::map<uint64_t, std::vector<unsigned char>> memory;
    std::vector<Mem::SymbolInfo> symbols{
        {0x1100, "first"}, {0x1200, "second"}, {0x1300, "third"}};
    uint64_t initializedSymbolBase = 0;
    int symbolInitializeCalls = 0;
    int symbolFetchCalls = 0;
    size_t lastSymbolFetchOffset = 0;
    size_t lastSymbolFetchLimit = 0;
    Mem::MemoryWriteBackendResult writeResult{true, true, 4};
    Mem::DriverInitializationBackendResult driverResult{
        true, true, true, "initialized", true, true, true};
    int driverCalls = 0;
    std::string lastDriverCard;
    bool lastDriverForceReclaim = false;
    uint64_t lastDriverContextGeneration = 0;
    bool cancelDuringDriver = false;
    bool changeGenerationAfterDriver = false;
    bool changeGenerationAfterServerVersion = false;
    bool changeGenerationAfterMemoryType = false;
    bool changeGenerationAfterProcesses = false;
    bool changeGenerationAfterOpenProcess = false;
    mutable bool changeGenerationAfterProcessName = false;
    mutable bool poisonAfterProcessName = false;
    mutable bool changeTargetAfterProcessName = false;
    int driverDelayMs = 0;
    bool connectSucceeds = true;
    bool connectLeavesDisconnected = false;
    bool connectLeavesPoisoned = false;
    bool disconnectSucceeds = true;
    bool disconnectLeavesConnected = false;
    bool disconnectLeavesPoisoned = false;
    Mem::BreakpointMutationBackendResult breakpointResult{true, true, true};
    bool changeGenerationAfterBreakpoint = false;
    size_t breakpointTotal = 1;
    int memoryType = 2;
    std::vector<Mem::BreakpointThreadSlots> breakpointSlotResponse;
    bool breakpointSlotFetchSucceeds = true;
    bool changeGenerationAfterBreakpointSlotQuery = false;
    uint32_t lastBreakpointSlotCapacity = 0;
    bool transactionValid = true;
    bool useBatchResultOverride = false;
    std::vector<Mem::MemoryBlock> batchResultOverride;
    Mem::UxnOperationBackendResult uxnResult{true, true, true, 0};
    Mem::UxnEvent uxnEvent;
    Mem::UxnStatus uxnStatus;
    int uxnInstallCalls = 0;
    int uxnRemoveCalls = 0;
    int uxnWaitCalls = 0;
    int uxnResumeCalls = 0;
    int uxnStatusCalls = 0;
    int uxnClearCalls = 0;
    Mem::UxnResumeRequest lastUxnResume;

    bool isConnected() const override { return connected; }
    bool isPoisoned() const override { return poisoned; }
    uint64_t connectionGeneration() const override { return generation; }
    Mem::TargetSnapshot targetSnapshot() const override { return target; }
    std::string processName() const override {
        if (changeGenerationAfterProcessName) {
            changeGenerationAfterProcessName = false;
            ++generation;
        }
        if (poisonAfterProcessName) {
            poisonAfterProcessName = false;
            connected = false;
            poisoned = true;
            ++generation;
        }
        if (changeTargetAfterProcessName) {
            changeTargetAfterProcessName = false;
            target.processRevision += 2;
        }
        return selectedName;
    }

    bool connect(const std::string&, uint16_t) override {
        ++generation;
        connected = connectSucceeds && !connectLeavesDisconnected &&
                    !connectLeavesPoisoned;
        poisoned = connectSucceeds && connectLeavesPoisoned;
        target = Mem::TargetSnapshot{0, 0, target.processRevision + 2,
                                     generation};
        selectedName.clear();
        return connectSucceeds;
    }

    bool disconnect() override {
        ++generation;
        connected = disconnectLeavesConnected;
        poisoned = disconnectLeavesPoisoned;
        target = Mem::TargetSnapshot{0, 0, target.processRevision + 2,
                                     generation};
        selectedName.clear();
        return disconnectSucceeds;
    }

    bool fetchServerVersion(int& version,
                            std::string& versionString) override {
        version = 1;
        versionString = "MiniMem test";
        if (changeGenerationAfterServerVersion)
            ++generation;
        return true;
    }

    bool fetchMemoryType(int& type, std::string& name) override {
        type = memoryType;
        name = type == 3 ? "Kernel" : "Syscall";
        if (changeGenerationAfterMemoryType)
            ++generation;
        return true;
    }

    Mem::DriverInitializationBackendResult initializeDriver(
        const Mem::OperationContext& context,
        const std::string& card,
        bool forceReclaimHardwareBreakpoints) override {
        ++driverCalls;
        lastDriverCard = card;
        lastDriverForceReclaim = forceReclaimHardwareBreakpoints;
        lastDriverContextGeneration = context.connectionGeneration;
        if (driverDelayMs > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(driverDelayMs));
        }
        if (cancelDuringDriver && context.cancellation) {
            context.cancellation->store(true, std::memory_order_release);
        }
        if (changeGenerationAfterDriver)
            ++generation;
        return driverResult;
    }

    bool fetchProcesses(const Mem::OperationContext&,
                        std::vector<Mem::ProcessInfo>& output) override {
        output = processes;
        if (changeGenerationAfterProcesses)
            ++generation;
        return true;
    }

    bool openProcess(const Mem::OperationContext&,
                     int pid,
                     const std::string& name) override {
        target = Mem::TargetSnapshot{pid, pid + 100,
                                     target.processRevision + 2,
                                     generation};
        selectedName = name;
        if (changeGenerationAfterOpenProcess)
            ++generation;
        return true;
    }

    std::unique_ptr<Mem::IMemReadTransaction> beginReadTransaction(
        const Mem::OperationContext&) override {
        return std::make_unique<FakeReadTransaction>(*this);
    }

    std::unique_ptr<Mem::IMemSymbolTransaction> beginSymbolTransaction(
        const Mem::OperationContext&) override {
        return std::make_unique<FakeSymbolTransaction>(*this);
    }

    bool readMemory(const Mem::OperationContext&,
                    uint64_t address,
                    uint32_t size,
                    std::vector<unsigned char>& bytes) override {
        const auto found = memory.find(address);
        if (found == memory.end() || found->second.size() < size) {
            return false;
        }
        bytes.assign(found->second.begin(), found->second.begin() + size);
        return true;
    }

    bool readMemoryBatch(
        const Mem::OperationContext& context,
        const std::vector<Mem::MemoryReadRequest>& requests,
        std::vector<Mem::MemoryBlock>& blocks) override {
        if (useBatchResultOverride) {
            blocks = batchResultOverride;
            return true;
        }
        blocks.clear();
        for (const auto& request : requests) {
            std::vector<unsigned char> bytes;
            if (readMemory(context, request.address, request.size, bytes)) {
                blocks.push_back(
                    Mem::MemoryBlock{request.address, std::move(bytes), {}});
            }
        }
        return true;
    }

    Mem::MemoryWriteBackendResult writeMemory(
        const Mem::OperationContext&,
        uint64_t,
        const std::vector<unsigned char>&) override {
        return writeResult;
    }

    Mem::BreakpointMutationBackendResult setBreakpoint(
        const Mem::OperationContext&,
        uint64_t,
        Mem::BreakpointAccess,
        uint32_t) override {
        if (changeGenerationAfterBreakpoint)
            ++generation;
        return breakpointResult;
    }

    Mem::BreakpointMutationBackendResult removeBreakpoint(
        const Mem::OperationContext&, uint64_t) override {
        if (changeGenerationAfterBreakpoint)
            ++generation;
        return breakpointResult;
    }

    Mem::BreakpointMutationBackendResult suspendBreakpoint(
        const Mem::OperationContext&, uint64_t) override {
        if (changeGenerationAfterBreakpoint)
            ++generation;
        return breakpointResult;
    }

    Mem::BreakpointMutationBackendResult resumeBreakpoint(
        const Mem::OperationContext&, uint64_t) override {
        if (changeGenerationAfterBreakpoint)
            ++generation;
        return breakpointResult;
    }

    bool fetchBreakpointHits(const Mem::OperationContext&,
                             uint64_t address,
                             size_t,
                             std::vector<Mem::BreakpointHit>& hits,
                             size_t& total) override {
        Mem::BreakpointHit hit;
        hit.hitAddress = address;
        hit.programCounter = 0x7777;
        hits = {hit};
        total = breakpointTotal;
        return true;
    }

    bool fetchBreakpointSlots(
        const Mem::OperationContext&, uint32_t capacity,
        std::vector<Mem::BreakpointThreadSlots>& threads) override {
        lastBreakpointSlotCapacity = capacity;
        threads = breakpointSlotResponse;
        if (changeGenerationAfterBreakpointSlotQuery)
            ++generation;
        return breakpointSlotFetchSucceeds;
    }

    Mem::UxnInstallBackendResult installUxnBreakpoint(
        const Mem::OperationContext&, uint64_t address) override {
        ++uxnInstallCalls;
        Mem::UxnInstallBackendResult output;
        static_cast<Mem::UxnOperationBackendResult&>(output) = uxnResult;
        output.pid = static_cast<uint32_t>(target.pid);
        output.address = address;
        output.slot = 2;
        return output;
    }

    Mem::UxnOperationBackendResult removeUxnBreakpoint(
        const Mem::OperationContext&, uint64_t) override {
        ++uxnRemoveCalls;
        return uxnResult;
    }

    Mem::UxnWaitBackendResult waitUxnBreakpoint(
        const Mem::OperationContext&, uint32_t, uint32_t,
        uint64_t) override {
        ++uxnWaitCalls;
        Mem::UxnWaitBackendResult output;
        static_cast<Mem::UxnOperationBackendResult&>(output) = uxnResult;
        output.event = uxnEvent;
        return output;
    }

    Mem::UxnOperationBackendResult resumeUxnBreakpoint(
        const Mem::OperationContext&, uint32_t slot,
        bool writeRegisters,
        const Mem::UxnRegisters& registers) override {
        ++uxnResumeCalls;
        lastUxnResume.slot = slot;
        lastUxnResume.writeRegisters = writeRegisters;
        lastUxnResume.registers = registers;
        return uxnResult;
    }

    Mem::UxnStatusBackendResult queryUxnBreakpointStatus(
        const Mem::OperationContext&, uint32_t) override {
        ++uxnStatusCalls;
        Mem::UxnStatusBackendResult output;
        static_cast<Mem::UxnOperationBackendResult&>(output) = uxnResult;
        output.status = uxnStatus;
        return output;
    }

    Mem::UxnOperationBackendResult clearUxnBreakpoints(
        const Mem::OperationContext&) override {
        ++uxnClearCalls;
        return uxnResult;
    }
};

FakeReadTransaction::FakeReadTransaction(FakeBackend& backend)
    : backend_(backend) {}

bool FakeReadTransaction::valid() const {
    return backend_.transactionValid;
}

bool FakeReadTransaction::fetchModules(
    std::vector<Mem::ModuleInfo>& modules) {
    modules = backend_.modules;
    return valid();
}

bool FakeReadTransaction::readMemory(
    uint64_t address,
    uint32_t size,
    std::vector<unsigned char>& bytes) {
    return valid() && backend_.readMemory({}, address, size, bytes);
}

bool FakeReadTransaction::readMemoryBatch(
    const std::vector<Mem::MemoryReadRequest>& requests,
    std::vector<Mem::MemoryBlock>& blocks) {
    return valid() && backend_.readMemoryBatch({}, requests, blocks);
}

FakeSymbolTransaction::FakeSymbolTransaction(FakeBackend& backend)
    : backend_(backend) {}

bool FakeSymbolTransaction::valid() const {
    return backend_.transactionValid;
}

bool FakeSymbolTransaction::fetchModules(
    std::vector<Mem::ModuleInfo>& modules) {
    modules = backend_.modules;
    return valid();
}

bool FakeSymbolTransaction::initialize(uint64_t moduleBase, int& totalCount) {
    ++backend_.symbolInitializeCalls;
    backend_.initializedSymbolBase = moduleBase;
    totalCount = static_cast<int>(backend_.symbols.size());
    return valid();
}

bool FakeSymbolTransaction::fetch(
    size_t offset,
    size_t limit,
    std::vector<Mem::SymbolInfo>& symbols,
    int& totalCount) {
    ++backend_.symbolFetchCalls;
    backend_.lastSymbolFetchOffset = offset;
    backend_.lastSymbolFetchLimit = limit;
    totalCount = static_cast<int>(backend_.symbols.size());
    const size_t end = std::min(backend_.symbols.size(), offset + limit);
    if (offset > end) {
        return false;
    }
    symbols.assign(backend_.symbols.begin() + offset,
                   backend_.symbols.begin() + end);
    return valid();
}

bool FakeSymbolTransaction::find(uint64_t moduleBase,
                                 const std::string& name,
                                 uint64_t& address) {
    backend_.initializedSymbolBase = moduleBase;
    const auto found = std::find_if(
        backend_.symbols.begin(), backend_.symbols.end(),
        [&](const Mem::SymbolInfo& symbol) { return symbol.name == name; });
    if (found == backend_.symbols.end()) {
        return false;
    }
    address = found->address;
    return valid();
}

void testContextValidation() {
    FakeBackend backend;
    Mem::MemService service(backend);
    const auto context = service.captureContext(true);

    auto status = service.status(context);
    check(status.ok(), "status accepts current context");
    check(status.ok() && status.value().target.pid == 42,
          "status returns current target");

    backend.generation = 2;
    backend.target.connectionGeneration = 2;
    auto staleConnection = service.readMemory(
        context, Mem::MemoryReadRequest{0x4000, 4});
    check(!staleConnection.ok() &&
              staleConnection.error().code == Mem::ErrorCode::ConnectionChanged,
          "stale connection generation is rejected");

    backend.generation = 1;
    backend.target = context.target.value();
    const auto current = service.captureContext(true);
    backend.target.pid = 77;
    auto staleTarget = service.readMemory(
        current, Mem::MemoryReadRequest{0x4000, 4});
    check(!staleTarget.ok() &&
              staleTarget.error().code == Mem::ErrorCode::TargetChanged,
          "stale target snapshot is rejected");
}

void testConnectionLifecyclePostconditions() {
    Mem::ConnectRequest request{"127.0.0.1", 52736};

    FakeBackend backend;
    Mem::MemService service(backend);
    const auto connected = service.connect(
        service.captureContext(false), request);
    check(connected.ok() && connected.value().connected &&
              connected.value().generation == 2,
          "connect returns the published connection generation");
    const auto disconnected = service.disconnect(
        service.captureContext(false));
    check(disconnected.ok() && disconnected.value().wasConnected &&
              !backend.connected && !backend.poisoned,
          "disconnect returns only after the backend is inactive");

    FakeBackend detachedBackend;
    detachedBackend.connectLeavesDisconnected = true;
    Mem::MemService detachedService(detachedBackend);
    const auto detached = detachedService.connect(
        detachedService.captureContext(false), request);
    check(!detached.ok() &&
              detached.error().code == Mem::ErrorCode::ProtocolError,
          "connect rejects a backend success without connected state");

    FakeBackend poisonedBackend;
    poisonedBackend.connectLeavesPoisoned = true;
    Mem::MemService poisonedService(poisonedBackend);
    const auto poisoned = poisonedService.connect(
        poisonedService.captureContext(false), request);
    check(!poisoned.ok() &&
              poisoned.error().code == Mem::ErrorCode::ProtocolError,
          "connect rejects a poisoned postcondition");

    FakeBackend activeBackend;
    activeBackend.disconnectLeavesConnected = true;
    Mem::MemService activeService(activeBackend);
    const auto active = activeService.disconnect(
        activeService.captureContext(false));
    check(!active.ok() &&
              active.error().code == Mem::ErrorCode::InternalError,
          "disconnect rejects a backend that remains connected");

    FakeBackend stillPoisonedBackend;
    stillPoisonedBackend.disconnectLeavesPoisoned = true;
    Mem::MemService stillPoisonedService(stillPoisonedBackend);
    const auto stillPoisoned = stillPoisonedService.disconnect(
        stillPoisonedService.captureContext(false));
    check(!stillPoisoned.ok() &&
              stillPoisoned.error().code == Mem::ErrorCode::InternalError,
          "disconnect rejects a backend that remains poisoned");
}

void testReadOnlyResultCommitValidation() {
    FakeBackend versionBackend;
    versionBackend.changeGenerationAfterServerVersion = true;
    Mem::MemService versionService(versionBackend);
    const auto version = versionService.serverVersion(
        versionService.captureContext(false));
    check(!version.ok() &&
              version.error().code == Mem::ErrorCode::ConnectionChanged,
          "server version rejects a result from a replaced connection");

    FakeBackend memoryTypeBackend;
    memoryTypeBackend.changeGenerationAfterMemoryType = true;
    Mem::MemService memoryTypeService(memoryTypeBackend);
    const auto memoryType = memoryTypeService.memoryType(
        memoryTypeService.captureContext(false));
    check(!memoryType.ok() &&
              memoryType.error().code == Mem::ErrorCode::ConnectionChanged,
          "memory type rejects a result from a replaced connection");

    FakeBackend processBackend;
    processBackend.changeGenerationAfterProcesses = true;
    Mem::MemService processService(processBackend);
    const auto processes = processService.listProcesses(
        processService.captureContext(false), Mem::ProcessListRequest{});
    check(!processes.ok() &&
              processes.error().code == Mem::ErrorCode::ConnectionChanged,
          "process page rejects a result from a replaced connection");

    FakeBackend changedStatusBackend;
    changedStatusBackend.changeGenerationAfterProcessName = true;
    Mem::MemService changedStatusService(changedStatusBackend);
    const auto changedStatus = changedStatusService.status(
        changedStatusService.captureContext(false));
    check(!changedStatus.ok() &&
              changedStatus.error().code ==
                  Mem::ErrorCode::ConnectionChanged,
          "status reports a replaced connection before target change");

    FakeBackend statusBackend;
    statusBackend.poisonAfterProcessName = true;
    Mem::MemService statusService(statusBackend);
    const auto status = statusService.status(
        statusService.captureContext(false));
    check(!status.ok() &&
              status.error().code == Mem::ErrorCode::ConnectionPoisoned,
          "status prioritizes a poisoned connection over target change");

    FakeBackend targetStatusBackend;
    targetStatusBackend.changeTargetAfterProcessName = true;
    Mem::MemService targetStatusService(targetStatusBackend);
    const auto targetStatus = targetStatusService.status(
        targetStatusService.captureContext(false));
    check(!targetStatus.ok() &&
              targetStatus.error().code == Mem::ErrorCode::TargetChanged,
          "status reports target change when the connection is stable");
}

void testOpenProcessResultCommitValidation() {
    FakeBackend connectionBackend;
    connectionBackend.changeGenerationAfterOpenProcess = true;
    Mem::MemService connectionService(connectionBackend);
    const auto replacedConnection = connectionService.openProcess(
        connectionService.captureContext(true),
        Mem::OpenProcessRequest{99, "next"});
    check(!replacedConnection.ok() &&
              replacedConnection.error().code ==
                  Mem::ErrorCode::ConnectionChanged,
          "process open rejects a receipt from a replaced connection");

    FakeBackend targetBackend;
    targetBackend.changeTargetAfterProcessName = true;
    Mem::MemService targetService(targetBackend);
    const auto replacedTarget = targetService.openProcess(
        targetService.captureContext(true),
        Mem::OpenProcessRequest{99, "next"});
    check(!replacedTarget.ok() &&
              replacedTarget.error().code == Mem::ErrorCode::TargetChanged,
          "process open rejects an incoherent target receipt");
}

void testDriverInitializationSemantics() {
    FakeBackend backend;
    Mem::MemService service(backend);
    const auto context = service.captureContext(false);

    const auto empty = service.initializeDriver(context, {});
    check(!empty.ok() &&
              empty.error().code == Mem::ErrorCode::InvalidArgument &&
              backend.driverCalls == 0,
          "empty driver cards fail before backend access");

    const std::string secret = "test-card-secret";
    const auto completed = service.initializeDriver(
        context, Mem::DriverInitializeRequest{secret, true});
    check(completed.ok() && backend.lastDriverCard == secret &&
              backend.lastDriverForceReclaim &&
              backend.lastDriverContextGeneration == 1 &&
              completed.value().connectionGeneration == 1,
          "confirmed driver initialization returns a connection receipt");

    backend.driverResult = {
        true, true, true, "initialized", true, false, false};
    const auto reclaimUnknown = service.initializeDriver(
        context, Mem::DriverInitializeRequest{secret, true});
    check(!reclaimUnknown.ok() &&
              reclaimUnknown.error().code ==
                  Mem::ErrorCode::CompletionUnknown,
          "unconfirmed breakpoint reclaim configuration is completion unknown");

    backend.driverResult.reclaimResponseReceived = true;
    const auto reclaimRejected = service.initializeDriver(
        context, Mem::DriverInitializeRequest{secret, true});
    check(!reclaimRejected.ok() &&
              reclaimRejected.error().code == Mem::ErrorCode::ProtocolError,
          "rejected breakpoint reclaim configuration is reported");

    backend.driverResult = {};
    const auto unsent = service.initializeDriver(
        context, Mem::DriverInitializeRequest{secret});
    check(!unsent.ok() &&
              unsent.error().code == Mem::ErrorCode::ProtocolError &&
              unsent.error().retryable,
          "unsent driver initialization remains retryable");

    backend.driverResult.requestStarted = true;
    const auto unknown = service.initializeDriver(
        context, Mem::DriverInitializeRequest{secret});
    check(!unknown.ok() &&
              unknown.error().code == Mem::ErrorCode::CompletionUnknown &&
              !unknown.error().retryable,
          "sent driver initialization without a response is completion unknown");

    backend.driverResult.responseReceived = true;
    backend.driverResult.message = "authorization rejected";
    const auto rejected = service.initializeDriver(
        context, Mem::DriverInitializeRequest{secret});
    check(!rejected.ok() &&
              rejected.error().code == Mem::ErrorCode::PermissionDenied &&
              !rejected.error().retryable &&
              std::string(Mem::errorCodeName(rejected.error().code)) ==
                  "permission_denied",
          "server rejection is a confirmed permission failure");

    backend.driverResult.accepted = true;
    backend.driverResult.message = "initialized";
    backend.driverResult.reclaimRequestStarted = true;
    backend.driverResult.reclaimResponseReceived = true;
    backend.driverResult.reclaimApplied = true;
    backend.cancelDuringDriver = true;
    Mem::OperationContext cancelledContext = context;
    cancelledContext.cancellation =
        std::make_shared<std::atomic<bool>>(false);
    const auto completedAfterCancel = service.initializeDriver(
        cancelledContext, Mem::DriverInitializeRequest{secret});
    check(completedAfterCancel.ok() &&
              completedAfterCancel.value().completedAfterCancelRequest,
          "confirmed initialization preserves a late cancellation marker");

    backend.cancelDuringDriver = false;
    backend.driverDelayMs = 20;
    Mem::OperationContext deadlineContext = context;
    deadlineContext.deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(1);
    const auto completedAfterDeadline = service.initializeDriver(
        deadlineContext, Mem::DriverInitializeRequest{secret});
    check(completedAfterDeadline.ok() &&
              completedAfterDeadline.value().completedAfterDeadline,
          "confirmed initialization preserves a late deadline marker");

    backend.driverDelayMs = 0;
    backend.changeGenerationAfterDriver = true;
    const auto replacedConnection = service.initializeDriver(
        context, Mem::DriverInitializeRequest{secret});
    check(!replacedConnection.ok() &&
              replacedConnection.error().code ==
                  Mem::ErrorCode::CompletionUnknown,
          "confirmed initialization on a replaced connection is completion unknown");
}

void testReadsAndPointerTransaction() {
    FakeBackend backend;
    backend.memory[0x4000] = {1, 2, 3, 4};
    backend.memory[0x5000] = {5, 6, 7, 8};
    backend.memory[0x1010] = bytesOf<uint64_t>(0x3000);
    Mem::MemService service(backend);
    const auto context = service.captureContext(true);

    auto read = service.readMemory(
        context, Mem::MemoryReadRequest{0x4000, 4});
    check(read.ok() && read.value().bytes.size() == 4,
          "raw memory read succeeds");

    Mem::MemoryBatchReadRequest batchRequest;
    batchRequest.items = {{0x4000, 4}, {0x5000, 4}};
    auto batch = service.readMemoryBatch(context, batchRequest);
    check(batch.ok() && batch.value().items.size() == 2,
          "batch memory read succeeds");

    backend.useBatchResultOverride = true;
    backend.batchResultOverride = {
        {0x5000, {5, 6, 7, 8}, {}},
        {0x4000, {1, 2, 3, 4}, {}}};
    auto reorderedBatch = service.readMemoryBatch(context, batchRequest);
    check(reorderedBatch.ok() &&
              reorderedBatch.value().items[0].address == 0x5000,
          "batch memory read accepts reordered protocol responses");

    backend.batchResultOverride = {{0x4000, {1, 2, 3, 4}, {}}};
    auto incompleteBatch = service.readMemoryBatch(context, batchRequest);
    check(!incompleteBatch.ok() &&
              incompleteBatch.error().code == Mem::ErrorCode::ProtocolError,
          "batch memory read rejects incomplete response sets");

    backend.batchResultOverride = {
        {0x6000, {1}, {}},
        {0x5000, {5, 6, 7, 8}, {}}};
    auto unknownBatch = service.readMemoryBatch(context, batchRequest);
    check(!unknownBatch.ok() &&
              unknownBatch.error().code == Mem::ErrorCode::ProtocolError,
          "batch memory read rejects unknown response addresses");

    backend.batchResultOverride = {
        {0x4000, {1, 2, 3, 4, 5}, {}},
        {0x5000, {5, 6, 7, 8}, {}}};
    auto oversizedBatch = service.readMemoryBatch(context, batchRequest);
    check(!oversizedBatch.ok() &&
              oversizedBatch.error().code == Mem::ErrorCode::ProtocolError,
          "batch memory read rejects oversized response blocks");
    backend.useBatchResultOverride = false;

    const uint64_t maxAddress = (std::numeric_limits<uint64_t>::max)();
    auto overflowingRead = service.readMemory(
        context, Mem::MemoryReadRequest{maxAddress - 1, 4});
    check(!overflowingRead.ok() &&
              overflowingRead.error().code == Mem::ErrorCode::InvalidArgument,
          "memory read rejects an overflowing address range");

    Mem::MemoryBatchReadRequest overflowingBatchRequest;
    overflowingBatchRequest.items = {{maxAddress - 1, 4}};
    auto overflowingBatch =
        service.readMemoryBatch(context, overflowingBatchRequest);
    check(!overflowingBatch.ok() &&
              overflowingBatch.error().code == Mem::ErrorCode::InvalidArgument,
          "batch memory read rejects an overflowing address range");

    Mem::PointerResolveRequest pointerRequest;
    pointerRequest.moduleName = "libfoo.so";
    pointerRequest.baseOffset = 0x10;
    pointerRequest.offsets = {0x20};
    pointerRequest.dereferenceFinal = false;
    auto pointer = service.resolvePointer(context, pointerRequest);
    check(pointer.ok() && pointer.value().module.base == 0x1000,
          "module resolution selects the lowest mapping base");
    check(pointer.ok() && pointer.value().address == 0x3020,
          "pointer chain resolves inside one transaction");

    backend.modules[0].base =
        (std::numeric_limits<uint64_t>::max)() - 1;
    backend.modules[0].size = 4;
    auto invalidModules = service.listModules(
        context, Mem::ModuleListRequest{});
    check(!invalidModules.ok() &&
              invalidModules.error().code == Mem::ErrorCode::ProtocolError,
          "module lists reject overflowing address ranges");
}

void testWriteCompletionSemantics() {
    FakeBackend backend;
    Mem::MemService service(backend);
    const auto context = service.captureContext(true);
    Mem::MemoryWriteRequest request{0x4000, {1, 2, 3, 4}};

    auto overflowing = service.writeMemory(
        context,
        Mem::MemoryWriteRequest{
            (std::numeric_limits<uint64_t>::max)() - 1, {1, 2, 3, 4}});
    check(!overflowing.ok() &&
              overflowing.error().code == Mem::ErrorCode::InvalidArgument,
          "memory write rejects an overflowing address range");

    backend.writeResult = {true, true, 4};
    auto full = service.writeMemory(context, request);
    check(full.ok() && full.value().writtenBytes == 4,
          "confirmed full write succeeds");

    backend.writeResult = {true, true, 2};
    auto partial = service.writeMemory(context, request);
    check(!partial.ok() &&
              partial.error().code == Mem::ErrorCode::PartialWrite &&
              partial.error().affectedBytes == 2,
          "partial write preserves affected byte count");

    backend.writeResult = {true, false, 0};
    auto unknown = service.writeMemory(context, request);
    check(!unknown.ok() &&
              unknown.error().code == Mem::ErrorCode::CompletionUnknown,
          "sent write without response reports completion unknown");
}

void testBreakpointCompletionSemantics() {
    FakeBackend backend;
    Mem::MemService service(backend);
    const auto context = service.captureContext(true);
    Mem::BreakpointSetRequest request;
    request.address = 0x6000;
    request.access = Mem::BreakpointAccess::Write;
    request.size = 4;

    backend.breakpointResult = {};
    const auto unsent = service.setBreakpoint(context, request);
    check(!unsent.ok() &&
              unsent.error().code == Mem::ErrorCode::ProtocolError &&
              unsent.error().retryable,
          "unsent breakpoint mutation remains retryable");

    backend.breakpointResult.requestStarted = true;
    const auto unknown = service.setBreakpoint(context, request);
    check(!unknown.ok() &&
              unknown.error().code == Mem::ErrorCode::CompletionUnknown &&
              !unknown.error().retryable,
          "sent breakpoint mutation without response is completion unknown");

    backend.breakpointResult.responseReceived = true;
    const auto rejected = service.setBreakpoint(context, request);
    check(!rejected.ok() &&
              rejected.error().code == Mem::ErrorCode::ProtocolError &&
              !rejected.error().retryable,
          "server-rejected breakpoint mutation is confirmed failure");

    backend.breakpointResult.applied = true;
    const auto applied = service.setBreakpoint(context, request);
    check(applied.ok() &&
              applied.value().action == Mem::BreakpointAction::Set,
          "confirmed breakpoint mutation returns a target receipt");

    backend.changeGenerationAfterBreakpoint = true;
    const auto replacedConnection = service.setBreakpoint(context, request);
    check(!replacedConnection.ok() &&
              replacedConnection.error().code ==
                  Mem::ErrorCode::CompletionUnknown,
          "confirmed breakpoint on a replaced connection is completion unknown");
}

void testProcessSymbolsAndBreakpoints() {
    FakeBackend backend;
    Mem::MemService service(backend);
    auto context = service.captureContext(true);

    auto opened = service.openProcess(
        context, Mem::OpenProcessRequest{99, {}});
    check(opened.ok() && opened.value().target.pid == 99 &&
              opened.value().name == "next",
          "process selection publishes one coherent target");

    context = service.captureContext(true);
    auto table = service.loadSymbolTable(
        context, Mem::SymbolTableRequest{0x2000});
    check(table.ok() && table.value().items.size() == 3,
          "symbol table loads all bounded pages");
    check(backend.initializedSymbolBase == 0x1000,
          "symbol table canonicalizes a module segment to its lowest base");

    const int fetchCallsBeforePage = backend.symbolFetchCalls;
    auto symbolPage = service.listSymbols(
        context, Mem::SymbolListRequest{0x2000, 1, 1});
    check(symbolPage.ok() && symbolPage.value().items.size() == 1 &&
              symbolPage.value().items[0].name == "second" &&
              symbolPage.value().total == 3 &&
              symbolPage.value().nextOffset == 2,
          "symbol page returns only the requested window");
    check(backend.symbolFetchCalls == fetchCallsBeforePage + 1 &&
              backend.lastSymbolFetchOffset == 1 &&
              backend.lastSymbolFetchLimit == 1,
          "symbol page performs one bounded backend fetch");

    auto symbol = service.resolveSymbol(
        context, Mem::SymbolResolveRequest{0x2000, "second"});
    check(symbol.ok() && symbol.value().address == 0x1200,
          "symbol lookup uses the target-bound transaction");

    Mem::BreakpointSetRequest breakpoint;
    breakpoint.address = 0x6000;
    breakpoint.access = Mem::BreakpointAccess::Write;
    breakpoint.size = 4;
    auto set = service.setBreakpoint(context, breakpoint);
    check(set.ok() && set.value().target == *context.target,
          "breakpoint mutation returns its target receipt");

    auto hits = service.breakpointHits(
        context, Mem::BreakpointHitBatchRequest{0x6000, 10});
    check(hits.ok() && hits.value().items.size() == 1 &&
              hits.value().items[0].programCounter == 0x7777,
          "breakpoint hit batch is bounded and converted");

    backend.breakpointTotal = 200000;
    auto cumulativeHits = service.breakpointHits(
        context, Mem::BreakpointHitBatchRequest{0x6000, 10});
    check(cumulativeHits.ok() &&
              cumulativeHits.value().available == 200000 &&
              cumulativeHits.value().dropped == 199998,
          "breakpoint cumulative hit count may exceed the returned record limit");

    backend.breakpointTotal = 200005;
    auto incrementalHits = service.breakpointHits(
        context, Mem::BreakpointHitBatchRequest{0x6000, 10});
    check(incrementalHits.ok() && incrementalHits.value().dropped == 4,
          "breakpoint dropped count uses the cumulative delta between polls");
}

void testBreakpointSlotQuery() {
    FakeBackend backend;
    Mem::MemService service(backend);
    const auto context = service.captureContext(true);

    const auto nonKernel = service.breakpointSlots(context);
    check(!nonKernel.ok() &&
              nonKernel.error().code == Mem::ErrorCode::PermissionDenied,
          "breakpoint slot query explicitly rejects non-Kernel mode");

    backend.memoryType = 3;
    Mem::BreakpointThreadSlots queried;
    queried.tid = 42;
    queried.querySucceeded = true;
    queried.count = 1;
    queried.totalCount = 1;
    queried.brpCount = 1;
    queried.enabledCount = 1;
    queried.activeCount = 1;
    queried.perfCount = 1;
    queried.slots.push_back(Mem::BreakpointSlot{
        0x11, 0x22, 0x1234, 42, 3, 4, 4, 6, 0, 3});

    Mem::BreakpointThreadSlots exited;
    exited.tid = 43;
    exited.querySucceeded = false;
    exited.errorCode = 3;
    backend.breakpointSlotResponse = {queried, exited};

    const auto snapshot = service.breakpointSlots(context, 64);
    check(snapshot.ok() && snapshot.value().target == *context.target &&
              snapshot.value().threads.size() == 2 &&
              snapshot.value().threads[0].slots.size() == 1 &&
              snapshot.value().threads[1].errorCode == 3 &&
              backend.lastBreakpointSlotCapacity == 64,
          "breakpoint slot query preserves successful slots and per-TID errors");

    const auto invalid = service.breakpointSlots(context, 0);
    check(!invalid.ok() &&
              invalid.error().code == Mem::ErrorCode::InvalidArgument,
          "breakpoint slot query validates per-thread capacity");

    backend.breakpointSlotFetchSucceeds = false;
    const auto failed = service.breakpointSlots(context);
    check(!failed.ok() &&
              failed.error().code == Mem::ErrorCode::ProtocolError &&
              failed.error().retryable,
          "breakpoint slot backend failure remains retryable");

    backend.breakpointSlotFetchSucceeds = true;
    backend.changeGenerationAfterBreakpointSlotQuery = true;
    const auto replacedConnection = service.breakpointSlots(context);
    check(!replacedConnection.ok() &&
              replacedConnection.error().code ==
                  Mem::ErrorCode::ConnectionChanged,
          "breakpoint slot query rejects a result from a replaced connection");
}

void testUxnBreakpointSemantics() {
    FakeBackend backend;
    Mem::MemService service(backend);
    const auto context = service.captureContext(true);

    const auto nonKernelInstall = service.installUxnBreakpoint(
        context, Mem::UxnInstallRequest{0x6000});
    const auto nonKernelRemove = service.removeUxnBreakpoint(
        context, Mem::UxnRemoveRequest{0x6000});
    const auto nonKernelWait = service.waitUxnBreakpoint(
        context, Mem::UxnWaitRequest{2, 1000, 0});
    const auto nonKernelResume = service.resumeUxnBreakpoint(
        context, Mem::UxnResumeRequest{2});
    const auto nonKernelStatus = service.queryUxnBreakpointStatus(
        context, Mem::UxnStatusRequest{2});
    const auto nonKernelClear = service.clearUxnBreakpoints(
        service.captureContext(false));
    check(!nonKernelInstall.ok() && !nonKernelRemove.ok() &&
              !nonKernelWait.ok() && !nonKernelResume.ok() &&
              !nonKernelStatus.ok() && !nonKernelClear.ok() &&
              nonKernelInstall.error().code ==
                  Mem::ErrorCode::PermissionDenied &&
              nonKernelRemove.error().code ==
                  Mem::ErrorCode::PermissionDenied &&
              nonKernelWait.error().code ==
                  Mem::ErrorCode::PermissionDenied &&
              nonKernelResume.error().code ==
                  Mem::ErrorCode::PermissionDenied &&
              nonKernelStatus.error().code ==
                  Mem::ErrorCode::PermissionDenied &&
              nonKernelClear.error().code ==
                  Mem::ErrorCode::PermissionDenied &&
              backend.uxnInstallCalls == 0 &&
              backend.uxnRemoveCalls == 0 &&
              backend.uxnWaitCalls == 0 &&
              backend.uxnResumeCalls == 0 &&
              backend.uxnStatusCalls == 0 &&
              backend.uxnClearCalls == 0,
          "all UXN operations reject non-Kernel mode before backend access");

    backend.memoryType = 3;

    const auto unaligned = service.installUxnBreakpoint(
        context, Mem::UxnInstallRequest{0x6002});
    check(!unaligned.ok() &&
              unaligned.error().code == Mem::ErrorCode::InvalidArgument &&
              backend.uxnInstallCalls == 0,
          "UXN install rejects unaligned addresses before backend access");

    const auto installed = service.installUxnBreakpoint(
        context, Mem::UxnInstallRequest{0x6000});
    check(installed.ok() && installed.value().slot == 2 &&
              installed.value().address == 0x6000 &&
              installed.value().target == *context.target,
          "UXN install returns the confirmed slot and target");

    backend.uxnEvent.slot = 2;
    backend.uxnEvent.pid = 42;
    backend.uxnEvent.tid = 43;
    backend.uxnEvent.state = Mem::UxnState::Paused;
    backend.uxnEvent.sequence = 9;
    backend.uxnEvent.address = 0x6000;
    backend.uxnEvent.registers.programCounter = 0x6000;
    const auto event = service.waitUxnBreakpoint(
        context, Mem::UxnWaitRequest{2, 1000, 8});
    check(event.ok() && event.value().sequence == 9 &&
              event.value().registers.programCounter == 0x6000,
          "UXN wait validates and returns a paused event");

    Mem::UxnResumeRequest resume;
    resume.slot = 2;
    resume.writeRegisters = true;
    resume.registers = event.value().registers;
    resume.registers.general[0] = 0x1234;
    const auto resumed = service.resumeUxnBreakpoint(context, resume);
    check(resumed.ok() && backend.lastUxnResume.writeRegisters &&
              backend.lastUxnResume.registers.general[0] == 0x1234,
          "UXN resume preserves register writeback requests");

    backend.uxnStatus.slot = 2;
    backend.uxnStatus.used = true;
    backend.uxnStatus.pid = 42;
    backend.uxnStatus.state = Mem::UxnState::Armed;
    backend.uxnStatus.address = 0x6000;
    const auto status = service.queryUxnBreakpointStatus(
        context, Mem::UxnStatusRequest{2});
    check(status.ok() && status.value().used &&
              status.value().state == Mem::UxnState::Armed &&
              status.value().target == *context.target,
          "UXN status preserves slot state and target");

    backend.uxnResult = {true, true, false, 110};
    const auto timedOut = service.waitUxnBreakpoint(
        context, Mem::UxnWaitRequest{2, 1000, 9});
    check(!timedOut.ok() &&
              timedOut.error().code == Mem::ErrorCode::Timeout &&
              timedOut.error().nativeCode == 110 &&
              timedOut.error().retryable,
          "UXN timeout preserves Linux errno and retryability");

    backend.uxnResult = {true, false, false, 0};
    const auto unknown = service.removeUxnBreakpoint(
        context, Mem::UxnRemoveRequest{0x6000});
    check(!unknown.ok() &&
              unknown.error().code == Mem::ErrorCode::CompletionUnknown &&
              !unknown.error().retryable,
          "UXN mutation without a response is completion unknown");

    backend.uxnResult = {true, true, true, 0};
    const auto cleared = service.clearUxnBreakpoints(
        service.captureContext(false));
    check(cleared.ok(), "UXN clear does not require an attached target");
}

} // namespace

int main() {
    testContextValidation();
    testConnectionLifecyclePostconditions();
    testReadOnlyResultCommitValidation();
    testOpenProcessResultCommitValidation();
    testDriverInitializationSemantics();
    testReadsAndPointerTransaction();
    testWriteCompletionSemantics();
    testBreakpointCompletionSemantics();
    testProcessSymbolsAndBreakpoints();
    testBreakpointSlotQuery();
    testUxnBreakpointSemantics();
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "MemService tests passed\n";
    return 0;
}
