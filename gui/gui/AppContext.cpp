#include "AppContext.h"

#include "../imgui/imgui.h"
#include "../mem/IMemService.h"

#include <algorithm>
#include <cstdio>
#include <limits>

AppContext::TargetMutation::TargetMutation(
    AppContext& owner,
    std::unique_lock<std::mutex> stateLock,
    Mem::TargetSnapshot previousTarget)
    : owner_(&owner),
      stateLock_(std::move(stateLock)),
      previousTarget_(previousTarget) {}

AppContext::TargetMutation::TargetMutation(TargetMutation&& other) noexcept
    : owner_(other.owner_),
      stateLock_(std::move(other.stateLock_)),
      previousTarget_(other.previousTarget_) {
    other.owner_ = nullptr;
}

AppContext::TargetMutation::~TargetMutation() {
    finish();
}

void AppContext::TargetMutation::publish(
    int pid, int handle, const std::string& name) {
    if (!owner_) {
        return;
    }
    owner_->selectedPid.store(pid, std::memory_order_relaxed);
    owner_->processHandle.store(handle, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(owner_->nameMutex_);
        owner_->selectedName_ = name;
    }
    owner_->moduleCache.invalidate();
}

void AppContext::TargetMutation::clear() {
    publish(0, 0, {});
}

void AppContext::TargetMutation::finish() {
    if (!owner_) {
        return;
    }
    owner_->processRevision.fetch_add(1, std::memory_order_release);
    owner_ = nullptr;
}

std::optional<AppContext::TargetMutation> AppContext::beginTargetMutation(
    const std::optional<Mem::TargetSnapshot>& expected,
    uint64_t connectionGeneration) {
    std::unique_lock<std::mutex> stateLock(processStateMutex_);
    const uint64_t revision =
        processRevision.load(std::memory_order_acquire);
    if ((revision & 1u) != 0u) {
        return std::nullopt;
    }

    Mem::TargetSnapshot current;
    current.pid = selectedPid.load(std::memory_order_relaxed);
    current.processHandle = processHandle.load(std::memory_order_relaxed);
    current.processRevision = revision;
    current.connectionGeneration = connectionGeneration;
    if (expected && current != *expected) {
        return std::nullopt;
    }

    processRevision.fetch_add(1, std::memory_order_acq_rel);
    return TargetMutation(*this, std::move(stateLock), current);
}

void AppContext::clearProcessForDisconnect() {
    auto mutation = beginTargetMutation(std::nullopt, 0);
    if (mutation) {
        mutation->clear();
    }
}

Mem::TargetSnapshot AppContext::snapshotTarget(
    uint64_t connectionGeneration) const {
    std::lock_guard<std::mutex> stateLock(processStateMutex_);
    Mem::TargetSnapshot snapshot;
    snapshot.pid = selectedPid.load(std::memory_order_relaxed);
    snapshot.processHandle = processHandle.load(std::memory_order_relaxed);
    snapshot.processRevision = processRevision.load(std::memory_order_acquire);
    snapshot.connectionGeneration = connectionGeneration;
    return snapshot;
}

bool AppContext::matchesStableTarget(
    const Mem::TargetSnapshot& expected,
    uint64_t connectionGeneration) const {
    if (expected.connectionGeneration != connectionGeneration) {
        return false;
    }
    const uint64_t revisionBefore =
        processRevision.load(std::memory_order_acquire);
    if ((revisionBefore & 1u) != 0u ||
        revisionBefore != expected.processRevision) {
        return false;
    }
    const int pid = selectedPid.load(std::memory_order_relaxed);
    const int handle = processHandle.load(std::memory_order_relaxed);
    const uint64_t revisionAfter =
        processRevision.load(std::memory_order_acquire);
    return revisionBefore == revisionAfter &&
           (revisionAfter & 1u) == 0u &&
           pid == expected.pid &&
           handle == expected.processHandle;
}

void AppContext::ModuleCache::refresh(Mem::IMemService& service) {
    const double now = ImGui::GetTime();
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (valid && (now - lastRefreshTime) < MIN_REFRESH_INTERVAL) {
            return;
        }
    }

    const Mem::OperationContext context = service.captureContext(true);
    if (!context.target || !context.target->isAttached()) {
        return;
    }
    std::vector<Mem::ModuleInfo> loaded;
    size_t offset = 0;
    while (true) {
        Mem::ModuleListRequest request;
        request.offset = offset;
        request.limit = Mem::kMaxModulePageSize;
        auto response = service.listModules(context, request);
        if (!response.ok() || response.value().target != *context.target) {
            return;
        }
        auto& page = response.value();
        loaded.insert(loaded.end(), page.items.begin(), page.items.end());
        if (!page.nextOffset) {
            break;
        }
        offset = *page.nextOffset;
    }

    const Mem::OperationContext current = service.captureContext(true);
    if (!current.target || *current.target != *context.target) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    modules = std::move(loaded);
    symbolCacheByModuleBase.clear();
    valid = true;
    lastRefreshTime = now;
}

Mem::ModuleInfo AppContext::ModuleCache::findByAddress(uint64_t addr) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& module : modules) {
        if (module.size == 0 ||
            module.base >
                (std::numeric_limits<uint64_t>::max)() - module.size) {
            continue;
        }
        if (addr >= module.base && addr < module.base + module.size) {
            return module;
        }
    }
    return {};
}

std::string AppContext::ModuleCache::formatWithModule(uint64_t addr) {
    const Mem::ModuleInfo module = findByAddress(addr);
    char buffer[512];
    if (!module.name.empty()) {
        std::snprintf(buffer, sizeof(buffer), "%s+0x%llX",
                      module.name.c_str(),
                      static_cast<unsigned long long>(addr - module.base));
    } else {
        std::snprintf(buffer, sizeof(buffer), "0x%llX",
                      static_cast<unsigned long long>(addr));
    }
    return buffer;
}

bool AppContext::ModuleCache::ensureSymbolListCached(
    const Mem::ModuleInfo& module,
    Mem::IMemService& service,
    std::vector<SymbolInfoItem>& outSymbols) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = symbolCacheByModuleBase.find(module.base);
        if (found != symbolCacheByModuleBase.end() && found->second.valid) {
            outSymbols = found->second.symbols;
            return true;
        }
    }

    const auto response = service.loadSymbolTable(
        service.captureContext(true), Mem::SymbolTableRequest{module.base});
    if (!response.ok() || response.value().items.empty()) {
        return false;
    }
    const Mem::TargetSnapshot expectedTarget = response.value().target;
    std::vector<SymbolInfoItem> loaded;
    loaded.reserve(response.value().items.size());
    for (const auto& symbol : response.value().items) {
        if (!symbol.name.empty()) {
            loaded.push_back(SymbolInfoItem{symbol.address, symbol.name});
        }
    }
    std::sort(loaded.begin(), loaded.end(),
              [](const SymbolInfoItem& lhs, const SymbolInfoItem& rhs) {
                  return lhs.address < rhs.address;
              });

    if (!AppContext::Get().matchesStableTarget(
            expectedTarget, expectedTarget.connectionGeneration)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex);
    auto& entry = symbolCacheByModuleBase[module.base];
    entry.symbols = std::move(loaded);
    entry.lastMatchedIndex = 0;
    entry.valid = true;
    outSymbols = entry.symbols;
    return true;
}

bool AppContext::ModuleCache::tryFindContainingSymbol(
    uint64_t addr,
    Mem::IMemService& service,
    SymbolInfoItem& outSymbol,
    uint64_t& outOffset) {
    outSymbol = {};
    outOffset = 0;
    const Mem::ModuleInfo module = findByAddress(addr);
    if (module.name.empty()) {
        return false;
    }
    std::vector<SymbolInfoItem> symbols;
    if (!ensureSymbolListCached(module, service, symbols) ||
        symbols.empty()) {
        return false;
    }
    const auto upper = std::upper_bound(
        symbols.begin(), symbols.end(), addr,
        [](uint64_t value, const SymbolInfoItem& symbol) {
            return value < symbol.address;
        });
    if (upper == symbols.begin()) {
        return false;
    }
    const auto found = std::prev(upper);
    outSymbol = *found;
    outOffset = addr - found->address;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto entry = symbolCacheByModuleBase.find(module.base);
        if (entry != symbolCacheByModuleBase.end()) {
            entry->second.lastMatchedIndex =
                static_cast<size_t>(std::distance(symbols.begin(), found));
        }
    }
    return true;
}

std::string AppContext::ModuleCache::formatWithSymbol(
    uint64_t addr, Mem::IMemService& service) {
    SymbolInfoItem symbol;
    uint64_t offset = 0;
    if (!tryFindContainingSymbol(addr, service, symbol, offset)) {
        return {};
    }
    if (offset == 0) {
        return symbol.name;
    }
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "%s+0x%llX",
                  symbol.name.c_str(),
                  static_cast<unsigned long long>(offset));
    return buffer;
}

std::string AppContext::ModuleCache::formatAddressWithModuleAndSymbol(
    uint64_t addr, Mem::IMemService& service) {
    std::string module = formatWithModule(addr);
    const std::string symbol = formatWithSymbol(addr, service);
    if (!symbol.empty()) {
        module += " (" + symbol + ")";
    }
    return module;
}
