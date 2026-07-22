#pragma once

#include <cstdint>
#include <string>

#ifdef HAVE_LUAJIT
// LuaJIT使用lua.hpp
extern "C" {
#include "lua.hpp"
}
#endif

// 前向声明
struct ImVec2;
struct ImVec4;
namespace Mem {
class IMemService;
struct OperationContext;
}

/**
 * Lua API绑定
 * 将所有C++ API暴露给Lua脚本
 */
class LuaAPI {
public:
    // 注册所有API到Lua状态机
    static void RegisterAll(lua_State* L, Mem::IMemService& service);
    static Mem::IMemService& Service(lua_State* L);
    static Mem::OperationContext GetOperationContext(
        lua_State* L, bool includeTarget);
    static Mem::OperationContext* BoundOperationContext(lua_State* L);
    static Mem::OperationContext* BindOperationContext(
        lua_State* L, Mem::OperationContext* context);

    // 所有宿主 C++ 回调统一经过此边界，禁止异常穿过 Lua C 调用栈。
    static void PushProtectedFunction(
        lua_State* L, lua_CFunction function, const char* apiName);
    static int InvokeProtected(
        lua_State* L, lua_CFunction function, const char* apiName);

    // ==================== 内存操作API ====================
    // 已移至 LuaAPI_Memory.h

    // ==================== 进程和模块API ====================
    static int GetProcessList(lua_State* L);
    static int AttachProcess(lua_State* L);
    static int GetCurrentPid(lua_State* L);
    static int GetModuleList(lua_State* L);
    static int GetModuleBase(lua_State* L);
    static int ResolveOffsetChain(lua_State* L);

    // ==================== 断点API ====================
    static int SetBreakpoint(lua_State* L);
    static int RemoveBreakpoint(lua_State* L);
    static int SuspendBreakpoint(lua_State* L);
    static int ResumeBreakpoint(lua_State* L);
    static int GetBreakpointInfo(lua_State* L);

    // ==================== 工具API ====================
    static int Log(lua_State* L);
    static int Sleep(lua_State* L);
    static int GetTime(lua_State* L);

    // ==================== ImGui API ====================
    // 已移至 LuaAPI_ImGui.h

    // ==================== 辅助函数 ====================
    // 这些函数需要被其他模块（LuaAPI_Memory, LuaAPI_ImGui 等）使用
    static uint64_t CheckAddress(lua_State* L, int index);
    static void PushError(lua_State* L, const std::string& msg);
};

