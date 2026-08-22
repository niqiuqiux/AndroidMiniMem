#pragma once
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <functional>
#include <utility>

#include <cstring>
#include <cstdint>

#include "socket_platform.h"
#include "socket_io_timeout.h"

// 命令常量 — 必须与后端 engine/ceserver/ceserver.h 逐一对应（协议契约）。
// opcode 从 0 连续编号（MiniMem 自有协议，已不兼容 Cheat Engine 原始编号）。

// —— 基础 / 连接 ——
#define CMD_GETVERSION               0
#define CMD_CLOSECONNECTION          1
#define CMD_TERMINATESERVER          2

// —— 内核 / 驱动（内核切换）——
#define CMD_GETMEMTYPE               3
#define CMD_INITRWDRIVER             4

// —— 进程 / 句柄 ——
#define CMD_OPENPROCESS              5
#define CMD_CLOSEHANDLE              6
#define CMD_GETPROCESSLIST           7
#define CMD_GETMODULELIST            8

// —— 内存读写 ——
#define CMD_READPROCESSMEMORY        9
#define CMD_WRITEPROCESSMEMORY       10
#define CMD_READBRATCHMEMORY         11
#define CMD_READBRATCHADDR           12

// —— 内核硬件断点 ——
#define CMD_KERNEL_SETBREAKPOINT     13
#define CMD_KERNEL_REMOVEBREAKPOINT  14
#define CMD_KERNEL_SUSPENDBREAKPOINT 15
#define CMD_KERNEL_RESUMEBREAKPOINT  16
#define CMD_KERNEL_READHWBPINFO      17

// —— ELF 符号 ——
#define CMD_SYMBOL_INIT              18
#define CMD_SYMBOL_GETLIST           19
#define CMD_SYMBOL_FIND              20

// —— 模块快捷查询 ——
#define CMD_GETSOBASE                21

// —— Kernel 断点策略 ——
#define CMD_SETKERNELHWBPRECLAIM     22
#define CMD_KERNEL_QUERYHWBPTHREADS  23

// —— Kernel UXN 异常断点 ——
#define CMD_KERNEL_UXN_INSTALL       24
#define CMD_KERNEL_UXN_REMOVE        25
#define CMD_KERNEL_UXN_WAIT          26
#define CMD_KERNEL_UXN_RESUME        27
#define CMD_KERNEL_UXN_STATUS        28
#define CMD_KERNEL_UXN_CLEAR         29


#pragma pack(1)
struct CeVersion {
    int version;
    unsigned char stringsize;
};

struct CeModuleListEntry {
    int result;
    int flag;
    uint64_t modulebase;
    uint32_t modulesize;
    int modulenamesize;
};

static_assert(sizeof(CeModuleListEntry) == 24,
              "CeModuleListEntry ABI size mismatch");

struct CeReadProcessMemoryInput {
    uint32_t handle;
    uint64_t address;
    uint32_t size;
    uint8_t compress;
};

struct CeWriteProcessMemoryInput {
	uint32_t handle;
	uint64_t address;
	uint32_t size;
};

struct CeWriteProcessMemoryOutput {
	int32_t written;
};

struct CeReadProcessMemoryOutput {
    int read;
};

struct _user_pt_regs {
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    uint64_t orig_x0;
    uint64_t syscallno;
};

struct HW_HIT_INFO {

    uint64_t hit_addr;
    uint64_t hit_time;
    struct _user_pt_regs regs_info;
};



struct CeReadBratchMemoryOutput{
		//int result;//发出去的都是有效页面，所以result可以不用
		uint64_t addr;
		std::vector<unsigned char> data;
};

struct CeReadBratchAddr{
	uint64_t addr;
	uint32_t size;
};

struct CeSymbolInitInput {
    uint32_t hProcess;
    uint64_t moduleBase;
};

struct CeSymbolInitOutput {
    int result;
    int totalCount;
};

struct CeGetSymbolListInput {
    int offset;
    int count;
};

struct CeGetSymbolListOutput {
    int totalCount;
    int actualCount;
};

struct CeSymbolEntry {
    uint64_t address;
    int nameSize;
};

struct CeFindSymbolInput {
    uint32_t hProcess;
    uint64_t moduleBase;
    int nameSize;
};

struct CeFindSymbolOutput {
    int result;
    uint64_t address;
};

struct CeGetSoBaseInput {
    uint32_t hProcess;
    int nameSize;
};

struct CeGetSoBaseOutput {
    int result;
    uint64_t base;
};

#pragma pack(push, 1)
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
#pragma pack(pop)

constexpr uint32_t CE_UXN_MAX_SLOTS = 16;
constexpr uint32_t CE_UXN_WAIT_ANY_SLOT = UINT32_MAX;
constexpr uint32_t CE_UXN_RESUME_SET_REGS = 1u << 0;
constexpr uint32_t CE_UXN_FPSIMD_VALID = 1u << 0;

enum CeUxnState : uint32_t {
    CE_UXN_STATE_EMPTY = 0,
    CE_UXN_STATE_ARMED = 1,
    CE_UXN_STATE_PAUSED = 2,
    CE_UXN_STATE_STEPPING = 3,
};

struct CeUxnResult {
    int32_t result;
    int32_t errorCode;
};

struct CeUxnRegisters {
    uint64_t registers[31];
    uint64_t stackPointer;
    uint64_t programCounter;
    uint64_t pstate;
};

struct CeUxnFpRegister {
    uint64_t low;
    uint64_t high;
};

struct CeUxnFpsimdRegisters {
    CeUxnFpRegister registers[32];
    uint32_t fpsr;
    uint32_t fpcr;
    uint32_t flags;
    uint32_t reserved;
};

struct CeUxnInstall {
    uint32_t pid;
    uint32_t flags;
    uint64_t address;
    uint32_t slot;
    uint32_t reserved;
};

struct CeUxnEvent {
    uint32_t slot;
    uint32_t pid;
    uint32_t tid;
    uint32_t state;
    uint64_t sequence;
    uint64_t address;
    uint64_t page;
    uint64_t faultAddress;
    uint64_t esr;
    uint64_t hits;
    uint64_t falseHits;
    CeUxnRegisters registers;
    uint64_t fpsimdAlignmentPadding;
    CeUxnFpsimdRegisters fpsimd;
};

struct CeUxnResume {
    uint32_t slot;
    uint32_t flags;
    CeUxnRegisters registers;
};

struct CeUxnStatus {
    uint32_t slot;
    uint32_t used;
    uint32_t pid;
    uint32_t tid;
    uint32_t state;
    int32_t lastError;
    uint64_t address;
    uint64_t page;
    uint64_t hits;
    uint64_t falseHits;
    uint64_t stepHits;
    uint64_t resumes;
    uint64_t sequence;
};

static_assert(sizeof(HwbpTaskThreadHeader) == 48,
              "HwbpTaskThreadHeader ABI size mismatch");
static_assert(sizeof(HwbpTaskSlot) == 56,
              "HwbpTaskSlot ABI size mismatch");
static_assert(sizeof(CeUxnResult) == 8, "CeUxnResult ABI size mismatch");
static_assert(sizeof(CeUxnRegisters) == 272,
              "CeUxnRegisters ABI size mismatch");
static_assert(sizeof(CeUxnFpsimdRegisters) == 528,
              "CeUxnFpsimdRegisters ABI size mismatch");
static_assert(sizeof(CeUxnInstall) == 24,
              "CeUxnInstall ABI size mismatch");
static_assert(sizeof(CeUxnEvent) == 880,
              "CeUxnEvent ABI size mismatch");
static_assert(offsetof(CeUxnEvent, faultAddress) == 40,
              "CeUxnEvent FAR offset mismatch");
static_assert(offsetof(CeUxnEvent, registers) == 72,
              "CeUxnEvent registers offset mismatch");
static_assert(offsetof(CeUxnEvent, fpsimd) == 352,
              "CeUxnEvent FPSIMD offset mismatch");
static_assert(sizeof(CeUxnResume) == 280,
              "CeUxnResume ABI size mismatch");
static_assert(sizeof(CeUxnStatus) == 80,
              "CeUxnStatus ABI size mismatch");

#pragma pack()

class WindowsSocketClient {
private:
    SocketPlatform::ScopedSocket sock_;
    bool connected_ = false;
    bool platformStarted_ = false;
    std::function<void()> poisonCallback_;

    void PoisonAndClose() {
        if (poisonCallback_) {
            poisonCallback_();
        }
        Close();
    }

public:
    WindowsSocketClient() {
        platformStarted_ = SocketPlatform::Startup();
        if (!platformStarted_) {
            std::cerr << "Socket startup failed: " << SocketPlatform::LastError() << std::endl;
        }
    }

    ~WindowsSocketClient() {
        Close();
        if (platformStarted_) {
            SocketPlatform::Cleanup();
        }
    }

    bool Connect(const std::string& host, uint16_t port) {
        if (connected_) {
            Close();
        }

        sock_.reset(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (!sock_.valid()) {
            std::cerr << "socket() failed: " << SocketPlatform::LastError() << std::endl;
            return false;
        }

        sockaddr_in serverAddr{};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port);

        if (inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr) != 1) {
            std::cerr << "Invalid address: " << host << std::endl;
            sock_.reset();
            return false;
        }

        if (connect(sock_.get(), (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
            std::cerr << "connect() failed: " << SocketPlatform::LastError() << std::endl;
            sock_.reset();
            return false;
        }

        connected_ = true;
        std::cout << "Connected to " << host << ":" << port << std::endl;
        return true;
    }

    bool Send(const void* data, size_t size) {
        if (!connected_ || !sock_.valid()) return false;

        SocketIoTimeout::SocketOptionTimeoutGuard timeoutGuard(sock_.get(), SO_SNDTIMEO);
        const char* buffer = static_cast<const char*>(data);
        size_t totalSent = 0;

        while (totalSent < size) {
            int sent = ::send(sock_.get(), buffer + totalSent, static_cast<int>(size - totalSent), 0);
            if (sent == SOCKET_ERROR) {
                int err = SocketPlatform::LastError();
                std::cerr << "send() failed: " << err << std::endl;
                // 无帧流发生任意 I/O 失败后都无法可靠恢复边界，必须显式重连。
                timeoutGuard.dismissRestore();
                PoisonAndClose();
                return false;
            }
            if (sent <= 0) {
                timeoutGuard.dismissRestore();
                PoisonAndClose();
                return false;
            }
            totalSent += sent;
        }
        return true;
    }

    bool Receive(void* buffer, size_t size) {
        if (!connected_ || !sock_.valid()) return false;

        SocketIoTimeout::SocketOptionTimeoutGuard timeoutGuard(sock_.get(), SO_RCVTIMEO);
        char* buf = static_cast<char*>(buffer);
        size_t totalReceived = 0;

        while (totalReceived < size) {
            int received = ::recv(sock_.get(), buf + totalReceived, static_cast<int>(size - totalReceived), 0);
            if (received == SOCKET_ERROR) {
                int err = SocketPlatform::LastError();
                std::cerr << "recv() failed: " << err << std::endl;
                timeoutGuard.dismissRestore();
                PoisonAndClose();
                return false;
            }
            if (received == 0) {
                std::cerr << "Connection closed by server" << std::endl;
                timeoutGuard.dismissRestore();
                PoisonAndClose();
                return false;
            }
            totalReceived += received;
        }
        return true;
    }

    void Close() {
        sock_.reset();
        connected_ = false;
    }

    bool IsConnected() const { return connected_; }

    // 无帧协议收到非法响应后无法证明下一条消息边界，必须废弃连接。
    void InvalidateProtocol() { PoisonAndClose(); }

    void SetPoisonCallback(std::function<void()> callback) {
        poisonCallback_ = std::move(callback);
    }
};


static void CloseServer(WindowsSocketClient& client) {
    std::cout << "\n=== Closing Server ===" << std::endl;
    unsigned char command = CMD_TERMINATESERVER;
    client.Send(&command, sizeof(command));
}
