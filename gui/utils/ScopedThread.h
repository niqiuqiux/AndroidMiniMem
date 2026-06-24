#pragma once

#include <thread>
#include <atomic>
#include <functional>

class ScopedThread {
public:
    ScopedThread() = default;
    ~ScopedThread() { stop(); }

    // 禁止拷贝
    ScopedThread(const ScopedThread&) = delete;
    ScopedThread& operator=(const ScopedThread&) = delete;

    // 启动线程，fn 的第一个参数为 const atomic<bool>& cancel
    template<typename Fn>
    void launch(Fn&& fn) {
        stop();
        m_cancel.store(false, std::memory_order_relaxed);
        m_done.store(false, std::memory_order_release);
        m_thread = std::thread([this, f = std::decay_t<Fn>(std::forward<Fn>(fn))]() mutable {
            f(m_cancel);
            m_done.store(true, std::memory_order_release);
        });
    }

    // 非阻塞请求取消
    void requestStop() {
        m_cancel.store(true, std::memory_order_release);
    }

    // 请求取消 + join
    void stop() {
        requestStop();
        if (m_thread.joinable())
            m_thread.join();
    }

    // 线程是否仍在执行（函数未返回）
    bool running() const {
        return m_thread.joinable() && !m_done.load(std::memory_order_acquire);
    }

    const std::atomic<bool>& cancelToken() const { return m_cancel; }

private:
    std::thread m_thread;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_done{true};
};
