#include "client_singleton.h"
#include "SocketCommand.h"
#include <ctime>
#include <cerrno>
#include <cstdlib>

namespace {
constexpr int kMaxProcessCount = 65536;
constexpr int kMaxProcessNameSize = 64 * 1024;
constexpr int kMaxModuleCount = 65536;
constexpr int kMaxModuleNameSize = 64 * 1024;
constexpr int kMaxDriverCardSize = 4096;
constexpr int kMaxDriverResponseSize = 64 * 1024;

bool isValidCount(int value, int maxValue) {
    return value >= 0 && value <= maxValue;
}

bool isValidModuleSize(int value) {
    return value > 0;
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
} // namespace

bool GetMemType(int &outType, PortType type) {
    return SocketCommand::executeNoHandle(type, [&](WindowsSocketClient* client) -> bool {
        unsigned char command = CMD_GETMEMTYPE;
        if (!client->Send(&command, sizeof(command)))
            return false;
        unsigned char t = 0;
        if (!client->Receive(&t, sizeof(t)))
            return false;
        outType = t;
        return true;
    });
}

bool InitDriver(std::string &Card, std::string &resStr, PortType type) {
    resStr.clear();
    if (Card.empty() || Card.size() > static_cast<size_t>(kMaxDriverCardSize)) {
        resStr = "invalid driver card length";
        return false;
    }

    int ret = 0;
    int resStrlen = 0;
    std::vector<char> resStrVec;

    bool success = SocketCommand::executeNoHandle(type, [&](WindowsSocketClient* client) -> bool {
        unsigned char command = CMD_INITRWDRIVER;
        if (!client->Send(&command, sizeof(command)))
            return false;
        int Cardlen = static_cast<int>(Card.size());
        if (!client->Send(&Cardlen, sizeof(Cardlen)))
            return false;
        if (!client->Send(Card.data(), static_cast<size_t>(Cardlen)))
            return false;
        if (!client->Receive(&ret, sizeof(ret)))
            return false;
        if (!client->Receive(&resStrlen, sizeof(resStrlen)))
            return false;
        if (!isValidCount(resStrlen, kMaxDriverResponseSize))
            return false;
        resStrVec.resize(resStrlen);
        if (resStrlen > 0 && !client->Receive(resStrVec.data(), static_cast<size_t>(resStrlen)))
            return false;
        return true;
    });

    if (!success)
        return false;

    if (ret > 0) {
        try {
            std::string timestampStr(resStrVec.data(), resStrVec.size());
            uint64_t timestamp_ms = 0;
            if (!parseTimestampMs(timestampStr, timestamp_ms)) {
                resStr = "时间戳解析失败";
                return true;
            }
            time_t timestamp = static_cast<time_t>(timestamp_ms / 1000);
            if (timestamp > 0) {
                std::tm timeinfo{};
#ifdef _WIN32
                if (localtime_s(&timeinfo, &timestamp) == 0) {
#else
                std::tm *local = std::localtime(&timestamp);
                if (local != nullptr) {
                    timeinfo = *local;
#endif
                    char dateTime[20];
                    std::strftime(dateTime, sizeof(dateTime), "%Y-%m-%d %H:%M:%S", &timeinfo);
                    resStr = dateTime;
                } else {
                    resStr = "时间格式化失败";
                }
            } else {
                resStr = "无效的时间戳";
            }
        } catch (const std::exception &) {
            resStr = "时间戳解析失败";
        }
    } else {
        resStr.assign(resStrVec.data(), resStrVec.size());
    }
    return true;
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
            return false;
        outList.clear();
        outList.reserve(static_cast<size_t>(len));
        for (int i = 0; i < len; ++i) {
            struct { int pid; int size; } proc{};
            if (!client->Receive(&proc, sizeof(proc)))
                return false;
            if (!isValidCount(proc.size, kMaxProcessNameSize))
                return false;
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
            return false;
        CeModuleListEntry entry{};
        outList.clear();
        outList.reserve(static_cast<size_t>(len));
        for (int i = 0; i < len; ++i) {
            std::memset(&entry, 0, sizeof(entry));
            if (!client->Receive(&entry, sizeof(entry)))
                return false;
            if (!isValidModuleSize(entry.modulesize) ||
                !isValidCount(entry.modulenamesize, kMaxModuleNameSize))
                return false;
            std::vector<char> name;
            if (entry.modulenamesize > 0) {
                name.resize(static_cast<size_t>(entry.modulenamesize));
                if (!client->Receive(name.data(), static_cast<size_t>(entry.modulenamesize)))
                    return false;
            }
            ModuleInfoItem mi{};
            mi.base = entry.modulebase;
            mi.size = entry.modulesize;
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

bool GetModuleBaseByName(const std::string &moduleName, uint64_t &outBase, PortType port) {
    auto* client = GetSocketMgr().GetClient(port);
    if (!client || !client->IsConnected())
        return false;
    std::vector<ModuleInfoItem> mods;
    if (!FetchModuleList(mods, port))
        return false;
    for (const auto &m : mods) {
        if (m.name.find(moduleName) != std::string::npos) {
            outBase = m.base;
            return true;
        }
    }
    return false;
}

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
