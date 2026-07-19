#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>
#include "../mem/MemTypes.h"

using json = nlohmann::json;
namespace Mem { class IMemService; }

/**
 * 轻量 HTTP Server，监听 localhost，接收 JSON-RPC 风格请求，
 * 分发到已注册的 handler，返回 JSON 响应。
 * 供外部 MCP Python 代理通过 HTTP 调用 GUI 已有的 C++ 能力。
 */
class IpcServer {
public:
    using Handler = std::function<json(const json& params)>;

    static IpcServer& GetInstance();

    // 启动/停止
    bool Start(Mem::IMemService& service, uint16_t port = 28100);
    void Stop();
    bool IsRunning() const { return running_.load(); }
    uint16_t GetPort() const { return port_; }

    // 注册路由
    void RegisterMethod(const std::string& method, Handler handler);

private:
    IpcServer() = default;
    ~IpcServer() { Stop(); }
    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    void ServerThread();
    void ClientWorker();
    void HandleClient(uintptr_t clientSocket);
    void CloseListenSocket();
    void WakeAcceptLoop();
    std::string BuildHttpResponse(int statusCode, const std::string& body);
    json DispatchRequest(const json& request);

    // 注册所有内置路由
    void RegisterBuiltinMethods();

    std::atomic<bool> running_{false};
    uint16_t port_ = 28100;
    std::atomic<uintptr_t> listenSocket_{~(uintptr_t)0}; // INVALID_SOCKET
    std::thread serverThread_;
    std::vector<std::thread> clientThreads_;
    std::mutex clientQueueMutex_;
    std::condition_variable clientQueueCv_;
    std::deque<uintptr_t> clientQueue_;
    bool stopClientWorkers_ = false;
    std::unordered_map<std::string, Handler> handlers_;
    Mem::IMemService* service_ = nullptr;
    std::mutex symbolCacheMutex_;
    std::optional<Mem::SymbolTable> legacySymbolTable_;
};
