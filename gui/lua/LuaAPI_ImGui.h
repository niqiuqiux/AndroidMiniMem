#pragma once

#include "LuaAPI.h"
#include "../imgui/imgui.h"

#ifdef HAVE_LUAJIT
extern "C" {
#include "lua.hpp"
}
#endif

/**
 * Lua ImGui API 绑定
 * 将 ImGui 功能暴露给 Lua 脚本
 */
class LuaAPI_ImGui {
public:
    // 注册 ImGui API 到 Lua 状态机
    static void Register(lua_State* L);

    // ==================== 窗口管理 ====================
    static int CreateWindow(lua_State* L);
    static int DestroyWindow(lua_State* L);
    static int IsWindowOpen(lua_State* L);
    static int SetWindowOpen(lua_State* L);
    
    // ==================== 窗口控制 ====================
    static int Begin(lua_State* L);
    static int End(lua_State* L);
    static int BeginChild(lua_State* L);
    static int EndChild(lua_State* L);
    
    // ==================== 文本和显示 ====================
    static int Text(lua_State* L);
    static int TextColored(lua_State* L);
    static int TextWrapped(lua_State* L);
    static int Separator(lua_State* L);
    static int Spacing(lua_State* L);
    static int NewLine(lua_State* L);
    
    // ==================== 按钮和输入 ====================
    static int Button(lua_State* L);
    static int SmallButton(lua_State* L);
    static int Checkbox(lua_State* L);
    static int InputText(lua_State* L);
    static int InputInt(lua_State* L);
    static int InputFloat(lua_State* L);
    static int SliderInt(lua_State* L);
    static int SliderFloat(lua_State* L);
    
    // ==================== 布局 ====================
    static int SameLine(lua_State* L);
    static int Columns(lua_State* L);
    static int NextColumn(lua_State* L);
    static int SetColumnWidth(lua_State* L);
    
    // ==================== 树形和折叠 ====================
    static int TreeNode(lua_State* L);
    static int TreePop(lua_State* L);
    static int CollapsingHeader(lua_State* L);
    
    // ==================== 列表和选择 ====================
    static int Selectable(lua_State* L);
    static int ListBox(lua_State* L);
    
    // ==================== 表格 ====================
    static int BeginTable(lua_State* L);
    static int EndTable(lua_State* L);
    static int TableNextRow(lua_State* L);
    static int TableNextColumn(lua_State* L);
    static int TableSetColumnIndex(lua_State* L);
    
    // ==================== 其他 ====================
    static int IsItemClicked(lua_State* L);
    static int IsItemHovered(lua_State* L);
    static int GetWindowSize(lua_State* L);
    static int SetWindowSize(lua_State* L);
    static int GetWindowPos(lua_State* L);
    static int SetWindowPos(lua_State* L);

private:
    // 辅助函数
    static ImVec4 ParseColor(lua_State* L, int index);
    static void PushVec2(lua_State* L, const ImVec2& vec);
    static ImVec2 GetVec2(lua_State* L, int index);
};

