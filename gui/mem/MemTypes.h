#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Mem {

using CancellationToken = std::shared_ptr<std::atomic<bool>>;

struct TargetSnapshot {
    int pid = 0;
    int processHandle = 0;
    uint64_t processRevision = 0;
    uint64_t connectionGeneration = 0;

    bool isAttached() const {
        return pid > 0 && processHandle != 0;
    }
};

inline bool operator==(const TargetSnapshot& lhs, const TargetSnapshot& rhs) {
    return lhs.pid == rhs.pid &&
           lhs.processHandle == rhs.processHandle &&
           lhs.processRevision == rhs.processRevision &&
           lhs.connectionGeneration == rhs.connectionGeneration;
}

inline bool operator!=(const TargetSnapshot& lhs, const TargetSnapshot& rhs) {
    return !(lhs == rhs);
}

struct OperationContext {
    uint64_t connectionGeneration = 0;
    std::optional<TargetSnapshot> target;
    CancellationToken cancellation;
    std::chrono::steady_clock::time_point deadline =
        (std::chrono::steady_clock::time_point::max)();
};

struct ConnectionSnapshot {
    bool connected = false;
    bool poisoned = false;
    uint64_t generation = 0;
};

struct Status {
    ConnectionSnapshot connection;
    TargetSnapshot target;
    std::string processName;
};

struct ServerVersion {
    int version = 0;
    std::string versionString;
};

struct MemoryTypeInfo {
    int type = 0;
    std::string name;
};

struct ConnectRequest {
    std::string host;
    uint16_t port = 0;
};

struct ConnectionReceipt {
    bool connected = false;
    uint64_t generation = 0;
};

struct DisconnectReceipt {
    bool wasConnected = false;
    uint64_t generation = 0;
};

struct DriverInitializeRequest {
    std::string card;
    bool forceReclaimHardwareBreakpoints = false;
};

struct DriverInitializationBackendResult {
    bool requestStarted = false;
    bool responseReceived = false;
    bool accepted = false;
    std::string message;
    // 驱动初始化成功后的 Kernel 断点槽抢占策略同步结果。
    bool reclaimRequestStarted = false;
    bool reclaimResponseReceived = false;
    bool reclaimApplied = false;
};

struct DriverInitializationReceipt {
    std::string message;
    bool completedAfterCancelRequest = false;
    bool completedAfterDeadline = false;
    uint64_t connectionGeneration = 0;
};

struct ProcessInfo {
    int pid = 0;
    std::string name;
};

struct ProcessListRequest {
    std::string filter;
    size_t offset = 0;
    size_t limit = 200;
};

struct ProcessPage {
    std::vector<ProcessInfo> items;
    size_t total = 0;
    size_t offset = 0;
    std::optional<size_t> nextOffset;
};

struct OpenProcessRequest {
    int pid = 0;
    std::string name;
};

struct OpenProcessResult {
    TargetSnapshot target;
    std::string name;
};

struct ModuleInfo {
    uint64_t base = 0;
    uint64_t size = 0;
    int type = 0;
    int flag = 0;
    std::string name;
};

struct ModuleListRequest {
    std::string filter;
    size_t offset = 0;
    size_t limit = 200;
};

struct ModulePage {
    std::vector<ModuleInfo> items;
    size_t total = 0;
    size_t offset = 0;
    std::optional<size_t> nextOffset;
    TargetSnapshot target;
};

struct ModuleResolveRequest {
    std::string name;
};

struct ResolvedModule {
    ModuleInfo module;
    TargetSnapshot target;
};

struct PointerResolveRequest {
    std::string moduleName;
    uint64_t baseOffset = 0;
    std::vector<uint64_t> offsets;
    bool dereferenceFinal = true;
};

struct PointerResolution {
    ModuleInfo module;
    uint64_t startAddress = 0;
    uint64_t address = 0;
    size_t dereferenceCount = 0;
    bool dereferencedFinal = false;
    TargetSnapshot target;
};

struct MemoryReadRequest {
    uint64_t address = 0;
    uint32_t size = 0;
};

struct MemoryBlock {
    uint64_t address = 0;
    std::vector<unsigned char> bytes;
    TargetSnapshot target;
};

struct MemoryBatchReadRequest {
    std::vector<MemoryReadRequest> items;
};

struct MemoryBatch {
    std::vector<MemoryBlock> items;
    TargetSnapshot target;
};

struct MemoryWriteRequest {
    uint64_t address = 0;
    std::vector<unsigned char> bytes;
};

struct MemoryWriteBackendResult {
    bool requestStarted = false;
    bool responseReceived = false;
    int32_t writtenBytes = 0;
};

struct WriteReceipt {
    uint64_t address = 0;
    uint32_t requestedBytes = 0;
    uint32_t writtenBytes = 0;
    bool completedAfterCancelRequest = false;
    bool completedAfterDeadline = false;
    TargetSnapshot target;
};

enum class BreakpointAccess : uint32_t {
    Read = 1,
    Write = 2,
    ReadWrite = 3,
    Execute = 4,
};

enum class BreakpointAction {
    Set,
    Remove,
    Suspend,
    Resume,
};

struct BreakpointMutationBackendResult {
    bool requestStarted = false;
    bool responseReceived = false;
    bool applied = false;
};

struct BreakpointSetRequest {
    uint64_t address = 0;
    BreakpointAccess access = BreakpointAccess::Write;
    uint32_t size = 4;
};

struct BreakpointAddressRequest {
    uint64_t address = 0;
};

struct BreakpointHitBatchRequest {
    uint64_t address = 0;
    size_t limit = 50000;
};

struct BreakpointMutationReceipt {
    uint64_t address = 0;
    BreakpointAction action = BreakpointAction::Set;
    TargetSnapshot target;
};

struct BreakpointHit {
    uint64_t hitAddress = 0;
    uint64_t hitTime = 0;
    std::array<uint64_t, 31> registers{};
    uint64_t stackPointer = 0;
    uint64_t programCounter = 0;
    uint64_t pstate = 0;
    uint64_t originalX0 = 0;
    uint64_t syscallNumber = 0;
};

struct BreakpointHitBatch {
    uint64_t address = 0;
    std::vector<BreakpointHit> items;
    size_t available = 0;
    size_t dropped = 0;
    TargetSnapshot target;
};

struct BreakpointSlot {
    uint64_t eventId = 0;
    uint64_t moduleHandle = 0;
    uint64_t address = 0;
    int32_t tid = 0;
    int32_t onCpu = 0;
    uint32_t type = 0;
    uint32_t length = 0;
    uint32_t state = 0;
    uint32_t source = 0;
    uint32_t flags = 0;
};

struct BreakpointThreadSlots {
    int32_t tid = 0;
    bool querySucceeded = false;
    int32_t errorCode = 0;
    uint32_t count = 0;
    uint32_t totalCount = 0;
    uint32_t brpCount = 0;
    uint32_t wrpCount = 0;
    uint32_t enabledCount = 0;
    uint32_t activeCount = 0;
    uint32_t perfCount = 0;
    uint32_t ptraceCount = 0;
    uint32_t moduleCount = 0;
    std::vector<BreakpointSlot> slots;
};

struct BreakpointSlotsSnapshot {
    std::vector<BreakpointThreadSlots> threads;
    TargetSnapshot target;
};

enum class UxnState : uint32_t {
    Empty = 0,
    Armed = 1,
    Paused = 2,
    Stepping = 3,
};

struct UxnRegisters {
    std::array<uint64_t, 31> general{};
    uint64_t stackPointer = 0;
    uint64_t programCounter = 0;
    uint64_t pstate = 0;
};

struct UxnFpRegister {
    uint64_t low = 0;
    uint64_t high = 0;
};

struct UxnFpsimdRegisters {
    std::array<UxnFpRegister, 32> vector{};
    uint32_t fpsr = 0;
    uint32_t fpcr = 0;
    bool valid = false;
};

struct UxnOperationBackendResult {
    bool requestStarted = false;
    bool responseReceived = false;
    bool applied = false;
    int32_t errorCode = 0;
};

struct UxnInstallBackendResult : UxnOperationBackendResult {
    uint32_t pid = 0;
    uint32_t flags = 0;
    uint64_t address = 0;
    uint32_t slot = 0;
};

struct UxnEvent {
    uint32_t slot = 0;
    uint32_t pid = 0;
    uint32_t tid = 0;
    UxnState state = UxnState::Empty;
    uint64_t sequence = 0;
    uint64_t address = 0;
    uint64_t page = 0;
    uint64_t faultAddress = 0;
    uint64_t esr = 0;
    uint64_t hits = 0;
    uint64_t falseHits = 0;
    UxnRegisters registers;
    UxnFpsimdRegisters fpsimd;
};

struct UxnWaitBackendResult : UxnOperationBackendResult {
    UxnEvent event;
};

struct UxnStatus {
    uint32_t slot = 0;
    bool used = false;
    uint32_t pid = 0;
    uint32_t tid = 0;
    UxnState state = UxnState::Empty;
    int32_t lastError = 0;
    uint64_t address = 0;
    uint64_t page = 0;
    uint64_t hits = 0;
    uint64_t falseHits = 0;
    uint64_t stepHits = 0;
    uint64_t resumes = 0;
    uint64_t sequence = 0;
    TargetSnapshot target;
};

struct UxnStatusBackendResult : UxnOperationBackendResult {
    UxnStatus status;
};

struct UxnInstallRequest {
    uint64_t address = 0;
};

struct UxnInstallReceipt {
    uint32_t slot = 0;
    uint64_t address = 0;
    TargetSnapshot target;
};

struct UxnRemoveRequest {
    uint64_t address = 0;
};

struct UxnWaitRequest {
    uint32_t slot = (std::numeric_limits<uint32_t>::max)();
    uint32_t timeoutMs = 1000;
    uint64_t lastSequence = 0;
};

struct UxnResumeRequest {
    uint32_t slot = 0;
    bool writeRegisters = false;
    UxnRegisters registers;
};

struct UxnMutationReceipt {
    uint32_t slot = 0;
    uint64_t address = 0;
    TargetSnapshot target;
};

struct UxnStatusRequest {
    uint32_t slot = 0;
};

struct UxnClearReceipt {
    TargetSnapshot target;
};

struct SymbolInfo {
    uint64_t address = 0;
    std::string name;
};

struct SymbolTableRequest {
    uint64_t moduleBase = 0;
};

struct SymbolListRequest {
    uint64_t moduleBase = 0;
    size_t offset = 0;
    size_t limit = 100;
};

struct SymbolResolveRequest {
    uint64_t moduleBase = 0;
    std::string name;
};

struct SymbolTable {
    ModuleInfo module;
    std::vector<SymbolInfo> items;
    TargetSnapshot target;
};

struct SymbolPage {
    ModuleInfo module;
    std::vector<SymbolInfo> items;
    size_t total = 0;
    size_t offset = 0;
    std::optional<size_t> nextOffset;
    TargetSnapshot target;
};

struct ResolvedSymbol {
    ModuleInfo module;
    std::string name;
    uint64_t address = 0;
    TargetSnapshot target;
};

inline constexpr uint32_t kMaxMemoryReadBytes = 16u * 1024u * 1024u;
inline constexpr uint32_t kMaxMemoryWriteBytes = 1024u * 1024u;
inline constexpr size_t kMaxMemoryBatchCount = 4096;
inline constexpr size_t kMaxMemoryBatchBytes = 16u * 1024u * 1024u;
inline constexpr size_t kMaxProcessPageSize = 1000;
inline constexpr size_t kMaxProcessNameBytesTotal = 64u * 1024u * 1024u;
inline constexpr size_t kMaxModulePageSize = 1000;
inline constexpr size_t kMaxModuleCount = 65536;
inline constexpr size_t kMaxModuleNameBytesTotal = 64u * 1024u * 1024u;
inline constexpr size_t kMaxPointerOffsetCount = 1024;
inline constexpr size_t kMaxSymbolPageSize = 1000;
inline constexpr size_t kMaxSymbolCount = 1000000;
inline constexpr size_t kMaxSymbolNameBytesTotal = 64u * 1024u * 1024u;
inline constexpr size_t kMaxBreakpointHitCount = 100000;
inline constexpr uint32_t kMaxBreakpointQueryEntries = 64;
inline constexpr size_t kMaxBreakpointQueryThreads = 65536;
inline constexpr uint32_t kMaxUxnSlots = 16;
inline constexpr uint32_t kUxnWaitAnySlot =
    (std::numeric_limits<uint32_t>::max)();
inline constexpr uint32_t kMaxUxnWaitTimeoutMs = 60000;
inline constexpr size_t kMaxTextBytes = 4096;
inline constexpr size_t kMaxDriverCardBytes = 4096;

} // namespace Mem
