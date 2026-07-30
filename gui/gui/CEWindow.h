#pragma once

#include "Window.h"
#include <string>
#include <vector>

// 前向声明
class LuaScriptWindow;
class ServerConnectWindow;
class LogWindow;
namespace Mem { class IMemService; }

class CEWindow : public Window {
public:
    explicit CEWindow(Mem::IMemService& service);
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
    void openUxnBreakpointWindow();

    // 状态变量
    bool openProcessModal = false;
    Mem::IMemService& service_;
};
