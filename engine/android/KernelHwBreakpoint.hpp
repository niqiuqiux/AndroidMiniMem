#pragma once
// ============================================================================
// KernelHwBreakpoint.hpp — 内核驱动硬件断点的进程级封装 + 自动跟随新线程
// ----------------------------------------------------------------------------
// 内核驱动断点本身是 per-tid 的（KernelDriver().AddHardwareBreakpoint(tid,...)），原先在
// CApi::SetBreakpoint 里一次性快照线程下断，不跟随设断点后新建的线程。本引擎
// 把它封装成"进程级逻辑断点"，与 PerfHwBreakpoint 对称：
//   - 一个 handle 代表"某进程某地址的断点"，内部为该进程每线程下一个内核断点；
//   - 后台线程周期 rescan /proc/<pid>/task，对新线程补下断、对已退出线程回收。
// 命中沿用内核"被动累积"模型：ReadHwBpInfo 时实时遍历各 tid 句柄向驱动拉取并
// 聚合（内核侧已累积，无需后台消费，比 perf 的 ring buffer 简单）。
// ============================================================================

#include <dirent.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <mutex>
#include <map>
#include <set>
#include <vector>
#include <algorithm>

#include "AndroidKernelDriver.h"   // KernelDriver + HW_HIT_ITEM
#include "Logger.hpp"

// 内核逻辑断点 handle 的高位标记（bit61）。与 perf 的 bit62 区分；归属判别仍以
// KernelHwBreakpoint::Owns()（查 map）为权威，tag 仅作快速预筛 / 日志可读。
static constexpr uint64_t KERNEL_BP_HANDLE_TAG = 0x2000000000000000ULL;

class KernelHwBreakpoint {
public:
    static KernelHwBreakpoint& Get() {
        static KernelHwBreakpoint inst;
        return inst;
    }

    // 新增进程级内核硬件断点。成功返回带 tag 的逻辑 handle，失败返回 0。
    uint64_t AddProcessHwBp(int pid, uint64_t addr, unsigned int len,
                            unsigned int type, bool forceReclaim) {
        if (pid <= 0 || addr == 0) {
            return 0;
        }
        if (!KernelDriver().IsConnected()) {
            LOGEF("[kbp] AddProcessHwBp: 驱动未连接");
            return 0;
        }
        std::vector<int> tids = ScanTids(pid);
        if (tids.empty()) {
            LOGEF("[kbp] AddProcessHwBp: 进程 %d 无可用线程", pid);
            return 0;
        }

        std::lock_guard<std::mutex> lk(mMutex);
        Entry e;
        e.pid = pid;
        e.bp_addr = addr;
        e.bp_len = len;
        e.bp_type = type;
        e.enabled = true;
        e.force_reclaim = forceReclaim;

        for (int tid : tids) {
            HardwareBreakpointInstallResult install =
                InstallForTid(pid, tid, addr, len, type, forceReclaim,
                              "initial", 0, true);
            uint64_t dh = install.handle;
            if (dh != 0) {
                e.subs[tid] = dh;
            } else {
                e.loggedInstallFailures.insert(tid);
            }
        }
        if (e.subs.empty()) {
            LOGEF("[kbp] AddProcessHwBp: pid=%d addr=0x%llx 所有线程下断失败",
                  pid, (unsigned long long)addr);
            return 0;
        }

        const size_t installedCount = e.subs.size();
        const size_t failedCount = tids.size() - installedCount;
        uint64_t handle = KERNEL_BP_HANDLE_TAG | (uint64_t)(++mSeq);
        mEntries.emplace(handle, std::move(e));
        EnsureWorkerLocked();
        Wake();
        LOGDF("[kbp] 新增断点 pid=%d addr=0x%llx type=%u len=%u "
              "force_reclaim=%d 扫描线程数=%zu 成功=%zu 失败=%zu handle=0x%llx",
              pid, (unsigned long long)addr, type, len,
              forceReclaim ? 1 : 0, tids.size(),
              installedCount, failedCount,
              (unsigned long long)handle);
        return handle;
    }

    bool DelProcessHwBp(uint64_t h) {
        std::lock_guard<std::mutex> lk(mMutex);
        auto it = mEntries.find(h);
        if (it == mEntries.end()) {
            return false;
        }
        for (auto& kv : it->second.subs) {
            KernelDriver().RemoveHardwareBreakpoint(kv.second);
        }
        mEntries.erase(it);
        Wake();
        LOGDF("[kbp] 删除断点 handle=0x%llx", (unsigned long long)h);
        return true;
    }

    bool SuspendProcessHwBp(uint64_t h) {
        std::lock_guard<std::mutex> lk(mMutex);
        auto it = mEntries.find(h);
        if (it == mEntries.end()) {
            return false;
        }
        for (auto& kv : it->second.subs) {
            KernelDriver().DisableHardwareBreakpoint(kv.second);
        }
        it->second.enabled = false;
        return true;
    }

    bool ResumeProcessHwBp(uint64_t h) {
        std::lock_guard<std::mutex> lk(mMutex);
        auto it = mEntries.find(h);
        if (it == mEntries.end()) {
            return false;
        }
        for (auto& kv : it->second.subs) {
            KernelDriver().EnableHardwareBreakpoint(kv.second);
        }
        it->second.enabled = true;
        return true;
    }

    // 实时遍历各 tid 句柄向驱动拉取命中并聚合（内核已被动累积）。
    bool ReadHwBpInfo(uint64_t h, uint64_t& nHitTotalCount, std::vector<HW_HIT_ITEM>& vOutput) {
        std::lock_guard<std::mutex> lk(mMutex);
        auto it = mEntries.find(h);
        if (it == mEntries.end()) {
            return false;
        }
        uint64_t total = 0;
        for (auto& kv : it->second.subs) {
            uint64_t cnt = 0;
            std::vector<HW_HIT_ITEM> sub;
            if (KernelDriver().ReadHardwareBreakpointInfo(kv.second, cnt, sub)) {
                total += cnt;
                // 与 perf 后端对齐：命中记录只保留通用寄存器 x0..x30 + sp + pc，
                // 清零驱动额外填充的 pstate/orig_x0/syscallno（HW_HIT_ITEM 已无 fpsimd_info 字段）。
                // regs_info 为 my_user_pt_regs（regs[0..30]=x0..x30, 后接 sp, pc），与 perf 填充顺序一致，
                // 故通用寄存器/pc/sp 原样保留即与 perf 对应，无需重排。
                for (auto& item : sub) {
                    item.regs_info.pstate = 0;
                    item.regs_info.orig_x0 = 0;
                    item.regs_info.syscallno = 0;
                }
                vOutput.insert(vOutput.end(), sub.begin(), sub.end());
            }
        }
        nHitTotalCount = total;
        return true;
    }

    // 后端归属判别（权威）。
    bool Owns(uint64_t h) {
        std::lock_guard<std::mutex> lk(mMutex);
        return mEntries.find(h) != mEntries.end();
    }

private:
    struct Entry {
        int pid = 0;
        uint64_t bp_addr = 0;
        unsigned int bp_len = 0;
        unsigned int bp_type = 0;
        bool enabled = true;
        bool force_reclaim = false;
        std::map<int, uint64_t> subs;   // tid -> 驱动 handle
        std::set<int> loggedInstallFailures; // 避免 rescan 对同一存活 TID 重复刷失败日志
    };

    std::mutex mMutex;                  // 保护 mEntries
    std::map<uint64_t, Entry> mEntries;
    std::atomic<uint32_t> mSeq{0};
    std::atomic<bool> mRunning{false};
    std::thread mWorker;                // 仅做周期 rescan（命中按需向驱动拉）
    int mWakeR = -1;
    int mWakeW = -1;

    KernelHwBreakpoint() = default;
    ~KernelHwBreakpoint() {
        mRunning.store(false);
        Wake();
        if (mWorker.joinable()) {
            mWorker.join();
        }
        std::lock_guard<std::mutex> lk(mMutex);
        for (auto& e : mEntries) {
            for (auto& kv : e.second.subs) {
                KernelDriver().RemoveHardwareBreakpoint(kv.second);
            }
        }
        mEntries.clear();
        if (mWakeR >= 0) close(mWakeR);
        if (mWakeW >= 0) close(mWakeW);
    }
    KernelHwBreakpoint(const KernelHwBreakpoint&) = delete;
    KernelHwBreakpoint& operator=(const KernelHwBreakpoint&) = delete;

    static HardwareBreakpointInstallResult InstallForTid(
        int pid, int tid, uint64_t addr, unsigned int len, unsigned int type,
        bool forceReclaim, const char* phase, uint64_t logicalHandle,
        bool logFailure) {
        HardwareBreakpointInstallResult install =
            KernelDriver().AddHardwareBreakpoint(tid, addr, len, type, false);
        if (install.handle != 0) {
            LOGDF("[kbp] 下断成功 phase=%s pid=%d tid=%d addr=0x%llx "
                  "type=%u len=%u interface_result=%d force_reclaim=0 "
                  "logical_handle=0x%llx driver_handle=0x%llx",
                  phase, pid, tid, (unsigned long long)addr, type, len,
                  install.interfaceResult, (unsigned long long)logicalHandle,
                  (unsigned long long)install.handle);
            return install;
        }

        if (!forceReclaim) {
            if (logFailure) {
                LOGEF("[kbp] 下断失败 phase=%s pid=%d tid=%d addr=0x%llx "
                      "type=%u len=%u interface_result=%d error=%d(%s) "
                      "force_reclaim=0 logical_handle=0x%llx",
                      phase, pid, tid, (unsigned long long)addr, type, len,
                      install.interfaceResult, install.errorCode,
                      strerror(install.errorCode),
                      (unsigned long long)logicalHandle);
            }
            return install;
        }

        if (logFailure) {
            LOGEF("[kbp] 普通下断失败，启动抢占重试 phase=%s pid=%d tid=%d "
                  "addr=0x%llx type=%u len=%u interface_result=%d error=%d(%s) "
                  "logical_handle=0x%llx",
                  phase, pid, tid, (unsigned long long)addr, type, len,
                  install.interfaceResult, install.errorCode,
                  strerror(install.errorCode),
                  (unsigned long long)logicalHandle);
        }

        HardwareBreakpointInstallResult reclaimed =
            KernelDriver().AddHardwareBreakpoint(tid, addr, len, type, true);
        if (reclaimed.handle != 0) {
            LOGDF("[kbp] 抢占重试成功 phase=%s pid=%d tid=%d addr=0x%llx "
                  "type=%u len=%u interface_result=%d reclaimed_slots=%u "
                  "logical_handle=0x%llx driver_handle=0x%llx",
                  phase, pid, tid, (unsigned long long)addr, type, len,
                  reclaimed.interfaceResult, reclaimed.reclaimedSlots,
                  (unsigned long long)logicalHandle,
                  (unsigned long long)reclaimed.handle);
        } else if (logFailure) {
            LOGEF("[kbp] 抢占重试失败 phase=%s pid=%d tid=%d addr=0x%llx "
                  "type=%u len=%u interface_result=%d error=%d(%s) "
                  "reclaimed_slots=%u logical_handle=0x%llx",
                  phase, pid, tid, (unsigned long long)addr, type, len,
                  reclaimed.interfaceResult, reclaimed.errorCode,
                  strerror(reclaimed.errorCode), reclaimed.reclaimedSlots,
                  (unsigned long long)logicalHandle);
        }
        return reclaimed;
    }

    // 读取 /proc/<pid>/task 下所有 tid（纯数字目录名）
    static std::vector<int> ScanTids(int pid) {
        std::vector<int> tids;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/task", pid);
        DIR* dir = opendir(path);
        if (!dir) {
            return tids;
        }
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            const char* name = ent->d_name;
            if (name[0] < '0' || name[0] > '9') {
                continue;
            }
            bool pure = true;
            for (const char* p = name; *p; ++p) {
                if (*p < '0' || *p > '9') { pure = false; break; }
            }
            if (pure) {
                tids.push_back(atoi(name));
            }
        }
        closedir(dir);
        return tids;
    }

    // rescan 所有断点的线程集合：补下断新线程、回收已退出线程（持锁调用）。
    void RescanAllLocked() {
        for (auto& kv : mEntries) {
            Entry& e = kv.second;
            std::vector<int> tids = ScanTids(e.pid);
            if (tids.empty()) {
                continue;  // 进程可能已退出，下次再试
            }
            // 补下断新线程
            for (int tid : tids) {
                if (e.subs.find(tid) == e.subs.end()) {
                    const bool logFailure =
                        e.loggedInstallFailures.find(tid) ==
                        e.loggedInstallFailures.end();
                    HardwareBreakpointInstallResult install =
                        InstallForTid(e.pid, tid, e.bp_addr, e.bp_len,
                                      e.bp_type, e.force_reclaim, "follow",
                                      kv.first, logFailure);
                    uint64_t dh = install.handle;
                    if (dh != 0) {
                        // 与逻辑断点当前启停状态保持一致
                        if (!e.enabled) {
                            KernelDriver().DisableHardwareBreakpoint(dh);
                        }
                        e.subs[tid] = dh;
                        e.loggedInstallFailures.erase(tid);
                    } else {
                        e.loggedInstallFailures.insert(tid);
                    }
                }
            }
            // 回收已退出线程
            for (auto sit = e.subs.begin(); sit != e.subs.end(); ) {
                if (std::find(tids.begin(), tids.end(), sit->first) == tids.end()) {
                    KernelDriver().RemoveHardwareBreakpoint(sit->second);
                    sit = e.subs.erase(sit);
                } else {
                    ++sit;
                }
            }
            for (auto fit = e.loggedInstallFailures.begin();
                 fit != e.loggedInstallFailures.end(); ) {
                if (std::find(tids.begin(), tids.end(), *fit) == tids.end()) {
                    fit = e.loggedInstallFailures.erase(fit);
                } else {
                    ++fit;
                }
            }
        }
    }

    void EnsureWorkerLocked() {
        if (mRunning.load()) {
            return;
        }
        int fds[2];
        if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) {
            mWakeR = fds[0];
            mWakeW = fds[1];
        } else {
            LOGEF("[kbp] pipe2 失败: %s（退化为定时轮询）", strerror(errno));
            mWakeR = mWakeW = -1;
        }
        mRunning.store(true);
        mWorker = std::thread(&KernelHwBreakpoint::WorkerLoop, this);
    }

    void Wake() {
        if (mWakeW >= 0) {
            char c = 1;
            ssize_t n = write(mWakeW, &c, 1);
            (void)n;
        }
    }

    // 后台线程：内核断点无 fd 可 poll，靠 wake 管道 + 1s 定时触发 rescan。
    void WorkerLoop() {
        while (mRunning.load()) {
            struct pollfd pf;
            int n = 0;
            if (mWakeR >= 0) {
                pf.fd = mWakeR; pf.events = POLLIN; pf.revents = 0;
                n = 1;
            }
            poll(n ? &pf : nullptr, n, 1000);   // 1s 定时 rescan
            if (n && (pf.revents & POLLIN)) {
                char buf[64];
                while (read(mWakeR, buf, sizeof(buf)) > 0) {}   // 排空唤醒管道
            }
            std::lock_guard<std::mutex> lk(mMutex);
            if (!mEntries.empty()) {
                RescanAllLocked();
            }
        }
    }
};
