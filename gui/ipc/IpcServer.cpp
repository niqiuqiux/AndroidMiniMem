#include "IpcServer.h"
#include "IpcHttpRequest.h"
#include "../mem/IMemService.h"
#include "../socket/socket_platform.h"
#include "../socket/socket_io_timeout.h"
#include "../gui/Gui.h"

#ifdef HAVE_LUAJIT
#include "../lua/LuaEngine.h"
#endif

#include <sstream>
#include <algorithm>
#include <iomanip>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace {
constexpr uint32_t kMaxIpcMemoryTransferBytes = 64 * 1024;
constexpr size_t kMaxIpcBatchReadCount = Mem::kMaxMemoryBatchCount;
constexpr uint64_t kMaxIpcBatchReadTotalBytes = Mem::kMaxMemoryBatchBytes;
constexpr int kDefaultIpcLuaTimeoutSeconds = 30;
constexpr int kMaxIpcLuaTimeoutSeconds = 300;
constexpr int kDefaultIpcRequestTimeoutSeconds = 30;
constexpr size_t kMaxIpcStringParamBytes = 4096;
constexpr size_t kMaxIpcLuaCodeBytes = 256 * 1024;
constexpr size_t kMaxIpcOffsetChainLength = 1024;
constexpr size_t kIpcWorkerCount = 4;
constexpr size_t kMaxPendingIpcClients = 64;

bool isBlankString(const std::string& value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
}

uint32_t getPositiveSizeParam(const json& params,
                              const char* key,
                              uint32_t defaultValue,
                              uint32_t maxValue) {
    if (!params.contains(key)) {
        return defaultValue;
    }

    uint64_t value = 0;
    const auto& raw = params.at(key);
    if (raw.is_number_unsigned()) {
        value = raw.get<uint64_t>();
    } else if (raw.is_number_integer()) {
        const int64_t signedValue = raw.get<int64_t>();
        if (signedValue <= 0) {
            throw std::invalid_argument(std::string(key) + " must be positive");
        }
        value = static_cast<uint64_t>(signedValue);
    } else {
        throw std::invalid_argument(std::string(key) + " must be an integer");
    }

    if (value == 0) {
        throw std::invalid_argument(std::string(key) + " must be positive");
    }
    if (value > maxValue) {
        value = maxValue;
    }
    return static_cast<uint32_t>(value);
}

std::string getStringParam(const json& params,
                           const char* key,
                           const std::string& defaultValue,
                           bool required,
                           bool allowEmpty,
                           size_t maxLength) {
    if (!params.contains(key)) {
        if (required) {
            throw std::invalid_argument(std::string(key) + " is required");
        }
        return defaultValue;
    }

    const auto& raw = params.at(key);
    if (!raw.is_string()) {
        throw std::invalid_argument(std::string(key) + " must be a string");
    }

    std::string value = raw.get<std::string>();
    if (!allowEmpty && isBlankString(value)) {
        throw std::invalid_argument(std::string(key) + " must not be empty");
    }
    if (value.size() > maxLength) {
        throw std::invalid_argument(std::string(key) + " is too long");
    }
    return value;
}

int getClampedIntParam(const json& params,
                       const char* key,
                       int defaultValue,
                       int minValue,
                       int maxValue) {
    if (!params.contains(key)) {
        return defaultValue;
    }

    int64_t value = 0;
    const auto& raw = params.at(key);
    if (raw.is_number_unsigned()) {
        const uint64_t unsignedValue = raw.get<uint64_t>();
        value = unsignedValue > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())
                    ? (std::numeric_limits<int64_t>::max)()
                    : static_cast<int64_t>(unsignedValue);
    } else if (raw.is_number_integer()) {
        value = raw.get<int64_t>();
    } else {
        throw std::invalid_argument(std::string(key) + " must be an integer");
    }

    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return static_cast<int>(value);
}

int getOptionalIntParam(const json& params, const char* key, int defaultValue) {
    if (!params.contains(key)) {
        return defaultValue;
    }

    int64_t value = 0;
    const auto& raw = params.at(key);
    if (raw.is_number_unsigned()) {
        const uint64_t unsignedValue = raw.get<uint64_t>();
        if (unsignedValue > static_cast<uint64_t>((std::numeric_limits<int>::max)())) {
            throw std::invalid_argument(std::string(key) + " out of range");
        }
        value = static_cast<int64_t>(unsignedValue);
    } else if (raw.is_number_integer()) {
        value = raw.get<int64_t>();
    } else {
        throw std::invalid_argument(std::string(key) + " must be an integer");
    }

    if (value < (std::numeric_limits<int>::min)() ||
        value > (std::numeric_limits<int>::max)()) {
        throw std::invalid_argument(std::string(key) + " out of range");
    }
    return static_cast<int>(value);
}

int getRequiredPositiveIntParam(const json& params, const char* key) {
    if (!params.contains(key)) {
        throw std::invalid_argument(std::string(key) + " is required");
    }

    uint64_t value = 0;
    const auto& raw = params.at(key);
    if (raw.is_number_unsigned()) {
        value = raw.get<uint64_t>();
    } else if (raw.is_number_integer()) {
        const int64_t signedValue = raw.get<int64_t>();
        if (signedValue <= 0) {
            throw std::invalid_argument(std::string(key) + " must be positive");
        }
        value = static_cast<uint64_t>(signedValue);
    } else {
        throw std::invalid_argument(std::string(key) + " must be an integer");
    }

    if (value == 0 ||
        value > static_cast<uint64_t>((std::numeric_limits<int>::max)())) {
        throw std::invalid_argument(std::string(key) + " out of range");
    }
    return static_cast<int>(value);
}

uint32_t getOptionalPositiveUintParam(const json& params,
                                      const char* key,
                                      uint32_t defaultValue,
                                      uint32_t maxValue) {
    if (!params.contains(key)) {
        return defaultValue;
    }

    uint64_t value = 0;
    const auto& raw = params.at(key);
    if (raw.is_number_unsigned()) {
        value = raw.get<uint64_t>();
    } else if (raw.is_number_integer()) {
        const int64_t signedValue = raw.get<int64_t>();
        if (signedValue <= 0) {
            throw std::invalid_argument(std::string(key) + " must be positive");
        }
        value = static_cast<uint64_t>(signedValue);
    } else {
        throw std::invalid_argument(std::string(key) + " must be an integer");
    }

    if (value == 0 || value > maxValue) {
        throw std::invalid_argument(std::string(key) + " out of range");
    }
    return static_cast<uint32_t>(value);
}

bool getOptionalBoolParam(const json& params, const char* key, bool defaultValue) {
    if (!params.contains(key)) {
        return defaultValue;
    }
    const auto& raw = params.at(key);
    if (!raw.is_boolean()) {
        throw std::invalid_argument(std::string(key) + " must be a boolean");
    }
    return raw.get<bool>();
}

bool isValidBreakpointSize(uint32_t size) {
    return size == 1 || size == 2 || size == 4 || size == 8;
}

json serviceFailure(const Mem::Error& error) {
    json response = {
        {"success", false},
        {"error", error.message},
        {"error_code", Mem::errorCodeName(error.code)},
        {"retryable", error.retryable}
    };
    if (error.affectedBytes) {
        response["result"] = {{"written", *error.affectedBytes}};
    }
    return response;
}

std::string formatAddress(uint64_t address) {
    std::ostringstream output;
    output << "0x" << std::hex << address;
    return output.str();
}
} // namespace

// ── 单例 ─────────────────────────────────────────────────────────
IpcServer& IpcServer::GetInstance() {
    static IpcServer instance;
    return instance;
}

// ── 启动/停止 ────────────────────────────────────────────────────
bool IpcServer::Start(Mem::IMemService& service, uint16_t port) {
    if (running_.load()) return true;

    service_ = &service;
    port_ = port;
    RegisterBuiltinMethods();

    if (!SocketPlatform::Startup()) {
        Gui::log("[IPC] 初始化 socket 失败");
        return false;
    }

    listenSocket_.store((uintptr_t)::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (listenSocket_.load() == (uintptr_t)INVALID_SOCKET) {
        Gui::log("[IPC] 创建 socket 失败");
        SocketPlatform::Cleanup();
        return false;
    }

    // 允许端口复用
    int opt = 1;
    ::setsockopt((SOCKET)listenSocket_.load(), SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 仅本地

    if (::bind((SOCKET)listenSocket_.load(), (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        Gui::log("[IPC] 绑定端口 %d 失败", (int)port_);
        CloseListenSocket();
        SocketPlatform::Cleanup();
        return false;
    }

    if (::listen((SOCKET)listenSocket_.load(), SOMAXCONN) == SOCKET_ERROR) {
        Gui::log("[IPC] listen 失败");
        CloseListenSocket();
        SocketPlatform::Cleanup();
        return false;
    }

    running_.store(true);
    {
        std::lock_guard<std::mutex> lock(clientQueueMutex_);
        stopClientWorkers_ = false;
        clientQueue_.clear();
    }
    clientThreads_.reserve(kIpcWorkerCount);
    for (size_t i = 0; i < kIpcWorkerCount; ++i) {
        clientThreads_.emplace_back(&IpcServer::ClientWorker, this);
    }
    serverThread_ = std::thread(&IpcServer::ServerThread, this);
    Gui::log("[IPC] 服务已启动，监听端口 %d", (int)port_);
    return true;
}
void IpcServer::Stop() {
    if (!running_.load()) return;
    running_.store(false);
    Gui::log("[IPC] 服务正在停止...");

    WakeAcceptLoop();
    CloseListenSocket();

    if (serverThread_.joinable())
        serverThread_.join();

    std::deque<uintptr_t> pendingClients;
    {
        std::lock_guard<std::mutex> lock(clientQueueMutex_);
        stopClientWorkers_ = true;
        pendingClients.swap(clientQueue_);
    }
    for (uintptr_t client : pendingClients) {
        SocketPlatform::Close(static_cast<SOCKET>(client));
    }
    clientQueueCv_.notify_all();
    for (auto& worker : clientThreads_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    clientThreads_.clear();

    SocketPlatform::Cleanup();
}

void IpcServer::CloseListenSocket() {
    const uintptr_t socket = listenSocket_.exchange((uintptr_t)INVALID_SOCKET);
    if (socket == (uintptr_t)INVALID_SOCKET) {
        return;
    }

    SocketPlatform::Close((SOCKET)socket);
}

void IpcServer::WakeAcceptLoop() {
    SocketPlatform::ScopedSocket wakeSocket(
        ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!wakeSocket.valid()) {
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    (void)::connect(wakeSocket.get(), (sockaddr*)&addr, sizeof(addr));
}

void IpcServer::ServerThread() {
    while (true) {
        const uintptr_t listenSocket = listenSocket_.load();
        if (listenSocket == (uintptr_t)INVALID_SOCKET) {
            break;
        }

        SOCKET client = ::accept((SOCKET)listenSocket, nullptr, nullptr);
        if (client == INVALID_SOCKET) break;
        if (!running_.load()) {
            SocketPlatform::Close(client);
            break;
        }

        bool queued = false;
        {
            std::lock_guard<std::mutex> lock(clientQueueMutex_);
            if (!stopClientWorkers_ &&
                clientQueue_.size() < kMaxPendingIpcClients) {
                clientQueue_.push_back(static_cast<uintptr_t>(client));
                queued = true;
            }
        }
        if (queued) {
            clientQueueCv_.notify_one();
        } else {
            SocketPlatform::Close(client);
        }
    }
}

void IpcServer::ClientWorker() {
    while (true) {
        uintptr_t client = static_cast<uintptr_t>(INVALID_SOCKET);
        {
            std::unique_lock<std::mutex> lock(clientQueueMutex_);
            clientQueueCv_.wait(lock, [&] {
                return stopClientWorkers_ || !clientQueue_.empty();
            });
            if (stopClientWorkers_ && clientQueue_.empty()) {
                return;
            }
            client = clientQueue_.front();
            clientQueue_.pop_front();
        }
        HandleClient(client);
    }
}

// ── HTTP 处理 ────────────────────────────────────────────────────
void IpcServer::HandleClient(uintptr_t clientSocket) {
    SocketPlatform::ScopedSocket client((SOCKET)clientSocket);
    SOCKET sock = client.get();

    // 设置超时
    auto timeout = SocketPlatform::MakeTimeoutValue(5000);
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                 SocketPlatform::OptionData(timeout), sizeof(timeout));

    // 读取完整 HTTP 请求（最大 1MB）
    std::string raw;
    raw.reserve(4096);
    char buf[4096];
    size_t contentLength = 0;
    bool contentLengthKnown = false;
    bool requestTooLarge = false;
    int earlyStatusCode = 0;
    std::string earlyError;
    size_t headerEnd = std::string::npos;

    while (true) {
        int n = ::recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw.append(buf, n);

        // 查找 header 结束
        if (headerEnd == std::string::npos) {
            headerEnd = raw.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                const std::string headers = raw.substr(0, headerEnd);
                const IpcHttp::ValidationResult validation =
                    IpcHttp::ValidateRequestHead(headers);
                if (!validation.accepted()) {
                    earlyStatusCode = validation.statusCode;
                    earlyError = validation.error;
                    break;
                }
                contentLength = validation.request.contentLength;
                contentLengthKnown = true;
            }
        }

        if (headerEnd != std::string::npos && contentLengthKnown) {
            size_t bodyStart = headerEnd + 4;
            if (raw.size() >= bodyStart && raw.size() - bodyStart >= contentLength) break;
        }

        if (raw.size() > IpcHttp::kMaxRequestBytes) {
            requestTooLarge = true;
            break;
        }
    }

    if (earlyStatusCode != 0) {
        json response = {{"success", false}, {"error", earlyError}};
        std::string httpResp = BuildHttpResponse(earlyStatusCode, response.dump());
        ::send(sock, httpResp.c_str(), (int)httpResp.size(), 0);
        return;
    }

    if (headerEnd == std::string::npos) {
        json response = {{"success", false},
                         {"error", earlyError.empty() ? "Invalid HTTP request"
                                                       : earlyError}};
        std::string httpResp = BuildHttpResponse(400, response.dump());
        ::send(sock, httpResp.c_str(), (int)httpResp.size(), 0);
        return;
    }

    if (requestTooLarge) {
        json response = {{"success", false}, {"error", "HTTP request too large"}};
        std::string httpResp = BuildHttpResponse(413, response.dump());
        ::send(sock, httpResp.c_str(), (int)httpResp.size(), 0);
        return;
    }

    // 提取 body
    const size_t bodyStart = headerEnd + 4;
    if (contentLengthKnown &&
        (raw.size() < bodyStart || raw.size() - bodyStart < contentLength)) {
        json response = {{"success", false},
                         {"error", "Incomplete HTTP request body"}};
        std::string httpResp = BuildHttpResponse(400, response.dump());
        ::send(sock, httpResp.c_str(), (int)httpResp.size(), 0);
        return;
    }

    std::string body;
    if (contentLengthKnown && raw.size() >= bodyStart)
        body = raw.substr(bodyStart, contentLength);

    // 解析 JSON 并分发
    json response;
    int statusCode = 200;
    try {
        json request = json::parse(body);
        SocketIoTimeout::ScopedTimeout requestTimeout(
            kDefaultIpcRequestTimeoutSeconds);
        response = DispatchRequest(request);
    } catch (const json::parse_error& e) {
        statusCode = 400;
        response = {{"success", false}, {"error", std::string("JSON 解析错误: ") + e.what()}};
        Gui::log("[IPC] JSON 解析错误: %s", e.what());
    } catch (const std::exception& e) {
        statusCode = 500;
        response = {{"success", false}, {"error", e.what()}};
        Gui::log("[IPC] 请求处理异常: %s", e.what());
    }

    std::string httpResp = BuildHttpResponse(statusCode, response.dump());
    ::send(sock, httpResp.c_str(), (int)httpResp.size(), 0);
}

std::string IpcServer::BuildHttpResponse(int statusCode, const std::string& body) {
    const char* reason = "OK";
    switch (statusCode) {
    case 200: reason = "OK"; break;
    case 400: reason = "Bad Request"; break;
    case 403: reason = "Forbidden"; break;
    case 404: reason = "Not Found"; break;
    case 405: reason = "Method Not Allowed"; break;
    case 413: reason = "Payload Too Large"; break;
    case 415: reason = "Unsupported Media Type"; break;
    case 500: reason = "Internal Server Error"; break;
    default:  reason = "Unknown"; break;
    }

    std::ostringstream oss;
    oss << "HTTP/1.1 " << statusCode << " " << reason << "\r\n"
        << "Content-Type: application/json; charset=utf-8\r\n"
        << "X-Content-Type-Options: nosniff\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;
    return oss.str();
}

json IpcServer::DispatchRequest(const json& request) {
    if (!request.contains("method") || !request["method"].is_string()) {
        return {{"success", false}, {"error", "缺少 method 字段"}};
    }

    std::string method = request["method"].get<std::string>();
    json params = request.value("params", json::object());
    if (!params.is_object()) {
        return {{"success", false}, {"error", "params must be an object"}};
    }

    Gui::log("[IPC] 收到请求: %s", method.c_str());

    auto it = handlers_.find(method);
    if (it == handlers_.end()) {
        Gui::log("[IPC] 未知方法: %s", method.c_str());
        return {{"success", false}, {"error", "未知方法: " + method}};
    }

    try {
        json result = it->second(params);
        bool ok = result.value("success", false);
        if (!ok) {
            std::string err = result.value("error", "");
            Gui::log("[IPC] %s 失败: %s", method.c_str(), err.c_str());
        }
        return result;
    } catch (const std::exception& e) {
        Gui::log("[IPC] %s 异常: %s", method.c_str(), e.what());
        return {{"success", false}, {"error", std::string("执行错误: ") + e.what()}};
    }
}

void IpcServer::RegisterMethod(const std::string& method, Handler handler) {
    handlers_[method] = std::move(handler);
}

// ── 辅助：bytes <-> hex string ───────────────────────────────────
static std::string BytesToHex(const std::vector<unsigned char>& data) {
    std::ostringstream oss;
    for (auto b : data) oss << std::hex << std::setfill('0') << std::setw(2) << (int)b;
    return oss.str();
}

static int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static std::vector<unsigned char> HexToBytes(const std::string& hex) {
    std::string cleaned;
    cleaned.reserve(hex.size());
    for (char ch : hex) {
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            cleaned.push_back(ch);
        }
    }

    if ((cleaned.size() % 2) != 0) {
        throw std::invalid_argument("hex string must contain an even number of digits");
    }

    std::vector<unsigned char> out;
    out.reserve(cleaned.size() / 2);
    for (size_t i = 0; i < cleaned.size(); i += 2) {
        int hi = HexNibble(cleaned[i]);
        int lo = HexNibble(cleaned[i + 1]);
        if (hi < 0 || lo < 0) {
            throw std::invalid_argument("hex string contains non-hex characters");
        }
        out.push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    return out;
}


static uint64_t ParseAddress(const json& params, const std::string& key) {
    auto& v = params.at(key);
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        size_t begin = s.find_first_not_of(" \t\r\n");
        size_t endPos = s.find_last_not_of(" \t\r\n");
        if (begin == std::string::npos || s[begin] == '-') {
            throw std::invalid_argument("invalid address: " + key);
        }
        s = s.substr(begin, endPos - begin + 1);

        char* end = nullptr;
        errno = 0;
        const int base = (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10;
        uint64_t parsed = std::strtoull(s.c_str(), &end, base);
        if (end == s.c_str() || *end != '\0' || errno == ERANGE) {
            throw std::invalid_argument("invalid address: " + key);
        }
        return parsed;
    }
    if (v.is_number_unsigned()) {
        return v.get<uint64_t>();
    }
    if (v.is_number_integer()) {
        int64_t parsed = v.get<int64_t>();
        if (parsed < 0) {
            throw std::invalid_argument("invalid address: " + key);
        }
        return static_cast<uint64_t>(parsed);
    }
    return v.get<uint64_t>();
}

// ── 注册所有内置路由 ─────────────────────────────────────────────
void IpcServer::RegisterBuiltinMethods() {

    // ── get_status ───────────────────────────────────────────────
    RegisterMethod("get_status", [this](const json&) -> json {
        auto status = service_->status(service_->captureContext(false));
        if (!status.ok()) {
            return serviceFailure(status.error());
        }
        const auto& value = status.value();
        return {
            {"success", true},
            {"result", {
                {"connected", value.connection.connected},
                {"connection_poisoned", value.connection.poisoned},
                {"connection_generation", value.connection.generation},
                {"pid", value.target.pid},
                {"process_name", value.processName},
                {"handle", value.target.processHandle},
                {"process_revision", value.target.processRevision}
            }}
        };
    });

    // ── get_version ──────────────────────────────────────────────
    RegisterMethod("get_version", [this](const json&) -> json {
        auto info = service_->serverVersion(service_->captureContext(false));
        if (!info.ok())
            return serviceFailure(info.error());
        return {{"success", true}, {"result", {
            {"version", info.value().version},
            {"version_string", info.value().versionString}
        }}};
    });

    // ── get_architecture ─────────────────────────────────────────
    RegisterMethod("get_architecture", [this](const json&) -> json {
        auto type = service_->memoryType(service_->captureContext(false));
        if (!type.ok())
            return serviceFailure(type.error());
        return {{"success", true}, {"result", {
            {"type", type.value().type}, {"name", type.value().name}}}};
    });

    // ── init_driver ──────────────────────────────────────────────
    RegisterMethod("init_driver", [this](const json& p) -> json {
        std::string card = getStringParam(
            p, "card", "", true, false, kMaxIpcStringParamBytes);
        auto result = service_->initializeDriver(
            service_->captureContext(false),
            Mem::DriverInitializeRequest{card});
        if (!result.ok())
            return serviceFailure(result.error());
        return {{"success", true}, {"result", {
            {"message", result.value().message}}}};
    });

    // ── list_processes ───────────────────────────────────────────
    RegisterMethod("list_processes", [this](const json&) -> json {
        const Mem::OperationContext context = service_->captureContext(false);
        std::vector<Mem::ProcessInfo> list;
        size_t offset = 0;
        while (true) {
            Mem::ProcessListRequest request;
            request.offset = offset;
            request.limit = Mem::kMaxProcessPageSize;
            auto result = service_->listProcesses(context, request);
            if (!result.ok())
                return serviceFailure(result.error());
            auto& page = result.value();
            list.insert(list.end(), page.items.begin(), page.items.end());
            if (!page.nextOffset)
                break;
            offset = *page.nextOffset;
        }
        json arr = json::array();
        for (auto& p : list)
            arr.push_back({{"pid", p.pid}, {"name", p.name}});
        return {{"success", true}, {"result", arr}};
    });
    // ── open_process ──────────────────────────────────────────────
    RegisterMethod("open_process", [this](const json& p) -> json {
        int pid = getRequiredPositiveIntParam(p, "pid");
        auto result = service_->openProcess(
            service_->captureContext(true), Mem::OpenProcessRequest{pid, {}});
        if (!result.ok())
            return serviceFailure(result.error());
        return {{"success", true}, {"result", {
            {"handle", result.value().target.processHandle},
            {"pid", result.value().target.pid},
            {"process_name", result.value().name},
            {"process_revision", result.value().target.processRevision},
            {"connection_generation",
             result.value().target.connectionGeneration}
        }}};
    });

    // ── list_modules ─────────────────────────────────────────────
    RegisterMethod("list_modules", [this](const json& p) -> json {
        std::string filter = getStringParam(
            p, "filter", "", false, true, kMaxIpcStringParamBytes);
        int offset = getClampedIntParam(
            p, "offset", 0, 0, (std::numeric_limits<int>::max)());
        int count = getClampedIntParam(p, "count", 200, 1, 1000);
        Mem::ModuleListRequest request;
        request.filter = filter;
        request.offset = static_cast<size_t>(offset);
        request.limit = static_cast<size_t>(count);
        auto result = service_->listModules(
            service_->captureContext(true), request);
        if (!result.ok())
            return serviceFailure(result.error());

        json arr = json::array();
        for (const auto& module : result.value().items) {
            arr.push_back({
                {"base", formatAddress(module.base)}, {"size", module.size},
                {"type", module.type}, {"flag", module.flag},
                {"name", module.name}
            });
        }
        return {{"success", true}, {"result", {
            {"total", result.value().total},
            {"offset", result.value().offset}, {"modules", arr}
        }}};
    });

    // ── read_memory ──────────────────────────────────────────────
    RegisterMethod("read_memory", [this](const json& p) -> json {
        uint64_t addr = ParseAddress(p, "address");
        uint32_t size = getPositiveSizeParam(p, "size", 256u, kMaxIpcMemoryTransferBytes);
        auto result = service_->readMemory(
            service_->captureContext(true),
            Mem::MemoryReadRequest{addr, size});
        if (!result.ok())
            return serviceFailure(result.error());
        return {{"success", true}, {"result", {
            {"hex", BytesToHex(result.value().bytes)},
            {"size", result.value().bytes.size()}
        }}};
    });

    // ── write_memory ─────────────────────────────────────────────
    RegisterMethod("write_memory", [this](const json& p) -> json {
        uint64_t addr = ParseAddress(p, "address");
        std::string hexStr = getStringParam(
            p, "hex", "", true, false, IpcHttp::kMaxRequestBytes);
        auto data = HexToBytes(hexStr);
        if (data.empty()) {
            return {{"success", false}, {"error", "hex is empty"}};
        }
        if (data.size() > kMaxIpcMemoryTransferBytes) {
            return {{"success", false}, {"error", "write data exceeds IPC limit"}};
        }
        Mem::MemoryWriteRequest request;
        request.address = addr;
        request.bytes = std::move(data);
        auto result = service_->writeMemory(
            service_->captureContext(true), request);
        if (!result.ok())
            return serviceFailure(result.error());
        return {{"success", true}, {"result", {
            {"written", result.value().writtenBytes}
        }}};
    });

    // ── read_batch ───────────────────────────────────────────────
    RegisterMethod("read_batch", [this](const json& p) -> json {
        if (!p.contains("addresses")) {
            throw std::invalid_argument("addresses is required");
        }
        const auto& addrsArr = p.at("addresses");
        if (!addrsArr.is_array()) {
            return {{"success", false}, {"error", "addresses 必须是数组"}};
        }
        if (addrsArr.empty() || addrsArr.size() > kMaxIpcBatchReadCount) {
            return {{"success", false}, {"error", "addresses 数量超出限制"}};
        }
        Mem::MemoryBatchReadRequest request;
        request.items.reserve(addrsArr.size());
        uint64_t totalBytes = 0;
        for (auto& item : addrsArr) {
            uint64_t a = ParseAddress(item, "address");
            uint32_t s = getPositiveSizeParam(item, "size", 4u, kMaxIpcMemoryTransferBytes);
            if (totalBytes > kMaxIpcBatchReadTotalBytes - s) {
                return {{"success", false}, {"error", "read_batch total size exceeds IPC limit"}};
            }
            totalBytes += s;
            request.items.push_back(Mem::MemoryReadRequest{a, s});
        }
        auto result = service_->readMemoryBatch(
            service_->captureContext(true), request);
        if (!result.ok())
            return serviceFailure(result.error());
        json arr = json::array();
        for (const auto& block : result.value().items) {
            arr.push_back({{"address", formatAddress(block.address)},
                           {"hex", BytesToHex(block.bytes)}});
        }
        return {{"success", true}, {"result", arr}};
    });
    // ── set_breakpoint ────────────────────────────────────────────
    RegisterMethod("set_breakpoint", [this](const json& p) -> json {
        uint64_t addr = ParseAddress(p, "address");
        uint32_t bpType = getOptionalPositiveUintParam(p, "bp_type", 2u, 4u);
        uint32_t bpSize = getOptionalPositiveUintParam(p, "bp_size", 4u, 8u);
        if (bpType < 1 || bpType > 4) {
            return {{"success", false}, {"error", "bp_type 必须在 1 到 4 之间"}};
        }
        if (!isValidBreakpointSize(bpSize)) {
            return {{"success", false}, {"error", "bp_size 必须为 1、2、4 或 8"}};
        }
        if (bpType == 4) {
            bpSize = 4;
        }
        Mem::BreakpointSetRequest request;
        request.address = addr;
        request.access = static_cast<Mem::BreakpointAccess>(bpType);
        request.size = bpSize;
        auto result = service_->setBreakpoint(
            service_->captureContext(true), request);
        if (!result.ok())
            return serviceFailure(result.error());
        return {{"success", true}, {"result", nullptr}};
    });

    // ── remove_breakpoint ────────────────────────────────────────
    RegisterMethod("remove_breakpoint", [this](const json& p) -> json {
        uint64_t addr = ParseAddress(p, "address");
        auto result = service_->removeBreakpoint(
            service_->captureContext(true),
            Mem::BreakpointAddressRequest{addr});
        if (!result.ok())
            return serviceFailure(result.error());
        return {{"success", true}, {"result", nullptr}};
    });

    // ── suspend_breakpoint ───────────────────────────────────────
    RegisterMethod("suspend_breakpoint", [this](const json& p) -> json {
        uint64_t addr = ParseAddress(p, "address");
        auto result = service_->suspendBreakpoint(
            service_->captureContext(true),
            Mem::BreakpointAddressRequest{addr});
        if (!result.ok())
            return serviceFailure(result.error());
        return {{"success", true}, {"result", nullptr}};
    });

    // ── resume_breakpoint ────────────────────────────────────────
    RegisterMethod("resume_breakpoint", [this](const json& p) -> json {
        uint64_t addr = ParseAddress(p, "address");
        auto result = service_->resumeBreakpoint(
            service_->captureContext(true),
            Mem::BreakpointAddressRequest{addr});
        if (!result.ok())
            return serviceFailure(result.error());
        return {{"success", true}, {"result", nullptr}};
    });

    // ── read_bp_info ─────────────────────────────────────────────
    RegisterMethod("read_bp_info", [this](const json& p) -> json {
        uint64_t addr = ParseAddress(p, "address");
        auto result = service_->breakpointHits(
            service_->captureContext(true),
            Mem::BreakpointHitBatchRequest{
                addr, Mem::kMaxBreakpointHitCount});
        if (!result.ok())
            return serviceFailure(result.error());
        json arr = json::array();
        for (const auto& h : result.value().items) {
            json regs = json::array();
            for (int i = 0; i < 31; i++)
                regs.push_back(h.registers[static_cast<size_t>(i)]);
            arr.push_back({
                {"hit_addr", formatAddress(h.hitAddress)},
                {"hit_time", h.hitTime},
                {"pc", formatAddress(h.programCounter)},
                {"sp", formatAddress(h.stackPointer)},
                {"pstate", h.pstate},
                {"regs", regs}
            });
        }
        return {{"success", true}, {"result",
            {{"total_hits", result.value().available},
             {"returned", result.value().items.size()},
             {"dropped", result.value().dropped}, {"hits", arr}}}};
    });
    // ── execute_lua ───────────────────────────────────────────────
#ifdef HAVE_LUAJIT
    RegisterMethod("execute_lua", [this](const json& p) -> json {
        std::string code = getStringParam(
            p, "code", "", true, false, kMaxIpcLuaCodeBytes);
        int timeoutSeconds = getClampedIntParam(
            p, "timeout_seconds", kDefaultIpcLuaTimeoutSeconds,
            1, kMaxIpcLuaTimeoutSeconds);
        auto& engine = LuaEngine::GetInstance();
        if (!engine.IsInitialized()) {
            if (!engine.Initialize(*service_))
                return {{"success", false}, {"error", "Lua 引擎初始化失败: " + engine.GetLastError()}};
        }
        std::string output;
        SocketIoTimeout::ScopedTimeout luaTimeout(timeoutSeconds);
        bool ok = engine.ExecuteStringCapture(
            code,
            "ipc",
            output,
            static_cast<int>(SocketIoTimeout::GetRemainingTimeoutMs()));
        if (!ok)
            return {{"success", false}, {"error", engine.GetLastError()}, {"output", output}};
        return {{"success", true}, {"result", {{"output", output}}}};
    });
#endif

    // ── get_module_base ──────────────────────────────────────────
    RegisterMethod("get_module_base", [this](const json& p) -> json {
        std::string name = getStringParam(
            p, "name", "", true, false, kMaxIpcStringParamBytes);
        auto result = service_->resolveModule(
            service_->captureContext(true), Mem::ModuleResolveRequest{name});
        if (!result.ok())
            return serviceFailure(result.error());
        return {{"success", true}, {"result", {
            {"base", formatAddress(result.value().module.base)}
        }}};
    });

    // ── resolve_offset_chain ─────────────────────────────────────
    RegisterMethod("resolve_offset_chain", [this](const json& p) -> json {
        std::string moduleName = getStringParam(
            p, "module", "", true, false, kMaxIpcStringParamBytes);
        uint64_t baseOffset = ParseAddress(p, "base_offset");
        std::vector<uint64_t> offsets;
        if (p.contains("offsets")) {
            const auto& offsetValues = p["offsets"];
            if (!offsetValues.is_array()) {
                throw std::invalid_argument("offsets must be an array");
            }
            if (offsetValues.size() > kMaxIpcOffsetChainLength) {
                throw std::invalid_argument("offsets is too long");
            }
            for (const auto& o : offsetValues) {
                offsets.push_back(ParseAddress(json{{"offset", o}}, "offset"));
            }
        }
        bool derefFinal = getOptionalBoolParam(p, "deref_final", true);
        Mem::PointerResolveRequest request;
        request.moduleName = moduleName;
        request.baseOffset = baseOffset;
        request.offsets = std::move(offsets);
        request.dereferenceFinal = derefFinal;
        auto result = service_->resolvePointer(
            service_->captureContext(true), request);
        if (!result.ok())
            return serviceFailure(result.error());
        return {{"success", true}, {"result", {
            {"address", formatAddress(result.value().address)},
            {"module_base", formatAddress(result.value().module.base)},
            {"dereference_count", result.value().dereferenceCount}
        }}};
    });

    // ── symbol_init ────────────────────────────────────────────────
    RegisterMethod("symbol_init", [this](const json& p) -> json {
        uint64_t moduleBase = ParseAddress(p, "module_base");
        auto result = service_->loadSymbolTable(
            service_->captureContext(true),
            Mem::SymbolTableRequest{moduleBase});
        if (!result.ok())
            return serviceFailure(result.error());
        const size_t totalCount = result.value().items.size();
        {
            std::lock_guard<std::mutex> lock(symbolCacheMutex_);
            legacySymbolTable_ = std::move(result.value());
        }
        return {{"success", true}, {"result", {
            {"total_count", totalCount}
        }}};
    });

    // ── symbol_list ────────────────────────────────────────────────
    RegisterMethod("symbol_list", [this](const json& p) -> json {
        int offset = getClampedIntParam(
            p, "offset", 0, 0, (std::numeric_limits<int>::max)());
        int count = getClampedIntParam(p, "count", 100, 1, 1000);

        std::vector<Mem::SymbolInfo> symbols;
        size_t totalCount = 0;
        size_t pageOffset = static_cast<size_t>(offset);
        bool hasModuleBase = false;
        uint64_t moduleBase = 0;
        if (p.contains("module_base") && !p.at("module_base").is_null()) {
            if (!p.at("module_base").is_string() ||
                !p.at("module_base").get<std::string>().empty()) {
                moduleBase = ParseAddress(p, "module_base");
                hasModuleBase = true;
            }
        }

        if (hasModuleBase) {
            Mem::SymbolListRequest request;
            request.moduleBase = moduleBase;
            request.offset = pageOffset;
            request.limit = static_cast<size_t>(count);
            auto result = service_->listSymbols(
                service_->captureContext(true), request);
            if (!result.ok())
                return serviceFailure(result.error());
            symbols = std::move(result.value().items);
            totalCount = result.value().total;
            pageOffset = result.value().offset;
        } else {
            const Mem::OperationContext current =
                service_->captureContext(true);
            std::lock_guard<std::mutex> lock(symbolCacheMutex_);
            if (!legacySymbolTable_ || !current.target ||
                legacySymbolTable_->target != *current.target) {
                return {{"success", false},
                        {"error", "symbol table is not initialized for the current target"},
                        {"error_code", "symbol_session_changed"},
                        {"retryable", false}};
            }
            totalCount = legacySymbolTable_->items.size();
            pageOffset = std::min(pageOffset, totalCount);
            const size_t end = std::min(
                totalCount, pageOffset + static_cast<size_t>(count));
            symbols.insert(symbols.end(),
                           legacySymbolTable_->items.begin() + pageOffset,
                           legacySymbolTable_->items.begin() + end);
        }

        json arr = json::array();
        for (const auto& symbol : symbols) {
            arr.push_back({
                {"address", formatAddress(symbol.address)},
                {"name", symbol.name}
            });
        }
        return {{"success", true}, {"result", {
            {"total", totalCount},
            {"offset", pageOffset},
            {"symbols", arr}
        }}};
    });

    // ── symbol_find ────────────────────────────────────────────────
    RegisterMethod("symbol_find", [this](const json& p) -> json {
        uint64_t moduleBase = ParseAddress(p, "module_base");
        std::string name = getStringParam(
            p, "name", "", true, false, kMaxIpcStringParamBytes);
        auto result = service_->resolveSymbol(
            service_->captureContext(true),
            Mem::SymbolResolveRequest{moduleBase, name});
        if (!result.ok())
            return serviceFailure(result.error());
        return {{"success", true}, {"result", {
            {"address", formatAddress(result.value().address)}
        }}};
    });

} // RegisterBuiltinMethods
