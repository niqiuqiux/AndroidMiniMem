#pragma once

#include "Window.h"
#include <string>
#include <functional>

#ifdef HAVE_LUAJIT
extern "C" {
#include "lua.hpp"
}
#endif

/**
 * Lua ImGui 窗口
 * 由 Lua 脚本创建和管理的 ImGui 窗口
 */
class LuaImGuiWindow : public Window {
public:
    LuaImGuiWindow(const std::string& windowName, const std::string& luaCallbackName);
    ~LuaImGuiWindow();

    void onDraw() override;

    // 设置 Lua 回调函数名
    void SetCallbackName(const std::string& callbackName) { luaCallbackName = callbackName; }
    std::string GetCallbackName() const { return luaCallbackName; }

    // 获取窗口 ID（用于 Lua 中引用）
    int GetWindowId() const { return windowId; }

private:
    std::string luaCallbackName;
    int windowId;
    static int nextWindowId;
};

