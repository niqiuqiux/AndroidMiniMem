#ifndef MEMORY_READER_WRITER_H_
#define MEMORY_READER_WRITER_H_

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include <mutex>
#include <fstream>

#include "IMemReaderWriterProxy.h"

#ifdef __linux__
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <errno.h>
#include <malloc.h>
#include <random>
#include <algorithm>
#include <filesystem>
#include "IoctlBufferPool.h"
#include "Logger.hpp"

// 安静输出模式
// #define QUIET_PRINTF
#endif /*__linux__*/

// Anon FD 连接方式 (使用 reboot syscall 触发内核安装 fd)
#define USE_ANON_FD

#ifdef USE_ANON_FD
// =================anon fd 连接===================
// Reboot syscall magic numbers for KSU install
#ifndef KERNELMEM_SYS_REBOOT
#if defined(SYS_reboot)
#define KERNELMEM_SYS_REBOOT SYS_reboot
#else
#define KERNELMEM_SYS_REBOOT __NR_reboot
#endif
#endif

// KSU Install Magic Numbers (与内核驱动保持一致)
#define KSU_INSTALL_MAGIC1 0xDEADBEEFu
#define KSU_INSTALL_MAGIC2 0x00114514u
#define KSU_INSTALL_CMD_UNUSED 0u

static inline ssize_t svc_call(long fd, void* buf, ssize_t size) {
    return _svc_call(fd, (long)buf, size, 0, 0, 0, 0);
}
#endif

#ifdef __linux__
// 前置声明结构体
#pragma pack(1)
struct my_user_pt_regs {
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    uint64_t orig_x0;
    uint64_t syscallno;
};

struct my_user_fpsimd_state {
    __uint128_t	vregs[32];
    __u32 fpsr;
    __u32 fpcr;
};

struct HW_HIT_ITEM {
    uint64_t task_id;
    uint64_t hit_addr;
    uint64_t hit_time;
    struct my_user_pt_regs regs_info;
    struct my_user_fpsimd_state fpsimd_info;
};
#pragma pack()
#endif

class CMemoryReaderWriter : public IMemReaderWriterProxy {
public:
    CMemoryReaderWriter() {}
    ~CMemoryReaderWriter() { DisconnectDriver(); }

    // 连接驱动
    // 参数 procNodeAuthKey：隐蔽节点连接密钥
    // 返回值：>=0成功，<0错误码
    int ConnectDriver(const std::string& procNodeAuthKey) {
        return _InternalConnectDriver(procNodeAuthKey);
    }

    // 断开驱动
    // 返回值：TRUE成功，FALSE失败
    BOOL DisconnectDriver() {
        return _InternalDisconnectDriver();
    }

    // 检查驱动连接状态
    // 返回值：TRUE已连接，FALSE未连接
    BOOL IsDriverConnected() {
        return _InternalIsDriverConnected();
    }

    //获取卡密时间
    BOOL GetCardTime(uint64_t& outCardTime) {
        return _InternalGetCardTime(outCardTime);
    }

    // 驱动_打开进程
    // 参数 pid：目标进程PID
    // 返回值：成功返回 pid（作为句柄），失败返回0
    uint64_t OpenProcess(uint64_t pid) {
        return _InternalOpenProcess(pid);
    }

    // 驱动_读取进程内存
    BOOL ReadProcessMemory(uint64_t pid,
                          uint64_t lpBaseAddress,
                          void* lpBuffer,
                          size_t nSize,
                          size_t* lpNumberOfBytesRead = NULL,
                          BOOL bIsForceRead = FALSE) override {
        return _InternalReadProcessMemory(pid, lpBaseAddress, lpBuffer, nSize, lpNumberOfBytesRead, bIsForceRead);
    }

    // 驱动_写入进程内存
    BOOL WriteProcessMemory(uint64_t pid,
                           uint64_t lpBaseAddress,
                           void* lpBuffer,
                           size_t nSize,
                           size_t* lpNumberOfBytesWritten = NULL,
                           BOOL bIsForceWrite = FALSE) override {
        return _InternalWriteProcessMemory(pid, lpBaseAddress, lpBuffer, nSize, lpNumberOfBytesWritten, bIsForceWrite);
    }

    // 驱动_关闭进程句柄
    BOOL CloseHandle(uint64_t pid) {
        return _InternalCloseHandle(pid);
    }

    // 驱动_获取进程内存块列表
    BOOL VirtualQueryExFull(uint64_t pid, BOOL showPhy, std::vector<DRIVER_REGION_INFO>& vOutput) override {
        return _InternalVirtualQueryExFull(pid, showPhy, vOutput);
    }

    // 驱动_检查进程内存地址有效性
    BOOL CheckProcessMemAddrValid(uint64_t pid, uint64_t lpBaseAddress) override {
        return _InternalCheckProcessMemAddrValid(pid, lpBaseAddress);
    }

    // 驱动_获取PID列表
    BOOL GetPidList(std::vector<int>& vOutput) {
        return _InternalGetPidList(vOutput);
    }

    // 驱动_提升进程权限至Root
    BOOL SetProcessRoot(uint64_t pid) {
        return _InternalSetProcessRoot(pid);
    }

    // 驱动_获取进程物理内存占用大小
    BOOL GetProcessPhyMemSize(uint64_t pid, uint64_t& outRss) {
        return _InternalGetProcessPhyMemSize(pid, outRss);
    }

    // 驱动_获取进程命令行
    BOOL GetProcessCmdline(uint64_t pid, char* lpOutCmdlineBuf, size_t bufSize) {
        return _InternalGetProcessCmdline(pid, lpOutCmdlineBuf, bufSize);
    }

    // 驱动_获取进程通信名
    BOOL GetProcessComm(uint64_t pid, char* lpOutCmdlineBuf, size_t bufSize) {
        return _InternalGetProcessComm(pid, lpOutCmdlineBuf, bufSize);
    }

    // 驱动_获取连接FD
    int GetLinkFD() {
        return _InternalGetLinkFD();
    }

    // 驱动_设置连接FD
    void SetLinkFD(int fd) {
        _InternalSetLinkFD(fd);
    }

    // 驱动_读取进程内存（大批量）
    BOOL ReadProcessMemoryBulk(uint64_t pid,
                              uint64_t lpBaseAddress,
                              void* lpBuffer,
                              size_t nSize,
                              size_t* lpNumberOfBytesRead = NULL,
                              BOOL bIsForceRead = FALSE) {
        return _InternalReadProcessMemoryBulk(pid, lpBaseAddress, lpBuffer, nSize, lpNumberOfBytesRead, bIsForceRead);
    }

    // 驱动_获取CPU支持硬件执行断点的数量
    int GetNumBRPS() {
        return _InternalGetNumBRPS();
    }

    // 驱动_获取CPU支持硬件访问断点的数量
    int GetNumWRPS() {
        return _InternalGetNumWRPS();
    }

    // 驱动_新增硬件断点，返回值：成功返回断点句柄，失败返回0
    uint64_t AddProcessHwBp(uint64_t hProcess,
                           uint64_t lpBaseAddress,
                           unsigned int hwbpLen,
                           unsigned int hwbpType) {
        return _InternalAddProcessHwBp(hProcess, lpBaseAddress, hwbpLen, hwbpType);
    }

    // 驱动_删除硬件断点，返回值：TRUE成功，FALSE失败
    BOOL DelProcessHwBp(uint64_t hHwbp) {
        return _InternalDelProcessHwBp(hHwbp);
    }

    // 驱动_暂停硬件断点，返回值：TRUE成功，FALSE失败
    BOOL SuspendProcessHwBp(uint64_t hHwbp) {
        return _InternalSuspendProcessHwBp(hHwbp);
    }

    // 驱动_恢复硬件断点，返回值：TRUE成功，FALSE失败
    BOOL ResumeProcessHwBp(uint64_t hHwbp) {
        return _InternalResumeProcessHwBp(hHwbp);
    }

    // 驱动_读取硬件断点命中记录信息，返回值：TRUE成功，FALSE失败
    BOOL ReadHwBpInfo(uint64_t hHwbp, uint64_t& nHitTotalCount, std::vector<HW_HIT_ITEM>& vOutput);
    
    // 驱动_设置无条件Hook跳转
    BOOL SetHookPC(uint64_t pc);

    // ======================= SDK 风格便捷函数 =======================
    // 模板读取 - 读取指定类型的值
    template <typename T>
    T read(uint64_t pid, uint64_t addr) {
        T value{};
        ReadProcessMemory(pid, addr, &value, sizeof(T));
        return value;
    }

    // 模板写入 - 写入指定类型的值
    template <typename T>
    BOOL write(uint64_t pid, uint64_t addr, const T& value) {
        return WriteProcessMemory(pid, addr, (void*)&value, sizeof(T));
    }

    // 读取字节向量
    std::vector<uint8_t> read_bytes(uint64_t pid, uint64_t addr, size_t size) {
        std::vector<uint8_t> buf(size);
        size_t bytesRead = 0;
        if (!ReadProcessMemory(pid, addr, buf.data(), size, &bytesRead)) {
            buf.clear();
        } else {
            buf.resize(bytesRead);
        }
        return buf;
    }

    // 获取进程内存映射
    BOOL get_maps(uint64_t pid, std::vector<DRIVER_REGION_INFO>& out);

    // 检查进程内存地址是否有效
    BOOL check_addr_valid(uint64_t pid, uint64_t addr) {
        return CheckProcessMemAddrValid(pid, addr);
    }

    // 内存保护属性枚举 (与 SDK 一致)
    enum MemProtection : uint32_t {
        MEM_PROT_NOACCESS = 1,
        MEM_PROT_READONLY = 2,
        MEM_PROT_READWRITE = 4,
        MEM_PROT_WRITECOPY = 8,
        MEM_PROT_EXECUTE = 16,
        MEM_PROT_EXECUTE_READ = 32,
        MEM_PROT_EXECUTE_READWRITE = 64,
    };

    // 内存类型枚举 (与 SDK 一致)
    enum MemType : uint32_t {
        MEM_TYPE_PRIVATE = 131072,
        MEM_TYPE_MAPPED = 262144,
    };

    // 内存映射模式 (与 SDK 一致)
    enum MemMapFlags : unsigned int {
        MEM_MAP_CACHED = 0x00,
        MEM_MAP_UNCACHED = 0x01,
        MEM_MAP_WRITE_COMBINE = 0x02,
    };

private:
    // ======================= 内部实现函数 =======================
    int _InternalConnectDriver(const std::string& procNodeAuthKey);
    BOOL _InternalDisconnectDriver();
    BOOL _InternalIsDriverConnected();
    BOOL _InternalGetCardTime(uint64_t& outCardTime);
    uint64_t _InternalOpenProcess(uint64_t pid);
    BOOL _InternalReadProcessMemory(uint64_t pid, uint64_t lpBaseAddress, void* lpBuffer, 
                                   size_t nSize, size_t* lpNumberOfBytesRead, BOOL bIsForceRead);
    BOOL _InternalWriteProcessMemory(uint64_t pid, uint64_t lpBaseAddress, void* lpBuffer, 
                                    size_t nSize, size_t* lpNumberOfBytesWritten, BOOL bIsForceWrite);
    BOOL _InternalCloseHandle(uint64_t pid);
    BOOL _InternalVirtualQueryExFull(uint64_t pid, BOOL showPhy, std::vector<DRIVER_REGION_INFO>& vOutput);
    BOOL _InternalCheckProcessMemAddrValid(uint64_t pid, uint64_t lpBaseAddress);
    BOOL _InternalGetPidList(std::vector<int>& vOutput);
    BOOL _InternalSetProcessRoot(uint64_t pid);
    BOOL _InternalGetProcessPhyMemSize(uint64_t pid, uint64_t& outRss);
    BOOL _InternalGetProcessCmdline(uint64_t pid, char* lpOutCmdlineBuf, size_t bufSize);
    BOOL _InternalGetProcessComm(uint64_t pid, char* lpOutCmdlineBuf, size_t bufSize);
    BOOL _InternalReadProcessMemoryBulk(uint64_t pid, uint64_t lpBaseAddress, void* lpBuffer, 
                                       size_t nSize, size_t* lpNumberOfBytesRead, BOOL bIsForceRead);
    
    // 硬件断点相关内部函数
    int _InternalGetNumBRPS();
    int _InternalGetNumWRPS();
    uint64_t _InternalAddProcessHwBp(uint64_t hProcess, uint64_t lpBaseAddress, unsigned int hwbpLen, unsigned int hwbpType);
    BOOL _InternalDelProcessHwBp(uint64_t hHwbp);
    BOOL _InternalSuspendProcessHwBp(uint64_t hHwbp);
    BOOL _InternalResumeProcessHwBp(uint64_t hHwbp);
    BOOL _InternalReadHwBpInfo(uint64_t hHwbp, uint64_t& nHitTotalCount, std::vector<HW_HIT_ITEM>& vOutput);
    void _InternalSetUseBypassSELinuxMode(BOOL bUseBypassSELinuxMode);
    int _InternalGetLinkFD();
    void _InternalSetLinkFD(int fd);

#ifdef __linux__
    // ======================= Linux平台特定定义 =======================
    #ifdef QUIET_PRINTF
    #undef TRACE
    #define TRACE(fmt, ...)
    #else
    #define TRACE LOGDF
    #endif

    #define MY_PATH_MAX_LEN 1024
    #define MY_TASK_COMM_LEN 16

    // 命令枚举 (与 kernelmem_sdk.h 保持一致)
    enum {
        CMD_INIT_DEVICE_INFO = 1,         // 保留编号，用户态不再发送
        CMD_OPEN_PROCESS,                 // 2
        CMD_READ_PROCESS_MEMORY,          // 3
        CMD_WRITE_PROCESS_MEMORY,         // 4
        CMD_CLOSE_PROCESS,                // 5
        CMD_GET_PROCESS_MAPS_COUNT,       // 6
        CMD_GET_PROCESS_MAPS_LIST,        // 7
        CMD_CHECK_PROCESS_ADDR_PHY,       // 8
        CMD_GET_PID_LIST,                 // 9
        CMD_SET_PROCESS_ROOT,             // 10
        CMD_GET_PROCESS_RSS,             // 11
        CMD_GET_PROCESS_CMDLINE,         // 12
        CMD_HIDE_USER_PROCESS,           // 13
        CMD_ALLOC_REMOTE_ANON,           // 14
        CMD_MMAP,                        // 15
        CMD_READ_PROCESS_MEMORY_BULK,    // 16
        CMD_GET_PROCESS_COMM,            // 17
    };

    // 硬件断点相关命令 (与 kernelmem_sdk.h 保持一致)
    enum {
        CMD_HWBP_GET_NUM_BRPS = 60,     // 获取CPU支持硬件执行断点的数量
        CMD_HWBP_GET_NUM_WRPS,          // 获取CPU支持硬件访问断点的数量
        CMD_HWBP_INSTALL,               // 设置进程硬件断点
        CMD_HWBP_UNINSTALL,             // 删除进程硬件断点
        CMD_HWBP_SUSPEND,               // 暂停进程硬件断点
        CMD_HWBP_RESUME,                // 恢复进程硬件断点
        CMD_HWBP_GET_HIT_COUNT,         // 获取硬件断点命中数量与队列长度
        CMD_HWBP_SET_HOOK_PC,           // 设置 hook pc
        CMD_HWBP_READ_INFO              // 读取命中明细（批量）
    };

    // 硬件断点长度枚举
    enum {
        HW_BREAKPOINT_LEN_1 = 1,
        HW_BREAKPOINT_LEN_2 = 2,
        HW_BREAKPOINT_LEN_4 = 4,
        HW_BREAKPOINT_LEN_8 = 8,
    };

    // 硬件断点类型枚举
    enum {
        HW_BREAKPOINT_EMPTY = 0,
        HW_BREAKPOINT_R = 1,
        HW_BREAKPOINT_W = 2,
        HW_BREAKPOINT_RW = HW_BREAKPOINT_R | HW_BREAKPOINT_W,
        HW_BREAKPOINT_X = 4,
        HW_BREAKPOINT_INVALID = HW_BREAKPOINT_RW | HW_BREAKPOINT_X,
    };

         // 数据结构定义
     #pragma pack(1)

    struct IoctlRequest {
        char cmd = 0;           // 操作命令
        uint64_t param1 = 0;   // 参数1
        uint64_t param2 = 0;   // 参数2
        uint64_t param3 = 0;   // 参数3
        uint64_t buf_size = 0; // 紧随其后的动态数据长度
    };

    struct init_device_info {
        int pid = 0;
        int tid = 0;
        char myName[MY_TASK_COMM_LEN + 1] = {0};
        char myCmdline[1024] = {0};
    };

    struct map_entry {
        uint64_t start = 0;
        uint64_t end = 0;
        unsigned char flags[4] = {0};
        char path[MY_PATH_MAX_LEN] = {0};
    };

    struct arg_info {
        uint64_t arg_start = 0;
        uint64_t arg_end = 0;
    };

    // SDK 风格结构体定义 (与 kernelmem_sdk.h 保持一致)
    struct AArch64Regs {
        uint64_t x[31];
        uint64_t sp;
        uint64_t pc;
        uint64_t pstate;
        uint64_t orig_x0;
        uint64_t syscallno;
    };

    struct AArch64FpRegs {
        __uint128_t v[32];
        uint32_t fpsr;
        uint32_t fpcr;
    };

    struct HwBpHit {
        uint64_t task_id;
        uint64_t hit_addr;
        uint64_t hit_time;
        AArch64Regs regs;
        AArch64FpRegs fp_regs;
    };

    struct RegionInfo {
        uint64_t base;
        uint64_t size;
        uint32_t protection;
        uint32_t type;
        char name[4096];
    };
    #pragma pack()

    // ======================= 驱动通信函数 =======================
    ssize_t _rwProcMemDriver_MyIoctl(int fd, char cmd, uint64_t param1, uint64_t param2, 
                                     uint64_t param3, char* buf, uint64_t bufSize);
    int _rwProcMemDriver_Connect(const std::string& procNodeAuthKey);
    BOOL _rwProcMemDriver_Disconnect(int nFd);
    BOOL _rwProcMemDriver_InitDeviceInfo(int nFd);
    uint64_t _rwProcMemDriver_OpenProcess(int nFd, uint64_t pid);
    BOOL _rwProcMemDriver_ReadProcessMemory(int nFd, uint64_t pid, uint64_t lpBaseAddress, 
                                           void* lpBuffer, size_t nSize, size_t* lpNumberOfBytesRead, BOOL bIsForceRead);
    BOOL _rwProcMemDriver_WriteProcessMemory(int nFd, uint64_t pid, uint64_t lpBaseAddress, 
                                            void* lpBuffer, size_t nSize, size_t* lpNumberOfBytesWritten, BOOL bIsForceWrite);
    BOOL _rwProcMemDriver_CloseHandle(int nFd, uint64_t pid);
    BOOL _rwProcMemDriver_VirtualQueryExFull(int nFd, uint64_t pid, BOOL showPhy, std::vector<DRIVER_REGION_INFO>& vOutput);
    BOOL _rwProcMemDriver_CheckProcessMemAddrValid(int nFd, uint64_t pid, uint64_t lpBaseAddress);
    BOOL _rwProcMemDriver_GetPidList(int nFd, std::vector<int>& vOutput);
    BOOL _rwProcMemDriver_SetProcessRoot(int nFd, uint64_t pid);
    BOOL _rwProcMemDriver_GetProcessPhyMemSize(int nFd, uint64_t pid, uint64_t* outRss);
    BOOL _rwProcMemDriver_GetProcessCmdline(int nFd, uint64_t pid, char* lpOutCmdlineBuf, size_t bufSize);
    BOOL _rwProcMemDriver_GetProcessComm(int nFd, uint64_t pid, char* lpOutCmdlineBuf, size_t bufSize);
    BOOL _rwProcMemDriver_ReadProcessMemoryBulk(int nFd, uint64_t pid, uint64_t lpBaseAddress, 
                                               void* lpBuffer, size_t nSize, size_t* lpNumberOfBytesRead, BOOL bIsForceRead);

    // 硬件断点相关函数
    int _hwbpProcDriver_GetNumBRPS(int nDriverLink);
    int _hwbpProcDriver_GetNumWRPS(int nDriverLink);
    uint64_t _hwbpProcDriver_InstProcessHwBp(int nDriverLink, uint64_t hProcess, uint64_t lpBaseAddress, 
                                            unsigned int hwbpLen, unsigned int hwbpType);
    BOOL _hwbpProcDriver_UninstProcessHwBp(int nDriverLink, uint64_t hHwbp);
    BOOL _hwbpProcDriver_SuspendProcessHwBp(int nDriverLink, uint64_t hHwbp);
    BOOL _hwbpProcDriver_ResumeProcessHwBp(int nDriverLink, uint64_t hHwbp);
    BOOL _hwbpProcDriver_ReadHwBpInfo(int nDriverLink, uint64_t hHwbp, uint64_t& nHitTotalCount, std::vector<HW_HIT_ITEM>& vOutput);
    BOOL _hwbpProcDriver_SetHookPC(int nDriverLink, uint64_t pc);
    void _hwbpProcDriver_SetUseBypassSELinuxMode(BOOL bUseBypassSELinuxMode);

    // 工具函数
    std::vector<uint8_t> generate_unique_non_zero_bytes(std::size_t count);
    std::string GenerateRandomString(size_t length, bool printable_only = true);

#endif /*__linux__*/

    // 工具函数
    std::string _GetFileContent(const char* lpszFilePath);

private:
    // 成员变量
    int m_nFd = -1;                         // 驱动文件描述符

};


//static CMemoryReaderWriter* driver_ = new CMemoryReaderWriter();

inline std::shared_ptr<CMemoryReaderWriter> driver_ = std::make_shared<CMemoryReaderWriter>();

#endif /* MEMORY_READER_WRITER_H_ */
