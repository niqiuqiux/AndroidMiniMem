#include "LuaAPI_ImGui.h"
#include "LuaAPI.h"
#include "LuaEngine.h"
#include "../gui/Gui.h"
#include "../gui/LuaImGuiWindow.h"
#include "../imgui/imgui.h"
#include <string>
#include <vector>
#include <map>
#include <array>
#include <cstring>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

// 全局窗口管理器：窗口ID -> 窗口指针
static std::map<int, LuaImGuiWindow*> luaWindows;

struct LuaInputTextState {
    std::array<char, 256> buffer{};
    std::string lastExternalValue;
    bool activeLastFrame = false;
};

static std::map<std::pair<lua_State*, ImGuiID>, LuaInputTextState> luaInputTextStates;
static std::map<lua_State*, std::vector<int>> luaTableStack;

namespace {
constexpr int kMaxLuaTableColumns = 511;
constexpr int kMaxLuaLegacyColumns = 512;
constexpr int kMaxLuaListBoxItems = 65536;

constexpr unsigned ExecutionModeBit(LuaEngine::ExecutionMode mode) {
    return 1u << static_cast<unsigned>(mode);
}

constexpr unsigned kGuiScriptMode =
    ExecutionModeBit(LuaEngine::ExecutionMode::GuiScript);
constexpr unsigned kGuiFrameMode =
    ExecutionModeBit(LuaEngine::ExecutionMode::GuiFrame);

struct GuardedImGuiFunction {
    lua_CFunction function = nullptr;
    unsigned allowedModes = 0;
    const char* name = nullptr;
};

int GuardedImGuiDispatch(lua_State* L) {
    auto* guarded = static_cast<GuardedImGuiFunction*>(
        lua_touserdata(L, lua_upvalueindex(1)));
    if (!guarded || !guarded->function) {
        return luaL_error(L, "invalid guarded ImGui function");
    }

    const LuaEngine::ExecutionMode mode =
        LuaEngine::CurrentExecutionMode(L);
    if ((guarded->allowedModes & ExecutionModeBit(mode)) == 0) {
        return luaL_error(
            L, "imgui.%s is not available in this Lua execution context",
            guarded->name ? guarded->name : "<unknown>");
    }
    return guarded->function(L);
}

void RegisterGuardedImGuiFunction(lua_State* L,
                                  const char* name,
                                  lua_CFunction function,
                                  unsigned allowedModes) {
    void* storage = lua_newuserdata(L, sizeof(GuardedImGuiFunction));
    new (storage) GuardedImGuiFunction{function, allowedModes, name};
    lua_pushcclosure(L, GuardedImGuiDispatch, 1);
    lua_setfield(L, -2, name);
}

int checkIntRange(lua_State* L, int index, int minValue, int maxValue, const char* name) {
    lua_Integer value = luaL_checkinteger(L, index);
    if (value < static_cast<lua_Integer>(minValue) || value > static_cast<lua_Integer>(maxValue)) {
        luaL_error(L, "%s out of range", name);
        return 0;
    }
    return static_cast<int>(value);
}

int checkOptionalIntRange(lua_State* L, int index, int defaultValue, int minValue, int maxValue, const char* name) {
    if (lua_isnoneornil(L, index)) {
        return defaultValue;
    }
    lua_Integer value = luaL_checkinteger(L, index);
    if (value < static_cast<lua_Integer>(minValue) || value > static_cast<lua_Integer>(maxValue)) {
        luaL_error(L, "%s out of range", name);
        return 0;
    }
    return static_cast<int>(value);
}

int checkInt(lua_State* L, int index, const char* name) {
    return checkIntRange(
        L, index, (std::numeric_limits<int>::min)(), (std::numeric_limits<int>::max)(), name);
}

int checkOptionalInt(lua_State* L, int index, int defaultValue, const char* name) {
    return checkOptionalIntRange(
        L, index, defaultValue, (std::numeric_limits<int>::min)(), (std::numeric_limits<int>::max)(), name);
}

double checkFiniteNumber(lua_State* L, int index, const char* name) {
    lua_Number value = luaL_checknumber(L, index);
    double number = static_cast<double>(value);
    if (!std::isfinite(number)) {
        luaL_error(L, "%s must be finite", name);
        return 0.0;
    }
    return number;
}

float checkFloat(lua_State* L, int index, const char* name) {
    double number = checkFiniteNumber(L, index, name);
    if (number < -(std::numeric_limits<float>::max)() || number > (std::numeric_limits<float>::max)()) {
        luaL_error(L, "%s out of range", name);
        return 0.0f;
    }
    return static_cast<float>(number);
}

float checkOptionalFloat(lua_State* L, int index, float defaultValue, const char* name) {
    if (lua_isnoneornil(L, index)) {
        return defaultValue;
    }
    return checkFloat(L, index, name);
}
} // namespace

// ==================== 辅助函数 ====================
ImVec4 LuaAPI_ImGui::ParseColor(lua_State* L, int index) {
    if (lua_istable(L, index)) {
        lua_pushinteger(L, 1);
        lua_gettable(L, index);
        float r = checkOptionalFloat(L, -1, 1.0f, "red");
        lua_pop(L, 1);
        
        lua_pushinteger(L, 2);
        lua_gettable(L, index);
        float g = checkOptionalFloat(L, -1, 1.0f, "green");
        lua_pop(L, 1);
        
        lua_pushinteger(L, 3);
        lua_gettable(L, index);
        float b = checkOptionalFloat(L, -1, 1.0f, "blue");
        lua_pop(L, 1);
        
        lua_pushinteger(L, 4);
        lua_gettable(L, index);
        float a = checkOptionalFloat(L, -1, 1.0f, "alpha");
        lua_pop(L, 1);
        
        return ImVec4(r, g, b, a);
    } else if (lua_isnumber(L, index)) {
        float r = checkFloat(L, index, "red");
        float g = checkOptionalFloat(L, index + 1, 1.0f, "green");
        float b = checkOptionalFloat(L, index + 2, 1.0f, "blue");
        float a = checkOptionalFloat(L, index + 3, 1.0f, "alpha");
        return ImVec4(r, g, b, a);
    }
    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

void LuaAPI_ImGui::PushVec2(lua_State* L, const ImVec2& vec) {
    lua_newtable(L);
    lua_pushnumber(L, vec.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, vec.y);
    lua_setfield(L, -2, "y");
}

ImVec2 LuaAPI_ImGui::GetVec2(lua_State* L, int index) {
    if (lua_istable(L, index)) {
        lua_getfield(L, index, "x");
        float x = checkOptionalFloat(L, -1, 0.0f, "x");
        lua_pop(L, 1);
        
        lua_getfield(L, index, "y");
        float y = checkOptionalFloat(L, -1, 0.0f, "y");
        lua_pop(L, 1);
        
        return ImVec2(x, y);
    } else if (lua_isnumber(L, index)) {
        float x = checkFloat(L, index, "x");
        float y = checkOptionalFloat(L, index + 1, 0.0f, "y");
        return ImVec2(x, y);
    }
    return ImVec2(0, 0);
}

// ==================== 注册 ImGui API ====================
void LuaAPI_ImGui::Register(lua_State* L) {
    // 每个函数都通过执行模式守卫，避免后台线程直接访问 ImGui 状态。
    lua_newtable(L);

    RegisterGuardedImGuiFunction(
        L, "createWindow", CreateWindow, kGuiScriptMode);

    constexpr unsigned frame = kGuiFrameMode;
    RegisterGuardedImGuiFunction(L, "destroyWindow", DestroyWindow, frame);
    RegisterGuardedImGuiFunction(L, "isWindowOpen", IsWindowOpen, frame);
    RegisterGuardedImGuiFunction(L, "setWindowOpen", SetWindowOpen, frame);
    RegisterGuardedImGuiFunction(L, "begin", Begin, frame);
    RegisterGuardedImGuiFunction(L, "end", End, frame);
    RegisterGuardedImGuiFunction(L, "beginChild", BeginChild, frame);
    RegisterGuardedImGuiFunction(L, "endChild", EndChild, frame);
    RegisterGuardedImGuiFunction(L, "text", Text, frame);
    RegisterGuardedImGuiFunction(L, "textColored", TextColored, frame);
    RegisterGuardedImGuiFunction(L, "textWrapped", TextWrapped, frame);
    RegisterGuardedImGuiFunction(L, "separator", Separator, frame);
    RegisterGuardedImGuiFunction(L, "spacing", Spacing, frame);
    RegisterGuardedImGuiFunction(L, "newLine", NewLine, frame);
    RegisterGuardedImGuiFunction(L, "button", Button, frame);
    RegisterGuardedImGuiFunction(L, "smallButton", SmallButton, frame);
    RegisterGuardedImGuiFunction(L, "checkbox", Checkbox, frame);
    RegisterGuardedImGuiFunction(L, "inputText", InputText, frame);
    RegisterGuardedImGuiFunction(L, "inputInt", InputInt, frame);
    RegisterGuardedImGuiFunction(L, "inputFloat", InputFloat, frame);
    RegisterGuardedImGuiFunction(L, "sliderInt", SliderInt, frame);
    RegisterGuardedImGuiFunction(L, "sliderFloat", SliderFloat, frame);
    RegisterGuardedImGuiFunction(L, "sameLine", SameLine, frame);
    RegisterGuardedImGuiFunction(L, "columns", Columns, frame);
    RegisterGuardedImGuiFunction(L, "nextColumn", NextColumn, frame);
    RegisterGuardedImGuiFunction(L, "setColumnWidth", SetColumnWidth, frame);
    RegisterGuardedImGuiFunction(L, "treeNode", TreeNode, frame);
    RegisterGuardedImGuiFunction(L, "treePop", TreePop, frame);
    RegisterGuardedImGuiFunction(
        L, "collapsingHeader", CollapsingHeader, frame);
    RegisterGuardedImGuiFunction(L, "selectable", Selectable, frame);
    RegisterGuardedImGuiFunction(L, "listBox", ListBox, frame);
    RegisterGuardedImGuiFunction(L, "beginTable", BeginTable, frame);
    RegisterGuardedImGuiFunction(L, "endTable", EndTable, frame);
    RegisterGuardedImGuiFunction(L, "tableNextRow", TableNextRow, frame);
    RegisterGuardedImGuiFunction(
        L, "tableNextColumn", TableNextColumn, frame);
    RegisterGuardedImGuiFunction(
        L, "tableSetColumnIndex", TableSetColumnIndex, frame);
    RegisterGuardedImGuiFunction(L, "isItemClicked", IsItemClicked, frame);
    RegisterGuardedImGuiFunction(L, "isItemHovered", IsItemHovered, frame);
    RegisterGuardedImGuiFunction(L, "getWindowSize", GetWindowSize, frame);
    RegisterGuardedImGuiFunction(L, "setWindowSize", SetWindowSize, frame);
    RegisterGuardedImGuiFunction(L, "getWindowPos", GetWindowPos, frame);
    RegisterGuardedImGuiFunction(L, "setWindowPos", SetWindowPos, frame);
    
    lua_setglobal(L, "imgui");
}

// ==================== 窗口管理API ====================
int LuaAPI_ImGui::CreateWindow(lua_State* L) {
    const char* windowName = luaL_checkstring(L, 1);
    const char* callbackName = luaL_checkstring(L, 2);
    
    auto* window = new LuaImGuiWindow(windowName, callbackName);
    int windowId = window->GetWindowId();
    Gui::postTask([window, windowId] {
        luaWindows[windowId] = window;
        Gui::addWindow(window);
    });
    
    lua_pushinteger(L, windowId);
    return 1;
}

int LuaAPI_ImGui::DestroyWindow(lua_State* L) {
    int windowId = checkInt(L, 1, "window id");
    
    auto it = luaWindows.find(windowId);
    if (it != luaWindows.end()) {
        it->second->pOpen = false;
        luaWindows.erase(it);
        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

int LuaAPI_ImGui::IsWindowOpen(lua_State* L) {
    int windowId = checkInt(L, 1, "window id");
    
    auto it = luaWindows.find(windowId);
    if (it != luaWindows.end()) {
        lua_pushboolean(L, it->second->pOpen ? 1 : 0);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

int LuaAPI_ImGui::SetWindowOpen(lua_State* L) {
    int windowId = checkInt(L, 1, "window id");
    bool open = lua_toboolean(L, 2) != 0;
    
    auto it = luaWindows.find(windowId);
    if (it != luaWindows.end()) {
        it->second->pOpen = open;
        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

// ==================== 窗口控制API ====================
int LuaAPI_ImGui::Begin(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    bool* pOpen = nullptr;
    if (lua_isboolean(L, 2)) {
        bool open = lua_toboolean(L, 2) != 0;
        pOpen = &open;
    }
    
    int flags = checkOptionalInt(L, 3, 0, "window flags");
    bool result = ImGui::Begin(name, pOpen, flags);
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

int LuaAPI_ImGui::End(lua_State* L) {
    ImGui::End();
    return 0;
}

int LuaAPI_ImGui::BeginChild(lua_State* L) {
    const char* strId = luaL_checkstring(L, 1);
    ImVec2 size = GetVec2(L, 2);
    bool border = lua_toboolean(L, 3) != 0;
    int flags = checkOptionalInt(L, 4, 0, "child flags");
    
    bool result = ImGui::BeginChild(strId, size, border, flags);
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

int LuaAPI_ImGui::EndChild(lua_State* L) {
    ImGui::EndChild();
    return 0;
}

// ==================== 文本和显示API ====================
int LuaAPI_ImGui::Text(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    ImGui::TextUnformatted(text);
    return 0;
}

int LuaAPI_ImGui::TextColored(lua_State* L) {
    ImVec4 color = ParseColor(L, 1);
    const char* text = luaL_checkstring(L, 2);
    ImGui::TextColored(color, "%s", text);
    return 0;
}

int LuaAPI_ImGui::TextWrapped(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    ImGui::TextWrapped("%s", text);
    return 0;
}

int LuaAPI_ImGui::Separator(lua_State* L) {
    ImGui::Separator();
    return 0;
}

int LuaAPI_ImGui::Spacing(lua_State* L) {
    ImGui::Spacing();
    return 0;
}

int LuaAPI_ImGui::NewLine(lua_State* L) {
    ImGui::NewLine();
    return 0;
}

// ==================== 按钮和输入API ====================
int LuaAPI_ImGui::Button(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    ImVec2 size = GetVec2(L, 2);
    
    bool result = ImGui::Button(label, size);
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

int LuaAPI_ImGui::SmallButton(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    bool result = ImGui::SmallButton(label);
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

int LuaAPI_ImGui::Checkbox(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    
    bool value = false;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "value");
        if (lua_isboolean(L, -1)) {
            value = lua_toboolean(L, -1) != 0;
        }
        lua_pop(L, 1);
    } else if (lua_isboolean(L, 2)) {
        value = lua_toboolean(L, 2) != 0;
    }
    
    bool result = ImGui::Checkbox(label, &value);
    
    if (lua_istable(L, 2)) {
        lua_pushboolean(L, value ? 1 : 0);
        lua_setfield(L, 2, "value");
    } else {
        lua_pushboolean(L, value ? 1 : 0);
    }
    
    lua_pushboolean(L, result ? 1 : 0);
    return 2;
}

int LuaAPI_ImGui::InputText(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);

    size_t len;
    const char* str = luaL_checklstring(L, 2, &len);
    std::string externalValue(str, len);

    ImGuiID id = ImGui::GetID(label);
    LuaInputTextState& state = luaInputTextStates[{L, id}];
    if (state.lastExternalValue != externalValue && !state.activeLastFrame) {
        size_t copyLen = (externalValue.size() < state.buffer.size() - 1) ? externalValue.size() : state.buffer.size() - 1;
        std::memcpy(state.buffer.data(), externalValue.data(), copyLen);
        state.buffer[copyLen] = '\0';
        state.lastExternalValue = externalValue.substr(0, copyLen);
    }

    int flags = checkOptionalInt(L, 3, 0, "input text flags");
    bool result = ImGui::InputText(label, state.buffer.data(), state.buffer.size(), flags);
    state.activeLastFrame = ImGui::IsItemActive();

    if (result) {
        state.lastExternalValue = state.buffer.data();
        lua_pushstring(L, state.buffer.data());
    } else {
        lua_pushstring(L, state.buffer.data());
    }
    lua_pushboolean(L, result ? 1 : 0);
    return 2;
}

int LuaAPI_ImGui::InputInt(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    int value = checkInt(L, 2, "value");
    int step = checkOptionalInt(L, 3, 1, "step");
    int stepFast = checkOptionalInt(L, 4, 100, "fast step");
    int flags = checkOptionalInt(L, 5, 0, "input int flags");
    
    bool result = ImGui::InputInt(label, &value, step, stepFast, flags);
    lua_pushinteger(L, value);
    lua_pushboolean(L, result ? 1 : 0);
    return 2;
}

int LuaAPI_ImGui::InputFloat(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    float value = checkFloat(L, 2, "value");
    float step = checkOptionalFloat(L, 3, 0.0f, "step");
    float stepFast = checkOptionalFloat(L, 4, 0.0f, "fast step");
    const char* format = luaL_optstring(L, 5, "%.3f");
    int flags = checkOptionalInt(L, 6, 0, "input float flags");
    
    bool result = ImGui::InputFloat(label, &value, step, stepFast, format, flags);
    lua_pushnumber(L, value);
    lua_pushboolean(L, result ? 1 : 0);
    return 2;
}

int LuaAPI_ImGui::SliderInt(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    int value = checkInt(L, 2, "value");
    int minVal = checkInt(L, 3, "minimum");
    int maxVal = checkInt(L, 4, "maximum");
    const char* format = luaL_optstring(L, 5, "%d");
    
    bool result = ImGui::SliderInt(label, &value, minVal, maxVal, format);
    lua_pushinteger(L, value);
    lua_pushboolean(L, result ? 1 : 0);
    return 2;
}

int LuaAPI_ImGui::SliderFloat(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    float value = checkFloat(L, 2, "value");
    float minVal = checkFloat(L, 3, "minimum");
    float maxVal = checkFloat(L, 4, "maximum");
    const char* format = luaL_optstring(L, 5, "%.3f");
    
    bool result = ImGui::SliderFloat(label, &value, minVal, maxVal, format);
    lua_pushnumber(L, value);
    lua_pushboolean(L, result ? 1 : 0);
    return 2;
}

// ==================== 布局API ====================
int LuaAPI_ImGui::SameLine(lua_State* L) {
    float offsetX = checkOptionalFloat(L, 1, 0.0f, "offset");
    float spacing = checkOptionalFloat(L, 2, -1.0f, "spacing");
    ImGui::SameLine(offsetX, spacing);
    return 0;
}

int LuaAPI_ImGui::Columns(lua_State* L) {
    int count = checkOptionalIntRange(L, 1, 1, 1, kMaxLuaLegacyColumns, "column count");
    const char* id = luaL_optstring(L, 2, nullptr);
    bool border = lua_toboolean(L, 3) != 0;
    
    ImGui::Columns(count, id, border);
    return 0;
}

int LuaAPI_ImGui::NextColumn(lua_State* L) {
    ImGui::NextColumn();
    return 0;
}

int LuaAPI_ImGui::SetColumnWidth(lua_State* L) {
    int columnIndex = checkIntRange(L, 1, 0, kMaxLuaLegacyColumns - 1, "column index");
    float width = checkFloat(L, 2, "column width");
    ImGui::SetColumnWidth(columnIndex, width);
    return 0;
}

// ==================== 树形和折叠API ====================
int LuaAPI_ImGui::TreeNode(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    bool result = ImGui::TreeNode(label);
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

int LuaAPI_ImGui::TreePop(lua_State* L) {
    ImGui::TreePop();
    return 0;
}

int LuaAPI_ImGui::CollapsingHeader(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    int flags = checkOptionalInt(L, 2, 0, "header flags");
    bool result = ImGui::CollapsingHeader(label, flags);
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

// ==================== 列表和选择API ====================
int LuaAPI_ImGui::Selectable(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    bool selected = lua_toboolean(L, 2) != 0;
    int flags = checkOptionalInt(L, 3, 0, "selectable flags");
    ImVec2 size = GetVec2(L, 4);
    
    bool result = ImGui::Selectable(label, selected, flags, size);
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

int LuaAPI_ImGui::ListBox(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    int currentItem = checkInt(L, 2, "current item") - 1;
    if (currentItem < 0) currentItem = 0;
    
    if (!lua_istable(L, 3)) {
        luaL_error(L, "Expected table for items");
    }
    
    size_t rawLen = lua_objlen(L, 3);
    if (rawLen > static_cast<size_t>(kMaxLuaListBoxItems)) {
        luaL_error(L, "list box item count out of range");
    }
    int len = static_cast<int>(rawLen);
    std::vector<std::string> itemStrings;
    itemStrings.reserve(len);
    
    for (int i = 1; i <= len; ++i) {
        lua_pushinteger(L, i);
        lua_gettable(L, 3);
        if (lua_isstring(L, -1)) {
            const char* str = lua_tostring(L, -1);
            if (str) {
                itemStrings.push_back(std::string(str));
            } else {
                itemStrings.push_back(std::string(""));
            }
        } else {
            itemStrings.push_back(std::string(""));
        }
        lua_pop(L, 1);
    }
    
    if (currentItem >= static_cast<int>(itemStrings.size())) {
        currentItem = static_cast<int>(itemStrings.size()) - 1;
    }
    if (currentItem < 0 && !itemStrings.empty()) {
        currentItem = 0;
    }
    
    std::vector<const char*> items;
    items.reserve(itemStrings.size());
    for (const auto& str : itemStrings) {
        items.push_back(str.c_str());
    }
    
    bool result = false;
    if (!items.empty() && currentItem >= 0) {
        result = ImGui::ListBox(label, &currentItem, items.data(), static_cast<int>(items.size()));
        currentItem = currentItem + 1;
    }
    
    lua_pushinteger(L, currentItem);
    lua_pushboolean(L, result ? 1 : 0);
    return 2;
}

// ==================== 表格API ====================
int LuaAPI_ImGui::BeginTable(lua_State* L) {
    const char* strId = luaL_checkstring(L, 1);
    int column = checkIntRange(L, 2, 1, kMaxLuaTableColumns, "table column count");
    int flags = checkOptionalInt(L, 3, 0, "table flags");
    ImVec2 outerSize = GetVec2(L, 4);
    float innerWidth = checkOptionalFloat(L, 5, 0.0f, "inner width");
    
    bool result = ImGui::BeginTable(strId, column, flags, outerSize, innerWidth);
    if (result) {
        luaTableStack[L].push_back(column);
    }
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

int LuaAPI_ImGui::EndTable(lua_State* L) {
    auto it = luaTableStack.find(L);
    if (it == luaTableStack.end() || it->second.empty()) {
        luaL_error(L, "EndTable called without active table");
        return 0;
    }
    it->second.pop_back();
    if (it->second.empty()) {
        luaTableStack.erase(it);
    }
    ImGui::EndTable();
    return 0;
}

int LuaAPI_ImGui::TableNextRow(lua_State* L) {
    auto it = luaTableStack.find(L);
    if (it == luaTableStack.end() || it->second.empty()) {
        luaL_error(L, "TableNextRow called without active table");
        return 0;
    }
    int rowFlags = checkOptionalInt(L, 1, 0, "row flags");
    float minRowHeight = checkOptionalFloat(L, 2, 0.0f, "minimum row height");
    ImGui::TableNextRow(rowFlags, minRowHeight);
    return 0;
}

int LuaAPI_ImGui::TableNextColumn(lua_State* L) {
    auto it = luaTableStack.find(L);
    if (it == luaTableStack.end() || it->second.empty()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    bool result = ImGui::TableNextColumn();
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

int LuaAPI_ImGui::TableSetColumnIndex(lua_State* L) {
    int columnN = checkIntRange(L, 1, 0, kMaxLuaTableColumns - 1, "table column index");
    auto it = luaTableStack.find(L);
    if (it == luaTableStack.end() || it->second.empty() || columnN >= it->second.back()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    bool result = ImGui::TableSetColumnIndex(columnN);
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

// ==================== 其他API ====================
int LuaAPI_ImGui::IsItemClicked(lua_State* L) {
    int button = checkOptionalInt(L, 1, 0, "mouse button");
    bool result = ImGui::IsItemClicked(button);
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

int LuaAPI_ImGui::IsItemHovered(lua_State* L) {
    int flags = checkOptionalInt(L, 1, 0, "hovered flags");
    bool result = ImGui::IsItemHovered(flags);
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

int LuaAPI_ImGui::GetWindowSize(lua_State* L) {
    ImVec2 size = ImGui::GetWindowSize();
    PushVec2(L, size);
    return 1;
}

int LuaAPI_ImGui::SetWindowSize(lua_State* L) {
    ImVec2 size = GetVec2(L, 1);
    int cond = checkOptionalInt(L, 2, 0, "window size condition");
    ImGui::SetWindowSize(size, cond);
    return 0;
}

int LuaAPI_ImGui::GetWindowPos(lua_State* L) {
    ImVec2 pos = ImGui::GetWindowPos();
    PushVec2(L, pos);
    return 1;
}

int LuaAPI_ImGui::SetWindowPos(lua_State* L) {
    ImVec2 pos = GetVec2(L, 1);
    int cond = checkOptionalInt(L, 2, 0, "window position condition");
    ImGui::SetWindowPos(pos, cond);
    return 0;
}

