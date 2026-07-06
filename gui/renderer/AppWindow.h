#pragma once

#include <functional>
#include <memory>

class AppWindow {
public:
    AppWindow();
    ~AppWindow();

    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;

    bool init(const char* title, int width, int height);
    void run(const std::function<void()>& frameCallback);
    void requestClose();
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
