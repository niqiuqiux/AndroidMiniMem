#ifndef CESERVER_H_
#define CESERVER_H_

#include <cstdint>
#include <stdint.h>
#include <sys/types.h>
#include <unordered_map>
#include <vector>
#include "ioserver.hpp"
#include "porthelp.h"

// ============================================================================
// MiniMem 命令字（单字节 opcode）
// ----------------------------------------------------------------------------
// 仅保留 DispatchCommand_V2 实际处理的命令；数据搜索 / 指针扫描 / 冻结 /
// SO注入 / 远程mmap / 线程上下文 / CE风格调试事件 / 旧快照(Process32/Module32)
// 等命令已随对应功能裁剪移除。
// ============================================================================

// opcode 从 0 连续编号（MiniMem 自有协议，已不兼容 Cheat Engine 原始编号）。
// 前端 gui/socket/client.hpp 必须与此处逐一对应，二者是协议契约。

// —— 基础 / 连接 ——
#define CMD_GETVERSION               0
#define CMD_CLOSECONNECTION          1
#define CMD_TERMINATESERVER          2

// —— 内核 / 驱动（内核切换）——
#define CMD_GETMEMTYPE               3   // 查询当前读写模式 (0:null 1:io 2:syscall 3:kernel 4:syshook)
#define CMD_INITRWDRIVER             4   // 初始化/加载内核读写驱动并热切换 g_memIO

// —— 进程 / 句柄 ——
#define CMD_OPENPROCESS              5
#define CMD_CLOSEHANDLE              6
#define CMD_GETPROCESSLIST           7
#define CMD_GETMODULELIST            8

// —— 内存读写 ——
#define CMD_READPROCESSMEMORY        9
#define CMD_WRITEPROCESSMEMORY       10
#define CMD_READBRATCHMEMORY         11  // 批量读取（按页返回有效数据）
#define CMD_READBRATCHADDR           12  // 批量按地址读取

// —— 内核硬件断点 ——
#define CMD_KERNEL_SETBREAKPOINT     13
#define CMD_KERNEL_REMOVEBREAKPOINT  14
#define CMD_KERNEL_SUSPENDBREAKPOINT 15
#define CMD_KERNEL_RESUMEBREAKPOINT  16
#define CMD_KERNEL_READHWBPINFO      17

// —— ELF 符号 ——
#define CMD_SYMBOL_INIT              18  // 初始化 ELF 符号解析
#define CMD_SYMBOL_GETLIST           19  // 分页获取符号列表
#define CMD_SYMBOL_FIND              20  // 按名称查找符号

// —— 模块快捷查询 ——
#define CMD_GETSOBASE                21  // 按 so 名称获取模块基址

// —— Kernel 断点策略 ——
#define CMD_SETKERNELHWBPRECLAIM     22  // 设置内核断点槽抢占开关
#define CMD_KERNEL_QUERYHWBPTHREADS  23  // 查询进程内各线程硬件断点槽位

// —— Kernel UXN 异常断点 ——
#define CMD_KERNEL_UXN_INSTALL       24
#define CMD_KERNEL_UXN_REMOVE        25
#define CMD_KERNEL_UXN_WAIT          26
#define CMD_KERNEL_UXN_RESUME        27
#define CMD_KERNEL_UXN_STATUS        28
#define CMD_KERNEL_UXN_CLEAR         29

// ============================================================================
// 协议结构体
// ============================================================================

// 含 std::vector 的结构体不能放在 pack(1) 内，否则 ARM 上未定义行为
struct CeReadBratchMemoryOutput {
	uint64_t addr;
	std::vector<unsigned char> data;
};

struct CeReadBratchAddrOutput {
	uint64_t addr;
	std::vector<unsigned char> data;
};

#pragma pack(1)

struct CeReadBratchMemory {
	uint64_t addr;
	uint32_t size;
};

struct CeReadBratchAddr {
	uint64_t addr;
	uint32_t size;
};

struct CeVersion {
	int version;
	unsigned char stringsize;
	//append the versionstring
};

struct CeModuleListEntry {
	int result;       // 模块类型
	int flag;         // 模块权限 1:读 2:写 4:执行 8:私有 16:共享
	uint64_t modulebase;
	int modulesize;
	int modulenamesize;
	//modulename
};

struct CeReadProcessMemoryInput {
	uint32_t handle;
	uint64_t address;
	uint32_t size;
	uint8_t  compress;
};

struct CeReadProcessMemoryOutput {
	int read;
};

struct CeWriteProcessMemoryInput {
	uint32_t handle;
	uint64_t address;
	uint32_t size;
};

struct CeWriteProcessMemoryOutput {
	int32_t written;
};

// ELF 符号初始化输入 (CMD_SYMBOL_INIT)
struct CeSymbolInitInput {
	HANDLE hProcess;
	uint64_t moduleBase;    // 模块基址
};

// ELF 符号初始化输出
struct CeSymbolInitOutput {
	int result;             // 0=成功, -1=失败
	int totalCount;         // 符号总数
};

// 分页获取符号输入 (CMD_SYMBOL_GETLIST)
struct CeGetSymbolListInput {
	int offset;             // 起始偏移
	int count;              // 请求数量
};

// 分页获取符号输出头
struct CeGetSymbolListOutput {
	int totalCount;         // 符号总数
	int actualCount;        // 本次实际返回数量
};

// 单个符号条目
struct CeSymbolEntry {
	uint64_t address;       // 符号地址
	int nameSize;           // 符号名长度
	// 后跟符号名字符串
};

// 按名称查找符号输入 (CMD_SYMBOL_FIND)
struct CeFindSymbolInput {
	HANDLE hProcess;
	uint64_t moduleBase;    // 模块基址
	int nameSize;           // 符号名长度
	// 后跟符号名字符串
};

// 按名称查找符号输出
struct CeFindSymbolOutput {
	int result;             // 0=找到, -1=未找到
	uint64_t address;       // 符号地址
};

// 按名称查找 so 基址输入 (CMD_GETSOBASE)
struct CeGetSoBaseInput {
	HANDLE hProcess;
	int nameSize;           // so 名称长度
	// 后跟 so 名称字符串
};

// 按名称查找 so 基址输出
struct CeGetSoBaseOutput {
	int result;             // 0=找到, -1=未找到
	uint64_t base;          // so 基址
};

struct HwbpTaskThreadHeader {
	int32_t tid;
	int32_t queryResult;
	int32_t errorCode;
	uint32_t count;
	uint32_t totalCount;
	uint32_t brpCount;
	uint32_t wrpCount;
	uint32_t enabledCount;
	uint32_t activeCount;
	uint32_t perfCount;
	uint32_t ptraceCount;
	uint32_t moduleCount;
};

struct HwbpTaskSlot {
	uint64_t eventId;
	uint64_t moduleHandle;
	uint64_t address;
	int32_t tid;
	int32_t onCpu;
	uint32_t type;
	uint32_t length;
	uint32_t state;
	uint32_t source;
	uint32_t flags;
	uint32_t reserved;
};

struct CeUxnResult {
	int32_t result;
	int32_t errorCode;
};

static_assert(sizeof(HwbpTaskThreadHeader) == 48,
	              "HwbpTaskThreadHeader ABI size mismatch");
static_assert(sizeof(HwbpTaskSlot) == 56,
	              "HwbpTaskSlot ABI size mismatch");
static_assert(sizeof(CeUxnResult) == 8,
	              "CeUxnResult ABI size mismatch");

#pragma pack()


// MiniMem: 仅保留 socket 接口使用的 V2 分发器；
// V1 (DispatchCommand / DispatchCmd_V1) 原供 pipe/stdio 接口使用，已随接口裁剪移除。
int DispatchCommand_V2(Ioserver* IOserver, unsigned char command);


#endif /* CESERVER_H_ */
