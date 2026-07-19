#include "client_singleton.h"
#include "SocketCommand.h"
#include <cstddef>
#include <limits>
#include <utility>

namespace {
constexpr size_t kBatchPageSize = 4096;
constexpr uint32_t kMaxNetworkAllocSize = 256u * 1024u * 1024u;
constexpr size_t kMaxBatchAddressCount = 100000;

bool isValidReadSize(uint32_t size) {
    return size > 0 && size <= kMaxNetworkAllocSize;
}

bool validateBatchReadAddresses(const std::vector<std::pair<uint64_t, int32_t>> &addrs) {
    if (addrs.empty() || addrs.size() > kMaxBatchAddressCount ||
        addrs.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }

    uint64_t totalBytes = 0;
    for (const auto &entry : addrs) {
        if (entry.second <= 0 ||
            static_cast<uint64_t>(entry.second) > kMaxNetworkAllocSize) {
            return false;
        }
        totalBytes += static_cast<uint64_t>(entry.second);
        if (totalBytes > kMaxNetworkAllocSize) {
            return false;
        }
    }

    return true;
}
} // namespace

bool ReadProcessMemoryBytes(uint64_t address, uint32_t size,
                            std::vector<unsigned char> &out, PortType port) {
    out.clear();
    if (!isValidReadSize(size))
        return false;

    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
#pragma pack(1)
        struct { unsigned char command; CeReadProcessMemoryInput input; } op;
#pragma pack()
        op.command = CMD_READPROCESSMEMORY;
        op.input.handle = handle;
        op.input.address = address;
        op.input.size = size;
        op.input.compress = 0;
        if (!client->Send(&op, sizeof(op)))
            return false;
        CeReadProcessMemoryOutput outHdr{};
        if (!client->Receive(&outHdr, sizeof(outHdr)))
            return false;

        out.resize(size);
        if (!client->Receive(out.data(), out.size())) {
            out.clear();
            return false;
        }

        if (outHdr.read < 0 || static_cast<uint32_t>(outHdr.read) > size) {
            out.clear();
            return SocketCommand::rejectMalformedResponse(client);
        }
        if (outHdr.read == 0) {
            out.clear();
            return false;
        }

        out.resize(static_cast<size_t>(outHdr.read));
        return true;
    });
}

MemoryWriteIoResult WriteProcessMemoryDetailed(
    uint64_t address, const std::vector<unsigned char> &data, PortType port) {
    MemoryWriteIoResult result;
    if (data.empty() || data.size() >
            static_cast<size_t>((std::numeric_limits<uint32_t>::max)()) ||
        !isValidReadSize(static_cast<uint32_t>(data.size()))) {
        return result;
    }

    const uint32_t size = static_cast<uint32_t>(data.size());
    (void)SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
#pragma pack(1)
        struct { unsigned char command; CeWriteProcessMemoryInput input; } op;
#pragma pack()
        op.command = CMD_WRITEPROCESSMEMORY;
        op.input.handle = handle;
        op.input.address = address;
        op.input.size = size;
        if (!client->Send(&op, sizeof(op)))
            return false;
        result.requestStarted = true;
        if (!client->Send(data.data(), data.size()))
            return false;
        CeWriteProcessMemoryOutput output{};
        if (!client->Receive(&output, sizeof(output)))
            return false;
        result.responseReceived = true;
        result.writtenBytes = output.written;
        if (output.written < 0 ||
            static_cast<uint32_t>(output.written) > size) {
            return SocketCommand::rejectMalformedResponse(client);
        }
        return true;
    });
    return result;
}

bool WriteProcessMemoryBytes(uint64_t address, uint32_t size,
                             std::vector<unsigned char> &data, PortType port,
                             int32_t *outWritten) {
    if (outWritten)
        *outWritten = 0;
    if (!isValidReadSize(size) || data.size() != static_cast<size_t>(size))
        return false;

    const MemoryWriteIoResult result =
        WriteProcessMemoryDetailed(address, data, port);
    if (outWritten)
        *outWritten = result.writtenBytes;
    return result.responseReceived &&
           result.writtenBytes == static_cast<int32_t>(size);
}

bool ReadProcessMemory_(uint64_t address, uint32_t size, void *out,
                        int32_t &Realread, PortType port) {
    Realread = 0;
    if (!isValidReadSize(size) || !out)
        return false;

    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
#pragma pack(1)
        struct { unsigned char command; CeReadProcessMemoryInput input; } op;
#pragma pack()
        op.command = CMD_READPROCESSMEMORY;
        op.input.handle = handle;
        op.input.address = address;
        op.input.size = size;
        op.input.compress = 0;
        if (!client->Send(&op, sizeof(op)))
            return false;
        if (!client->Receive(&Realread, sizeof(Realread)))
            return false;
        if (!client->Receive(out, size))
            return false;
        if (Realread < 0 || static_cast<uint32_t>(Realread) > size)
            return SocketCommand::rejectMalformedResponse(client);
        if (Realread == 0)
            return false;
        return true;
    });
}

bool ReadBratchMemory(uint64_t address, uint32_t size,
                      std::vector<std::pair<uint64_t, std::vector<uint8_t>>> &out,
                      PortType port) {
    out.clear();
    if (!isValidReadSize(size))
        return false;

    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
#pragma pack(1)
        struct { unsigned char command; int handle; uint64_t address; uint32_t size; } op;
#pragma pack()
        op.command = CMD_READBRATCHMEMORY;
        op.handle = handle;
        op.address = address;
        op.size = size;
        if (!client->Send(&op, sizeof(op)))
            return false;
        int len = 0;
        if (!client->Receive(&len, sizeof(len)))
            return false;
        if (len == 0) {
            return true;
        }
        if (len < 0)
            return SocketCommand::rejectMalformedResponse(client);

        const uint64_t expectedPages =
            (static_cast<uint64_t>(size) + kBatchPageSize - 1) / kBatchPageSize;
        if (static_cast<uint64_t>(len) > expectedPages)
            return SocketCommand::rejectMalformedResponse(client);

        std::vector<std::pair<uint64_t, std::vector<uint8_t>>> receivedPages;
        receivedPages.reserve(static_cast<size_t>(len));
        for (int i = 0; i < len; i++) {
            uint64_t addr = 0;
            std::vector<unsigned char> data(kBatchPageSize);
            if (!client->Receive(&addr, sizeof(addr)))
                return false;
            if (!client->Receive(data.data(), data.size()))
                return false;
            receivedPages.emplace_back(addr, std::move(data));
        }
        out.swap(receivedPages);
        return true;
    });
}

bool ReadBratchAddr(std::vector<std::pair<uint64_t, int32_t>> &addrs,
                    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> &out,
                    PortType port) {
    out.clear();
    if (!validateBatchReadAddresses(addrs))
        return false;

    return SocketCommand::execute(port, [&](WindowsSocketClient* client, int handle) -> bool {
        unsigned char command = CMD_READBRATCHADDR;
        if (!SocketCommand::sendCommandWithHandle(client, command, handle))
            return false;
        int len = static_cast<int>(addrs.size());
        if (!client->Send(&len, sizeof(len)))
            return false;
        std::vector<CeReadBratchAddr> input(len);
        for (int i = 0; i < len; i++) {
            if (addrs[i].second <= 0)
                return false;
            input[i].addr = addrs[i].first;
            input[i].size = static_cast<uint32_t>(addrs[i].second);
        }
        if (!client->Send(input.data(), len * sizeof(CeReadBratchAddr)))
            return false;
        int result = 0;
        if (!client->Receive(&result, sizeof(result)))
            return false;
        if (result < 0 || result > len)
            return SocketCommand::rejectMalformedResponse(client);

        std::vector<std::pair<uint64_t, std::vector<uint8_t>>> receivedItems;
        receivedItems.reserve(static_cast<size_t>(result));
        for (int i = 0; i < result; i++) {
            uint64_t addr = 0;
            if (!client->Receive(&addr, sizeof(addr)))
                return false;
            // 每条带有效长度前缀：连续可读字节数（可小于请求 size，0=该地址不可读）
            uint32_t vlen = 0;
            if (!client->Receive(&vlen, sizeof(vlen)))
                return false;
            uint32_t requestedSize = 0;
            for (const auto& request : input) {
                if (request.addr == addr) {
                    requestedSize = (std::max)(requestedSize, request.size);
                }
            }
            if (requestedSize == 0 || vlen > requestedSize)
                return SocketCommand::rejectMalformedResponse(client);
            std::vector<unsigned char> data(vlen);
            if (vlen > 0 && !client->Receive(data.data(), data.size()))
                return false;
            receivedItems.emplace_back(addr, std::move(data));
        }
        out.swap(receivedItems);
        return true;
    });
}
