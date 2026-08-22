#include "SystemMemService.h"

#include "IMemBackend.h"
#include "MemService.h"
#include "../gui/AppContext.h"
#include "../socket/SocketCommand.h"
#include "../socket/client_singleton.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace Mem {
namespace {

const char* memoryTypeName(int type) {
    switch (type) {
    case MemType_Null: return "Null";
    case MemType_IO: return "IO";
    case MemType_Syscall: return "Syscall";
    case MemType_Kernel: return "Kernel";
    case MemType_SysHook: return "SysHook";
    default: return "Unknown";
    }
}

bool contextMatches(const OperationContext& context,
                    const SocketCommand::TransactionLease& transaction) {
    return context.target && transaction &&
           transaction.generation() == context.connectionGeneration &&
           AppContext::Get().matchesStableTarget(
               *context.target, transaction.generation());
}

BreakpointMutationBackendResult toBreakpointBackendResult(
    const BreakpointMutationIoResult& io) {
    return BreakpointMutationBackendResult{
        io.requestStarted, io.responseReceived, io.applied};
}

UxnOperationBackendResult toUxnBackendResult(
    const UxnOperationIoResult& io) {
    return UxnOperationBackendResult{
        io.requestStarted, io.responseReceived, io.applied, io.errorCode};
}

UxnRegisters toUxnRegisters(const CeUxnRegisters& source) {
    UxnRegisters output;
    std::copy(std::begin(source.registers), std::end(source.registers),
              output.general.begin());
    output.stackPointer = source.stackPointer;
    output.programCounter = source.programCounter;
    output.pstate = source.pstate;
    return output;
}

CeUxnRegisters toWireUxnRegisters(const UxnRegisters& source) {
    CeUxnRegisters output{};
    std::copy(source.general.begin(), source.general.end(),
              std::begin(output.registers));
    output.stackPointer = source.stackPointer;
    output.programCounter = source.programCounter;
    output.pstate = source.pstate;
    return output;
}

UxnEvent toUxnEvent(const CeUxnEvent& source) {
    UxnEvent output;
    output.slot = source.slot;
    output.pid = source.pid;
    output.tid = source.tid;
    output.state = static_cast<UxnState>(source.state);
    output.sequence = source.sequence;
    output.address = source.address;
    output.page = source.page;
    output.faultAddress = source.faultAddress;
    output.esr = source.esr;
    output.hits = source.hits;
    output.falseHits = source.falseHits;
    output.registers = toUxnRegisters(source.registers);
    for (size_t i = 0; i < output.fpsimd.vector.size(); ++i) {
        output.fpsimd.vector[i] = UxnFpRegister{
            source.fpsimd.registers[i].low,
            source.fpsimd.registers[i].high};
    }
    output.fpsimd.fpsr = source.fpsimd.fpsr;
    output.fpsimd.fpcr = source.fpsimd.fpcr;
    output.fpsimd.valid =
        (source.fpsimd.flags & CE_UXN_FPSIMD_VALID) != 0;
    return output;
}

bool fetchSystemModules(std::vector<ModuleInfo>& modules) {
    std::vector<ModuleInfoItem> items;
    if (!FetchModuleList(items, PORT_MAIN)) {
        return false;
    }
    modules.clear();
    modules.reserve(items.size());
    for (auto& item : items) {
        ModuleInfo module;
        module.base = item.base;
        module.size = item.size;
        module.type = item.type;
        module.flag = item.flag;
        module.name = std::move(item.name);
        modules.push_back(std::move(module));
    }
    return true;
}

void clearUxnIfKernel() {
    int memoryType = 0;
    if (GetMemType(memoryType, PORT_MAIN) &&
        memoryType == MemType_Kernel) {
        (void)ClearUxnBreakpointsTracked(PORT_MAIN);
    }
}

class SystemReadTransaction final : public IMemReadTransaction {
public:
    explicit SystemReadTransaction(const OperationContext& context)
        : context_(context), transaction_(PORT_MAIN) {
        valid_ = contextMatches(context_, transaction_);
    }

    bool valid() const override {
        return valid_ && contextMatches(context_, transaction_);
    }

    bool fetchModules(std::vector<ModuleInfo>& modules) override {
        return valid() && fetchSystemModules(modules);
    }

    bool readMemory(uint64_t address,
                    uint32_t size,
                    std::vector<unsigned char>& bytes) override {
        return valid() &&
               ReadProcessMemoryBytes(address, size, bytes, PORT_MAIN);
    }

    bool readMemoryBatch(
        const std::vector<MemoryReadRequest>& requests,
        std::vector<MemoryBlock>& blocks) override {
        if (!valid()) {
            return false;
        }
        std::vector<std::pair<uint64_t, int32_t>> input;
        input.reserve(requests.size());
        for (const auto& request : requests) {
            input.emplace_back(request.address,
                               static_cast<int32_t>(request.size));
        }
        std::vector<std::pair<uint64_t, std::vector<uint8_t>>> output;
        if (!ReadBratchAddr(input, output, PORT_MAIN)) {
            return false;
        }
        blocks.clear();
        blocks.reserve(output.size());
        for (auto& item : output) {
            blocks.push_back(MemoryBlock{item.first, std::move(item.second), {}});
        }
        return true;
    }

private:
    OperationContext context_;
    SocketCommand::TransactionLease transaction_;
    bool valid_ = false;
};

class SystemSymbolTransaction final : public IMemSymbolTransaction {
public:
    explicit SystemSymbolTransaction(const OperationContext& context)
        : context_(context), transaction_(PORT_MAIN) {
        valid_ = contextMatches(context_, transaction_);
    }

    bool valid() const override {
        return valid_ && contextMatches(context_, transaction_);
    }

    bool fetchModules(std::vector<ModuleInfo>& modules) override {
        return valid() && fetchSystemModules(modules);
    }

    bool initialize(uint64_t moduleBase, int& totalCount) override {
        return valid() && SymbolInit(moduleBase, totalCount, PORT_MAIN);
    }

    bool fetch(size_t offset,
               size_t limit,
               std::vector<SymbolInfo>& symbols,
               int& totalCount) override {
        if (!valid() || offset > static_cast<size_t>((std::numeric_limits<int>::max)()) ||
            limit > static_cast<size_t>((std::numeric_limits<int>::max)())) {
            return false;
        }
        std::vector<std::pair<uint64_t, std::string>> raw;
        if (!SymbolGetList(static_cast<int>(offset), static_cast<int>(limit),
                           raw, &totalCount, PORT_MAIN)) {
            return false;
        }
        symbols.clear();
        symbols.reserve(raw.size());
        for (auto& item : raw) {
            symbols.push_back(SymbolInfo{item.first, std::move(item.second)});
        }
        return true;
    }

    bool find(uint64_t moduleBase,
              const std::string& name,
              uint64_t& address) override {
        return valid() && SymbolFind(moduleBase, name, address, PORT_MAIN);
    }

private:
    OperationContext context_;
    SocketCommand::TransactionLease transaction_;
    bool valid_ = false;
};

class SystemMemBackend final : public IMemBackend {
public:
    bool isConnected() const override {
        return IsMultiPortConnected();
    }

    bool isPoisoned() const override {
        return IsConnectionPoisoned();
    }

    uint64_t connectionGeneration() const override {
        return GetConnectionGeneration();
    }

    TargetSnapshot targetSnapshot() const override {
        return AppContext::Get().snapshotTarget(connectionGeneration());
    }

    std::string processName() const override {
        return AppContext::Get().getSelectedName();
    }

    bool connect(const std::string& host, uint16_t port) override {
        if (IsMultiPortConnected()) {
            clearUxnIfKernel();
        }
        return ConnectMultiPort(host, port);
    }

    bool disconnect() override {
        if (IsMultiPortConnected()) {
            clearUxnIfKernel();
        }
        DisconnectMultiPort();
        return true;
    }

    bool fetchServerVersion(int& version,
                            std::string& versionString) override {
        ServerVersionInfo info;
        if (!FetchServerVersion(info, PORT_MAIN)) {
            return false;
        }
        version = info.version;
        versionString = std::move(info.versionString);
        return true;
    }

    bool fetchMemoryType(int& type, std::string& name) override {
        if (!GetMemType(type, PORT_MAIN)) {
            return false;
        }
        name = memoryTypeName(type);
        return true;
    }

    DriverInitializationBackendResult initializeDriver(
        const OperationContext& context,
        const std::string& card,
        bool forceReclaimHardwareBreakpoints) override {
        SocketCommand::TransactionLease transaction(PORT_MAIN);
        if (!transaction ||
            transaction.generation() != context.connectionGeneration) {
            return {};
        }
        const DriverInitializationIoResult io =
            InitDriverTracked(card, PORT_MAIN);
        DriverInitializationBackendResult result;
        result.requestStarted = io.requestStarted;
        result.responseReceived = io.responseReceived;
        result.accepted = io.accepted;
        result.message = io.message;
        if (!io.accepted) {
            return result;
        }

        const KernelBreakpointReclaimIoResult reclaim =
            SetKernelBreakpointForceReclaim(
                forceReclaimHardwareBreakpoints, PORT_MAIN);
        result.reclaimRequestStarted = reclaim.requestStarted;
        result.reclaimResponseReceived = reclaim.responseReceived;
        result.reclaimApplied = reclaim.applied;
        if (!reclaim.responseReceived) {
            result.message = "driver initialized; breakpoint reclaim configuration response missing";
        } else if (!reclaim.applied) {
            result.message = "driver initialized; breakpoint reclaim configuration rejected";
        }
        return result;
    }

    bool fetchProcesses(const OperationContext& context,
                        std::vector<ProcessInfo>& processes) override {
        SocketCommand::TransactionLease transaction(PORT_MAIN);
        if (!transaction ||
            transaction.generation() != context.connectionGeneration) {
            return false;
        }
        std::vector<ProcessInfoItem> items;
        if (!FetchProcessList(items, PORT_MAIN)) {
            return false;
        }
        processes.clear();
        processes.reserve(items.size());
        for (auto& item : items) {
            processes.push_back(ProcessInfo{item.pid, std::move(item.name)});
        }
        return true;
    }

    bool openProcess(const OperationContext& context,
                     int pid,
                     const std::string& name) override {
        auto requestLease = GetSocketMgr().AcquireRequestLease();
        if (!requestLease || !requestLease.isCurrent() ||
            requestLease.generation() != context.connectionGeneration) {
            return false;
        }
        auto mutation = AppContext::Get().beginTargetMutation(
            context.target, requestLease.generation());
        if (!mutation) {
            return false;
        }

        SocketCommand::TransactionLease transaction(PORT_MAIN);
        if (!transaction ||
            transaction.generation() != requestLease.generation()) {
            mutation->clear();
            return false;
        }

        const TargetSnapshot previous = mutation->previousTarget();
        if (previous.isAttached()) {
            clearUxnIfKernel();
            (void)ClearTrackedKernelBreakpoints(PORT_MAIN);
            (void)CloseProcessHandle(previous.processHandle, PORT_MAIN);
        }
        if (!requestLease.isCurrent()) {
            mutation->clear();
            return false;
        }

        int handle = 0;
        if (!OpenProcessHandle(pid, handle, PORT_MAIN) || handle == 0 ||
            !requestLease.isCurrent()) {
            mutation->clear();
            return false;
        }
        mutation->publish(pid, handle, name);
        return true;
    }

    std::unique_ptr<IMemReadTransaction> beginReadTransaction(
        const OperationContext& context) override {
        auto transaction = std::make_unique<SystemReadTransaction>(context);
        if (!transaction->valid()) {
            return nullptr;
        }
        return transaction;
    }

    std::unique_ptr<IMemSymbolTransaction> beginSymbolTransaction(
        const OperationContext& context) override {
        auto transaction = std::make_unique<SystemSymbolTransaction>(context);
        if (!transaction->valid()) {
            return nullptr;
        }
        return transaction;
    }

    bool readMemory(const OperationContext& context,
                    uint64_t address,
                    uint32_t size,
                    std::vector<unsigned char>& bytes) override {
        SystemReadTransaction transaction(context);
        return transaction.valid() &&
               transaction.readMemory(address, size, bytes);
    }

    bool readMemoryBatch(
        const OperationContext& context,
        const std::vector<MemoryReadRequest>& requests,
        std::vector<MemoryBlock>& blocks) override {
        SystemReadTransaction transaction(context);
        return transaction.valid() &&
               transaction.readMemoryBatch(requests, blocks);
    }

    MemoryWriteBackendResult writeMemory(
        const OperationContext& context,
        uint64_t address,
        const std::vector<unsigned char>& bytes) override {
        SocketCommand::TransactionLease transaction(PORT_MAIN);
        if (!contextMatches(context, transaction)) {
            return {};
        }
        const MemoryWriteIoResult result =
            WriteProcessMemoryDetailed(address, bytes, PORT_MAIN);
        return MemoryWriteBackendResult{
            result.requestStarted,
            result.responseReceived,
            result.writtenBytes};
    }

    BreakpointMutationBackendResult setBreakpoint(
        const OperationContext& context,
        uint64_t address,
        BreakpointAccess access,
        uint32_t size) override {
        SocketCommand::TransactionLease transaction(PORT_MAIN);
        if (!contextMatches(context, transaction))
            return {};
        return toBreakpointBackendResult(SetKernelBreakpointTracked(
            address, static_cast<uint32_t>(access), size, PORT_MAIN));
    }

    BreakpointMutationBackendResult removeBreakpoint(
        const OperationContext& context, uint64_t address) override {
        SocketCommand::TransactionLease transaction(PORT_MAIN);
        if (!contextMatches(context, transaction))
            return {};
        return toBreakpointBackendResult(
            RemoveKernelBreakpointTracked(address, PORT_MAIN));
    }

    BreakpointMutationBackendResult suspendBreakpoint(
        const OperationContext& context, uint64_t address) override {
        SocketCommand::TransactionLease transaction(PORT_MAIN);
        if (!contextMatches(context, transaction))
            return {};
        return toBreakpointBackendResult(
            SuspendKernelBreakpointTracked(address, PORT_MAIN));
    }

    BreakpointMutationBackendResult resumeBreakpoint(
        const OperationContext& context, uint64_t address) override {
        SocketCommand::TransactionLease transaction(PORT_MAIN);
        if (!contextMatches(context, transaction))
            return {};
        return toBreakpointBackendResult(
            ResumeKernelBreakpointTracked(address, PORT_MAIN));
    }

    bool fetchBreakpointHits(const OperationContext& context,
                             uint64_t address,
                             size_t limit,
                             std::vector<BreakpointHit>& hits,
                             size_t& total) override {
        SocketCommand::TransactionLease transaction(PORT_DEBUG);
        if (!contextMatches(context, transaction)) {
            return false;
        }
        std::vector<HW_HIT_INFO> raw;
        uint64_t totalHits = 0;
        if (!ReadKernelBreakpointInfo(
                address, raw, PORT_DEBUG, &totalHits)) {
            return false;
        }
        total = static_cast<size_t>(totalHits);
        const size_t begin = raw.size() > limit ? raw.size() - limit : 0;
        hits.clear();
        hits.reserve(raw.size() - begin);
        for (size_t i = begin; i < raw.size(); ++i) {
            BreakpointHit hit;
            hit.hitAddress = raw[i].hit_addr;
            hit.hitTime = raw[i].hit_time;
            std::copy(std::begin(raw[i].regs_info.regs),
                      std::end(raw[i].regs_info.regs),
                      hit.registers.begin());
            hit.stackPointer = raw[i].regs_info.sp;
            hit.programCounter = raw[i].regs_info.pc;
            hit.pstate = raw[i].regs_info.pstate;
            hit.originalX0 = raw[i].regs_info.orig_x0;
            hit.syscallNumber = raw[i].regs_info.syscallno;
            hits.push_back(std::move(hit));
        }
        return true;
    }

    bool fetchBreakpointSlots(
        const OperationContext& context, uint32_t capacity,
        std::vector<BreakpointThreadSlots>& threads) override {
        SocketCommand::TransactionLease transaction(PORT_MAIN);
        if (!contextMatches(context, transaction)) {
            return false;
        }
        std::vector<KernelBreakpointThreadInfo> raw;
        if (!QueryKernelBreakpointThreads(raw, capacity, PORT_MAIN)) {
            return false;
        }
        threads.clear();
        threads.reserve(raw.size());
        for (auto& source : raw) {
            BreakpointThreadSlots thread;
            thread.tid = source.tid;
            thread.querySucceeded = source.querySucceeded;
            thread.errorCode = source.errorCode;
            thread.count = source.count;
            thread.totalCount = source.totalCount;
            thread.brpCount = source.brpCount;
            thread.wrpCount = source.wrpCount;
            thread.enabledCount = source.enabledCount;
            thread.activeCount = source.activeCount;
            thread.perfCount = source.perfCount;
            thread.ptraceCount = source.ptraceCount;
            thread.moduleCount = source.moduleCount;
            thread.slots.reserve(source.slots.size());
            for (const auto& slot : source.slots) {
                thread.slots.push_back(BreakpointSlot{
                    slot.eventId, slot.moduleHandle, slot.address, slot.tid,
                    slot.onCpu, slot.type, slot.length, slot.state,
                    slot.source, slot.flags});
            }
            threads.push_back(std::move(thread));
        }
        return true;
    }

    UxnInstallBackendResult installUxnBreakpoint(
        const OperationContext& context, uint64_t address) override {
        SocketCommand::TransactionLease transaction(PORT_MAIN);
        if (!contextMatches(context, transaction)) {
            return {};
        }
        const UxnInstallIoResult io =
            InstallUxnBreakpointTracked(address, 0, PORT_MAIN);
        UxnInstallBackendResult output;
        static_cast<UxnOperationBackendResult&>(output) =
            toUxnBackendResult(io);
        output.pid = io.install.pid;
        output.flags = io.install.flags;
        output.address = io.install.address;
        output.slot = io.install.slot;
        return output;
    }

    UxnOperationBackendResult removeUxnBreakpoint(
        const OperationContext& context, uint64_t address) override {
        SocketCommand::TransactionLease transaction(PORT_MAIN);
        if (!contextMatches(context, transaction)) {
            return {};
        }
        return toUxnBackendResult(
            RemoveUxnBreakpointTracked(address, PORT_MAIN));
    }

    UxnWaitBackendResult waitUxnBreakpoint(
        const OperationContext& context, uint32_t slot,
        uint32_t timeoutMs, uint64_t lastSequence) override {
        SocketCommand::TransactionLease transaction(PORT_DEBUG);
        if (!contextMatches(context, transaction)) {
            return {};
        }
        const UxnWaitIoResult io = WaitUxnBreakpointTracked(
            slot, timeoutMs, lastSequence, PORT_DEBUG);
        UxnWaitBackendResult output;
        static_cast<UxnOperationBackendResult&>(output) =
            toUxnBackendResult(io);
        output.event = toUxnEvent(io.event);
        return output;
    }

    UxnOperationBackendResult resumeUxnBreakpoint(
        const OperationContext& context, uint32_t slot,
        bool writeRegisters, const UxnRegisters& registers) override {
        SocketCommand::TransactionLease transaction(PORT_MAIN);
        if (!contextMatches(context, transaction)) {
            return {};
        }
        CeUxnResume request{};
        request.slot = slot;
        request.flags = writeRegisters ? CE_UXN_RESUME_SET_REGS : 0;
        if (writeRegisters) {
            request.registers = toWireUxnRegisters(registers);
        }
        return toUxnBackendResult(
            ResumeUxnBreakpointTracked(request, PORT_MAIN));
    }

    UxnStatusBackendResult queryUxnBreakpointStatus(
        const OperationContext& context, uint32_t slot) override {
        SocketCommand::TransactionLease transaction(PORT_MAIN);
        if (!contextMatches(context, transaction)) {
            return {};
        }
        const UxnStatusIoResult io =
            QueryUxnBreakpointStatusTracked(slot, PORT_MAIN);
        UxnStatusBackendResult output;
        static_cast<UxnOperationBackendResult&>(output) =
            toUxnBackendResult(io);
        output.status.slot = io.status.slot;
        output.status.used = io.status.used != 0;
        output.status.pid = io.status.pid;
        output.status.tid = io.status.tid;
        output.status.state = static_cast<UxnState>(io.status.state);
        output.status.lastError = io.status.lastError;
        output.status.address = io.status.address;
        output.status.page = io.status.page;
        output.status.hits = io.status.hits;
        output.status.falseHits = io.status.falseHits;
        output.status.stepHits = io.status.stepHits;
        output.status.resumes = io.status.resumes;
        output.status.sequence = io.status.sequence;
        return output;
    }

    UxnOperationBackendResult clearUxnBreakpoints(
        const OperationContext& context) override {
        SocketCommand::TransactionLease transaction(PORT_MAIN);
        if (!transaction ||
            transaction.generation() != context.connectionGeneration) {
            return {};
        }
        return toUxnBackendResult(ClearUxnBreakpointsTracked(PORT_MAIN));
    }
};

} // namespace

IMemService& getSystemMemService() {
    static SystemMemBackend backend;
    static MemService service(backend);
    return service;
}

} // namespace Mem
