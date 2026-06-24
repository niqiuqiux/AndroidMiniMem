#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "../common/IMemoryOp.hpp"
#include "../common/LinuxProc.hpp"
#include "../common/Logger.hpp"


#include "MemoryReaderWriter.h"


class AndroidMemKernel : public IMemoryOp {
private:
    pid_t pid_ = 0;
    std::string processName_;
    std::shared_ptr<CMemoryReaderWriter> driver = driver_;
    uint64_t driverProcessHandle_ = 0;

public:
    AndroidMemKernel() {
        type = 3;
    }

    ~AndroidMemKernel() {
        driver->DisconnectDriver();
    }

    bool connect() {
        LOGDF("Connecting via anon_fd...");
        int rc = driver->ConnectDriver("");
        if (rc < 0) {
            LOGEF("ConnectDriver failed rc=%d %s", rc, strerror(errno));
            return false;
        }
        LOGDF("ConnectDriver success");
        return driver->IsDriverConnected();
    }

    bool isConnected() {
        bool ok = driver->IsDriverConnected();
        if (!ok) {
            LOGEF("IsDriverConnected failed");
        }
        return ok;
    }

    bool GetCardTime(uint64_t& outCardTime) override {
        return driver->GetCardTime(outCardTime);
    }


    bool OpenProcess(pid_t pid) override {
        if (pid == pid_) {
            return true;
        }
        pid_ = pid;
        if (pid_ <= 0) return false;

        if (!isConnected()) {
            if (!connect()) {
                LOGEF("connect() failed before OpenProcess, pid: %d", pid_);
                return false;
            }
        }

        driverProcessHandle_ = driver->OpenProcess(static_cast<uint64_t>(pid_));
        if (driverProcessHandle_ == 0) {
            LOGEF("OpenProcess failed, pid: %d", pid_);
            return false;
        }
        // 尝试读取命令行作为进程名
        char name[256] = {0};
        if (driver->GetProcessComm(static_cast<uint64_t>(pid_), name, sizeof(name))) {
            processName_ = name;
        }
        return true;
    }

    bool CloseHandle() override {
        pid_ = 0;
        bool ok = true;
        if (driverProcessHandle_) {
            ok = driver->CloseHandle(driverProcessHandle_);
        }
        driverProcessHandle_ = 0;
        return ok;
    }

    size_t Read(uintptr_t address, void* buffer, size_t len) const override {
        if (address == 0 || buffer == nullptr || len == 0) return 0;
        if (!driverProcessHandle_) return 0;

        // 获取系统页面大小
        static const size_t page_size = getpagesize();
        size_t total_bytes_read = 0;
        uintptr_t current_addr = address;
        char* current_buffer = static_cast<char*>(buffer);
        size_t remaining_len = len;

        while (remaining_len > 0) {
            // 计算到下一页边界的距离
            uintptr_t page_offset = current_addr & (page_size - 1);
            size_t bytes_to_page_boundary = page_size - page_offset;

            // 计算本次读取的大小：不超过页边界、剩余长度
            size_t chunk_size = (remaining_len < bytes_to_page_boundary) ?
                               remaining_len : bytes_to_page_boundary;

            // 执行分页读取
            size_t bytes_read = 0;
            int ok = const_cast<AndroidMemKernel*>(this)->driver->ReadProcessMemory(
                const_cast<AndroidMemKernel*>(this)->driverProcessHandle_,
                static_cast<uint64_t>(current_addr),
                current_buffer,
                chunk_size,
                &bytes_read,
                false
            );

            // 连续前缀语义：成功则前进，遇不可读/短读即停止
            if (ok > 0 && bytes_read > 0) {
                total_bytes_read += bytes_read;
                current_addr += bytes_read;
                current_buffer += bytes_read;
                remaining_len -= bytes_read;

                // 短读说明到达不可读边界，连续区到此为止
                if (bytes_read < chunk_size) {
                    break;
                }
            } else {
                // 首个不可读页：停止（不跳过、不续读后续页）
                break;
            }
        }

        // 契约：未读区 [total_bytes_read, len) 清零
        if (remaining_len > 0) {
            memset(current_buffer, 0, remaining_len);
        }
        return total_bytes_read;
    }

    int ReadProcessMemory(uint64_t hProcess,
        uint64_t lpBaseAddress,
        void* lpBuffer,
        size_t nSize,
        size_t* lpNumberOfBytesRead = NULL) override {

        size_t bytesRead = 0;
        bool ok = driver->ReadProcessMemory(driverProcessHandle_, lpBaseAddress, lpBuffer, nSize,
                 &bytesRead, false);
        if (lpNumberOfBytesRead) *lpNumberOfBytesRead = bytesRead;
        return ok ? static_cast<int>(bytesRead) : 0;
    }

    size_t Write(uintptr_t address, const void* buffer, size_t len) const override {
        if (address == 0 || buffer == nullptr || len == 0) return 0;
        size_t bytesWritten = 0;
        if (!const_cast<AndroidMemKernel*>(this)->driverProcessHandle_) return 0;
        bool ok = const_cast<AndroidMemKernel*>(this)->driver->WriteProcessMemory(
            const_cast<AndroidMemKernel*>(this)->driverProcessHandle_,
            static_cast<uint64_t>(address),
            const_cast<void*>(buffer),
            len,
            &bytesWritten,
            false
        );
        return ok ? bytesWritten : 0;
    }

    pid_t GetProcessId() const override {
        return pid_;
    }

    std::string GetProcessName() const override {
        return processName_;
    }

    std::string ReadString(uintptr_t address, size_t maxLen) override {
        std::vector<char> buffer(maxLen + 1, '\0');
        size_t read = Read(address, buffer.data(), maxLen);
        if (read == 0) return "";
        buffer[read] = '\0';
        return std::string(buffer.data());
    }

    bool WriteString(uintptr_t address, const std::string& str) override {
        return Write(address, (const void*)str.c_str(), str.length() + 1) == str.length() + 1;
    }

    std::vector<std::pair<int, std::string>> GetProcessPidList() override {
        std::vector<std::pair<int, std::string>> result;
        std::vector<int> pids;
        if (!driver->GetPidList(pids)) {
            return result;
        }
        LOGDF("GetProcessPidList pids.size() %zu", pids.size());
        result.reserve(pids.size());
        char name[256];
        for (int pid : pids) {
            memset(name, 0, sizeof(name));
            driver->GetProcessCmdline(static_cast<uint64_t>(pid), name, sizeof(name));
            if (std::string(name).empty()) {
                memset(name, 0, sizeof(name));
                driver->GetProcessComm(static_cast<uint64_t>(pid), name, sizeof(name));
            }
            std::string name_str = name;
            // 读取链接
            std::string link = "/proc/" + std::to_string(pid) + "/exe";
            char buf[1024] = {0};
            readlink(link.c_str(), buf, sizeof(buf) - 1);
            link = buf;
            if (link == "/system/bin/app_process64" ||
                link == "/system/bin/app_process32") {
                // app
                std::string app = link.substr(link.find_last_of('/') + 1);
                std::string _name = app + " " + name_str;
                result.emplace_back(pid, _name);
                continue;
            } else if (link.starts_with("/system/bin/") || link.empty() ||
                       link.starts_with("/system_ext/bin/") ||
                       link.starts_with("/vendor/bin/") ||
                       link.starts_with("/apex/com.")) {
                // ignore system process
                continue;
            } else {
                // other process
                std::string _name = "elf " + name_str;
                result.emplace_back(pid, _name);
                continue;
            }
        }
        return result;
    }

    std::vector<ProcessMap> GetProcessMaps() const override {
        std::vector<ProcessMap> maps;
        if (driverProcessHandle_ == 0) return maps;

        std::vector<DRIVER_REGION_INFO> regions;
        if (!const_cast<AndroidMemKernel*>(this)->driver->VirtualQueryExFull(
                const_cast<AndroidMemKernel*>(this)->driverProcessHandle_, FALSE, regions)) {
            return maps;
        }

        maps.reserve(regions.size());
        for (const auto& r : regions) {
            ProcessMap m;
            m.pid = pid_;
            m.startAddress = static_cast<uintptr_t>(r.baseaddress);
            m.endAddress = static_cast<uintptr_t>(r.baseaddress + r.size);
            m.offset = 0;
            m.length = r.size;
            m.inode = 0;
            m.dev = "";
            m.pathname = r.name;
            m.protection = r.protection;

            // 由 PAGE_* 推导 r/w/x 标志
            bool canRead = (r.protection == PAGE_READONLY) ||
                           (r.protection == PAGE_READWRITE) ||
                           (r.protection == PAGE_EXECUTE_READ) ||
                           (r.protection == PAGE_EXECUTE_READWRITE);
            bool canWrite = (r.protection == PAGE_READWRITE) ||
                            (r.protection == PAGE_EXECUTE_READWRITE);
            bool canExec = (r.protection == PAGE_EXECUTE) ||
                           (r.protection == PAGE_EXECUTE_READ) ||
                           (r.protection == PAGE_EXECUTE_READWRITE);

            m.readable = canRead;
            m.writable = canWrite;
            m.executable = canExec;

            // perms 字符串与私有/共享标记
            m.is_private = (r.type == MEM_PRIVATE);
            m.is_shared = (r.type == MEM_MAPPED);
            m.flag = 0;
            if (canRead) m.flag |= 1;
            if (canWrite) m.flag |= 2;
            if (canExec) m.flag |= 4;
            if (m.is_private) m.flag |= 8;
            if (m.is_shared) m.flag |= 16;
            m.perms.clear();
            m.perms.push_back(canRead ? 'r' : '-');
            m.perms.push_back(canWrite ? 'w' : '-');
            m.perms.push_back(canExec ? 'x' : '-');
            m.perms.push_back(m.is_private ? 'p' : (m.is_shared ? 's' : '-'));

            // fix type
            m.type = DetermineMemoryType(m.pathname, m.perms);

            maps.push_back(std::move(m));
        }
        return maps;
    }

    ProcessMap GetAddressMap(uintptr_t address) const override {
        auto maps = GetProcessMaps();
        for (const auto& map : maps) {
            if (map.Contains(address)) return map;
        }
        return ProcessMap();
    }

    std::vector<ProcessMap> FindMapsByName(const std::string& name) const override {
        std::vector<ProcessMap> result;
        auto maps = GetProcessMaps();
        for (const auto& map : maps) {
            if (!map.IsUnknown() && map.pathname.find(name) != std::string::npos) {
                result.push_back(map);
            }
        }
        return result;
    }

    // todo mmap munmap hidetask inject hwbp
};
