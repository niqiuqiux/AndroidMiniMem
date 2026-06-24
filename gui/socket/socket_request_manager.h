#pragma once

#include "socket_io_timeout.h"

#include <mutex>
#include <chrono>
#include <thread>

/**
 * Socket请求管理器
 * 
 * 目的：解决多个窗口（BreakpointWindow、ScanWindow、MemoryViewerWindow等）
 * 同时使用同一个Socket连接时产生的数据接收冲突问题。
 * 
 * 问题场景：
 * - 窗口A发送CMD_READPROCESSMEMORY命令
 * - 窗口B在窗口A接收响应前发送CMD_KERNEL_READHWBPINFO命令
 * - 结果：窗口A接收到窗口B的数据，窗口B接收到窗口A的数据
 * 
 * 解决方案：
 * - 使用互斥锁确保每次Socket请求-响应操作是原子的
 * - 支持自定义锁，允许为不同端口使用独立的锁，减少锁争用
 * - 防止多个线程同时访问Socket导致数据错乱
 */
class SocketRequestManager {
public:
    static SocketRequestManager& GetInstance() {
        static SocketRequestManager instance;
        return instance;
    }

    // 禁止拷贝和赋值
    SocketRequestManager(const SocketRequestManager&) = delete;
    SocketRequestManager& operator=(const SocketRequestManager&) = delete;

    /**
     * 执行一个Socket请求（使用内部全局锁）
     * @param request 请求执行函数（包含Send和Receive操作）
     * @return 请求是否成功
     * 
     * 使用示例：
     * bool result = manager.ExecuteRequest([&]() -> bool {
     *     // 发送命令
     *     if (!client.Send(&command, sizeof(command))) return false;
     *     // 接收响应
     *     if (!client.Receive(&response, sizeof(response))) return false;
     *     return true;
     * });
     */
    template<typename RequestFunc>
    bool ExecuteRequest(RequestFunc request) {
        if (SocketIoTimeout::HasThreadTimeout()) {
            if (SocketIoTimeout::IsThreadTimeoutExpired()) {
                return false;
            }
            return TryExecuteRequest(
                request,
                static_cast<int>(SocketIoTimeout::GetRemainingTimeoutMs()));
        }

        std::lock_guard<std::mutex> lock(requestMutex_);
        return request();
    }
    
    /**
     * 执行一个Socket请求（使用自定义锁）
     * @param customMutex 自定义互斥锁（例如端口专属锁）
     * @param request 请求执行函数（包含Send和Receive操作）
     * @return 请求是否成功
     * 
     * 使用示例：
     * std::mutex* portMutex = GetSocketMgr().GetMutex(PORT_MAIN);
     * bool result = manager.ExecuteRequestWithLock(portMutex, [&]() -> bool {
     *     // 发送命令到主端口
     *     if (!client->Send(&command, sizeof(command))) return false;
     *     // 接收响应
     *     if (!client->Receive(&response, sizeof(response))) return false;
     *     return true;
     * });
     * 
     * 优势：不同端口使用不同的锁，减少锁争用，提高并发性能
     */
    template<typename RequestFunc>
    bool ExecuteRequestWithLock(std::mutex* customMutex, RequestFunc request) {
        if (SocketIoTimeout::HasThreadTimeout()) {
            if (SocketIoTimeout::IsThreadTimeoutExpired()) {
                return false;
            }
            return TryExecuteRequestWithLock(customMutex,
                                             request,
                                             static_cast<int>(SocketIoTimeout::GetRemainingTimeoutMs()));
        }

        if (!customMutex) {
            // 如果未提供自定义锁，使用全局锁
            return ExecuteRequest(request);
        }
        std::lock_guard<std::mutex> lock(*customMutex);
        return request();
    }

    /**
     * 尝试执行Socket请求（带超时，使用内部全局锁）
     * @param request 请求执行函数
     * @param timeoutMs 超时时间（毫秒）
     * @return 请求是否成功
     */
    template<typename RequestFunc>
    bool TryExecuteRequest(RequestFunc request, int timeoutMs = 5000) {
        auto start = std::chrono::steady_clock::now();
        
        // 尝试获取锁
        std::unique_lock<std::mutex> lock(requestMutex_, std::defer_lock);
        
        while (!lock.try_lock()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start
            ).count();
            
            if (elapsed > timeoutMs) {
                return false; // 超时
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        return request();
    }
    
    /**
     * 尝试执行Socket请求（带超时，使用自定义锁）
     * @param customMutex 自定义互斥锁
     * @param request 请求执行函数
     * @param timeoutMs 超时时间（毫秒）
     * @return 请求是否成功
     */
    template<typename RequestFunc>
    bool TryExecuteRequestWithLock(std::mutex* customMutex, RequestFunc request, int timeoutMs = 5000) {
        if (!customMutex) {
            // 如果未提供自定义锁，使用全局锁
            return TryExecuteRequest(request, timeoutMs);
        }
        
        auto start = std::chrono::steady_clock::now();
        
        // 尝试获取自定义锁
        std::unique_lock<std::mutex> lock(*customMutex, std::defer_lock);
        
        while (!lock.try_lock()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start
            ).count();
            
            if (elapsed > timeoutMs) {
                return false; // 超时
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        return request();
    }

    /**
     * 获取互斥锁（供高级用户使用）
     * 注意：使用后需要手动释放
     */
    std::mutex& GetMutex() {
        return requestMutex_;
    }

private:
    SocketRequestManager() = default;
    ~SocketRequestManager() = default;

    std::mutex requestMutex_; // 保护Socket操作的互斥锁
};

// 便捷宏定义
#define EXECUTE_SOCKET_REQUEST(request) \
    SocketRequestManager::GetInstance().ExecuteRequest([&]() -> bool { return (request); })

#define TRY_EXECUTE_SOCKET_REQUEST(request, timeout) \
    SocketRequestManager::GetInstance().TryExecuteRequest([&]() -> bool { return (request); }, timeout)

// 使用自定义锁的便捷宏
#define EXECUTE_SOCKET_REQUEST_WITH_LOCK(mutex, request) \
    SocketRequestManager::GetInstance().ExecuteRequestWithLock(mutex, [&]() -> bool { return (request); })

#define TRY_EXECUTE_SOCKET_REQUEST_WITH_LOCK(mutex, request, timeout) \
    SocketRequestManager::GetInstance().TryExecuteRequestWithLock(mutex, [&]() -> bool { return (request); }, timeout)
