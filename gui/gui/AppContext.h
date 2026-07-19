#pragma once

#include "../mem/MemTypes.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Mem {
class IMemService;
}

class AppContext {
public:
    class TargetMutation {
    public:
        TargetMutation(TargetMutation&& other) noexcept;
        TargetMutation& operator=(TargetMutation&&) = delete;
        TargetMutation(const TargetMutation&) = delete;
        TargetMutation& operator=(const TargetMutation&) = delete;
        ~TargetMutation();

        explicit operator bool() const { return owner_ != nullptr; }
        const Mem::TargetSnapshot& previousTarget() const {
            return previousTarget_;
        }
        void publish(int pid, int processHandle, const std::string& name);
        void clear();

    private:
        friend class AppContext;
        TargetMutation(AppContext& owner,
                       std::unique_lock<std::mutex> stateLock,
                       Mem::TargetSnapshot previousTarget);
        void finish();

        AppContext* owner_ = nullptr;
        std::unique_lock<std::mutex> stateLock_;
        Mem::TargetSnapshot previousTarget_;
    };

    static AppContext& Get() {
        static AppContext instance;
        return instance;
    }

    std::atomic<int> selectedPid{0};
    std::atomic<int> processHandle{0};
    std::atomic<uint64_t> processRevision{0};

    std::optional<TargetMutation> beginTargetMutation(
        const std::optional<Mem::TargetSnapshot>& expected,
        uint64_t connectionGeneration);
    void clearProcessForDisconnect();
    Mem::TargetSnapshot snapshotTarget(uint64_t connectionGeneration) const;
    bool matchesStableTarget(const Mem::TargetSnapshot& expected,
                             uint64_t connectionGeneration) const;

    bool hasProcess() const {
        return selectedPid.load(std::memory_order_relaxed) != 0 &&
               processHandle.load(std::memory_order_relaxed) != 0;
    }

    std::string getSelectedName() const {
        std::lock_guard<std::mutex> lock(nameMutex_);
        return selectedName_;
    }

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

        std::vector<Mem::ModuleInfo> modules;
        std::unordered_map<uint64_t, SymbolListCacheEntry>
            symbolCacheByModuleBase;
        bool valid = false;
        double lastRefreshTime = 0.0;
        std::mutex mutex;
        static constexpr double MIN_REFRESH_INTERVAL = 1.0;

        void refresh(Mem::IMemService& service);
        Mem::ModuleInfo findByAddress(uint64_t addr);
        std::string formatWithModule(uint64_t addr);
        std::string formatWithSymbol(uint64_t addr,
                                     Mem::IMemService& service);
        std::string formatAddressWithModuleAndSymbol(
            uint64_t addr, Mem::IMemService& service);
        bool tryFindContainingSymbol(uint64_t addr,
                                     Mem::IMemService& service,
                                     SymbolInfoItem& outSymbol,
                                     uint64_t& outOffset);
        bool ensureSymbolListCached(
            const Mem::ModuleInfo& module,
            Mem::IMemService& service,
            std::vector<SymbolInfoItem>& outSymbols);
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
    mutable std::mutex processStateMutex_;
    std::string selectedName_;
};
