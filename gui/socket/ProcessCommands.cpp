#include "client_singleton.h"
#include "SocketCommand.h"
#include <ctime>
#include <cerrno>
#include <cstdlib>

namespace {
constexpr int kMaxProcessCount = 65536;
constexpr int kMaxProcessNameSize = 64 * 1024;
constexpr size_t kMaxProcessNameBytesTotal = 64u * 1024u * 1024u;
constexpr int kMaxModuleCount = 65536;
constexpr int kMaxModuleNameSize = 64 * 1024;
constexpr size_t kMaxModuleNameBytesTotal = 64u * 1024u * 1024u;
constexpr int kMaxDriverCardSize = 4096;
constexpr int kMaxDriverResponseSize = 64 * 1024;
constexpr size_t kMaxSoNameSize = 4096;

bool isValidCount(int value, int maxValue) {
    return value >= 0 && value <= maxValue;
}

uint64_t decodeModuleSize(uint32_t wireValue) {
    // CeModuleListEntry.modulesize 是历史协议字段，在线布局为 4 字节。
    // 必须按无符号值解码，不能把 2 GiB 以上映射的最高位当作符号位。
    return static_cast<uint64_t>(wireValue);
}

bool isValidModuleSize(uint64_t value) {
    return value != 0;
}

bool parseTimestampMs(const std::string& text, uint64_t& value) {
    const char* str = text.c_str();
    char* end = nullptr;
    errno = 0;
    value = std::strtoull(str, &end, 10);
    if (end == str || errno == ERANGE) {
        return false;
    }
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
        ++end;
    }
    return *end == '\0';
}

bool addAddressOffset(uint64_t base, uint64_t offset, uint64_t& out) {
    if (base > UINT64_MAX - offset)
        return false;
    out = base + offset;
    return true;
}

bool isSharedObjectName(const std::string& moduleName) {
    return !moduleName.empty() &&
           moduleName.size() <= kMaxSoNameSize &&
           moduleName.find(".so") != std::string::npos;
}

std::string baseNameOf(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool moduleNameExactMatch(const ModuleInfoItem& module, const std::string& moduleName) {
    return module.name == moduleName || baseNameOf(module.name) == moduleName;
}

bool moduleFlagMatch(const ModuleInfoItem& module, int requiredFlag) {
    return requiredFlag < 0 || module.flag == requiredFlag;
}
} // namespace

bool GetMemType(int &outType, PortType type) {
    return SocketCommand::executeNoHandle(type, [&](WindowsSocketClient* client) -> bool {
        unsigned char command = CMD_GETMEMTYPE;
        if (!client->Send(&command, sizeof(command)))
            return false;
        unsigned char t = 0;
        if (!client->Receive(&t, sizeof(t)))
            return false;
        if (t > MemType_SysHook)
            return SocketCommand::rejectMalformedResponse(client);
        outType = t;
        return true;
    });
}

DriverInitializationIoResult InitDriverTracked(
    const std::string& card, PortType type) {
    DriverInitializationIoResult result;
    if (card.empty() ||
        card.size() > static_cast<size_t>(kMaxDriverCardSize)) {
        result.message = "invalid driver card length";
        return result;
    }

    int serverResult = 0;
    int responseLength = 0;
    std::vector<char> response;
    (void)SocketCommand::executeNoHandle(
        type, [&](WindowsSocketClient* client) -> bool {
            result.requestStarted = true;
            unsigned char command = CMD_INITRWDRIVER;
            if (!client->Send(&command, sizeof(command)))
                return false;
            const int cardLength = static_cast<int>(card.size());
            if (!client->Send(&cardLength, sizeof(cardLength)))
                return false;
            if (!client->Send(card.data(), static_cast<size_t>(cardLength)))
                return false;
            if (!client->Receive(&serverResult, sizeof(serverResult)))
                return false;
            if (!client->Receive(&responseLength, sizeof(responseLength)))
                return false;
            if (!isValidCount(responseLength, kMaxDriverResponseSize))
                return SocketCommand::rejectMalformedResponse(client);
            response.resize(static_cast<size_t>(responseLength));
            if (responseLength > 0 &&
                !client->Receive(response.data(), response.size())) {
                return false;
            }
            result.responseReceived = true;
            result.accepted = serverResult > 0;
            return true;
        });

    if (!result.responseReceived)
        return result;

    if (!result.accepted) {
        result.message.assign(response.begin(), response.end());
        return result;
    }

    const std::string timestampText(response.begin(), response.end());
    uint64_t timestampMs = 0;
    if (!parseTimestampMs(timestampText, timestampMs)) {
        result.message = "driver initialized; timestamp parse failed";
        return result;
    }
    const time_t timestamp = static_cast<time_t>(timestampMs / 1000);
    if (timestamp <= 0) {
        result.message = "driver initialized; invalid timestamp";
        return result;
    }

    std::tm timeInfo{};
#ifdef _WIN32
    const bool converted = localtime_s(&timeInfo, &timestamp) == 0;
#else
    const std::tm* local = std::localtime(&timestamp);
    const bool converted = local != nullptr;
    if (converted)
        timeInfo = *local;
#endif
    if (!converted) {
        result.message = "driver initialized; timestamp formatting failed";
        return result;
    }

    char dateTime[20];
    std::strftime(dateTime, sizeof(dateTime), "%Y-%m-%d %H:%M:%S", &timeInfo);
    result.message = dateTime;
    return result;
}

bool InitDriver(std::string &Card, std::string &resStr, PortType type) {
    const DriverInitializationIoResult result =
        InitDriverTracked(Card, type);
    resStr = result.message;
    return result.responseReceived;
}

bool FetchServerVersion(ServerVersionInfo &outInfo, PortType type) {
    return SocketCommand::executeNoHandle(type, [&](WindowsSocketClient* client) -> bool {
        unsigned char command = CMD_GETVERSION;
        if (!client->Send(&command, sizeof(command)))
            return false;
        CeVersion version{};
        if (!client->Receive(&version, sizeof(version)))
            return false;
        ServerVersionInfo info{};
        info.version = version.version;
        if (version.stringsize > 0) {
            std::vector<char> versionString(version.stringsize);
            if (!client->Receive(versionString.data(), versionString.size()))
                return false;
            info.versionString.assign(versionString.data(), versionString.size());
        }
        outInfo = std::move(info);
        return true;
    });
}

bool FetchProcessList(std::vector<ProcessInfoItem> &outList, PortType type) {
    return SocketCommand::executeNoHandle(type, [&](WindowsSocketClient* client) -> bool {
        unsigned char command = CMD_GETPROCESSLIST;
        if (!client->Send(&command, sizeof(command)))
            return false;
        int len = 0;
        if (!client->Receive(&len, 4))
            return false;
        if (!isValidCount(len, kMaxProcessCount))
            return SocketCommand::rejectMalformedResponse(client);
        outList.clear();
        outList.reserve(static_cast<size_t>(len));
        size_t totalNameBytes = 0;
        for (int i = 0; i < len; ++i) {
            struct { int pid; int size; } proc{};
            if (!client->Receive(&proc, sizeof(proc)))
                return false;
            if (!isValidCount(proc.size, kMaxProcessNameSize) ||
                totalNameBytes > kMaxProcessNameBytesTotal -
                    static_cast<size_t>(proc.size))
                return SocketCommand::rejectMalformedResponse(client);
            totalNameBytes += static_cast<size_t>(proc.size);
            std::vector<char> name(proc.size);
            if (proc.size > 0 && !client->Receive(name.data(), proc.size))
                return false;
            ProcessInfoItem item{};
            item.pid = proc.pid;
            item.name.assign(name.data(), name.size());
            outList.push_back(std::move(item));
        }
        return true;
    });
}

bool FetchModuleList(std::vector<ModuleInfoItem> &outList, PortType type) {
    return SocketCommand::execute(type, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_GETMODULELIST;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        int len = 0;
        if (!client->Receive(&len, 4))
            return false;
        if (!isValidCount(len, kMaxModuleCount))
            return SocketCommand::rejectMalformedResponse(client);
        CeModuleListEntry entry{};
        outList.clear();
        outList.reserve(static_cast<size_t>(len));
        size_t totalNameBytes = 0;
        for (int i = 0; i < len; ++i) {
            std::memset(&entry, 0, sizeof(entry));
            if (!client->Receive(&entry, sizeof(entry)))
                return false;
            const uint64_t moduleSize = decodeModuleSize(entry.modulesize);
            if (!isValidModuleSize(moduleSize) ||
                !isValidCount(entry.modulenamesize, kMaxModuleNameSize) ||
                totalNameBytes > kMaxModuleNameBytesTotal -
                    static_cast<size_t>(entry.modulenamesize))
                return SocketCommand::rejectMalformedResponse(client);
            totalNameBytes += static_cast<size_t>(entry.modulenamesize);
            std::vector<char> name;
            if (entry.modulenamesize > 0) {
                name.resize(static_cast<size_t>(entry.modulenamesize));
                if (!client->Receive(name.data(), static_cast<size_t>(entry.modulenamesize)))
                    return false;
            }
            ModuleInfoItem mi{};
            mi.base = entry.modulebase;
            mi.size = moduleSize;
            mi.type = entry.result;
            mi.flag = entry.flag;
            if (entry.modulenamesize > 0) {
                mi.name.assign(name.data(), static_cast<size_t>(entry.modulenamesize));
            }
            outList.push_back(std::move(mi));
        }
        return true;
    });
}

bool GetSoBaseByName(const std::string &moduleName, uint64_t &outBase, PortType port) {
    if (!isSharedObjectName(moduleName))
        return false;

    bool found = false;
    uint64_t base = 0;
    bool commandOk = SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_GETSOBASE;
        if (!client->Send(&command, sizeof(command)))
            return false;
        CeGetSoBaseInput input{};
        input.hProcess = static_cast<uint32_t>(handle);
        input.nameSize = static_cast<int>(moduleName.size());
        if (!client->Send(&input, sizeof(input)))
            return false;
        if (!client->Send(moduleName.data(), moduleName.size()))
            return false;
        CeGetSoBaseOutput output{};
        if (!client->Receive(&output, sizeof(output)))
            return false;
        if (output.result == 0 && output.base != 0) {
            found = true;
            base = output.base;
        }
        return true;
    });
    if (!commandOk || !found)
        return false;
    outBase = base;
    return true;
}

bool FindModuleSegmentsByName(const std::string &moduleName,
                              std::vector<ModuleInfoItem> &outList,
                              PortType port,
                              int requiredFlag) {
    outList.clear();
    if (moduleName.empty())
        return false;

    std::vector<ModuleInfoItem> mods;
    if (!FetchModuleList(mods, port))
        return false;

    for (const auto &m : mods) {
        if (moduleNameExactMatch(m, moduleName) && moduleFlagMatch(m, requiredFlag)) {
            outList.push_back(m);
        }
    }
    if (!outList.empty())
        return true;

    // 兼容传入部分路径/部分名的旧用法；只在没有精确匹配时回退子串匹配。
    for (const auto &m : mods) {
        if (m.name.find(moduleName) != std::string::npos && moduleFlagMatch(m, requiredFlag)) {
            outList.push_back(m);
        }
    }
    return !outList.empty();
}

bool GetModuleBaseByName(const std::string &moduleName, uint64_t &outBase, PortType port) {
    return GetSoBaseByName(moduleName, outBase, port);
}

// 本项目仅支持 arm64，指针恒为 8 字节小端
static bool read_u64(uint64_t address, uint64_t &value, PortType port) {
    std::vector<unsigned char> buf;
    if (!ReadProcessMemoryBytes(address, 8, buf, port))
        return false;
    if (buf.size() < 8)
        return false;
    value = (uint64_t)buf[0] | ((uint64_t)buf[1] << 8) |
            ((uint64_t)buf[2] << 16) | ((uint64_t)buf[3] << 24) |
            ((uint64_t)buf[4] << 32) | ((uint64_t)buf[5] << 40) |
            ((uint64_t)buf[6] << 48) | ((uint64_t)buf[7] << 56);
    return true;
}

bool ResolveModuleOffsetChain(uint64_t &outAddress, const std::string &moduleName,
                              uint64_t baseOffset, const std::vector<uint64_t> &offsets,
                              bool derefFinal, PortType port) {
    SocketCommand::TransactionLease transaction(port);
    if (!transaction)
        return false;

    uint64_t base = 0;
    if (!GetModuleBaseByName(moduleName, base, port))
        return false;
    uint64_t addr = 0;
    if (!addAddressOffset(base, baseOffset, addr))
        return false;
    if (offsets.empty()) {
        outAddress = addr;
        return true;
    }
    for (size_t i = 0; i < offsets.size(); ++i) {
        uint64_t ptr = 0;
        if (!read_u64(addr, ptr, port))
            return false;
        if (!addAddressOffset(ptr, offsets[i], addr))
            return false;
    }
    if (derefFinal) {
        uint64_t finalPtr = 0;
        if (!read_u64(addr, finalPtr, port))
            return false;
        addr = finalPtr;
    }
    outAddress = addr;
    return true;
}
