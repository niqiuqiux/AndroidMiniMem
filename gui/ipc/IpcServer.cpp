#include "IpcServer.h"
#include "../socket/client_singleton.h"
#include "../socket/socket_io_timeout.h"
#include "../gui/AppContext.h"
#include "../gui/Gui.h"
#include "../gui/MemoryTypes.h"

#ifdef HAVE_LUAJIT
#include "../lua/LuaEngine.h"
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <stdexcept>

#pragma comment(lib, "ws2_32.lib")

namespace {
constexpr size_t kMaxHttpRequestBytes = 1024 * 1024;
constexpr uint32_t kMaxIpcMemoryTransferBytes = 64 * 1024;
constexpr size_t kMaxIpcBatchReadCount = 100000;
constexpr uint64_t kMaxIpcBatchReadTotalBytes = 256ull * 1024ull * 1024ull;
constexpr int kDefaultIpcLuaTimeoutSeconds = 30;
constexpr int kMaxIpcLuaTimeoutSeconds = 300;
constexpr size_t kMaxIpcStringParamBytes = 4096;
constexpr size_t kMaxIpcLuaCodeBytes = 256 * 1024;
constexpr size_t kMaxIpcScanHexBytes = 4096;
constexpr size_t kMaxIpcOffsetChainLength = 1024;
constexpr int kKnownMemoryTypeMask =
    Anonymous | C_Alloc | C_Heap | C_Data | C_Bss | Java_Heap |
    Java | Stack | Video | Code_App | Code_System | Ashmem | Bad;

bool isBlankString(const std::string& value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
}

bool equalsIgnoreCase(const std::string& text, size_t begin, size_t end, const char* expected) {
    size_t expectedLen = 0;
    while (expected[expectedLen] != '\0') {
        ++expectedLen;
    }
    if (end < begin || end - begin != expectedLen) {
        return false;
    }

    for (size_t i = 0; i < expectedLen; ++i) {
        if (std::tolower(static_cast<unsigned char>(text[begin + i])) !=
            std::tolower(static_cast<unsigned char>(expected[i]))) {
            return false;
        }
    }
    return true;
}

struct HttpRequestLine {
    std::string method;
    std::string target;
};

bool parseRequestLine(const std::string& headers, HttpRequestLine& out) {
    const size_t lineEnd = headers.find("\r\n");
    const std::string line =
        headers.substr(0, lineEnd == std::string::npos ? headers.size() : lineEnd);
    const size_t methodEnd = line.find(' ');
    if (methodEnd == std::string::npos || methodEnd == 0) {
        return false;
    }

    const size_t targetEnd = line.find(' ', methodEnd + 1);
    if (targetEnd == std::string::npos || targetEnd == methodEnd + 1) {
        return false;
    }

    const std::string version = line.substr(targetEnd + 1);
    if (version.rfind("HTTP/", 0) != 0) {
        return false;
    }

    out.method = line.substr(0, methodEnd);
    out.target = line.substr(methodEnd + 1, targetEnd - methodEnd - 1);
    return true;
}

std::string pathWithoutQuery(const std::string& target) {
    const size_t query = target.find('?');
    return query == std::string::npos ? target : target.substr(0, query);
}

bool parseContentLengthHeader(const std::string& headers,
                              size_t& outLength,
                              bool& outFound) {
    outLength = 0;
    outFound = false;
    size_t lineStart = 0;
    while (lineStart < headers.size()) {
        size_t lineEnd = headers.find("\r\n", lineStart);
        if (lineEnd == std::string::npos) {
            lineEnd = headers.size();
        }

        const size_t colon = headers.find(':', lineStart);
        if (colon != std::string::npos && colon < lineEnd) {
            size_t nameBegin = lineStart;
            size_t nameEnd = colon;
            while (nameBegin < nameEnd &&
                   std::isspace(static_cast<unsigned char>(headers[nameBegin]))) {
                ++nameBegin;
            }
            while (nameEnd > nameBegin &&
                   std::isspace(static_cast<unsigned char>(headers[nameEnd - 1]))) {
                --nameEnd;
            }

            if (equalsIgnoreCase(headers, nameBegin, nameEnd, "content-length")) {
                size_t valueBegin = colon + 1;
                size_t valueEnd = lineEnd;
                while (valueBegin < valueEnd &&
                       std::isspace(static_cast<unsigned char>(headers[valueBegin]))) {
                    ++valueBegin;
                }
                while (valueEnd > valueBegin &&
                       std::isspace(static_cast<unsigned char>(headers[valueEnd - 1]))) {
                    --valueEnd;
                }
                if (valueBegin == valueEnd) {
                    return false;
                }

                size_t parsed = 0;
                for (size_t i = valueBegin; i < valueEnd; ++i) {
                    const unsigned char ch = static_cast<unsigned char>(headers[i]);
                    if (!std::isdigit(ch)) {
                        return false;
                    }
                    const size_t digit = static_cast<size_t>(ch - '0');
                    if (parsed > ((std::numeric_limits<size_t>::max)() - digit) / 10) {
                        return false;
                    }
                    parsed = parsed * 10 + digit;
                }
                outLength = parsed;
                outFound = true;
                return true;
            }
        }

        if (lineEnd == headers.size()) {
            break;
        }
        lineStart = lineEnd + 2;
    }
    return true;
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

uint32_t getRequiredUint32Param(const json& params, const char* key) {
    if (!params.contains(key)) {
        throw std::invalid_argument(std::string(key) + " is required");
    }

    uint64_t value = 0;
    const auto& raw = params.at(key);
    if (raw.is_number_unsigned()) {
        value = raw.get<uint64_t>();
    } else if (raw.is_number_integer()) {
        const int64_t signedValue = raw.get<int64_t>();
        if (signedValue < 0) {
            throw std::invalid_argument(std::string(key) + " must be non-negative");
        }
        value = static_cast<uint64_t>(signedValue);
    } else {
        throw std::invalid_argument(std::string(key) + " must be an integer");
    }

    if (value > static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())) {
        throw std::invalid_argument(std::string(key) + " out of range");
    }
    return static_cast<uint32_t>(value);
}

uint32_t getRequiredScanFlagsParam(const json& params) {
    uint32_t flags = getRequiredUint32Param(params, "flags");
    if (params.contains("scan_flag")) {
        uint32_t scanFlag = getRequiredUint32Param(params, "scan_flag");
        if (scanFlag != flags) {
            throw std::invalid_argument(
                "flags and scan_flag must match when both are provided");
        }
    }
    return flags;
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

bool isValidMemoryTypeFlags(int type) {
    if (type == All || type == Other) {
        return true;
    }
    return type > 0 && (type & ~kKnownMemoryTypeMask) == 0;
}
} // namespace

// ── 单例 ─────────────────────────────────────────────────────────
IpcServer& IpcServer::GetInstance() {
    static IpcServer instance;
    return instance;
}

// ── 启动/停止 ────────────────────────────────────────────────────
bool IpcServer::Start(uint16_t port) {
    if (running_.load()) return true;

    port_ = port;
    RegisterBuiltinMethods();

    // 确保 Winsock 已初始化（可能在 WindowsSocketClient 之前启动）
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    listenSocket_ = (uintptr_t)::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket_ == (uintptr_t)INVALID_SOCKET) {
        Gui::log("[IPC] 创建 socket 失败");
        return false;
    }

    // 允许端口复用
    int opt = 1;
    ::setsockopt((SOCKET)listenSocket_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 仅本地

    if (::bind((SOCKET)listenSocket_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        Gui::log("[IPC] 绑定端口 %d 失败", (int)port_);
        ::closesocket((SOCKET)listenSocket_);
        listenSocket_ = (uintptr_t)INVALID_SOCKET;
        return false;
    }

    if (::listen((SOCKET)listenSocket_, SOMAXCONN) == SOCKET_ERROR) {
        Gui::log("[IPC] listen 失败");
        ::closesocket((SOCKET)listenSocket_);
        listenSocket_ = (uintptr_t)INVALID_SOCKET;
        return false;
    }

    running_.store(true);
    serverThread_ = std::thread(&IpcServer::ServerThread, this);
    Gui::log("[IPC] 服务已启动，监听端口 %d", (int)port_);
    return true;
}
void IpcServer::Stop() {
    if (!running_.load()) return;
    running_.store(false);
    Gui::log("[IPC] 服务正在停止...");

    // 关闭监听 socket 以唤醒 accept()
    if (listenSocket_ != (uintptr_t)INVALID_SOCKET) {
        ::closesocket((SOCKET)listenSocket_);
        listenSocket_ = (uintptr_t)INVALID_SOCKET;
    }

    if (serverThread_.joinable())
        serverThread_.join();
}

void IpcServer::ServerThread() {
    while (running_.load()) {
        SOCKET client = ::accept((SOCKET)listenSocket_, nullptr, nullptr);
        if (client == INVALID_SOCKET) break;

        // 每个请求在独立线程处理（短连接）
        std::thread([this, client]() {
            HandleClient((uintptr_t)client);
        }).detach();
    }
}

// ── HTTP 处理 ────────────────────────────────────────────────────
void IpcServer::HandleClient(uintptr_t clientSocket) {
    SOCKET sock = (SOCKET)clientSocket;

    // 设置超时
    DWORD timeout = 5000;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    // 读取完整 HTTP 请求（最大 1MB）
    std::string raw;
    raw.reserve(4096);
    char buf[4096];
    size_t contentLength = 0;
    bool contentLengthKnown = false;
    bool badRequest = false;
    bool requestTooLarge = false;
    int earlyStatusCode = 0;
    std::string earlyError;
    HttpRequestLine requestLine;
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
                if (!parseRequestLine(headers, requestLine)) {
                    badRequest = true;
                    earlyError = "Invalid HTTP request line";
                    break;
                }

                const std::string path = pathWithoutQuery(requestLine.target);
                if (path != "/") {
                    earlyStatusCode = 404;
                    earlyError = "Unknown IPC endpoint";
                    break;
                }

                if (requestLine.method != "POST" &&
                    requestLine.method != "OPTIONS") {
                    earlyStatusCode = 405;
                    earlyError = "Unsupported HTTP method";
                    break;
                }

                if (!parseContentLengthHeader(headers,
                                              contentLength,
                                              contentLengthKnown)) {
                    badRequest = true;
                    earlyError = "Invalid Content-Length";
                    break;
                }

                if (requestLine.method == "POST" && !contentLengthKnown) {
                    badRequest = true;
                    earlyError = "Missing Content-Length";
                    break;
                }

                if (contentLength > kMaxHttpRequestBytes ||
                    headerEnd + 4 > kMaxHttpRequestBytes - contentLength) {
                    requestTooLarge = true;
                    break;
                }
            }
        }

        if (headerEnd != std::string::npos && contentLengthKnown) {
            size_t bodyStart = headerEnd + 4;
            if (raw.size() >= bodyStart && raw.size() - bodyStart >= contentLength) break;
        }

        if (headerEnd != std::string::npos &&
            requestLine.method == "OPTIONS" &&
            !contentLengthKnown) {
            break;
        }

        if (raw.size() > kMaxHttpRequestBytes) {
            requestTooLarge = true;
            break;
        }
    }

    if (earlyStatusCode != 0) {
        json response = {{"success", false}, {"error", earlyError}};
        std::string httpResp = BuildHttpResponse(earlyStatusCode, response.dump());
        ::send(sock, httpResp.c_str(), (int)httpResp.size(), 0);
        ::closesocket(sock);
        return;
    }

    if (badRequest || headerEnd == std::string::npos) {
        json response = {{"success", false},
                         {"error", earlyError.empty() ? "Invalid HTTP request"
                                                       : earlyError}};
        std::string httpResp = BuildHttpResponse(400, response.dump());
        ::send(sock, httpResp.c_str(), (int)httpResp.size(), 0);
        ::closesocket(sock);
        return;
    }

    if (requestTooLarge) {
        json response = {{"success", false}, {"error", "HTTP request too large"}};
        std::string httpResp = BuildHttpResponse(413, response.dump());
        ::send(sock, httpResp.c_str(), (int)httpResp.size(), 0);
        ::closesocket(sock);
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
        ::closesocket(sock);
        return;
    }

    std::string body;
    if (contentLengthKnown && raw.size() >= bodyStart)
        body = raw.substr(bodyStart, contentLength);

    // 处理 CORS preflight
    if (requestLine.method == "OPTIONS") {
        std::string resp = "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Content-Length: 0\r\n\r\n";
        ::send(sock, resp.c_str(), (int)resp.size(), 0);
        ::closesocket(sock);
        return;
    }

    // 解析 JSON 并分发
    json response;
    int statusCode = 200;
    try {
        json request = json::parse(body);
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
    ::closesocket(sock);
}

std::string IpcServer::BuildHttpResponse(int statusCode, const std::string& body) {
    const char* reason = "OK";
    switch (statusCode) {
    case 200: reason = "OK"; break;
    case 204: reason = "No Content"; break;
    case 400: reason = "Bad Request"; break;
    case 404: reason = "Not Found"; break;
    case 405: reason = "Method Not Allowed"; break;
    case 413: reason = "Payload Too Large"; break;
    case 500: reason = "Internal Server Error"; break;
    default:  reason = "Unknown"; break;
    }

    std::ostringstream oss;
    oss << "HTTP/1.1 " << statusCode << " " << reason << "\r\n"
        << "Content-Type: application/json; charset=utf-8\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
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


// 断点失败时：若当前不在内核读写模式，给出可操作的明确提示（硬件断点依赖内核驱动）
static json breakpointFailure(const char* genericMsg) {
    int memType = 0;
    if (GetMemType(memType) && memType != MemType_Kernel) {
        return {{"success", false},
                {"error", std::string("断点功能需要内核读写模式(当前模式 ") +
                          std::to_string(memType) +
                          ")，请先调用 init_driver 切换到内核模式"}};
    }
    return {{"success", false}, {"error", genericMsg}};
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
    RegisterMethod("get_status", [](const json&) -> json {
        auto& ctx = AppContext::Get();
        bool connected = IsMultiPortConnected();
        return {
            {"success", true},
            {"result", {
                {"connected", connected},
                {"pid", ctx.selectedPid.load()},
                {"process_name", ctx.getSelectedName()},
                {"handle", ctx.processHandle.load()}
            }}
        };
    });

    // ── get_version ──────────────────────────────────────────────
    RegisterMethod("get_version", [](const json&) -> json {
        ServerVersionInfo info;
        if (!FetchServerVersion(info))
            return {{"success", false}, {"error", "获取版本失败"}};
        return {{"success", true}, {"result", {
            {"version", info.version},
            {"version_string", info.versionString}
        }}};
    });

    // ── get_architecture ─────────────────────────────────────────
    RegisterMethod("get_architecture", [](const json&) -> json {
        int type = 0;
        if (!GetMemType(type))
            return {{"success", false}, {"error", "获取架构失败"}};
        const char* names[] = {"Null", "IO", "Syscall", "Kernel", "SysHook"};
        std::string name = (type >= 0 && type <= 4) ? names[type] : "Unknown";
        return {{"success", true}, {"result", {{"type", type}, {"name", name}}}};
    });

    // ── init_driver ──────────────────────────────────────────────
    RegisterMethod("init_driver", [](const json& p) -> json {
        std::string card = getStringParam(
            p, "card", "", true, false, kMaxIpcStringParamBytes);
        std::string resStr;
        if (!InitDriver(card, resStr))
            return {{"success", false}, {"error", "初始化驱动失败: " + resStr}};
        return {{"success", true}, {"result", {{"message", resStr}}}};
    });

    // ── list_processes ───────────────────────────────────────────
    RegisterMethod("list_processes", [](const json&) -> json {
        std::vector<ProcessInfoItem> list;
        if (!FetchProcessList(list))
            return {{"success", false}, {"error", "获取进程列表失败"}};
        json arr = json::array();
        for (auto& p : list)
            arr.push_back({{"pid", p.pid}, {"name", p.name}});
        return {{"success", true}, {"result", arr}};
    });
    // ── open_process ──────────────────────────────────────────────
    RegisterMethod("open_process", [](const json& p) -> json {
        int pid = getRequiredPositiveIntParam(p, "pid");
        AppContext::Get().selectProcess(pid, "");
        int handle = AppContext::Get().processHandle.load(std::memory_order_relaxed);
        if (handle == 0)
            return {{"success", false}, {"error", "打开进程失败"}};
        return {{"success", true}, {"result", {{"handle", handle}}}};
    });

    // ── list_modules ─────────────────────────────────────────────
    RegisterMethod("list_modules", [](const json& p) -> json {
        std::vector<ModuleInfoItem> list;
        if (!FetchModuleList(list))
            return {{"success", false}, {"error", "获取模块列表失败"}};

        // 可选：名称过滤（大小写不敏感子串匹配）
        std::string filter = getStringParam(
            p, "filter", "", false, true, kMaxIpcStringParamBytes);
        std::vector<ModuleInfoItem*> filtered;
        if (!filter.empty()) {
            std::string lowerFilter = filter;
            std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(),
                [](unsigned char c) { return (char)std::tolower(c); });
            for (auto& m : list) {
                std::string lowerName = m.name;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                    [](unsigned char c) { return (char)std::tolower(c); });
                if (lowerName.find(lowerFilter) != std::string::npos)
                    filtered.push_back(&m);
            }
        } else {
            for (auto& m : list) filtered.push_back(&m);
        }

        int total = (int)filtered.size();
        int offset = getClampedIntParam(p, "offset", 0, 0, total);
        int count = getClampedIntParam(p, "count", 200, 1, 1000);
        int end = (std::min)(offset + count, total);

        json arr = json::array();
        for (int i = offset; i < end; i++) {
            auto* m = filtered[i];
            std::ostringstream baseStr;
            baseStr << "0x" << std::hex << m->base;
            arr.push_back({
                {"base", baseStr.str()}, {"size", m->size},
                {"type", m->type}, {"flag", m->flag}, {"name", m->name}
            });
        }
        return {{"success", true}, {"result", {{"total", total}, {"offset", offset}, {"modules", arr}}}};
    });

    // ── read_memory ──────────────────────────────────────────────
    RegisterMethod("read_memory", [](const json& p) -> json {
        uint64_t addr = ParseAddress(p, "address");
        uint32_t size = getPositiveSizeParam(p, "size", 256u, kMaxIpcMemoryTransferBytes);
        std::vector<unsigned char> data;
        if (!ReadProcessMemoryBytes(addr, size, data))
            return {{"success", false}, {"error", "读取内存失败"}};
        return {{"success", true}, {"result", {
            {"hex", BytesToHex(data)}, {"size", (int)data.size()}
        }}};
    });

    // ── write_memory ─────────────────────────────────────────────
    RegisterMethod("write_memory", [](const json& p) -> json {
        uint64_t addr = ParseAddress(p, "address");
        std::string hexStr = getStringParam(
            p, "hex", "", true, false, kMaxHttpRequestBytes);
        auto data = HexToBytes(hexStr);
        if (data.empty()) {
            return {{"success", false}, {"error", "hex is empty"}};
        }
        if (data.size() > kMaxIpcMemoryTransferBytes) {
            return {{"success", false}, {"error", "write data exceeds IPC limit"}};
        }
        uint32_t size = (uint32_t)data.size();
        int32_t written = 0;
        if (!WriteProcessMemoryBytes(addr, size, data, PORT_MAIN, &written)) {
            // 三态：完全失败 vs 部分写入（已产生副作用，不可当作未写入）
            if (written > 0) {
                std::ostringstream oss;
                oss << "部分写入：仅连续写入 " << written << "/" << size
                    << " 字节（已修改目标内存，剩余部分因不可写中断）";
                return {{"success", false}, {"error", oss.str()},
                        {"result", {{"written", written}}}};
            }
            return {{"success", false}, {"error", "写入内存失败"},
                    {"result", {{"written", 0}}}};
        }
        return {{"success", true}, {"result", {{"written", (int)size}}}};
    });

    // ── read_batch ───────────────────────────────────────────────
    RegisterMethod("read_batch", [](const json& p) -> json {
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
        std::vector<std::pair<uint64_t, int32_t>> addrs;
        addrs.reserve(addrsArr.size());
        uint64_t totalBytes = 0;
        for (auto& item : addrsArr) {
            uint64_t a = ParseAddress(item, "address");
            uint32_t s = getPositiveSizeParam(item, "size", 4u, kMaxIpcMemoryTransferBytes);
            if (totalBytes > kMaxIpcBatchReadTotalBytes - s) {
                return {{"success", false}, {"error", "read_batch total size exceeds IPC limit"}};
            }
            totalBytes += s;
            addrs.push_back({a, s});
        }
        std::vector<std::pair<uint64_t, std::vector<uint8_t>>> out;
        if (!ReadBratchAddr(addrs, out))
            return {{"success", false}, {"error", "批量读取失败"}};
        json arr = json::array();
        for (auto& [a, d] : out) {
            std::ostringstream addrStr;
            addrStr << "0x" << std::hex << a;
            std::vector<unsigned char> uc(d.begin(), d.end());
            arr.push_back({{"address", addrStr.str()}, {"hex", BytesToHex(uc)}});
        }
        return {{"success", true}, {"result", arr}};
    });
    // ── set_breakpoint ────────────────────────────────────────────
    RegisterMethod("set_breakpoint", [](const json& p) -> json {
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
        if (!SetKernelBreakpoint(addr, bpType, bpSize))
            return breakpointFailure("设置断点失败");
        return {{"success", true}, {"result", nullptr}};
    });

    // ── remove_breakpoint ────────────────────────────────────────
    RegisterMethod("remove_breakpoint", [](const json& p) -> json {
        uint64_t addr = ParseAddress(p, "address");
        if (!RemoveKernelBreakpoint(addr))
            return breakpointFailure("移除断点失败");
        return {{"success", true}, {"result", nullptr}};
    });

    // ── suspend_breakpoint ───────────────────────────────────────
    RegisterMethod("suspend_breakpoint", [](const json& p) -> json {
        uint64_t addr = ParseAddress(p, "address");
        if (!SuspendKernelBreakpoint(addr))
            return breakpointFailure("暂停断点失败");
        return {{"success", true}, {"result", nullptr}};
    });

    // ── resume_breakpoint ────────────────────────────────────────
    RegisterMethod("resume_breakpoint", [](const json& p) -> json {
        uint64_t addr = ParseAddress(p, "address");
        if (!ResumeKernelBreakpoint(addr))
            return breakpointFailure("恢复断点失败");
        return {{"success", true}, {"result", nullptr}};
    });

    // ── read_bp_info ─────────────────────────────────────────────
    RegisterMethod("read_bp_info", [](const json& p) -> json {
        uint64_t addr = ParseAddress(p, "address");
        std::vector<HW_HIT_INFO> infos;
        uint64_t totalHits = 0;
        if (!ReadKernelBreakpointInfo(addr, infos, PORT_MAIN, &totalHits))
            return breakpointFailure("读取断点信息失败");
        json arr = json::array();
        for (auto& h : infos) {
            json regs = json::array();
            for (int i = 0; i < 31; i++)
                regs.push_back(h.regs_info.regs[i]);
            std::ostringstream hitStr, pcStr, spStr;
            hitStr << "0x" << std::hex << h.hit_addr;
            pcStr << "0x" << std::hex << h.regs_info.pc;
            spStr << "0x" << std::hex << h.regs_info.sp;
            arr.push_back({
                {"hit_addr", hitStr.str()},
                {"hit_time", h.hit_time},
                {"pc", pcStr.str()},
                {"sp", spStr.str()},
                {"pstate", h.regs_info.pstate},
                {"regs", regs}
            });
        }
        return {{"success", true}, {"result",
            {{"total_hits", totalHits}, {"returned", (int)infos.size()}, {"hits", arr}}}};
    });
    // ── execute_lua ───────────────────────────────────────────────
#ifdef HAVE_LUAJIT
    RegisterMethod("execute_lua", [](const json& p) -> json {
        std::string code = getStringParam(
            p, "code", "", true, false, kMaxIpcLuaCodeBytes);
        int timeoutSeconds = getClampedIntParam(
            p, "timeout_seconds", kDefaultIpcLuaTimeoutSeconds,
            1, kMaxIpcLuaTimeoutSeconds);
        auto& engine = LuaEngine::GetInstance();
        if (!engine.IsInitialized()) {
            if (!engine.Initialize())
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
    RegisterMethod("get_module_base", [](const json& p) -> json {
        std::string name = getStringParam(
            p, "name", "", true, false, kMaxIpcStringParamBytes);
        uint64_t base = 0;
        if (!GetModuleBaseByName(name, base))
            return {{"success", false}, {"error", "获取模块基址失败"}};
        std::ostringstream oss;
        oss << "0x" << std::hex << base;
        return {{"success", true}, {"result", {{"base", oss.str()}}}};
    });

    // ── resolve_offset_chain ─────────────────────────────────────
    RegisterMethod("resolve_offset_chain", [](const json& p) -> json {
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
        uint64_t result = 0;
        if (!ResolveModuleOffsetChain(result, moduleName, baseOffset, offsets, derefFinal))
            return {{"success", false}, {"error", "解析偏移链失败"}};
        std::ostringstream oss;
        oss << "0x" << std::hex << result;
        return {{"success", true}, {"result", {{"address", oss.str()}}}};
    });

    // ── symbol_init ────────────────────────────────────────────────
    RegisterMethod("symbol_init", [](const json& p) -> json {
        uint64_t moduleBase = ParseAddress(p, "module_base");
        int totalCount = 0;
        if (!SymbolInit(moduleBase, totalCount))
            return {{"success", false}, {"error", "初始化符号表失败"}};
        return {{"success", true}, {"result", {{"total_count", totalCount}}}};
    });

    // ── symbol_list ────────────────────────────────────────────────
    RegisterMethod("symbol_list", [](const json& p) -> json {
        int offset = getClampedIntParam(
            p, "offset", 0, 0, (std::numeric_limits<int>::max)());
        int count = getClampedIntParam(p, "count", 100, 1, 1000);

        int totalCount = 0;
        std::vector<std::pair<uint64_t, std::string>> symbols;
        if (!SymbolGetList(offset, count, symbols, &totalCount))
            return {{"success", false}, {"error", "获取符号列表失败，请先初始化符号表"}};

        json arr = json::array();
        for (const auto& [address, name] : symbols) {
            std::ostringstream addrStr;
            addrStr << "0x" << std::hex << address;
            arr.push_back({
                {"address", addrStr.str()},
                {"name", name}
            });
        }
        return {{"success", true}, {"result", {
            {"total", totalCount},
            {"offset", offset},
            {"symbols", arr}
        }}};
    });

    // ── symbol_find ────────────────────────────────────────────────
    RegisterMethod("symbol_find", [](const json& p) -> json {
        uint64_t moduleBase = ParseAddress(p, "module_base");
        std::string name = getStringParam(
            p, "name", "", true, false, kMaxIpcStringParamBytes);
        uint64_t address = 0;
        if (!SymbolFind(moduleBase, name, address) || address == 0)
            return {{"success", false}, {"error", "查找符号失败"}};

        std::ostringstream oss;
        oss << "0x" << std::hex << address;
        return {{"success", true}, {"result", {{"address", oss.str()}}}};
    });

} // RegisterBuiltinMethods
