#include "MemService.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace Mem {
namespace {

using Clock = std::chrono::steady_clock;
constexpr int kKernelMemoryType = 3;

std::string trimAscii(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return value;
}

std::string moduleBaseName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool addAddressOffset(uint64_t base, uint64_t offset, uint64_t& result) {
    if (base > (std::numeric_limits<uint64_t>::max)() - offset) {
        return false;
    }
    result = base + offset;
    return true;
}

std::optional<uint64_t> decodePointer(
    const std::vector<unsigned char>& bytes) {
    if (bytes.size() != sizeof(uint64_t)) {
        return std::nullopt;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        value |= static_cast<uint64_t>(bytes[i]) << (i * 8u);
    }
    return value;
}

std::optional<Error> validateModules(const std::vector<ModuleInfo>& modules) {
    if (modules.size() > kMaxModuleCount) {
        return Error{ErrorCode::ProtocolError,
                     "module response exceeds the service limit", false};
    }
    size_t totalNameBytes = 0;
    for (const auto& module : modules) {
        if (module.base == 0 || module.size == 0 ||
            module.base >
                (std::numeric_limits<uint64_t>::max)() - module.size ||
            module.name.size() > kMaxTextBytes * 16 ||
            totalNameBytes >
                kMaxModuleNameBytesTotal - module.name.size()) {
            return Error{ErrorCode::ProtocolError,
                         "module response contains an invalid entry", false};
        }
        totalNameBytes += module.name.size();
    }
    return std::nullopt;
}

struct ModuleGroup {
    std::string key;
    ModuleInfo module;
};

std::vector<ModuleGroup> groupModules(const std::vector<ModuleInfo>& modules) {
    std::vector<ModuleGroup> groups;
    for (const auto& module : modules) {
        const std::string key = lowerAscii(module.name);
        auto found = std::find_if(groups.begin(), groups.end(),
                                  [&](const ModuleGroup& group) {
                                      return group.key == key;
                                  });
        if (found == groups.end()) {
            groups.push_back(ModuleGroup{key, module});
        } else if (module.base < found->module.base) {
            found->module = module;
        }
    }
    return groups;
}

std::optional<Error> resolveModuleByName(
    const std::vector<ModuleInfo>& modules,
    const std::string& rawQuery,
    ModuleInfo& resolved) {
    const std::string query = lowerAscii(trimAscii(rawQuery));
    if (query.empty()) {
        return Error{ErrorCode::InvalidArgument,
                     "module name must not be empty", false};
    }

    const auto groups = groupModules(modules);
    std::vector<const ModuleGroup*> full;
    std::vector<const ModuleGroup*> base;
    std::vector<const ModuleGroup*> substring;
    for (const auto& group : groups) {
        if (group.key == query) {
            full.push_back(&group);
        }
        if (lowerAscii(moduleBaseName(group.module.name)) == query) {
            base.push_back(&group);
        }
        if (group.key.find(query) != std::string::npos) {
            substring.push_back(&group);
        }
    }

    const std::vector<const ModuleGroup*>* matches = nullptr;
    if (!full.empty()) {
        matches = &full;
    } else if (!base.empty()) {
        matches = &base;
    } else {
        matches = &substring;
    }
    if (matches->empty()) {
        return Error{ErrorCode::InvalidArgument,
                     "module name did not match a loaded module", false};
    }
    if (matches->size() != 1) {
        return Error{ErrorCode::InvalidArgument,
                     "module name is ambiguous", false};
    }
    resolved = matches->front()->module;
    return std::nullopt;
}

std::optional<Error> resolveModuleByBase(
    const std::vector<ModuleInfo>& modules,
    uint64_t base,
    ModuleInfo& resolved) {
    const auto exact = std::find_if(modules.begin(), modules.end(),
                                    [&](const ModuleInfo& module) {
                                        return module.base == base;
                                    });
    if (exact == modules.end()) {
        return Error{ErrorCode::InvalidArgument,
                     "module base did not match a loaded module", false};
    }
    const std::string key = lowerAscii(exact->name);
    resolved = *exact;
    for (const auto& module : modules) {
        if (lowerAscii(module.name) == key && module.base < resolved.base) {
            resolved = module;
        }
    }
    return std::nullopt;
}

template<typename T>
Result<T> failureFrom(const Error& error) {
    return Result<T>::failure(error.code, error.message, error.retryable,
                              error.affectedBytes);
}

const char* breakpointActionName(BreakpointAction action) {
    switch (action) {
    case BreakpointAction::Set: return "set";
    case BreakpointAction::Remove: return "remove";
    case BreakpointAction::Suspend: return "suspend";
    case BreakpointAction::Resume: return "resume";
    }
    return "mutate";
}

} // namespace

MemService::MemService(IMemBackend& backend) : backend_(backend) {}

OperationContext MemService::captureContext(bool includeTarget) const {
    OperationContext context;
    context.connectionGeneration = backend_.connectionGeneration();
    if (includeTarget) {
        context.target = backend_.targetSnapshot();
    }
    return context;
}

ConnectionSnapshot MemService::connectionSnapshot() const {
    return ConnectionSnapshot{backend_.isConnected(), backend_.isPoisoned(),
                              backend_.connectionGeneration()};
}

std::optional<Error> MemService::validateContext(
    const OperationContext& context,
    bool requireConnected,
    bool requireTarget,
    bool checkCancellation) const {
    if (checkCancellation && context.cancellation &&
        context.cancellation->load(std::memory_order_acquire)) {
        return Error{ErrorCode::CancelRequested,
                     "operation was cancelled", false};
    }
    if (checkCancellation && Clock::now() >= context.deadline) {
        return Error{ErrorCode::Timeout,
                     "operation deadline has expired", true};
    }

    const uint64_t generation = backend_.connectionGeneration();
    if (generation != context.connectionGeneration) {
        if (backend_.isPoisoned()) {
            return Error{ErrorCode::ConnectionPoisoned,
                         "connection lost protocol synchronization; reconnect is required",
                         true};
        }
        return Error{ErrorCode::ConnectionChanged,
                     "connection changed while the operation was pending", true};
    }
    if (requireConnected && backend_.isPoisoned()) {
        return Error{ErrorCode::ConnectionPoisoned,
                     "connection is poisoned and must be reconnected", true};
    }
    if (requireConnected && !backend_.isConnected()) {
        return Error{ErrorCode::NotConnected,
                     "MiniMem is not connected to the Android server", true};
    }
    if (requireTarget) {
        if (!context.target || !context.target->isAttached()) {
            return Error{ErrorCode::NoTarget,
                         "no target process is attached", false};
        }
        if (context.target->connectionGeneration !=
            context.connectionGeneration) {
            return Error{ErrorCode::ConnectionChanged,
                         "target belongs to another connection generation", true};
        }
        if (backend_.targetSnapshot() != *context.target) {
            return Error{ErrorCode::TargetChanged,
                         "target process changed while the operation was pending",
                         false};
        }
    }
    return std::nullopt;
}

Result<Status> MemService::status(const OperationContext& context) {
    if (const auto error = validateContext(context, false, false, true)) {
        return failureFrom<Status>(*error);
    }
    Status result;
    result.connection = connectionSnapshot();
    result.target = backend_.targetSnapshot();
    result.processName = backend_.processName();
    const TargetSnapshot finalTarget = backend_.targetSnapshot();
    if (const auto error = validateContext(context, false, false, true)) {
        return failureFrom<Status>(*error);
    }
    if (finalTarget != result.target) {
        return Result<Status>::failure(
            ErrorCode::TargetChanged,
            "status changed while it was being collected", true);
    }
    return Result<Status>::success(std::move(result));
}

Result<ServerVersion> MemService::serverVersion(
    const OperationContext& context) {
    if (const auto error = validateContext(context, true, false, true)) {
        return failureFrom<ServerVersion>(*error);
    }
    ServerVersion result;
    if (!backend_.fetchServerVersion(result.version, result.versionString)) {
        if (const auto error = validateContext(context, true, false, false)) {
            return failureFrom<ServerVersion>(*error);
        }
        return Result<ServerVersion>::failure(
            ErrorCode::ProtocolError, "failed to fetch server version", true);
    }
    if (const auto error = validateContext(context, true, false, true)) {
        return failureFrom<ServerVersion>(*error);
    }
    return Result<ServerVersion>::success(std::move(result));
}

Result<MemoryTypeInfo> MemService::memoryType(
    const OperationContext& context) {
    if (const auto error = validateContext(context, true, false, true)) {
        return failureFrom<MemoryTypeInfo>(*error);
    }
    MemoryTypeInfo result;
    if (!backend_.fetchMemoryType(result.type, result.name)) {
        if (const auto error = validateContext(context, true, false, false)) {
            return failureFrom<MemoryTypeInfo>(*error);
        }
        return Result<MemoryTypeInfo>::failure(
            ErrorCode::ProtocolError, "failed to fetch memory type", true);
    }
    if (const auto error = validateContext(context, true, false, true)) {
        return failureFrom<MemoryTypeInfo>(*error);
    }
    return Result<MemoryTypeInfo>::success(std::move(result));
}

Result<ConnectionReceipt> MemService::connect(
    const OperationContext& context,
    const ConnectRequest& request) {
    if (request.host.empty() || request.host.size() > kMaxTextBytes ||
        request.port == 0) {
        return Result<ConnectionReceipt>::failure(
            ErrorCode::InvalidArgument, "host and port are required");
    }
    if (const auto error = validateContext(context, false, false, true)) {
        return failureFrom<ConnectionReceipt>(*error);
    }
    std::lock_guard<std::mutex> lock(connectionMutex_);
    if (backend_.connectionGeneration() != context.connectionGeneration) {
        return Result<ConnectionReceipt>::failure(
            ErrorCode::ConnectionChanged,
            "connection changed before connect started", true);
    }
    if (!backend_.connect(request.host, request.port)) {
        return Result<ConnectionReceipt>::failure(
            ErrorCode::ProtocolError,
            "failed to connect all Android server ports", true);
    }
    if (!backend_.isConnected() || backend_.isPoisoned()) {
        return Result<ConnectionReceipt>::failure(
            ErrorCode::ProtocolError,
            "connection backend did not enter a usable connected state",
            true);
    }
    return Result<ConnectionReceipt>::success(
        ConnectionReceipt{true, backend_.connectionGeneration()});
}

Result<DisconnectReceipt> MemService::disconnect(
    const OperationContext& context) {
    if (const auto error = validateContext(context, false, false, true)) {
        return failureFrom<DisconnectReceipt>(*error);
    }
    std::lock_guard<std::mutex> lock(connectionMutex_);
    if (backend_.connectionGeneration() != context.connectionGeneration) {
        return Result<DisconnectReceipt>::failure(
            ErrorCode::ConnectionChanged,
            "connection changed before disconnect started", true);
    }
    const bool wasConnected = backend_.isConnected() || backend_.isPoisoned();
    if (!backend_.disconnect()) {
        return Result<DisconnectReceipt>::failure(
            ErrorCode::InternalError, "failed to disconnect socket clients");
    }
    if (backend_.isConnected() || backend_.isPoisoned()) {
        return Result<DisconnectReceipt>::failure(
            ErrorCode::InternalError,
            "connection backend remained active after disconnect");
    }
    return Result<DisconnectReceipt>::success(
        DisconnectReceipt{wasConnected, backend_.connectionGeneration()});
}

Result<DriverInitializationReceipt> MemService::initializeDriver(
    const OperationContext& context,
    const DriverInitializeRequest& request) {
    if (request.card.empty() || request.card.size() > kMaxDriverCardBytes) {
        return Result<DriverInitializationReceipt>::failure(
            ErrorCode::InvalidArgument,
            "driver card name must contain 1 to 4096 bytes");
    }
    if (const auto error = validateContext(context, true, false, true)) {
        return failureFrom<DriverInitializationReceipt>(*error);
    }
    const DriverInitializationBackendResult backendResult =
        backend_.initializeDriver(
            context, request.card, request.forceReclaimHardwareBreakpoints);
    if (!backendResult.responseReceived) {
        if (backendResult.requestStarted) {
            return Result<DriverInitializationReceipt>::failure(
                ErrorCode::CompletionUnknown,
                "driver initialization was sent but its completion could not be confirmed; reconnect before continuing and do not retry automatically",
                false);
        }
        if (const auto error = validateContext(context, true, false, true)) {
            return failureFrom<DriverInitializationReceipt>(*error);
        }
        return Result<DriverInitializationReceipt>::failure(
            ErrorCode::ProtocolError,
            "driver initialization could not be sent", true);
    }
    if (!backendResult.accepted) {
        return Result<DriverInitializationReceipt>::failure(
            ErrorCode::PermissionDenied,
            backendResult.message.empty()
                ? "Android server rejected driver initialization"
                : backendResult.message,
            false);
    }
    if (!backendResult.reclaimResponseReceived) {
        if (backendResult.reclaimRequestStarted) {
            return Result<DriverInitializationReceipt>::failure(
                ErrorCode::CompletionUnknown,
                "driver initialized, but Kernel breakpoint reclaim configuration completion could not be confirmed; reconnect before continuing",
                false);
        }
        return Result<DriverInitializationReceipt>::failure(
            ErrorCode::ProtocolError,
            "driver initialized, but Kernel breakpoint reclaim configuration could not be sent",
            true);
    }
    if (!backendResult.reclaimApplied) {
        return Result<DriverInitializationReceipt>::failure(
            ErrorCode::ProtocolError,
            "driver initialized, but Kernel breakpoint reclaim configuration was rejected",
            false);
    }
    if (const auto error = validateContext(context, true, false, false)) {
        return Result<DriverInitializationReceipt>::failure(
            ErrorCode::CompletionUnknown,
            "server confirmed driver initialization, but the original connection context is no longer current: " +
                error->message,
            false);
    }

    DriverInitializationReceipt receipt;
    receipt.message = backendResult.message;
    receipt.completedAfterCancelRequest =
        context.cancellation &&
        context.cancellation->load(std::memory_order_acquire);
    receipt.completedAfterDeadline = Clock::now() >= context.deadline;
    receipt.connectionGeneration = context.connectionGeneration;
    return Result<DriverInitializationReceipt>::success(std::move(receipt));
}

Result<ProcessPage> MemService::listProcesses(
    const OperationContext& context,
    const ProcessListRequest& request) {
    if (request.limit == 0 || request.limit > kMaxProcessPageSize ||
        request.filter.size() > kMaxTextBytes) {
        return Result<ProcessPage>::failure(
            ErrorCode::InvalidArgument,
            "invalid process list filter or page size");
    }
    if (const auto error = validateContext(context, true, false, true)) {
        return failureFrom<ProcessPage>(*error);
    }
    std::vector<ProcessInfo> processes;
    if (!backend_.fetchProcesses(context, processes)) {
        if (const auto error = validateContext(context, true, false, false)) {
            return failureFrom<ProcessPage>(*error);
        }
        return Result<ProcessPage>::failure(
            ErrorCode::ProtocolError, "failed to fetch process list", true);
    }

    const std::string filter = lowerAscii(request.filter);
    std::vector<ProcessInfo> filtered;
    filtered.reserve(processes.size());
    size_t totalNameBytes = 0;
    for (auto& process : processes) {
        if (process.pid <= 0 || process.name.size() > kMaxTextBytes * 16 ||
            totalNameBytes >
                kMaxProcessNameBytesTotal - process.name.size()) {
            return Result<ProcessPage>::failure(
                ErrorCode::ProtocolError,
                "process response contains an invalid entry");
        }
        totalNameBytes += process.name.size();
        if (filter.empty() ||
            lowerAscii(process.name).find(filter) != std::string::npos) {
            filtered.push_back(std::move(process));
        }
    }

    ProcessPage page;
    page.total = filtered.size();
    page.offset = std::min(request.offset, page.total);
    const size_t end = std::min(page.total, page.offset + request.limit);
    for (size_t i = page.offset; i < end; ++i) {
        page.items.push_back(std::move(filtered[i]));
    }
    if (end < page.total) {
        page.nextOffset = end;
    }
    if (const auto error = validateContext(context, true, false, true)) {
        return failureFrom<ProcessPage>(*error);
    }
    return Result<ProcessPage>::success(std::move(page));
}

Result<OpenProcessResult> MemService::openProcess(
    const OperationContext& context,
    const OpenProcessRequest& request) {
    if (request.pid <= 0 || request.name.size() > kMaxTextBytes) {
        return Result<OpenProcessResult>::failure(
            ErrorCode::InvalidArgument,
            "pid must be positive and process name must be bounded");
    }
    if (const auto error = validateContext(context, true, false, true)) {
        return failureFrom<OpenProcessResult>(*error);
    }
    if (context.target && backend_.targetSnapshot() != *context.target) {
        return Result<OpenProcessResult>::failure(
            ErrorCode::TargetChanged,
            "target changed before process selection started");
    }

    std::string name = request.name;
    if (name.empty()) {
        std::vector<ProcessInfo> processes;
        if (backend_.fetchProcesses(context, processes)) {
            const auto found = std::find_if(
                processes.begin(), processes.end(),
                [&](const ProcessInfo& process) {
                    return process.pid == request.pid;
                });
            if (found != processes.end()) {
                name = found->name;
            }
        }
    }

    std::lock_guard<std::mutex> lock(processMutex_);
    if (const auto error = validateContext(context, true, false, true)) {
        return failureFrom<OpenProcessResult>(*error);
    }
    if (context.target && backend_.targetSnapshot() != *context.target) {
        return Result<OpenProcessResult>::failure(
            ErrorCode::TargetChanged,
            "target changed before process open was sent");
    }
    if (!backend_.openProcess(context, request.pid, name)) {
        if (const auto error = validateContext(context, true, false, false)) {
            return failureFrom<OpenProcessResult>(*error);
        }
        return Result<OpenProcessResult>::failure(
            ErrorCode::ProtocolError,
            "failed to open the requested process");
    }
    if (const auto error = validateContext(context, true, false, false)) {
        return failureFrom<OpenProcessResult>(*error);
    }
    const TargetSnapshot target = backend_.targetSnapshot();
    if (!target.isAttached() || target.pid != request.pid ||
        target.connectionGeneration != context.connectionGeneration) {
        return Result<OpenProcessResult>::failure(
            ErrorCode::TargetChanged,
            "opened process does not match the requested target");
    }
    OpenProcessResult result{target, backend_.processName()};
    const TargetSnapshot finalTarget = backend_.targetSnapshot();
    if (const auto error = validateContext(context, true, false, false)) {
        return failureFrom<OpenProcessResult>(*error);
    }
    if (finalTarget != target) {
        return Result<OpenProcessResult>::failure(
            ErrorCode::TargetChanged,
            "target changed while the process open result was collected");
    }
    return Result<OpenProcessResult>::success(std::move(result));
}

Result<ModulePage> MemService::listModules(
    const OperationContext& context,
    const ModuleListRequest& request) {
    if (request.limit == 0 || request.limit > kMaxModulePageSize ||
        request.filter.size() > kMaxTextBytes) {
        return Result<ModulePage>::failure(
            ErrorCode::InvalidArgument,
            "invalid module list filter or page size");
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<ModulePage>(*error);
    }
    auto transaction = backend_.beginReadTransaction(context);
    std::vector<ModuleInfo> modules;
    if (!transaction || !transaction->valid() ||
        !transaction->fetchModules(modules)) {
        if (const auto error = validateContext(context, true, true, false)) {
            return failureFrom<ModulePage>(*error);
        }
        return Result<ModulePage>::failure(
            ErrorCode::ProtocolError, "failed to fetch module list", true);
    }
    transaction.reset();
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<ModulePage>(*error);
    }
    if (const auto error = validateModules(modules)) {
        return failureFrom<ModulePage>(*error);
    }

    const std::string filter = lowerAscii(request.filter);
    std::vector<ModuleInfo> filtered;
    for (auto& module : modules) {
        if (filter.empty() ||
            lowerAscii(module.name).find(filter) != std::string::npos) {
            filtered.push_back(std::move(module));
        }
    }
    ModulePage page;
    page.total = filtered.size();
    page.offset = std::min(request.offset, page.total);
    const size_t end = std::min(page.total, page.offset + request.limit);
    for (size_t i = page.offset; i < end; ++i) {
        page.items.push_back(std::move(filtered[i]));
    }
    if (end < page.total) {
        page.nextOffset = end;
    }
    page.target = *context.target;
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<ModulePage>(*error);
    }
    return Result<ModulePage>::success(std::move(page));
}

Result<ResolvedModule> MemService::resolveModule(
    const OperationContext& context,
    const ModuleResolveRequest& request) {
    if (request.name.size() > kMaxTextBytes) {
        return Result<ResolvedModule>::failure(
            ErrorCode::InvalidArgument, "module name exceeds 4096 bytes");
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<ResolvedModule>(*error);
    }
    auto transaction = backend_.beginReadTransaction(context);
    std::vector<ModuleInfo> modules;
    if (!transaction || !transaction->valid() ||
        !transaction->fetchModules(modules)) {
        if (const auto error = validateContext(context, true, true, false)) {
            return failureFrom<ResolvedModule>(*error);
        }
        return Result<ResolvedModule>::failure(
            ErrorCode::ProtocolError, "failed to fetch modules", true);
    }
    transaction.reset();
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<ResolvedModule>(*error);
    }
    if (const auto error = validateModules(modules)) {
        return failureFrom<ResolvedModule>(*error);
    }
    ModuleInfo module;
    if (const auto error = resolveModuleByName(modules, request.name, module)) {
        return failureFrom<ResolvedModule>(*error);
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<ResolvedModule>(*error);
    }
    return Result<ResolvedModule>::success(
        ResolvedModule{std::move(module), *context.target});
}

Result<PointerResolution> MemService::resolvePointer(
    const OperationContext& context,
    const PointerResolveRequest& request) {
    if (request.moduleName.empty() ||
        request.moduleName.size() > kMaxTextBytes ||
        request.offsets.size() > kMaxPointerOffsetCount) {
        return Result<PointerResolution>::failure(
            ErrorCode::InvalidArgument,
            "invalid module name or pointer offset count");
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<PointerResolution>(*error);
    }
    auto transaction = backend_.beginReadTransaction(context);
    if (!transaction || !transaction->valid()) {
        if (const auto error = validateContext(context, true, true, false)) {
            return failureFrom<PointerResolution>(*error);
        }
        return Result<PointerResolution>::failure(
            ErrorCode::ProtocolError,
            "failed to acquire pointer transaction", true);
    }
    std::vector<ModuleInfo> modules;
    if (!transaction->fetchModules(modules)) {
        return Result<PointerResolution>::failure(
            ErrorCode::ProtocolError,
            "failed to fetch modules inside pointer transaction", true);
    }
    if (const auto error = validateModules(modules)) {
        return failureFrom<PointerResolution>(*error);
    }
    ModuleInfo module;
    if (const auto error = resolveModuleByName(
            modules, request.moduleName, module)) {
        return failureFrom<PointerResolution>(*error);
    }

    PointerResolution result;
    result.module = module;
    if (!addAddressOffset(module.base, request.baseOffset,
                          result.startAddress)) {
        return Result<PointerResolution>::failure(
            ErrorCode::InvalidArgument,
            "module base plus offset overflows uint64");
    }
    result.address = result.startAddress;

    auto dereference = [&](uint64_t offset, bool addOffset) -> bool {
        if (const auto error = validateContext(context, true, false, true)) {
            return false;
        }
        std::vector<unsigned char> bytes;
        if (!transaction->readMemory(result.address, sizeof(uint64_t), bytes)) {
            return false;
        }
        const auto pointer = decodePointer(bytes);
        if (!pointer) {
            return false;
        }
        uint64_t next = *pointer;
        if (addOffset && !addAddressOffset(next, offset, next)) {
            return false;
        }
        result.address = next;
        ++result.dereferenceCount;
        return true;
    };

    for (uint64_t offset : request.offsets) {
        if (!dereference(offset, true)) {
            transaction.reset();
            if (const auto error = validateContext(
                    context, true, true, false)) {
                return failureFrom<PointerResolution>(*error);
            }
            return Result<PointerResolution>::failure(
                ErrorCode::ProtocolError,
                "failed to resolve pointer offset chain", true);
        }
    }
    if (request.dereferenceFinal) {
        if (!dereference(0, false)) {
            transaction.reset();
            if (const auto error = validateContext(
                    context, true, true, false)) {
                return failureFrom<PointerResolution>(*error);
            }
            return Result<PointerResolution>::failure(
                ErrorCode::ProtocolError,
                "failed to dereference final pointer", true);
        }
        result.dereferencedFinal = true;
    }
    transaction.reset();
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<PointerResolution>(*error);
    }
    result.target = *context.target;
    return Result<PointerResolution>::success(std::move(result));
}

Result<MemoryBlock> MemService::readMemory(
    const OperationContext& context,
    const MemoryReadRequest& request) {
    if (request.size == 0 || request.size > kMaxMemoryReadBytes) {
        return Result<MemoryBlock>::failure(
            ErrorCode::InvalidArgument,
            "memory read size is outside the service limit");
    }
    if (request.address >
        (std::numeric_limits<uint64_t>::max)() - (request.size - 1u)) {
        return Result<MemoryBlock>::failure(
            ErrorCode::InvalidArgument,
            "memory read range overflows the address space");
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<MemoryBlock>(*error);
    }
    std::vector<unsigned char> bytes;
    if (!backend_.readMemory(context, request.address, request.size, bytes)) {
        if (const auto error = validateContext(context, true, true, false)) {
            return failureFrom<MemoryBlock>(*error);
        }
        return Result<MemoryBlock>::failure(
            ErrorCode::ProtocolError, "failed to read target memory", true);
    }
    if (bytes.empty() || bytes.size() > request.size) {
        return Result<MemoryBlock>::failure(
            ErrorCode::ProtocolError,
            "memory read returned an invalid byte count");
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<MemoryBlock>(*error);
    }
    return Result<MemoryBlock>::success(
        MemoryBlock{request.address, std::move(bytes), *context.target});
}

Result<MemoryBatch> MemService::readMemoryBatch(
    const OperationContext& context,
    const MemoryBatchReadRequest& request) {
    if (request.items.empty() || request.items.size() > kMaxMemoryBatchCount) {
        return Result<MemoryBatch>::failure(
            ErrorCode::InvalidArgument,
            "memory batch count is outside the service limit");
    }
    uint64_t totalBytes = 0;
    for (const auto& item : request.items) {
        if (item.size == 0 || item.size > kMaxMemoryReadBytes ||
            totalBytes > kMaxMemoryBatchBytes - item.size) {
            return Result<MemoryBatch>::failure(
                ErrorCode::InvalidArgument,
                "memory batch byte size exceeds the service limit");
        }
        if (item.address >
            (std::numeric_limits<uint64_t>::max)() - (item.size - 1u)) {
            return Result<MemoryBatch>::failure(
                ErrorCode::InvalidArgument,
                "memory batch item range overflows the address space");
        }
        totalBytes += item.size;
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<MemoryBatch>(*error);
    }
    std::vector<MemoryBlock> blocks;
    if (!backend_.readMemoryBatch(context, request.items, blocks)) {
        if (const auto error = validateContext(context, true, true, false)) {
            return failureFrom<MemoryBatch>(*error);
        }
        return Result<MemoryBatch>::failure(
            ErrorCode::ProtocolError, "failed to read memory batch", true);
    }
    if (blocks.size() != request.items.size()) {
        return Result<MemoryBatch>::failure(
            ErrorCode::ProtocolError,
            "memory batch returned an incomplete result set");
    }
    std::vector<bool> matchedRequests(request.items.size(), false);
    for (auto& block : blocks) {
        size_t bestMatch = request.items.size();
        uint32_t bestMatchSize = (std::numeric_limits<uint32_t>::max)();
        for (size_t i = 0; i < request.items.size(); ++i) {
            const auto& requested = request.items[i];
            if (!matchedRequests[i] && requested.address == block.address &&
                block.bytes.size() <= requested.size &&
                requested.size < bestMatchSize) {
                bestMatch = i;
                bestMatchSize = requested.size;
            }
        }
        if (bestMatch == request.items.size()) {
            return Result<MemoryBatch>::failure(
                ErrorCode::ProtocolError,
                "memory batch returned an unknown address or invalid byte count");
        }
        matchedRequests[bestMatch] = true;
        block.target = *context.target;
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<MemoryBatch>(*error);
    }
    return Result<MemoryBatch>::success(
        MemoryBatch{std::move(blocks), *context.target});
}

Result<WriteReceipt> MemService::writeMemory(
    const OperationContext& context,
    const MemoryWriteRequest& request) {
    if (request.bytes.empty() || request.bytes.size() > kMaxMemoryWriteBytes) {
        return Result<WriteReceipt>::failure(
            ErrorCode::InvalidArgument,
            "memory write size is outside the service limit");
    }
    if (request.address >
        (std::numeric_limits<uint64_t>::max)() -
            (request.bytes.size() - 1u)) {
        return Result<WriteReceipt>::failure(
            ErrorCode::InvalidArgument,
            "memory write range overflows the address space");
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<WriteReceipt>(*error);
    }
    const auto backendResult =
        backend_.writeMemory(context, request.address, request.bytes);
    if (!backendResult.requestStarted) {
        if (const auto error = validateContext(context, true, true, false)) {
            return failureFrom<WriteReceipt>(*error);
        }
        return Result<WriteReceipt>::failure(
            ErrorCode::ProtocolError,
            "memory write was rejected before it started", true);
    }
    if (!backendResult.responseReceived) {
        return Result<WriteReceipt>::failure(
            ErrorCode::CompletionUnknown,
            "memory write was sent but no complete response was received; do not retry automatically",
            false);
    }
    if (backendResult.writtenBytes < 0 ||
        static_cast<size_t>(backendResult.writtenBytes) >
            request.bytes.size()) {
        return Result<WriteReceipt>::failure(
            ErrorCode::ProtocolError,
            "memory write returned an invalid byte count");
    }
    if (static_cast<size_t>(backendResult.writtenBytes) !=
        request.bytes.size()) {
        return Result<WriteReceipt>::failure(
            ErrorCode::PartialWrite,
            "memory write completed only a contiguous prefix",
            false,
            static_cast<uint64_t>(backendResult.writtenBytes));
    }
    if (const auto error = validateContext(context, true, true, false)) {
        return Result<WriteReceipt>::failure(
            ErrorCode::CompletionUnknown,
            "server confirmed the write after the original target changed; do not retry automatically");
    }
    WriteReceipt receipt;
    receipt.address = request.address;
    receipt.requestedBytes = static_cast<uint32_t>(request.bytes.size());
    receipt.writtenBytes = static_cast<uint32_t>(backendResult.writtenBytes);
    receipt.completedAfterCancelRequest =
        context.cancellation &&
        context.cancellation->load(std::memory_order_acquire);
    receipt.completedAfterDeadline = Clock::now() >= context.deadline;
    receipt.target = *context.target;
    return Result<WriteReceipt>::success(std::move(receipt));
}

Result<BreakpointMutationReceipt> MemService::mutateBreakpoint(
    const OperationContext& context,
    uint64_t address,
    BreakpointAction action,
    const std::optional<BreakpointSetRequest>& setRequest) {
    if (address == 0) {
        return Result<BreakpointMutationReceipt>::failure(
            ErrorCode::InvalidArgument,
            "breakpoint address must not be zero");
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<BreakpointMutationReceipt>(*error);
    }
    std::lock_guard<std::mutex> lock(breakpointMutex_);
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<BreakpointMutationReceipt>(*error);
    }
    if (breakpointCounterTarget_ != *context.target) {
        breakpointCounterTarget_ = *context.target;
        breakpointHitTotals_.clear();
    }
    BreakpointMutationBackendResult backendResult;
    switch (action) {
    case BreakpointAction::Set:
        if (setRequest) {
            backendResult = backend_.setBreakpoint(
                context, address, setRequest->access, setRequest->size);
        }
        break;
    case BreakpointAction::Remove:
        backendResult = backend_.removeBreakpoint(context, address);
        break;
    case BreakpointAction::Suspend:
        backendResult = backend_.suspendBreakpoint(context, address);
        break;
    case BreakpointAction::Resume:
        backendResult = backend_.resumeBreakpoint(context, address);
        break;
    }
    if (!backendResult.responseReceived) {
        if (backendResult.requestStarted) {
            return Result<BreakpointMutationReceipt>::failure(
                ErrorCode::CompletionUnknown,
                std::string("breakpoint ") + breakpointActionName(action) +
                    " was sent but its completion could not be confirmed; reconnect before continuing and do not retry automatically");
        }
        if (const auto error = validateContext(context, true, true, true)) {
            return failureFrom<BreakpointMutationReceipt>(*error);
        }
        return Result<BreakpointMutationReceipt>::failure(
            ErrorCode::ProtocolError,
            std::string("breakpoint ") + breakpointActionName(action) +
                " could not be sent",
            true);
    }
    if (!backendResult.applied) {
        return Result<BreakpointMutationReceipt>::failure(
            ErrorCode::ProtocolError,
            std::string("Android server rejected breakpoint ") +
                breakpointActionName(action),
            false);
    }
    if (const auto error = validateContext(context, true, true, false)) {
        return Result<BreakpointMutationReceipt>::failure(
            ErrorCode::CompletionUnknown,
            std::string("server confirmed breakpoint ") +
                breakpointActionName(action) +
                " after the original target changed; do not retry automatically");
    }
    if (action == BreakpointAction::Set ||
        action == BreakpointAction::Remove) {
        breakpointHitTotals_.erase(address);
    }
    return Result<BreakpointMutationReceipt>::success(
        BreakpointMutationReceipt{address, action, *context.target});
}

Result<BreakpointMutationReceipt> MemService::setBreakpoint(
    const OperationContext& context,
    const BreakpointSetRequest& request) {
    const uint32_t access = static_cast<uint32_t>(request.access);
    if (access < 1 || access > 4 ||
        (request.size != 1 && request.size != 2 &&
         request.size != 4 && request.size != 8) ||
        (request.access == BreakpointAccess::Execute && request.size != 4)) {
        return Result<BreakpointMutationReceipt>::failure(
            ErrorCode::InvalidArgument,
            "invalid breakpoint access or size");
    }
    return mutateBreakpoint(context, request.address,
                            BreakpointAction::Set, request);
}

Result<BreakpointMutationReceipt> MemService::removeBreakpoint(
    const OperationContext& context,
    const BreakpointAddressRequest& request) {
    return mutateBreakpoint(context, request.address,
                            BreakpointAction::Remove);
}

Result<BreakpointMutationReceipt> MemService::suspendBreakpoint(
    const OperationContext& context,
    const BreakpointAddressRequest& request) {
    return mutateBreakpoint(context, request.address,
                            BreakpointAction::Suspend);
}

Result<BreakpointMutationReceipt> MemService::resumeBreakpoint(
    const OperationContext& context,
    const BreakpointAddressRequest& request) {
    return mutateBreakpoint(context, request.address,
                            BreakpointAction::Resume);
}

Result<BreakpointHitBatch> MemService::breakpointHits(
    const OperationContext& context,
    const BreakpointHitBatchRequest& request) {
    if (request.address == 0 || request.limit == 0 ||
        request.limit > kMaxBreakpointHitCount) {
        return Result<BreakpointHitBatch>::failure(
            ErrorCode::InvalidArgument,
            "invalid breakpoint address or hit limit");
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<BreakpointHitBatch>(*error);
    }
    std::lock_guard<std::mutex> lock(breakpointMutex_);
    if (breakpointCounterTarget_ != *context.target) {
        breakpointCounterTarget_ = *context.target;
        breakpointHitTotals_.clear();
    }
    const auto previousTotal = breakpointHitTotals_.find(request.address);
    std::vector<BreakpointHit> hits;
    size_t total = 0;
    if (!backend_.fetchBreakpointHits(
            context, request.address, request.limit, hits, total)) {
        if (const auto error = validateContext(context, true, true, false)) {
            return failureFrom<BreakpointHitBatch>(*error);
        }
        return Result<BreakpointHitBatch>::failure(
            ErrorCode::ProtocolError,
            "failed to fetch breakpoint hits", true);
    }
    if (hits.size() > request.limit || total < hits.size()) {
        return Result<BreakpointHitBatch>::failure(
            ErrorCode::ProtocolError,
            "breakpoint hit response is inconsistent");
    }
    size_t newHits = total;
    if (previousTotal != breakpointHitTotals_.end() &&
        total >= previousTotal->second) {
        newHits = total - previousTotal->second;
        if (newHits < hits.size()) {
            return Result<BreakpointHitBatch>::failure(
                ErrorCode::ProtocolError,
                "breakpoint cumulative count regressed behind returned hits");
        }
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<BreakpointHitBatch>(*error);
    }
    BreakpointHitBatch batch;
    batch.address = request.address;
    batch.available = total;
    batch.dropped = newHits > hits.size() ? newHits - hits.size() : 0;
    batch.items = std::move(hits);
    batch.target = *context.target;
    breakpointHitTotals_[request.address] = total;
    return Result<BreakpointHitBatch>::success(std::move(batch));
}

Result<BreakpointSlotsSnapshot> MemService::breakpointSlots(
    const OperationContext& context, uint32_t capacity) {
    if (capacity == 0 || capacity > kMaxBreakpointQueryEntries) {
        return Result<BreakpointSlotsSnapshot>::failure(
            ErrorCode::InvalidArgument,
            "breakpoint slot capacity must be between 1 and 64");
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<BreakpointSlotsSnapshot>(*error);
    }

    // hwbp_query_task 是驱动接口，只允许在 Kernel 后端使用；在服务边界
    // 明确拒绝其它读写模式，避免把失败误报成普通协议错误。
    int memoryType = 0;
    std::string memoryTypeName;
    if (!backend_.fetchMemoryType(memoryType, memoryTypeName)) {
        return Result<BreakpointSlotsSnapshot>::failure(
            ErrorCode::ProtocolError,
            "failed to query current memory mode", true);
    }
    if (memoryType != kKernelMemoryType) {
        return Result<BreakpointSlotsSnapshot>::failure(
            ErrorCode::PermissionDenied,
            "hwbp_query_task requires Kernel memory mode");
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<BreakpointSlotsSnapshot>(*error);
    }

    std::lock_guard<std::mutex> lock(breakpointMutex_);
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<BreakpointSlotsSnapshot>(*error);
    }
    std::vector<BreakpointThreadSlots> threads;
    if (!backend_.fetchBreakpointSlots(context, capacity, threads)) {
        if (const auto error = validateContext(context, true, true, false)) {
            return failureFrom<BreakpointSlotsSnapshot>(*error);
        }
        return Result<BreakpointSlotsSnapshot>::failure(
            ErrorCode::ProtocolError,
            "failed to query hardware breakpoint slots", true);
    }
    if (threads.size() > kMaxBreakpointQueryThreads) {
        return Result<BreakpointSlotsSnapshot>::failure(
            ErrorCode::ProtocolError,
            "hardware breakpoint slot response contains too many threads");
    }
    for (const auto& thread : threads) {
        if (thread.tid <= 0 || thread.count > capacity ||
            thread.count != thread.slots.size() ||
            thread.totalCount < thread.count ||
            thread.brpCount > thread.totalCount ||
            thread.wrpCount > thread.totalCount ||
            thread.enabledCount > thread.totalCount ||
            thread.activeCount > thread.totalCount ||
            thread.perfCount > thread.totalCount ||
            thread.ptraceCount > thread.totalCount ||
            thread.moduleCount > thread.totalCount ||
            (thread.querySucceeded && thread.errorCode != 0) ||
            (!thread.querySucceeded &&
             (thread.count != 0 || thread.errorCode <= 0))) {
            return Result<BreakpointSlotsSnapshot>::failure(
                ErrorCode::ProtocolError,
                "hardware breakpoint slot response is inconsistent");
        }
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<BreakpointSlotsSnapshot>(*error);
    }
    BreakpointSlotsSnapshot snapshot;
    snapshot.threads = std::move(threads);
    snapshot.target = *context.target;
    return Result<BreakpointSlotsSnapshot>::success(std::move(snapshot));
}

Result<SymbolTable> MemService::loadSymbolTable(
    const OperationContext& context,
    const SymbolTableRequest& request) {
    if (request.moduleBase == 0) {
        return Result<SymbolTable>::failure(
            ErrorCode::InvalidArgument,
            "module base must not be zero");
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<SymbolTable>(*error);
    }
    std::lock_guard<std::mutex> lock(symbolMutex_);
    auto transaction = backend_.beginSymbolTransaction(context);
    if (!transaction || !transaction->valid()) {
        if (const auto error = validateContext(context, true, true, false)) {
            return failureFrom<SymbolTable>(*error);
        }
        return Result<SymbolTable>::failure(
            ErrorCode::ProtocolError,
            "failed to acquire symbol transaction", true);
    }
    std::vector<ModuleInfo> modules;
    if (!transaction->fetchModules(modules)) {
        return Result<SymbolTable>::failure(
            ErrorCode::ProtocolError,
            "failed to fetch modules inside symbol transaction", true);
    }
    if (const auto error = validateModules(modules)) {
        return failureFrom<SymbolTable>(*error);
    }
    ModuleInfo module;
    if (const auto error = resolveModuleByBase(
            modules, request.moduleBase, module)) {
        return failureFrom<SymbolTable>(*error);
    }
    int totalCount = 0;
    if (!transaction->initialize(module.base, totalCount) ||
        totalCount < 0 ||
        static_cast<size_t>(totalCount) > kMaxSymbolCount) {
        return Result<SymbolTable>::failure(
            ErrorCode::ProtocolError,
            "failed to initialize module symbol table", true);
    }

    SymbolTable table;
    table.module = module;
    table.items.reserve(static_cast<size_t>(totalCount));
    size_t nameBytes = 0;
    constexpr size_t kPageSize = kMaxSymbolPageSize;
    for (size_t offset = 0;
         offset < static_cast<size_t>(totalCount);
         offset += kPageSize) {
        const size_t count = std::min(
            kPageSize, static_cast<size_t>(totalCount) - offset);
        std::vector<SymbolInfo> page;
        int observedTotal = 0;
        if (!transaction->fetch(offset, count, page, observedTotal) ||
            observedTotal != totalCount || page.size() > count) {
            return Result<SymbolTable>::failure(
                ErrorCode::SymbolSessionChanged,
                "symbol table changed while it was being loaded");
        }
        for (auto& symbol : page) {
            if (symbol.name.size() > kMaxTextBytes * 16 ||
                nameBytes > kMaxSymbolNameBytesTotal - symbol.name.size()) {
                return Result<SymbolTable>::failure(
                    ErrorCode::ProtocolError,
                    "symbol response exceeds the service limit");
            }
            nameBytes += symbol.name.size();
            table.items.push_back(std::move(symbol));
        }
    }
    transaction.reset();
    if (table.items.size() != static_cast<size_t>(totalCount)) {
        return Result<SymbolTable>::failure(
            ErrorCode::ProtocolError,
            "symbol table returned an incomplete item count");
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<SymbolTable>(*error);
    }
    table.target = *context.target;
    return Result<SymbolTable>::success(std::move(table));
}

Result<SymbolPage> MemService::listSymbols(
    const OperationContext& context,
    const SymbolListRequest& request) {
    if (request.moduleBase == 0 || request.limit == 0 ||
        request.limit > kMaxSymbolPageSize ||
        request.offset >
            static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return Result<SymbolPage>::failure(
            ErrorCode::InvalidArgument,
            "invalid module base, symbol offset, or page size");
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<SymbolPage>(*error);
    }

    std::lock_guard<std::mutex> lock(symbolMutex_);
    auto transaction = backend_.beginSymbolTransaction(context);
    if (!transaction || !transaction->valid()) {
        if (const auto error = validateContext(context, true, true, false)) {
            return failureFrom<SymbolPage>(*error);
        }
        return Result<SymbolPage>::failure(
            ErrorCode::ProtocolError,
            "failed to acquire symbol transaction", true);
    }

    std::vector<ModuleInfo> modules;
    if (!transaction->fetchModules(modules)) {
        return Result<SymbolPage>::failure(
            ErrorCode::ProtocolError,
            "failed to fetch modules inside symbol transaction", true);
    }
    if (const auto error = validateModules(modules)) {
        return failureFrom<SymbolPage>(*error);
    }
    ModuleInfo module;
    if (const auto error = resolveModuleByBase(
            modules, request.moduleBase, module)) {
        return failureFrom<SymbolPage>(*error);
    }

    int initializedTotal = 0;
    if (!transaction->initialize(module.base, initializedTotal) ||
        initializedTotal < 0 ||
        static_cast<size_t>(initializedTotal) > kMaxSymbolCount) {
        return Result<SymbolPage>::failure(
            ErrorCode::ProtocolError,
            "failed to initialize module symbol table", true);
    }

    const size_t total = static_cast<size_t>(initializedTotal);
    const size_t pageOffset = std::min(request.offset, total);
    const size_t pageLimit = std::min(request.limit, total - pageOffset);
    std::vector<SymbolInfo> symbols;
    if (pageLimit > 0) {
        int observedTotal = 0;
        if (!transaction->fetch(
                pageOffset, pageLimit, symbols, observedTotal) ||
            observedTotal != initializedTotal || symbols.empty() ||
            symbols.size() > pageLimit) {
            return Result<SymbolPage>::failure(
                ErrorCode::SymbolSessionChanged,
                "symbol table changed while its page was being loaded");
        }
    }

    size_t nameBytes = 0;
    for (const auto& symbol : symbols) {
        if (symbol.name.size() > kMaxTextBytes * 16 ||
            nameBytes > kMaxSymbolNameBytesTotal - symbol.name.size()) {
            return Result<SymbolPage>::failure(
                ErrorCode::ProtocolError,
                "symbol page exceeds the service limit");
        }
        nameBytes += symbol.name.size();
    }

    transaction.reset();
    SymbolPage page;
    page.module = std::move(module);
    page.items = std::move(symbols);
    page.total = total;
    page.offset = pageOffset;
    const size_t end = page.offset + page.items.size();
    if (end < page.total) {
        page.nextOffset = end;
    }
    page.target = *context.target;
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<SymbolPage>(*error);
    }
    return Result<SymbolPage>::success(std::move(page));
}

Result<ResolvedSymbol> MemService::resolveSymbol(
    const OperationContext& context,
    const SymbolResolveRequest& request) {
    if (request.moduleBase == 0 || request.name.empty() ||
        request.name.size() > kMaxTextBytes * 16) {
        return Result<ResolvedSymbol>::failure(
            ErrorCode::InvalidArgument,
            "invalid module base or symbol name");
    }
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<ResolvedSymbol>(*error);
    }
    std::lock_guard<std::mutex> lock(symbolMutex_);
    auto transaction = backend_.beginSymbolTransaction(context);
    if (!transaction || !transaction->valid()) {
        return Result<ResolvedSymbol>::failure(
            ErrorCode::ProtocolError,
            "failed to acquire symbol transaction", true);
    }
    std::vector<ModuleInfo> modules;
    if (!transaction->fetchModules(modules)) {
        return Result<ResolvedSymbol>::failure(
            ErrorCode::ProtocolError,
            "failed to fetch modules inside symbol transaction", true);
    }
    ModuleInfo module;
    if (const auto error = resolveModuleByBase(
            modules, request.moduleBase, module)) {
        return failureFrom<ResolvedSymbol>(*error);
    }
    uint64_t address = 0;
    if (!transaction->find(module.base, request.name, address)) {
        if (const auto error = validateContext(context, true, true, false)) {
            return failureFrom<ResolvedSymbol>(*error);
        }
        return Result<ResolvedSymbol>::failure(
            ErrorCode::InvalidArgument,
            "symbol was not found in the requested module");
    }
    transaction.reset();
    if (const auto error = validateContext(context, true, true, true)) {
        return failureFrom<ResolvedSymbol>(*error);
    }
    return Result<ResolvedSymbol>::success(
        ResolvedSymbol{std::move(module), request.name, address,
                       *context.target});
}

} // namespace Mem
