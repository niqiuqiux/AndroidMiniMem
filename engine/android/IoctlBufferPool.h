#ifndef IOCTL_BUFFER_POOL_H_
#define IOCTL_BUFFER_POOL_H_
#ifdef __linux__
#include <cstddef>  // for size_t
#include <new>      // for std::nothrow
#include <algorithm> // for std::max

// 优化的内存缓冲池，避免重复分配内存
// 特性：
// 1. 小请求使用栈上固定缓冲区，零动态分配
// 2. 大请求使用指数增长策略，减少重新分配次数
// 3. 内存对齐优化，提高访问效率
// 4. 支持线程本地存储 (thread_local)
class IoctlBufferPool {
    static constexpr size_t kDefaultBuffer = 4096;        // 默认小缓冲区 (4KB)
    static constexpr size_t kMaxBuffer = 64 * 1024 * 1024; // 最大缓冲区限制 (64MB)
    static constexpr double kGrowthFactor = 1.5;          // 增长因子

    alignas(64) char _smallBuf[kDefaultBuffer];           // 小缓冲区（缓存行对齐）
    char*   _largeBuf     = nullptr;                      // 大缓冲区指针
    size_t  _largeBufSize = 0;                            // 已分配大缓冲区容量

public:
    ~IoctlBufferPool() {
        // 在线程退出时自动释放大缓冲区
        if (_largeBuf) {
            delete[] _largeBuf;
            _largeBuf = nullptr;
            _largeBufSize = 0;
        }
    }

    // 禁止拷贝和赋值
    IoctlBufferPool(const IoctlBufferPool&) = delete;
    IoctlBufferPool& operator=(const IoctlBufferPool&) = delete;
    IoctlBufferPool() = default;

    // 获取至少 capacity 字节的缓冲区
    // 返回的缓冲区保证至少有 capacity 字节可用
    char* getBuffer(size_t capacity) {
        // 小请求直接使用栈上缓冲区
        if (capacity <= kDefaultBuffer) {
            return _smallBuf;
        }

        // 安全检查：防止请求过大的内存
        if (capacity > kMaxBuffer) {
            return nullptr;
        }

        // 大请求需要动态分配
        if (_largeBufSize < capacity) {
            // 计算新的容量：使用增长因子避免频繁重新分配
            // 策略：新容量 = max(当前容量 * 1.5, 请求容量)
            size_t newCapacity = capacity;
            if (_largeBufSize > 0) {
                size_t grownSize = static_cast<size_t>(_largeBufSize * kGrowthFactor);
                newCapacity = std::max(grownSize, capacity);
            }

            // 限制最大容量
            newCapacity = std::min(newCapacity, kMaxBuffer);

            // 分配新缓冲区
            char* newBuf = new (std::nothrow) char[newCapacity];
            if (!newBuf) {
                // 分配失败，尝试精确分配请求的容量
                newBuf = new (std::nothrow) char[capacity];
                if (!newBuf) {
                    return nullptr; // 分配失败
                }
                newCapacity = capacity;
            }

            // 释放旧缓冲区并更新
            if (_largeBuf) {
                delete[] _largeBuf;
            }
            _largeBuf = newBuf;
            _largeBufSize = newCapacity;
        }

        return _largeBuf;
    }

    // 获取当前缓冲区容量（用于调试和统计）
    size_t getCurrentCapacity() const {
        return _largeBuf ? _largeBufSize : kDefaultBuffer;
    }

    // 重置缓冲池，释放大缓冲区（可选的内存回收功能）
    void reset() {
        if (_largeBuf) {
            delete[] _largeBuf;
            _largeBuf = nullptr;
            _largeBufSize = 0;
        }
    }
};

#endif /*__linux__*/

#endif /* IOCTL_BUFFER_POOL_H_ */
