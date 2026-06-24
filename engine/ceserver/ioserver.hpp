#pragma once
#include <cstddef>

class Ioserver {
public:
    virtual ~Ioserver() = default;
    virtual bool IsValid() const = 0;
    virtual bool Send(const void* data, size_t size) = 0;
    virtual bool Receive(void* buffer, size_t size) = 0;
    virtual int GetReadFd() = 0;
    virtual int GetWriteFd() = 0;
    virtual void Close() = 0;
    // 设置 Receive 超时（毫秒），0 表示无超时（默认）
    virtual void SetReceiveTimeout(int timeout_ms) { (void)timeout_ms; }
};
