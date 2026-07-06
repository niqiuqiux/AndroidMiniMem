#pragma once
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>

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


#pragma pack(1)
struct CeVersion {
    int version;
    unsigned char stringsize;
};

struct CeModuleListEntry {
    int result;
    int flag;
    uint64_t modulebase;
    int modulesize;
    int modulenamesize;
};

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

#pragma pack()

class WindowsSocketClient {
private:
    SocketPlatform::ScopedSocket sock_;
    bool connected_ = false;
    bool platformStarted_ = false;

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
                // 仅在真正的连接错误时关闭：这条 socket 由 GUI / AI / IPC 共享，
                // 单次请求的超时（WSAETIMEDOUT）只是预算事件，不能拆掉共享连接，
                // 下一次请求前的 DrainPending() 会重新同步协议。
                if (SocketPlatform::IsConnectionResetError(err)) {
                    timeoutGuard.dismissRestore();
                    Close();
                }
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
                // 仅在真正的连接错误时关闭：这条 socket 由 GUI / AI / IPC 共享，
                // 单次请求的超时（WSAETIMEDOUT）只是预算事件，不能拆掉共享连接。
                // 此时设备的响应可能仍在途/已在缓冲区，下一次请求前的
                // DrainPending() 会清掉这些过期字节、重新同步协议。
                if (SocketPlatform::IsConnectionResetError(err)) {
                    timeoutGuard.dismissRestore();
                    Close();
                }
                return false;
            }
            if (received == 0) {
                std::cerr << "Connection closed by server" << std::endl;
                connected_ = false;
                return false;
            }
            totalReceived += received;
        }
        return true;
    }

    size_t DrainPending(size_t maxBytes = 16 * 1024 * 1024) {
        if (!connected_ || !sock_.valid()) return 0;

        std::vector<char> buffer(4096);
        size_t totalDrained = 0;
        while (totalDrained < maxBytes) {
            SocketPlatform::AvailableBytes pending = 0;
            if (SocketPlatform::GetAvailableBytes(sock_.get(), pending) == SOCKET_ERROR || pending <= 0) {
                break;
            }

            const size_t toRead = std::min<size_t>(
                {buffer.size(), static_cast<size_t>(pending), maxBytes - totalDrained});
            int received = ::recv(sock_.get(), buffer.data(), static_cast<int>(toRead), 0);
            if (received == SOCKET_ERROR) {
                std::cerr << "drain recv() failed: " << SocketPlatform::LastError() << std::endl;
                break;
            }
            if (received == 0) {
                connected_ = false;
                break;
            }
            totalDrained += static_cast<size_t>(received);
        }

        if (totalDrained > 0) {
            std::cerr << "Drained " << totalDrained << " stale socket bytes before request" << std::endl;
        }
        return totalDrained;
    }

    void Close() {
        sock_.reset();
        connected_ = false;
    }

    bool IsConnected() const { return connected_; }
};


static void CloseServer(WindowsSocketClient& client) {
    std::cout << "\n=== Closing Server ===" << std::endl;
    unsigned char command = CMD_TERMINATESERVER;
    client.Send(&command, sizeof(command));
}
