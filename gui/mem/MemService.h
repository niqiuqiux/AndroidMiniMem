#pragma once

#include "IMemBackend.h"
#include "IMemService.h"

#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Mem {

class MemService final : public IMemService {
public:
    explicit MemService(IMemBackend& backend);

    OperationContext captureContext(bool includeTarget) const override;
    ConnectionSnapshot connectionSnapshot() const override;
    Result<Status> status(const OperationContext& context) override;
    Result<ServerVersion> serverVersion(
        const OperationContext& context) override;
    Result<MemoryTypeInfo> memoryType(
        const OperationContext& context) override;
    Result<ConnectionReceipt> connect(
        const OperationContext& context,
        const ConnectRequest& request) override;
    Result<DisconnectReceipt> disconnect(
        const OperationContext& context) override;
    Result<DriverInitializationReceipt> initializeDriver(
        const OperationContext& context,
        const DriverInitializeRequest& request) override;
    Result<ProcessPage> listProcesses(
        const OperationContext& context,
        const ProcessListRequest& request) override;
    Result<OpenProcessResult> openProcess(
        const OperationContext& context,
        const OpenProcessRequest& request) override;
    Result<ModulePage> listModules(
        const OperationContext& context,
        const ModuleListRequest& request) override;
    Result<ResolvedModule> resolveModule(
        const OperationContext& context,
        const ModuleResolveRequest& request) override;
    Result<PointerResolution> resolvePointer(
        const OperationContext& context,
        const PointerResolveRequest& request) override;
    Result<MemoryBlock> readMemory(
        const OperationContext& context,
        const MemoryReadRequest& request) override;
    Result<MemoryBatch> readMemoryBatch(
        const OperationContext& context,
        const MemoryBatchReadRequest& request) override;
    Result<WriteReceipt> writeMemory(
        const OperationContext& context,
        const MemoryWriteRequest& request) override;
    Result<BreakpointMutationReceipt> setBreakpoint(
        const OperationContext& context,
        const BreakpointSetRequest& request) override;
    Result<BreakpointMutationReceipt> removeBreakpoint(
        const OperationContext& context,
        const BreakpointAddressRequest& request) override;
    Result<BreakpointMutationReceipt> suspendBreakpoint(
        const OperationContext& context,
        const BreakpointAddressRequest& request) override;
    Result<BreakpointMutationReceipt> resumeBreakpoint(
        const OperationContext& context,
        const BreakpointAddressRequest& request) override;
    Result<BreakpointHitBatch> breakpointHits(
        const OperationContext& context,
        const BreakpointHitBatchRequest& request) override;
    Result<BreakpointSlotsSnapshot> breakpointSlots(
        const OperationContext& context,
        uint32_t capacity = kMaxBreakpointQueryEntries) override;
    Result<SymbolTable> loadSymbolTable(
        const OperationContext& context,
        const SymbolTableRequest& request) override;
    Result<SymbolPage> listSymbols(
        const OperationContext& context,
        const SymbolListRequest& request) override;
    Result<ResolvedSymbol> resolveSymbol(
        const OperationContext& context,
        const SymbolResolveRequest& request) override;

private:
    std::optional<Error> validateContext(
        const OperationContext& context,
        bool requireConnected,
        bool requireTarget,
        bool checkCancellation) const;
    Result<BreakpointMutationReceipt> mutateBreakpoint(
        const OperationContext& context,
        uint64_t address,
        BreakpointAction action,
        const std::optional<BreakpointSetRequest>& setRequest = std::nullopt);

    IMemBackend& backend_;
    std::mutex connectionMutex_;
    std::mutex processMutex_;
    std::mutex breakpointMutex_;
    std::mutex symbolMutex_;
    TargetSnapshot breakpointCounterTarget_;
    std::unordered_map<uint64_t, size_t> breakpointHitTotals_;
};

} // namespace Mem
