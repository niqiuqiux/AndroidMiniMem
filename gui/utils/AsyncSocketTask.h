#pragma once

#include "ScopedThread.h"
#include <mutex>
#include <optional>
#include <functional>

template<typename T>
class AsyncSocketTask {
public:
    AsyncSocketTask() = default;
    ~AsyncSocketTask() { cancel(); }

    // 禁止拷贝
    AsyncSocketTask(const AsyncSocketTask&) = delete;
    AsyncSocketTask& operator=(const AsyncSocketTask&) = delete;

    // 后台执行 work(const atomic<bool>& cancel) -> T
    template<typename Fn>
    void run(Fn&& work) {
        cancel();
        m_thread.launch([this, work = std::forward<Fn>(work)](const std::atomic<bool>& cancelToken) {
            T result = work(cancelToken);
            if (!cancelToken.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_result.emplace(std::move(result));
            }
        });
    }

    // UI 线程每帧调用，结果就绪时回调一次，返回 true 表示有结果
    template<typename Callback>
    bool poll(Callback&& cb) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_result.has_value()) {
            cb(m_result.value());
            m_result.reset();
            return true;
        }
        return false;
    }

    bool busy() const {
        return m_thread.running();
    }

    void cancel() {
        m_thread.stop();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_result.reset();
    }

private:
    ScopedThread m_thread;
    std::mutex m_mutex;
    std::optional<T> m_result;
};
