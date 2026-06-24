#include "AppContext.h"
#include "Gui.h"
#include "../socket/client_singleton.h"
#include "../imgui/imgui.h"
#include <algorithm>

void AppContext::cleanupCurrentProcessServices() {
    if (!hasProcess()) {
        return;
    }

    const int handle = processHandle.load(std::memory_order_relaxed);
    ClearTrackedKernelBreakpoints(PORT_MAIN);
    CloseProcessHandle(handle, PORT_MAIN);
}

void AppContext::selectProcess(int pid, const std::string& name) {
    const bool hadProcess = hasProcess();
    if (hadProcess) {
        processRevision.fetch_add(1, std::memory_order_release);
    }

    cleanupCurrentProcessServices();
    selectedPid.store(0, std::memory_order_relaxed);
    processHandle.store(0, std::memory_order_relaxed);
    int handle = 0;
    if (OpenProcessHandle(pid, handle)) {
        SetCurrentPid(pid);
        processHandle.store(handle, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(nameMutex_);
            selectedName_ = name;
        }
        Gui::log("进程已打开，句柄=%d", handle);
    } else {
        SetCurrentPid(0);
        processHandle.store(0, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(nameMutex_);
            selectedName_.clear();
        }
        Gui::log("无法打开进程句柄 %d", pid);
    }

    moduleCache.invalidate();
    if (!hadProcess) {
        processRevision.fetch_add(1, std::memory_order_release);
    }
}

void AppContext::clearProcess() {
    const bool hadProcess = hasProcess();
    if (hadProcess) {
        processRevision.fetch_add(1, std::memory_order_release);
    }

    cleanupCurrentProcessServices();
    selectedPid.store(0, std::memory_order_relaxed);
    processHandle.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(nameMutex_);
        selectedName_.clear();
    }
    moduleCache.invalidate();
    if (!hadProcess) {
        processRevision.fetch_add(1, std::memory_order_release);
    }
}

void AppContext::ModuleCache::refresh() {
    std::lock_guard<std::mutex> lock(mutex);

    // 节流：如果缓存有效且距上次刷新不足 MIN_REFRESH_INTERVAL 秒，跳过
    double now = ImGui::GetTime();
    if (valid && (now - lastRefreshTime) < MIN_REFRESH_INTERVAL) {
        return;
    }

    std::vector<ModuleInfoItem> newList;
    if (FetchModuleList(newList)) {
        modules = std::move(newList);
        symbolCacheByModuleBase.clear();
        valid = true;
        lastRefreshTime = now;
    }
}

ModuleInfoItem AppContext::ModuleCache::findByAddress(uint64_t addr) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& m : modules) {
        if (m.size <= 0) {
            continue;
        }
        const uint64_t moduleSize = static_cast<uint64_t>(m.size);
        if (m.base > UINT64_MAX - moduleSize) {
            continue;
        }
        if (addr >= m.base && addr < m.base + moduleSize) {
            return m;  // 返回拷贝
        }
    }
    return ModuleInfoItem{};  // 空对象，name 为空表示未找到
}

std::string AppContext::ModuleCache::formatWithModule(uint64_t addr) {
    ModuleInfoItem mod = findByAddress(addr);
    if (!mod.name.empty()) {
        char buf[256];
        uint64_t offset = addr - mod.base;
        snprintf(buf, sizeof(buf), "%s+0x%llX", mod.name.c_str(), (unsigned long long)offset);
        return buf;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)addr);
    return buf;
}

bool AppContext::ModuleCache::ensureSymbolListCached(const ModuleInfoItem& module, std::vector<SymbolInfoItem>& outSymbols) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = symbolCacheByModuleBase.find(module.base);
        if (it != symbolCacheByModuleBase.end() && it->second.valid) {
            outSymbols = it->second.symbols;
            return true;
        }
    }

    int totalCount = 0;
    if (!SymbolInit(module.base, totalCount) || totalCount <= 0) {
        return false;
    }

    constexpr int kBatchSize = 256;
    std::vector<SymbolInfoItem> loadedSymbols;
    loadedSymbols.reserve(totalCount);

    for (int offset = 0; offset < totalCount; offset += kBatchSize) {
        int requestCount = (kBatchSize < (totalCount - offset)) ? kBatchSize : (totalCount - offset);
        int fetchedTotalCount = totalCount;
        std::vector<std::pair<uint64_t, std::string>> symbols;
        if (!SymbolGetList(offset, requestCount, symbols, &fetchedTotalCount)) {
            return false;
        }

        for (const auto& symbol : symbols) {
            if (symbol.second.empty()) {
                continue;
            }

            SymbolInfoItem item;
            item.address = symbol.first;
            item.name = symbol.second;
            loadedSymbols.push_back(std::move(item));
        }
    }

    std::sort(loadedSymbols.begin(), loadedSymbols.end(), [](const SymbolInfoItem& lhs, const SymbolInfoItem& rhs) {
        return lhs.address < rhs.address;
    });

    {
        std::lock_guard<std::mutex> lock(mutex);
        auto& cacheEntry = symbolCacheByModuleBase[module.base];
        cacheEntry.symbols = loadedSymbols;
        cacheEntry.lastMatchedIndex = 0;
        cacheEntry.valid = true;
        outSymbols = cacheEntry.symbols;
    }
    return true;
}

bool AppContext::ModuleCache::tryFindContainingSymbol(uint64_t addr, SymbolInfoItem& outSymbol, uint64_t& outOffset) {
    outSymbol = SymbolInfoItem{};
    outOffset = 0;

    ModuleInfoItem mod = findByAddress(addr);
    if (mod.name.empty() || mod.base == 0) {
        return false;
    }

    std::vector<SymbolInfoItem> symbols;
    if (!ensureSymbolListCached(mod, symbols) || symbols.empty()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        auto cacheIt = symbolCacheByModuleBase.find(mod.base);
        if (cacheIt != symbolCacheByModuleBase.end() && cacheIt->second.valid && !cacheIt->second.symbols.empty()) {
            const auto& cachedSymbols = cacheIt->second.symbols;
            if (cacheIt->second.lastMatchedIndex < cachedSymbols.size()) {
                const size_t idx = cacheIt->second.lastMatchedIndex;
                const uint64_t start = cachedSymbols[idx].address;
                const uint64_t end = (idx + 1 < cachedSymbols.size()) ? cachedSymbols[idx + 1].address : UINT64_MAX;
                if (addr >= start && addr < end) {
                    outSymbol.address = cachedSymbols[idx].address;
                    outSymbol.name = cachedSymbols[idx].name;
                    outOffset = addr - cachedSymbols[idx].address;
                    return true;
                }
            }
        }
    }

    auto it = std::upper_bound(symbols.begin(), symbols.end(), addr,
        [](uint64_t target, const SymbolInfoItem& symbol) {
            return target < symbol.address;
        });

    if (it == symbols.begin()) {
        return false;
    }

    --it;
    const size_t matchedIndex = static_cast<size_t>(std::distance(symbols.begin(), it));
    outSymbol.address = it->address;
    outSymbol.name = it->name;
    outOffset = addr - it->address;

    {
        std::lock_guard<std::mutex> lock(mutex);
        auto cacheIt = symbolCacheByModuleBase.find(mod.base);
        if (cacheIt != symbolCacheByModuleBase.end() && cacheIt->second.valid) {
            cacheIt->second.lastMatchedIndex = matchedIndex;
        }
    }
    return true;
}

std::string AppContext::ModuleCache::formatWithSymbol(uint64_t addr) {
    SymbolInfoItem symbol;
    uint64_t symbolOffset = 0;
    if (!tryFindContainingSymbol(addr, symbol, symbolOffset)) {
        return "";
    }

    char buf[512];
    if (symbolOffset == 0) {
        snprintf(buf, sizeof(buf), "%s", symbol.name.c_str());
    } else {
        snprintf(buf, sizeof(buf), "%s+0x%llX", symbol.name.c_str(), (unsigned long long)symbolOffset);
    }
    return buf;
}

std::string AppContext::ModuleCache::formatAddressWithModuleAndSymbol(uint64_t addr) {
    std::string moduleText = formatWithModule(addr);
    std::string symbolText = formatWithSymbol(addr);
    if (symbolText.empty()) {
        return moduleText;
    }
    return moduleText + " (" + symbolText + ")";
}
