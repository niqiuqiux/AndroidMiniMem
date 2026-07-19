#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "client.hpp"
#include "DeviceSession.h"


// 端口类型枚举（与服务端保持一致）
enum PortType {
  PORT_MAIN = 1,  // 主通信端口
  PORT_ERROR = 2, // 错误通知端口
  PORT_DEBUG = 3, // 调试端口
};

/**
 * Windows Socket客户端管理器
 * 管理三个端口的连接：主端口、调试端口、错误端口
 */
class WinSocketClientMgr {
private:
  // 三个端口的客户端
  WindowsSocketClient m_main_client;
  WindowsSocketClient m_debug_client;
  WindowsSocketClient m_error_client;

  // 每个端口的互斥锁
  std::mutex m_main_mutex;
  std::mutex m_debug_mutex;
  std::mutex m_error_mutex;

  // 业务事务锁允许同一线程在复合操作内重入底层命令。
  std::recursive_timed_mutex m_main_transaction_mutex;
  std::recursive_timed_mutex m_debug_transaction_mutex;
  std::recursive_timed_mutex m_error_transaction_mutex;

  // 禁止拷贝和赋值
  WinSocketClientMgr(const WinSocketClientMgr &) = delete;
  WinSocketClientMgr &operator=(const WinSocketClientMgr &) = delete;

  // 私有构造函数（单例模式）
  WinSocketClientMgr();
  ~WinSocketClientMgr();

  void CloseClients();

public:
  // 获取单例实例
  static WinSocketClientMgr &GetInstance() {
    static WinSocketClientMgr instance;
    return instance;
  }

  // 获取指定端口的客户端
  WindowsSocketClient *GetClient(PortType type);

  // 获取对应端口的锁
  std::mutex *GetMutex(PortType type);
  std::recursive_timed_mutex *GetTransactionMutex(PortType type);

  DeviceSession::RequestLease AcquireRequestLease() {
    return DeviceSession::GetInstance().AcquireRequest();
  }

  uint64_t GetConnectionGeneration() const {
    return DeviceSession::GetInstance().GetGeneration();
  }

  bool IsConnectionPoisoned() const {
    return DeviceSession::GetInstance().IsPoisoned();
  }

  // 连接到服务器的所有端口
  bool ConnectMultiPort(const std::string &host, uint16_t Port);

  // 断开所有端口
  void DisconnectMultiPort();

  // 检查多端口是否已连接
  bool IsMultiPortConnected() const;

};

// ==================== 全局便捷接口（向后兼容） ====================
// 获取单例管理器
inline WinSocketClientMgr &GetSocketMgr() {
  return WinSocketClientMgr::GetInstance();
}

// 便捷函数（直接调用WinSocketClientMgr的方法）
inline bool ConnectMultiPort(const std::string &host, uint16_t Port) {
  return GetSocketMgr().ConnectMultiPort(host, Port);
}

inline void DisconnectMultiPort() { GetSocketMgr().DisconnectMultiPort(); }

inline bool IsMultiPortConnected() {
  return GetSocketMgr().IsMultiPortConnected();
}

inline bool IsConnectionPoisoned() {
  return GetSocketMgr().IsConnectionPoisoned();
}

inline uint64_t GetConnectionGeneration() {
  return GetSocketMgr().GetConnectionGeneration();
}

inline WindowsSocketClient *GetPortClient(PortType type) {
  return GetSocketMgr().GetClient(type);
}

struct ServerVersionInfo {
  int version = 0;
  std::string versionString;
};

struct ProcessInfoItem {
  int pid = 0;
  std::string name;
};

struct ModuleInfoItem {
  uint64_t base = 0;
  int type; // 模块类型
  int flag; // 模块读写标志位
  int size = 0;
  std::string name;
};

enum MemType {
  MemType_Null = 0,
  MemType_IO = 1,
  MemType_Syscall = 2,
  MemType_Kernel = 3,
  MemType_SysHook = 4
};

//============ 命令全部默认主端口 ============//

bool FetchServerVersion(ServerVersionInfo &outInfo, PortType type = PORT_MAIN);
bool GetMemType(int &outType, PortType type = PORT_MAIN);

struct DriverInitializationIoResult {
  bool requestStarted = false;
  bool responseReceived = false;
  bool accepted = false;
  std::string message;
};

DriverInitializationIoResult InitDriverTracked(
    const std::string &card, PortType type = PORT_MAIN);
bool InitDriver(std::string &Card, std::string &resStr,
                PortType type = PORT_MAIN);

bool FetchProcessList(std::vector<ProcessInfoItem> &outList,
                      PortType type = PORT_MAIN);

// Process / Module helpers
bool OpenProcessHandle(int pid, int &outHandle, PortType type = PORT_MAIN);
bool EnsureOpenHandle(int &outHandle, PortType type = PORT_MAIN);
bool CloseProcessHandle(int handle, PortType type = PORT_MAIN);
bool FetchModuleList(std::vector<ModuleInfoItem> &outList,
                     PortType type = PORT_MAIN);
bool GetSoBaseByName(const std::string &moduleName, uint64_t &outBase,
                     PortType type = PORT_MAIN);
bool FindModuleSegmentsByName(const std::string &moduleName,
                              std::vector<ModuleInfoItem> &outList,
                              PortType type = PORT_MAIN,
                              int requiredFlag = -1);

// Memory helpers
bool ReadProcessMemoryBytes(uint64_t address, uint32_t size,
                            std::vector<unsigned char> &out,
                            PortType type = PORT_MAIN);
bool ReadProcessMemory_(uint64_t address, uint32_t size, void *out,
                        int32_t &Realread, PortType type = PORT_MAIN);
bool ReadBratchMemory(
    uint64_t address, uint32_t size,
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> &out,
    PortType type = PORT_MAIN);

bool ReadBratchAddr(
    std::vector<std::pair<uint64_t, int32_t> /*addr,size*/> &addrs,
    std::vector<std::pair<uint64_t, std::vector<uint8_t>> /*addr,data*/> &out,
    PortType type = PORT_MAIN);

struct MemoryWriteIoResult {
  bool requestStarted = false;
  bool responseReceived = false;
  int32_t writtenBytes = 0;
};

MemoryWriteIoResult WriteProcessMemoryDetailed(
    uint64_t address, const std::vector<unsigned char> &data,
    PortType type = PORT_MAIN);

// outWritten（可选）回传连续写入的字节数：返回 true 表示全部写入；
// 返回 false 且 *outWritten>0 表示【部分写入】(已产生副作用)；*outWritten==0 表示完全失败。
bool WriteProcessMemoryBytes(uint64_t address, uint32_t size,
                             std::vector<unsigned char> &data,
                             PortType type = PORT_MAIN,
                             int32_t *outWritten = nullptr);

// Resolve helpers
bool GetModuleBaseByName(const std::string &moduleName, uint64_t &outBase,
                         PortType type = PORT_MAIN);
bool ResolveModuleOffsetChain(uint64_t &outAddress,
                              const std::string &moduleName,
                              uint64_t baseOffset,
                              const std::vector<uint64_t> &offsets,
                              bool derefFinal = true,
                              PortType type = PORT_MAIN);

// 内核断点相关
struct BreakpointMutationIoResult {
  bool requestStarted = false;
  bool responseReceived = false;
  bool applied = false;
};

BreakpointMutationIoResult SetKernelBreakpointTracked(
    uint64_t address, uint32_t bpType, uint32_t bpSize,
    PortType type = PORT_MAIN);
BreakpointMutationIoResult RemoveKernelBreakpointTracked(
    uint64_t address, PortType type = PORT_MAIN);
BreakpointMutationIoResult SuspendKernelBreakpointTracked(
    uint64_t address, PortType type = PORT_MAIN);
BreakpointMutationIoResult ResumeKernelBreakpointTracked(
    uint64_t address, PortType type = PORT_MAIN);
bool SetKernelBreakpoint(uint64_t address, uint32_t bpType, uint32_t bpSize,
                         PortType type = PORT_MAIN);
bool RemoveKernelBreakpoint(uint64_t address, PortType type = PORT_MAIN);
bool SuspendKernelBreakpoint(uint64_t address, PortType type = PORT_MAIN);
bool ResumeKernelBreakpoint(uint64_t address, PortType type = PORT_MAIN);
bool ReadKernelBreakpointInfo(uint64_t address, std::vector<HW_HIT_INFO> &infos,
                              PortType type = PORT_MAIN,
                              uint64_t *outTotalHits = nullptr);  // 回传设备累计命中数
bool ClearTrackedKernelBreakpoints(PortType type = PORT_MAIN);
void ResetTrackedKernelBreakpoints();

// ELF 符号接口
bool SymbolInit(uint64_t moduleBase, int &outTotalCount,
                PortType type = PORT_MAIN);
bool SymbolGetList(int offset, int count,
                   std::vector<std::pair<uint64_t, std::string>> &outSymbols,
                   int *outTotalCount = nullptr,
                   PortType type = PORT_MAIN);
bool SymbolFind(uint64_t moduleBase, const std::string &name,
                uint64_t &outAddress, PortType type = PORT_MAIN);
