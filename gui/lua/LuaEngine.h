#pragma once

#ifdef HAVE_LUAJIT
// LuaJIT使用lua.hpp（包含所有必要的头文件）
extern "C" {
#include "lua.hpp"
}
#else
// 如果没有LuaJIT，使用标准Lua头文件
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#endif

#include <string>
#include <map>
#include <vector>
#include <functional>
#include <mutex>

/**
 * LuaJIT引擎核心类
 * 单例模式，管理Lua状态机和脚本执行
 */
class LuaEngine {
public:
    // 获取单例实例
    static LuaEngine& GetInstance();

    // 禁止拷贝和赋值
    LuaEngine(const LuaEngine&) = delete;
    LuaEngine& operator=(const LuaEngine&) = delete;

    // 初始化和清理
    bool Initialize();
    void Shutdown();

    // 脚本执行
    bool ExecuteFile(const std::string& filepath);
    bool ExecuteString(const std::string& code);
    bool ExecuteString(const std::string& code, const std::string& chunkName);

    // 执行代码并捕获 print 输出（线程安全，用于 IPC）
    bool ExecuteStringCapture(const std::string& code,
                              const std::string& chunkName,
                              std::string& output,
                              int timeoutMs = 0);

    // 脚本管理
    bool ReloadScript(const std::string& name);
    void UnloadScript(const std::string& name);
    bool IsScriptLoaded(const std::string& name) const;

    // 回调注册（用于C++调用Lua函数）
    bool RegisterCallback(const std::string& name, const std::string& luaFunctionName);
    bool CallCallback(const std::string& name, int nargs = 0, int nresults = 0);

    // 状态管理
    lua_State* GetState() { return L; }
    bool IsInitialized() const { return initialized; }

    // 错误处理
    std::string GetLastError() const { return lastError; }
    void ClearError() { lastError.clear(); }

    // 设置脚本搜索路径
    void AddScriptPath(const std::string& path);
    void SetScriptBasePath(const std::string& path);

    // 获取已加载的脚本列表
    std::vector<std::string> GetLoadedScripts() const;

private:
    LuaEngine() = default;
    ~LuaEngine();

    // 内部辅助方法
    bool ExecuteFileLocked(const std::string& filepath);
    void AddScriptPathLocked(const std::string& path);
    bool CheckLuaError(int result);
    void PushErrorHandler();
    std::string GetLuaError(lua_State* L);
    
    // 注册标准库
    void RegisterStandardLibs();
    
    // 注册自定义API
    void RegisterAPIs();

    lua_State* L = nullptr;
    bool initialized = false;
    std::string lastError;
    std::string scriptBasePath = "./scripts";
    
    // 已加载的脚本（文件名 -> 文件路径）
    std::map<std::string, std::string> loadedScripts;
    
    // 回调函数映射（回调名 -> Lua函数名）
    std::map<std::string, std::string> callbacks;
    
    // 线程安全
    mutable std::mutex mutex;
};

