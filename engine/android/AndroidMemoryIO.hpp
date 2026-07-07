#pragma once
#include <memory>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "../common/IMemoryOp.hpp"
#include "../common/LinuxProc.hpp"

class AndroidMemoryIO : public IMemoryOp {
private:
    pid_t pid_;
    int memFd_;
    std::string processName_;



public:
    AndroidMemoryIO() : pid_(0), memFd_(-1) {}
    
    ~AndroidMemoryIO() {
        if (memFd_ >= 0) {
            close(memFd_);
        }
    }

    bool OpenProcess(pid_t pid) override{
        pid_ = pid;
        std::string memPath = "/proc/" + std::to_string(pid) + "/mem";
        memFd_ = open(memPath.c_str(), O_RDWR);
        if (memFd_ < 0) return false;

        // 读取进程名
        processName_ = LinuxProc::GetProcessName(pid_);

        return true;
    }

    bool CloseHandle() override {
        if (memFd_ >= 0) {
            close(memFd_);
            memFd_ = -1;
        }
        pid_ = 0;
        return true;
    }
    int ReadProcessMemory(uint64_t hProcess,
        uint64_t lpBaseAddress,
        void* lpBuffer,
        size_t nSize,
        size_t* lpNumberOfBytesRead = NULL) override{ return 0; };

    size_t Read(uintptr_t address, void* buffer, size_t len) const override {
        if (len == 0 || !buffer) return 0;
        // pread64 在 /proc/pid/mem 上天然是连续前缀语义：读到首个不可读页即止；
        // 返回 -1 时归零（避免 (size_t)-1 巨值），并清零未读区以满足读契约。
        ssize_t r = pread64(memFd_, buffer, len, address);
        size_t got = (r > 0) ? static_cast<size_t>(r) : 0;
        if (got < len)
            std::memset(static_cast<char*>(buffer) + got, 0, len - got);
        return got;
    }

    size_t Write(uintptr_t address, const void* buffer, size_t len) const override {
        if (len == 0 || !buffer) return 0;
        // pwrite64 连续写入，返回连续写入字节数；-1 归零（避免 (size_t)-1 巨值）。
        ssize_t r = pwrite64(memFd_, buffer, len, address);
        return (r > 0) ? static_cast<size_t>(r) : 0;
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
        return LinuxProc::GetProcessPidList();
    }
    
    std::vector<ProcessMap> GetProcessMaps() const override {
        return LinuxProc::GetProcessMaps(pid_);
    }

    uint64_t GetSoBase(const std::string& name) const override {
        if (pid_ <= 0 || name.empty()) return 0;
        return LinuxProc::GetModBase(pid_, name);
    }

    ProcessMap GetAddressMap(uintptr_t address)     const override {
        auto maps = GetProcessMaps();
        for (const auto& map : maps) {
            if (map.Contains(address)) {
                return map;
            }
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

    bool GetCardTime(uint64_t& outCardTime) override {
        return false;
    }

    // 实现其他接口...
};
