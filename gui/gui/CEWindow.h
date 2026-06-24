#pragma once

#include "Window.h"
#include <string>
#include <vector>

// 前向声明
class LuaScriptWindow;
class ServerConnectWindow;
class LogWindow;

class CEWindow : public Window {
public:
    CEWindow();
    unsigned int getWindowFlags() const override;

protected:
    void onDraw() override;

private:
    void drawMenuBar();
    void drawTopProcessBar();
    void drawSelectedProcessBanner();
    void drawProcessSelectModal();

    // 窗口管理方法
    void openModulesWindow();
    void openLuaScriptWindow();
    void openServerConnectWindow();
    void openLogWindow();

    // 状态变量
    bool openProcessModal = false;
};