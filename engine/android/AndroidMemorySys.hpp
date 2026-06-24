#pragma once
#include <asm-generic/fcntl.h>
#include <asm-generic/mman-common.h>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include "../common/IMemoryOp.hpp"
#include "../common/LinuxProc.hpp"
#include "Logger.hpp"
#include <unistd.h>
#include <vector>
#include <algorithm>


class AndroidMemorySys : public IMemoryOp {
private:
    pid_t pid_;
    std::string processName_;
  

    static ssize_t process_vm_readv(pid_t pid,
                                  const struct iovec* local_iov,
                                  unsigned long liovcnt,
                                  const struct iovec* remote_iov,
                                  unsigned long riovcnt,
                                  unsigned long flags) {
        return syscall(SYS_process_vm_readv, pid, local_iov, liovcnt, remote_iov, riovcnt, flags);
       
    }

    static ssize_t process_vm_writev(pid_t pid,
                                   const struct iovec* local_iov,
                                   unsigned long liovcnt,
                                   const struct iovec* remote_iov,
                                   unsigned long riovcnt,
                                   unsigned long flags) {

        return syscall(SYS_process_vm_writev, pid, local_iov, liovcnt, remote_iov, riovcnt, flags);
      
    }

    static ssize_t WriteMem(pid_t pid, uintptr_t address, const void* buffer, size_t len) {
    
        std::string memPath = "/proc/" + std::to_string(pid) + "/mem";
        int memFd = open(memPath.c_str(), O_RDWR);
        if (memFd < 0) {
            LOGDF("WriteMem: Failed to open %s, errno: %s\n", memPath.c_str(), strerror(errno));
            return -1;
        }
        
        // 执行写入操作
        ssize_t result = pwrite64(memFd, buffer, len, address);
        int writeErrno = errno; // 保存errno，避免close影响
        
        close(memFd);
        LOGDF("WriteMem: Successfully wrote %zd bytes to pid %d, addr 0x%lx\n", result, pid, address);
        return result;
    }

public:
 
    AndroidMemorySys() : pid_(0) {
      type = 2;
    }

    bool OpenProcess(pid_t pid) override {
        pid_ = pid;
        if (pid_ <= 0) return false;

        if (!LinuxProc::IsProcessAlive(pid_)) {
            return false;
        }

        // 读取进程名
        processName_ = LinuxProc::GetProcessName(pid_);

        // 验证系统调用是否可用
        struct iovec local = {nullptr, 0};
        struct iovec remote = {nullptr, 0};
        return process_vm_readv(pid, &local, 1, &remote, 1, 0) != -1;
    }

    bool CloseHandle() override {
        pid_ = 0;
        return true;
    }

    size_t Read(uintptr_t address, void *buffer, size_t len) const override {
      if (len == 0 || !buffer)
        return 0;

      // struct iovec local[1];
      // struct iovec remote[1];

      // local[0].iov_base = buffer;
      // local[0].iov_len = len;
      // remote[0].iov_base = (void*)address;
      // remote[0].iov_len = len;

      // ssize_t result = process_vm_readv(pid_, local, 1, remote, 1, 0);
      // if (result <= 0) {
      //     LOGDF("Read %lx %d real %d err: %s\n", address, len, result,
      //     strerror(errno));
      // }
      // return result <= 0 ? 0 : result;

      // 获取系统页面大小
      static const size_t pageSize = getpagesize();
      static const size_t pageMask = pageSize - 1;

      // 计算需要的页面数量
      uintptr_t startPage = address & ~pageMask;
      uintptr_t endPage = (address + len - 1) & ~pageMask;
      size_t pageCount = (endPage - startPage) / pageSize + 1;

      // 如果只需要一个页面，直接读取
      if (pageCount == 1) {
        struct iovec local[1];
        struct iovec remote[1];

        local[0].iov_base = buffer;
        local[0].iov_len = len;
        remote[0].iov_base = (void *)address;
        remote[0].iov_len = len;

        ssize_t result = process_vm_readv(pid_, local, 1, remote, 1, 0);
        if (result <= 0) {
          LOGDF("Read %lx %zu real %zd err: %s\n", address, len, result,
                strerror(errno));
          std::memset(buffer, 0, len);
          return 0;
        }
        // 连续前缀语义：短读说明页内出现不可读边界，清零其余部分
        if (static_cast<size_t>(result) < len) {
          std::memset(static_cast<char *>(buffer) + result, 0, len - result);
        }
        return static_cast<size_t>(result);
      }

      // 多页面读取
      size_t totalRead = 0;
      uintptr_t currentAddr = address;
      char *currentBuffer = static_cast<char *>(buffer);
      size_t remainingLen = len;

      //LOGDF("Read %lx len %zu pageCount %zu\n", address, len, pageCount);

      while (remainingLen > 0) {
        size_t pageOffset = currentAddr & pageMask;
        size_t pageReadLen = std::min(remainingLen, pageSize - pageOffset);

        struct iovec local;
        struct iovec remote;
        local.iov_base = currentBuffer;
        local.iov_len = pageReadLen;
        remote.iov_base = (void *)currentAddr;
        remote.iov_len = pageReadLen;

        ssize_t result = process_vm_readv(pid_, &local, 1, &remote, 1, 0);
        if (result <= 0) {
          // 连续前缀语义：遇到首个不可读页即停止（不跳过、不续读后续页）
          LOGDF("PageRead %lx len=%zu result=%zd err: %s\n", (uintptr_t)remote.iov_base,
                pageReadLen, result, strerror(errno));
          break;
        }

        totalRead += result;
        currentAddr += result;
        currentBuffer += result;
        remainingLen -= result;

        // 短读说明该页内部出现不可读边界，连续区到此为止
        if (static_cast<size_t>(result) < pageReadLen) {
          break;
        }
      }

      // 契约：未读区 [totalRead, len) 清零，调用方据 totalRead 即可定位有效数据
      if (remainingLen > 0) {
        std::memset(currentBuffer, 0, remainingLen);
      }

      return totalRead;
    }

    int ReadProcessMemory(uint64_t hProcess, uint64_t lpBaseAddress,
                          void *lpBuffer, size_t nSize,
                          size_t *lpNumberOfBytesRead = NULL) override {

      ssize_t bread;
      if (hProcess) {
        struct iovec local[1];
        struct iovec remote[1];

        local[0].iov_base = lpBuffer;
        local[0].iov_len = nSize;
        remote[0].iov_base = (void *)lpBaseAddress;
        remote[0].iov_len = nSize;
        bread = process_vm_readv(hProcess, local, 1, remote, 1, 0);
      } else {
        bread = Read(lpBaseAddress, lpBuffer, nSize);
      }

      if (lpNumberOfBytesRead) {
        *lpNumberOfBytesRead = bread > 0 ? static_cast<size_t>(bread) : 0;
      }
      return bread;
    }

    size_t Write(uintptr_t address, const void* buffer, size_t len) const override {
        struct iovec local[1];
        struct iovec remote[1];

        local[0].iov_base = const_cast<void*>(buffer);
        local[0].iov_len = len;
        remote[0].iov_base = (void*)address;
        remote[0].iov_len = len;

        ssize_t result = process_vm_writev(pid_, local, 1, remote, 1, 0);
        if (result <= 0) {
            LOGDF("Write %lx %zu real %zd err: %s\n", address, len, result,
            strerror(errno));
            result = WriteMem(pid_, address, buffer, len);
        }
        return result <= 0 ? 0 : result;
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

    ProcessMap GetAddressMap(uintptr_t address) const override {
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
}; 