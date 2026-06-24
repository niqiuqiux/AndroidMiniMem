#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ProcessMap.hpp"

enum MemType {
    MemType_Null = 0,
    MemType_IO = 1,
    MemType_Syscall = 2,
    MemType_Kernel = 3,
    MemType_SysHook = 4
};

class IMemoryOp {
public:
    virtual ~IMemoryOp() = default;
    uint8_t  type = 0;//0:null 1:io  2:syscall 3:kernel

    virtual bool OpenProcess(pid_t pid) = 0;
    virtual bool CloseHandle() = 0;
    
    // 基础内存操作
    // 【连续前缀契约】从 address 起连续读取，遇到首个不可读页即停止：
    //   - 返回值 = 从 address 起【连续可读】的字节数，即有效数据位于 buffer[0, 返回值)；
    //   - 不跳过空洞、不续读后续页（稀疏/大范围读取请用批量接口 ReadBratch*）；
    //   - 实现须将 buffer[返回值, len) 清零，使调用方仅凭返回值即可定位有效区，
    //     从根本上消除"多页读取返回量无法判断有效区位置"的歧义；
    //   - address 起始即不可读、或 len==0 时返回 0。
    virtual size_t Read(uintptr_t address, void* buffer, size_t len) const = 0;
    virtual int ReadProcessMemory(uint64_t hProcess,
        uint64_t lpBaseAddress,
        void* lpBuffer,
        size_t nSize,
        size_t* lpNumberOfBytesRead = NULL) = 0;
    // 【连续前缀契约】从 address 起连续写入，遇到首个不可写页即停止：
    //   - 返回值 = 连续成功写入的字节数；< len 表示部分写入（[0,返回值) 已写、其余未写）；
    //   - 写入是破坏性操作：部分写入已留下副作用，调用方据返回值重试剩余或如实上报，
    //     绝不能把"部分写入"当作"未写入"。
    virtual size_t Write(uintptr_t address, const void* buffer, size_t len) const = 0;
    
    // 进程信息
    virtual pid_t GetProcessId() const = 0;
    virtual std::string GetProcessName() const = 0;
    
  
    virtual std::string ReadString(uintptr_t address, size_t maxLen) = 0;
    virtual bool WriteString(uintptr_t address, const std::string& str) = 0;


    virtual std::vector<std::pair<int, std::string>> GetProcessPidList() = 0;
    virtual std::vector<ProcessMap> GetProcessMaps() const = 0;
    virtual ProcessMap GetAddressMap(uintptr_t address) const = 0;
    virtual std::vector<ProcessMap> FindMapsByName(const std::string& name) const = 0;

    virtual bool GetCardTime(uint64_t& outCardTime) = 0;
}; 