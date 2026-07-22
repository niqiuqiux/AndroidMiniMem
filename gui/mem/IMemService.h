#pragma once

#include "MemResult.h"
#include "MemTypes.h"

namespace Mem {

class IMemService {
public:
    virtual ~IMemService() = default;

    virtual OperationContext captureContext(bool includeTarget) const = 0;
    virtual ConnectionSnapshot connectionSnapshot() const = 0;
    virtual Result<Status> status(const OperationContext& context) = 0;
    virtual Result<ServerVersion> serverVersion(
        const OperationContext& context) = 0;
    virtual Result<MemoryTypeInfo> memoryType(
        const OperationContext& context) = 0;
    virtual Result<ConnectionReceipt> connect(
        const OperationContext& context,
        const ConnectRequest& request) = 0;
    virtual Result<DisconnectReceipt> disconnect(
        const OperationContext& context) = 0;
    virtual Result<DriverInitializationReceipt> initializeDriver(
        const OperationContext& context,
        const DriverInitializeRequest& request) = 0;
    virtual Result<ProcessPage> listProcesses(
        const OperationContext& context,
        const ProcessListRequest& request) = 0;
    virtual Result<OpenProcessResult> openProcess(
        const OperationContext& context,
        const OpenProcessRequest& request) = 0;
    virtual Result<ModulePage> listModules(
        const OperationContext& context,
        const ModuleListRequest& request) = 0;
    virtual Result<ResolvedModule> resolveModule(
        const OperationContext& context,
        const ModuleResolveRequest& request) = 0;
    virtual Result<PointerResolution> resolvePointer(
        const OperationContext& context,
        const PointerResolveRequest& request) = 0;
    virtual Result<MemoryBlock> readMemory(
        const OperationContext& context,
        const MemoryReadRequest& request) = 0;
    virtual Result<MemoryBatch> readMemoryBatch(
        const OperationContext& context,
        const MemoryBatchReadRequest& request) = 0;
    virtual Result<WriteReceipt> writeMemory(
        const OperationContext& context,
        const MemoryWriteRequest& request) = 0;
    virtual Result<BreakpointMutationReceipt> setBreakpoint(
        const OperationContext& context,
        const BreakpointSetRequest& request) = 0;
    virtual Result<BreakpointMutationReceipt> removeBreakpoint(
        const OperationContext& context,
        const BreakpointAddressRequest& request) = 0;
    virtual Result<BreakpointMutationReceipt> suspendBreakpoint(
        const OperationContext& context,
        const BreakpointAddressRequest& request) = 0;
    virtual Result<BreakpointMutationReceipt> resumeBreakpoint(
        const OperationContext& context,
        const BreakpointAddressRequest& request) = 0;
    virtual Result<BreakpointHitBatch> breakpointHits(
        const OperationContext& context,
        const BreakpointHitBatchRequest& request) = 0;
    virtual Result<BreakpointSlotsSnapshot> breakpointSlots(
        const OperationContext& context, uint32_t capacity = kMaxBreakpointQueryEntries) = 0;
    virtual Result<SymbolTable> loadSymbolTable(
        const OperationContext& context,
        const SymbolTableRequest& request) = 0;
    virtual Result<SymbolPage> listSymbols(
        const OperationContext& context,
        const SymbolListRequest& request) = 0;
    virtual Result<ResolvedSymbol> resolveSymbol(
        const OperationContext& context,
        const SymbolResolveRequest& request) = 0;
};

} // namespace Mem
