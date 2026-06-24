#pragma once

#include "../socket/client_singleton.h"
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <cstdint>

class AppContext {
public:
    static AppContext& Get() {
        static AppContext instance;
        return instance;
    }

    // 进程状态
    std::atomic<int> selectedPid{0};
    std::atomic<int> processHandle{0};
    std::atomic<uint64_t> processRevision{0};

    void selectProcess(int pid, const std::string& name);
    void clearProcess();
    void cleanupCurrentProcessServices();
    bool hasProcess() const {
        return selectedPid.load(std::memory_order_relaxed) != 0 &&
               processHandle.load(std::memory_order_relaxed) != 0;
    }

    // 线程安全的 selectedName 访问
    std::string getSelectedName() const {
        std::lock_guard<std::mutex> lock(nameMutex_);
        return selectedName_;
    }

    // 兼容旧代码的只读引用（仅限主线程使用）
    const std::string& selectedName = selectedName_;

    // 模块缓存
    struct SymbolInfoItem {
        uint64_t address = 0;
        std::string name;
    };

    struct ModuleCache {
        struct SymbolListCacheEntry {
            std::vector<SymbolInfoItem> symbols;
            size_t lastMatchedIndex = 0;
            bool valid = false;
        };

        std::vector<ModuleInfoItem> modules;
        std::unordered_map<uint64_t, SymbolListCacheEntry> symbolCacheByModuleBase;
        bool valid = false;
        double lastRefreshTime = 0.0;
        std::mutex mutex;
        static constexpr double MIN_REFRESH_INTERVAL = 1.0;  // 最小刷新间隔（秒）

        void refresh();
        ModuleInfoItem findByAddress(uint64_t addr);  // 返回值拷贝，避免悬空指针
        std::string formatWithModule(uint64_t addr);
        std::string formatWithSymbol(uint64_t addr);
        std::string formatAddressWithModuleAndSymbol(uint64_t addr);
        bool tryFindContainingSymbol(uint64_t addr, SymbolInfoItem& outSymbol, uint64_t& outOffset);
        bool ensureSymbolListCached(const ModuleInfoItem& module, std::vector<SymbolInfoItem>& outSymbols);
        void invalidate() {
            std::lock_guard<std::mutex> lock(mutex);
            valid = false;
            symbolCacheByModuleBase.clear();
        }
    } moduleCache;

private:
    AppContext() = default;
    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;

    mutable std::mutex nameMutex_;
    std::string selectedName_;
};
