#pragma once

#ifdef HAVE_LUAJIT
extern "C" {
#include "lua.hpp"
}
#else
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#endif

#include "../mem/MemTypes.h"

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Mem { class IMemService; }

struct LuaExecutionResult {
    bool success = false;
    bool busy = false;
    std::string error;
};

/**
 * LuaJIT 引擎核心类。
 *
 * MiniMem 只维护一个 Lua 状态机，所有状态访问都必须经过本类的锁和执行
 * 上下文，避免 GUI 回调与 IPC 工作线程并发操作 lua_State。
 */
class LuaEngine {
public:
    using Clock = std::chrono::steady_clock;
    using Deadline = Clock::time_point;

    enum class ExecutionMode {
        None,
        GuiScript,
        GuiFrame,
        Ipc,
    };

    static LuaEngine& GetInstance();

    LuaEngine(const LuaEngine&) = delete;
    LuaEngine& operator=(const LuaEngine&) = delete;

    LuaExecutionResult Initialize(
        Mem::IMemService& service,
        Deadline deadline = (Deadline::max)());
    void Shutdown();

    LuaExecutionResult ExecuteFile(
        const std::string& filepath,
        Mem::CancellationToken cancellation = {},
        Deadline deadline = (Deadline::max)());
    LuaExecutionResult ExecuteString(
        const std::string& code,
        const std::string& chunkName = "=string",
        Mem::CancellationToken cancellation = {},
        Deadline deadline = (Deadline::max)());

    // IPC 专用入口：在每次调用独立的白名单环境中执行并捕获 print 输出。
    LuaExecutionResult ExecuteStringCapture(
        const std::string& code,
        const std::string& chunkName,
        std::string& output,
        Deadline deadline);

    LuaExecutionResult ReloadScript(
        const std::string& name,
        Mem::CancellationToken cancellation = {},
        Deadline deadline = (Deadline::max)());
    void UnloadScript(const std::string& name);
    bool IsScriptLoaded(const std::string& name) const;

    // GUI 每帧调用使用非阻塞锁；引擎忙时直接跳过该帧。
    LuaExecutionResult InvokeGuiCallback(
        const std::string& luaFunctionName,
        int windowId);

    void AddScriptPath(const std::string& path);
    void SetScriptBasePath(const std::string& path);
    std::vector<std::string> GetLoadedScripts() const;

    // 仅供受守卫的 Lua C API 判断当前调用边界，不暴露 lua_State 所有权。
    static ExecutionMode CurrentExecutionMode(lua_State* state);

private:
    LuaEngine() = default;
    ~LuaEngine();

    LuaExecutionResult ExecuteFileLocked(
        const std::string& filepath,
        const Mem::CancellationToken& cancellation,
        Deadline deadline);
    LuaExecutionResult ExecuteStringLocked(
        const std::string& code,
        const std::string& chunkName,
        const Mem::CancellationToken& cancellation,
        Deadline deadline);
    void AddScriptPathLocked(const std::string& path);
    std::string GetLuaError(lua_State* state);
    void RegisterStandardLibs();
    void RegisterAPIs();

    lua_State* L = nullptr;
    bool initialized = false;
    std::string scriptBasePath = "./scripts";
    std::map<std::string, std::string> loadedScripts;
    mutable std::timed_mutex mutex;
    Mem::IMemService* memService_ = nullptr;
};
