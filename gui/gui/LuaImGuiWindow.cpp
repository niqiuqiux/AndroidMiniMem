#include "LuaImGuiWindow.h"
#include "ColorScheme.h"
#include "../lua/LuaEngine.h"
#include "../imgui/imgui.h"

int LuaImGuiWindow::nextWindowId = 1;

LuaImGuiWindow::LuaImGuiWindow(const std::string& windowName, const std::string& luaCallbackName)
    : luaCallbackName(luaCallbackName), windowId(nextWindowId++) {
    name = windowName;
    pOpen = true;
}

LuaImGuiWindow::~LuaImGuiWindow() {
}

void LuaImGuiWindow::onDraw() {
    if (!pOpen) return;

    auto& engine = LuaEngine::GetInstance();
    if (!engine.IsInitialized()) {
        ImGui::Text("Lua引擎未初始化");
        return;
    }

    lua_State* L = engine.GetState();
    if (!L) {
        ImGui::Text("Lua状态机无效");
        return;
    }

    // 调用 Lua 回调函数来绘制窗口内容
    lua_getglobal(L, luaCallbackName.c_str());
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        ImGui::TextColored(ColorScheme::ErrorBright, 
            "Lua函数未找到: %s", luaCallbackName.c_str());
        return;
    }

    // 传递窗口ID作为参数
    lua_pushinteger(L, windowId);

    // 调用 Lua 函数
    int result = lua_pcall(L, 1, 0, 0);
    if (result != LUA_OK) {
        const char* error = lua_tostring(L, -1);
        ImGui::TextColored(ColorScheme::ErrorBright, 
            "Lua错误: %s", error ? error : "未知错误");
        lua_pop(L, 1);
    }
}

