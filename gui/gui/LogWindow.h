#pragma once

#include "Window.h"

class LogWindow : public Window {
public:
    LogWindow();
    void onDraw() override;
    unsigned int getWindowFlags() const override;

private:
    bool autoScroll = true;        // 自动滚动到底部
    char filterBuf[128] = "";      // 日志文本过滤
};

