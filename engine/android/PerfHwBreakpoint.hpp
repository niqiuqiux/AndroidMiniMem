#pragma once
// ============================================================================
// PerfHwBreakpoint.hpp — 用户态 perf_event_open 硬件断点引擎
// ----------------------------------------------------------------------------
// 在仅有 root、无自定义内核驱动的设备上，用 perf_event_open(PERF_TYPE_BREAKPOINT)
// 直接驱动 ARM64 硬件 debug 寄存器（执行断点 DBGBVR / watchpoint DBGWVR），作为
// 内核驱动断点之外的第三条路径。
//
// 命中模型对齐内核驱动的"被动累积 + 前端轮询"：后台线程持续消费 perf ring buffer，
// 把命中记录解析后累积到用户态队列；ReadHwBpInfo 拉取即清空。
//
// 进程级逻辑断点：一个 handle 代表"某进程某地址的断点"，引擎内部为该进程每个线程
// (tid) 各开一个 perf fd；后台线程周期性 rescan /proc/<pid>/task 跟随新线程、
// 回收已退出线程。
//
// 参考实现：enenH/pwatch-c。
// ============================================================================

#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <asm/perf_regs.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <mutex>
#include <map>
#include <deque>
#include <vector>
#include <algorithm>

#include "AndroidDriverTypes.h"
#include "Logger.hpp"

// PERF_REG_ARM64_MAX 来自 <asm/perf_regs.h>（aarch64 = 33：x0..x29 + lr + sp + pc）。
// 提供兜底以防个别编译环境头缺失。
#ifndef PERF_REG_ARM64_MAX
#define PERF_REG_ARM64_MAX 33
#endif

// perf 断点 handle 的高位标记（bit62）。仅用于快速预筛 / 日志可读；后端归属判别
// 以 PerfHwBreakpoint::Owns()（查 map）为权威，不依赖位模式——ARM64 内核驱动
// handle 可能是内核指针（bit63 恒 1），故不可用 bit63 作 tag。
static constexpr uint64_t PERF_BP_HANDLE_TAG = 0x4000000000000000ULL;

class PerfHwBreakpoint {
public:
    static PerfHwBreakpoint& Get() {
        static PerfHwBreakpoint inst;   // Meyer's 单例，函数局部 static，跨 TU 唯一
        return inst;
    }

    // 新增进程级硬件断点。成功返回带 tag 的逻辑 handle，失败返回 0。
    // 注意：参数是 pid（进程），引擎内部遍历其所有线程各开一个 perf fd。
    uint64_t AddProcessHwBp(int pid, uint64_t addr, unsigned int len, unsigned int type) {
        if (pid <= 0 || addr == 0) {
            return 0;
        }
        std::vector<int> tids = ScanTids(pid);
        if (tids.empty()) {
            LOGEF("[perf] AddProcessHwBp: 进程 %d 无可用线程", pid);
            return 0;
        }

        std::lock_guard<std::mutex> lk(mMutex);
        Entry e;
        e.pid = pid;
        e.bp_addr = addr;
        e.bp_len = len;
        e.bp_type = type;
        e.enabled = true;
        e.totalHits = 0;

        for (int tid : tids) {
            SubFd s;
            if (OpenSub(e, tid, /*enable*/ true, s)) {
                e.subs[tid] = s;
            }
        }
        if (e.subs.empty()) {
            LOGEF("[perf] AddProcessHwBp: pid=%d addr=0x%llx 所有线程下断失败",
                  pid, (unsigned long long)addr);
            return 0;
        }

        uint64_t handle = PERF_BP_HANDLE_TAG | (uint64_t)(++mSeq);
        mEntries.emplace(handle, std::move(e));
        EnsureWorkerLocked();
        Wake();
        LOGDF("[perf] 新增断点 pid=%d addr=0x%llx type=%u len=%u 线程数=%zu handle=0x%llx",
              pid, (unsigned long long)addr, type, len, tids.size(),
              (unsigned long long)handle);
        return handle;
    }

    // 删除逻辑断点（关闭其所有线程的 fd）。
    bool DelProcessHwBp(uint64_t h) {
        std::lock_guard<std::mutex> lk(mMutex);
        auto it = mEntries.find(h);
        if (it == mEntries.end()) {
            return false;
        }
        for (auto& kv : it->second.subs) {
            CloseSub(kv.second);
        }
        mEntries.erase(it);
        Wake();
        LOGDF("[perf] 删除断点 handle=0x%llx", (unsigned long long)h);
        return true;
    }

    // 暂停：对所有子 fd ioctl DISABLE（保留 fd 与命中记录）。
    bool SuspendProcessHwBp(uint64_t h) {
        std::lock_guard<std::mutex> lk(mMutex);
        auto it = mEntries.find(h);
        if (it == mEntries.end()) {
            return false;
        }
        for (auto& kv : it->second.subs) {
            if (kv.second.fd >= 0) {
                ioctl(kv.second.fd, PERF_EVENT_IOC_DISABLE, 0);
            }
        }
        it->second.enabled = false;
        Wake();
        return true;
    }

    // 恢复：对所有子 fd ioctl ENABLE。
    bool ResumeProcessHwBp(uint64_t h) {
        std::lock_guard<std::mutex> lk(mMutex);
        auto it = mEntries.find(h);
        if (it == mEntries.end()) {
            return false;
        }
        for (auto& kv : it->second.subs) {
            if (kv.second.fd >= 0) {
                ioctl(kv.second.fd, PERF_EVENT_IOC_ENABLE, 0);
            }
        }
        it->second.enabled = true;
        Wake();
        return true;
    }

    // 拉取自上次以来的命中记录（清空队列），返回累计命中数。
    bool ReadHwBpInfo(uint64_t h, uint64_t& nHitTotalCount, std::vector<HW_HIT_ITEM>& vOutput) {
        std::lock_guard<std::mutex> lk(mMutex);
        auto it = mEntries.find(h);
        if (it == mEntries.end()) {
            return false;
        }
        vOutput.insert(vOutput.end(), it->second.hits.begin(), it->second.hits.end());
        it->second.hits.clear();
        nHitTotalCount = it->second.totalHits;
        return true;
    }

    // 后端归属判别（权威）：handle 是否属于本引擎。
    bool Owns(uint64_t h) {
        std::lock_guard<std::mutex> lk(mMutex);
        return mEntries.find(h) != mEntries.end();
    }

private:
    // 单个线程的 perf fd + ring buffer
    struct SubFd {
        int fd = -1;
        void* mmap_addr = MAP_FAILED;
        size_t mmap_size = 0;
        uint8_t* data_addr = nullptr;   // 数据区起点 = mmap_addr + 1 页
        uint64_t data_size = 0;         // 数据区字节数 = 2^n 页
        uint64_t tail = 0;              // 消费游标（uint64，避免 pwatch-c 的 int 溢出）
        int tid = 0;
    };

    // 进程级逻辑断点（聚合该进程所有线程）
    struct Entry {
        int pid = 0;
        uint64_t bp_addr = 0;
        unsigned int bp_len = 0;
        unsigned int bp_type = 0;
        bool enabled = true;
        uint64_t totalHits = 0;         // 累计命中（含 LOST）
        std::deque<HW_HIT_ITEM> hits;   // 自上次 Read 以来的新记录（跨所有线程聚合）
        std::map<int, SubFd> subs;      // tid -> SubFd
    };

    static constexpr int kRingPages = 2;   // 数据区 2^2 = 4 页

    std::mutex mMutex;                      // 保护 mEntries 及全部 SubFd 字段
    std::map<uint64_t, Entry> mEntries;
    std::atomic<uint32_t> mSeq{0};
    std::atomic<bool> mRunning{false};
    std::thread mWorker;
    int mWakeR = -1;                        // 自唤醒管道（打断 poll）
    int mWakeW = -1;

    PerfHwBreakpoint() = default;
    ~PerfHwBreakpoint() {
        mRunning.store(false);
        Wake();
        if (mWorker.joinable()) {
            mWorker.join();
        }
        std::lock_guard<std::mutex> lk(mMutex);
        for (auto& e : mEntries) {
            for (auto& kv : e.second.subs) {
                CloseSub(kv.second);
            }
        }
        mEntries.clear();
        if (mWakeR >= 0) close(mWakeR);
        if (mWakeW >= 0) close(mWakeW);
    }
    PerfHwBreakpoint(const PerfHwBreakpoint&) = delete;
    PerfHwBreakpoint& operator=(const PerfHwBreakpoint&) = delete;

    static long PerfEventOpen(struct perf_event_attr* attr, pid_t pid, int cpu,
                              int group_fd, unsigned long flags) {
        return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
    }

    static uint64_t NowNs() {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
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

    // 为单个 tid 打开 perf 断点并 mmap ring buffer。成功填充 out 返回 true。
    bool OpenSub(const Entry& e, int tid, bool enable, SubFd& out) {
        struct perf_event_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.size = sizeof(attr);
        attr.type = PERF_TYPE_BREAKPOINT;
        attr.bp_type = e.bp_type;                 // 与 <linux/hw_breakpoint.h> 枚举一致
        attr.bp_addr = e.bp_addr;
        attr.bp_len = e.bp_len;                   // X 已在 CApi 层规整为 4
        attr.sample_period = 1;                   // 每命中 1 次产生一条采样记录
        attr.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_REGS_USER;
        attr.sample_regs_user = ((1ULL << PERF_REG_ARM64_MAX) - 1);  // 33 个用户寄存器
        attr.disabled = 1;
        attr.exclude_kernel = 1;                  // 权限要求：不监控内核
        attr.exclude_hv = 1;

        long fd = PerfEventOpen(&attr, tid, -1, -1, PERF_FLAG_FD_CLOEXEC);
        if (fd < 0) {
            // errno 分类：EACCES(paranoid/SELinux)、EINVAL(参数/对齐/X 与 RW 混用)、
            // ENOSPC/EBUSY(硬件 slot 耗尽 ~4/线程/类)、ESRCH(线程已退出，正常竞态)
            LOGEF("[perf] perf_event_open tid=%d addr=0x%llx type=%u len=%u 失败: %s",
                  tid, (unsigned long long)e.bp_addr, e.bp_type, e.bp_len, strerror(errno));
            return false;
        }

        long page = sysconf(_SC_PAGESIZE);
        size_t mmap_size = (size_t)(1 + (1u << kRingPages)) * (size_t)page;  // 1 元数据页 + 数据页
        void* m = mmap(nullptr, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, (int)fd, 0);
        if (m == MAP_FAILED) {
            LOGEF("[perf] mmap tid=%d 失败: %s", tid, strerror(errno));
            close((int)fd);
            return false;
        }

        ioctl((int)fd, PERF_EVENT_IOC_RESET, 0);
        if (enable) {
            ioctl((int)fd, PERF_EVENT_IOC_ENABLE, 0);
        }

        out.fd = (int)fd;
        out.mmap_addr = m;
        out.mmap_size = mmap_size;
        out.data_addr = (uint8_t*)m + page;                              // 跳过元数据页
        out.data_size = (uint64_t)((1u << kRingPages) * (size_t)page);   // 不依赖 meta->data_size，兼容老内核
        out.tail = 0;
        out.tid = tid;
        return true;
    }

    // 关闭单个子 fd（统一先 munmap 后 close）。
    static void CloseSub(SubFd& s) {
        if (s.fd >= 0) {
            ioctl(s.fd, PERF_EVENT_IOC_DISABLE, 0);
        }
        if (s.mmap_addr && s.mmap_addr != MAP_FAILED) {
            munmap(s.mmap_addr, s.mmap_size);
        }
        if (s.fd >= 0) {
            close(s.fd);
        }
        s.fd = -1;
        s.mmap_addr = MAP_FAILED;
        s.data_addr = nullptr;
    }

    // 环形缓冲跨界安全拷贝（off 已 % size）。
    static void CopyRing(void* dst, const uint8_t* base, uint64_t size, uint64_t off, uint64_t n) {
        if (off + n <= size) {
            memcpy(dst, base + off, n);
        } else {
            uint64_t first = size - off;
            memcpy(dst, base + off, first);
            memcpy((uint8_t*)dst + first, base, n - first);
        }
    }

    // 解析单个 SubFd 的 ring buffer（持锁调用）。
    void ConsumeRingLocked(SubFd& s, Entry& e) {
        auto* meta = (struct perf_event_mmap_page*)s.mmap_addr;
        uint64_t head = __atomic_load_n(&meta->data_head, __ATOMIC_ACQUIRE);  // 内核写，acquire
        uint64_t tail = s.tail;
        const uint8_t* base = s.data_addr;
        const uint64_t size = s.data_size;

        while (tail < head) {
            uint64_t off = tail % size;
            struct perf_event_header hdr;
            CopyRing(&hdr, base, size, off, sizeof(hdr));
            if (hdr.size == 0) {
                break;  // 防御异常记录，避免死循环
            }

            if (hdr.type == PERF_RECORD_SAMPLE) {
                // 按 sample_type 固定顺序：PERF_SAMPLE_TID(u32 pid,u32 tid) → PERF_SAMPLE_REGS_USER(u64 abi, 33×u64)
                uint64_t cur = (off + sizeof(hdr)) % size;
                uint32_t pidtid[2];
                CopyRing(pidtid, base, size, cur, sizeof(pidtid));
                cur = (cur + sizeof(pidtid)) % size;
                uint64_t abi;
                CopyRing(&abi, base, size, cur, sizeof(abi));
                cur = (cur + sizeof(abi)) % size;
                uint64_t regs[PERF_REG_ARM64_MAX];
                CopyRing(regs, base, size, cur, sizeof(regs));

                HW_HIT_ITEM item;
                memset(&item, 0, sizeof(item));
                item.task_id = (uint64_t)pidtid[1];     // tid
                item.hit_addr = e.bp_addr;              // perf 不直接给命中地址，用断点地址（与驱动语义一致）
                item.hit_time = NowNs();
                for (int i = 0; i <= 30; ++i) {         // x0..x29 + lr(x30)
                    item.regs_info.regs[i] = regs[i];
                }
                item.regs_info.sp = regs[31];
                item.regs_info.pc = regs[32];
                // pstate/orig_x0/syscallno 已随 item 整体 memset 清零（perf 不提供；HW_HIT_ITEM 已无 fpsimd_info）
                e.hits.push_back(item);
                e.totalHits++;
            } else if (hdr.type == PERF_RECORD_LOST) {
                // 记录格式：header 之后接 u64 id, u64 lost
                uint64_t lost[2];
                CopyRing(lost, base, size, (off + sizeof(hdr)) % size, sizeof(lost));
                e.totalHits += lost[1];
                LOGDF("[perf] RECORD_LOST tid=%d lost=%llu", s.tid, (unsigned long long)lost[1]);
            }

            tail += hdr.size;
        }

        s.tail = tail;
        __atomic_store_n(&meta->data_tail, tail, __ATOMIC_RELEASE);  // 归还空间给内核，release
    }

    // rescan 所有断点的线程集合：补开新线程、回收已退出线程（持锁调用）。
    void RescanAllLocked() {
        for (auto& kv : mEntries) {
            Entry& e = kv.second;
            std::vector<int> tids = ScanTids(e.pid);
            if (tids.empty()) {
                continue;  // 进程可能已退出，下次再试；不在此清空（避免误删瞬时读取失败）
            }
            // 补开新线程
            for (int tid : tids) {
                if (e.subs.find(tid) == e.subs.end()) {
                    SubFd s;
                    if (OpenSub(e, tid, e.enabled, s)) {
                        e.subs[tid] = s;
                        LOGDF("[perf] 跟随新线程 tid=%d handle=0x%llx", tid,
                              (unsigned long long)kv.first);
                    }
                }
            }
            // 回收已退出线程
            for (auto sit = e.subs.begin(); sit != e.subs.end(); ) {
                if (std::find(tids.begin(), tids.end(), sit->first) == tids.end()) {
                    CloseSub(sit->second);
                    sit = e.subs.erase(sit);
                } else {
                    ++sit;
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
            LOGEF("[perf] pipe2 失败: %s（退化为 poll 超时轮询）", strerror(errno));
            mWakeR = mWakeW = -1;
        }
        mRunning.store(true);
        mWorker = std::thread(&PerfHwBreakpoint::WorkerLoop, this);
    }

    void Wake() {
        if (mWakeW >= 0) {
            char c = 1;
            ssize_t n = write(mWakeW, &c, 1);
            (void)n;
        }
    }

    // 后台消费线程：单 mutex + pipe 自唤醒 + poll 200ms 兜底。
    void WorkerLoop() {
        std::vector<struct pollfd> pfds;
        std::vector<uint64_t> handleOf;   // 平行数组：pfds[i] 对应的 handle（0 = 唤醒管道）
        std::vector<int> tidOf;
        int rescanCounter = 0;

        while (mRunning.load()) {
            pfds.clear();
            handleOf.clear();
            tidOf.clear();
            {
                std::lock_guard<std::mutex> lk(mMutex);
                if (mWakeR >= 0) {
                    struct pollfd pf;
                    pf.fd = mWakeR; pf.events = POLLIN; pf.revents = 0;
                    pfds.push_back(pf); handleOf.push_back(0); tidOf.push_back(-1);
                }
                for (auto& kv : mEntries) {
                    if (!kv.second.enabled) continue;
                    for (auto& sub : kv.second.subs) {
                        struct pollfd pf;
                        pf.fd = sub.second.fd; pf.events = POLLIN; pf.revents = 0;
                        pfds.push_back(pf);
                        handleOf.push_back(kv.first);
                        tidOf.push_back(sub.first);
                    }
                }
            }

            int ret = poll(pfds.data(), (nfds_t)pfds.size(), 200);
            if (ret < 0 && errno != EINTR) {
                continue;  // 其他错误下一轮重建快照
            }

            std::lock_guard<std::mutex> lk(mMutex);
            for (size_t i = 0; i < pfds.size(); ++i) {
                if (!(pfds[i].revents & POLLIN)) continue;
                if (handleOf[i] == 0) {
                    char buf[64];
                    while (read(mWakeR, buf, sizeof(buf)) > 0) {}  // 排空唤醒管道
                    continue;
                }
                // 反查 map：entry 可能已被 Del（关 fd），找不到则跳过——杜绝 use-after-close
                auto it = mEntries.find(handleOf[i]);
                if (it == mEntries.end()) continue;
                auto sit = it->second.subs.find(tidOf[i]);
                if (sit == it->second.subs.end()) continue;
                ConsumeRingLocked(sit->second, it->second);
            }
            // 约每 5 轮(~1s) rescan 一次，跟随新线程 / 回收退出线程
            if (++rescanCounter >= 5) {
                rescanCounter = 0;
                if (!mEntries.empty()) {
                    RescanAllLocked();
                }
            }
        }
    }
};
