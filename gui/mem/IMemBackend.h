#pragma once

#include "MemTypes.h"

#include <memory>
#include <string>
#include <vector>

namespace Mem {

class IMemReadTransaction {
public:
    virtual ~IMemReadTransaction() = default;
    virtual bool valid() const = 0;
    virtual bool fetchModules(std::vector<ModuleInfo>& modules) = 0;
    virtual bool readMemory(uint64_t address,
                            uint32_t size,
                            std::vector<unsigned char>& bytes) = 0;
    virtual bool readMemoryBatch(
        const std::vector<MemoryReadRequest>& requests,
        std::vector<MemoryBlock>& blocks) = 0;
};

class IMemSymbolTransaction {
public:
    virtual ~IMemSymbolTransaction() = default;
    virtual bool valid() const = 0;
    virtual bool fetchModules(std::vector<ModuleInfo>& modules) = 0;
    virtual bool initialize(uint64_t moduleBase, int& totalCount) = 0;
    virtual bool fetch(size_t offset,
                       size_t limit,
                       std::vector<SymbolInfo>& symbols,
                       int& totalCount) = 0;
    virtual bool find(uint64_t moduleBase,
                      const std::string& name,
                      uint64_t& address) = 0;
};

class IMemBackend {
public:
    virtual ~IMemBackend() = default;

    virtual bool isConnected() const = 0;
    virtual bool isPoisoned() const = 0;
    virtual uint64_t connectionGeneration() const = 0;
    virtual TargetSnapshot targetSnapshot() const = 0;
    virtual std::string processName() const = 0;

    virtual bool connect(const std::string& host, uint16_t port) = 0;
    virtual bool disconnect() = 0;
    virtual bool fetchServerVersion(int& version,
                                    std::string& versionString) = 0;
    virtual bool fetchMemoryType(int& type, std::string& name) = 0;
    virtual DriverInitializationBackendResult initializeDriver(
        const OperationContext& context,
        const std::string& card) = 0;
    virtual bool fetchProcesses(const OperationContext& context,
                                std::vector<ProcessInfo>& processes) = 0;
    virtual bool openProcess(const OperationContext& context,
                             int pid,
                             const std::string& name) = 0;

    virtual std::unique_ptr<IMemReadTransaction> beginReadTransaction(
        const OperationContext& context) = 0;
    virtual std::unique_ptr<IMemSymbolTransaction> beginSymbolTransaction(
        const OperationContext& context) = 0;

    virtual bool readMemory(const OperationContext& context,
                            uint64_t address,
                            uint32_t size,
                            std::vector<unsigned char>& bytes) = 0;
    virtual bool readMemoryBatch(
        const OperationContext& context,
        const std::vector<MemoryReadRequest>& requests,
        std::vector<MemoryBlock>& blocks) = 0;
    virtual MemoryWriteBackendResult writeMemory(
        const OperationContext& context,
        uint64_t address,
        const std::vector<unsigned char>& bytes) = 0;

    virtual BreakpointMutationBackendResult setBreakpoint(
        const OperationContext& context,
        uint64_t address,
        BreakpointAccess access,
        uint32_t size) = 0;
    virtual BreakpointMutationBackendResult removeBreakpoint(
        const OperationContext& context, uint64_t address) = 0;
    virtual BreakpointMutationBackendResult suspendBreakpoint(
        const OperationContext& context, uint64_t address) = 0;
    virtual BreakpointMutationBackendResult resumeBreakpoint(
        const OperationContext& context, uint64_t address) = 0;
    virtual bool fetchBreakpointHits(const OperationContext& context,
                                     uint64_t address,
                                     size_t limit,
                                     std::vector<BreakpointHit>& hits,
                                     size_t& total) = 0;
};

} // namespace Mem
