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
    const LuaExecutionResult result =
        engine.InvokeGuiCallback(luaCallbackName, windowId);
    if (result.busy) {
        ImGui::TextDisabled("Lua引擎正忙，当前帧已跳过");
        return;
    }
    if (!result.success) {
        ImGui::TextColored(ColorScheme::ErrorBright, 
            "Lua错误: %s", result.error.c_str());
    }
}

