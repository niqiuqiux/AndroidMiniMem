#pragma once

#include <functional>
#include <vector>
#include <atomic>
#include <algorithm>

class EventBus {
public:
    static EventBus& Get() {
        static EventBus instance;
        return instance;
    }

    template<typename Event>
    using Handler = std::function<void(const Event&)>;

    // 订阅事件，返回订阅 ID 用于取消订阅
    template<typename Event>
    int subscribe(Handler<Event> handler) {
        int id = nextId_.fetch_add(1, std::memory_order_relaxed);
        auto& handlers = getHandlers<Event>();
        handlers.push_back({id, std::move(handler)});
        return id;
    }

    // 取消订阅
    template<typename Event>
    void unsubscribe(int id) {
        auto& handlers = getHandlers<Event>();
        handlers.erase(
            std::remove_if(handlers.begin(), handlers.end(),
                [id](const auto& entry) { return entry.id == id; }),
            handlers.end());
    }

    // 发布事件（拷贝 handler 列表后迭代，防止回调中 unsubscribe 导致迭代器失效）
    template<typename Event>
    void publish(const Event& event) {
        auto handlersCopy = getHandlers<Event>();
        for (auto& entry : handlersCopy) {
            entry.handler(event);
        }
    }

private:
    EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    template<typename Event>
    struct HandlerEntry {
        int id;
        Handler<Event> handler;
    };

    template<typename Event>
    std::vector<HandlerEntry<Event>>& getHandlers() {
        // 使用堆分配避免静态析构顺序问题：
        // Gui::windows 析构时会调用 unsubscribe，此时 function-local static
        // 可能已被销毁。堆分配的对象永不析构，进程退出时由 OS 回收。
        static auto* handlers = new std::vector<HandlerEntry<Event>>();
        return *handlers;
    }

    std::atomic<int> nextId_{1};
};
