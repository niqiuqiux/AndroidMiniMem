#include "LuaEngine.h"
#include "LuaAPI.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef HAVE_LUAJIT
// LuaJIT使用lua.hpp（已包含所有头文件）
extern "C" {
#include "lua.hpp"
}
#else
// 标准Lua需要单独包含
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#endif

namespace {

constexpr size_t kMaxCapturedOutputBytes = 1024 * 1024;
constexpr int kLuaTimeoutInstructionInterval = 10000;

char g_luaTimeoutRegistryKey;

struct LuaTimeoutContext {
    std::chrono::steady_clock::time_point deadline;
    bool expired = false;
};

struct LuaCaptureContext {
    std::string* output = nullptr;
    bool truncated = false;
};

void LuaTimeoutHook(lua_State* L, lua_Debug*) {
    lua_pushlightuserdata(L, &g_luaTimeoutRegistryKey);
    lua_gettable(L, LUA_REGISTRYINDEX);
    LuaTimeoutContext* ctx =
        static_cast<LuaTimeoutContext*>(lua_touserdata(L, -1));
    lua_pop(L, 1);

    if (!ctx) {
        return;
    }

    if (std::chrono::steady_clock::now() >= ctx->deadline) {
        ctx->expired = true;
        luaL_error(L, "Lua execution timed out");
    }
}

void AppendCapturedOutput(LuaCaptureContext* ctx, const char* text) {
    if (!ctx || !ctx->output || !text || ctx->truncated) {
        return;
    }

    const size_t remaining =
        kMaxCapturedOutputBytes > ctx->output->size()
            ? kMaxCapturedOutputBytes - ctx->output->size()
            : 0;
    if (remaining == 0) {
        ctx->truncated = true;
        ctx->output->append("\n[output truncated]\n");
        return;
    }

    const size_t len = std::strlen(text);
    ctx->output->append(text, (std::min)(len, remaining));
    if (len > remaining) {
        ctx->truncated = true;
        ctx->output->append("\n[output truncated]\n");
    }
}

} // namespace

LuaEngine& LuaEngine::GetInstance() {
    static LuaEngine instance;
    return instance;
}

bool LuaEngine::Initialize() {
    std::lock_guard<std::mutex> lock(mutex);
    
    if (initialized) {
        return true;
    }

    // 创建Lua状态机
    L = luaL_newstate();
    if (!L) {
        lastError = "Failed to create Lua state";
        return false;
    }

    // 注册标准库
    RegisterStandardLibs();
    
    // 注册自定义API
    RegisterAPIs();

    initialized = true;
    return true;
}

void LuaEngine::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex);
    
    if (L) {
        lua_close(L);
        L = nullptr;
    }
    
    initialized = false;
    loadedScripts.clear();
    callbacks.clear();
    lastError.clear();
}

LuaEngine::~LuaEngine() {
    Shutdown();
}

void LuaEngine::RegisterStandardLibs() {
    if (!L) return;
    
    // 打开标准库（LuaJIT和标准Lua都支持）
    luaL_openlibs(L);
}

void LuaEngine::RegisterAPIs() {
    if (!L) return;
    
    // 注册所有自定义API
    LuaAPI::RegisterAll(L);
}

bool LuaEngine::ExecuteFile(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(mutex);
    return ExecuteFileLocked(filepath);
}

bool LuaEngine::ExecuteFileLocked(const std::string& filepath) {
    if (!initialized || !L) {
        lastError = "Lua engine not initialized";
        return false;
    }

    // 检查文件是否存在
    if (!std::filesystem::exists(filepath)) {
        lastError = "File not found: " + filepath;
        return false;
    }

    // 加载并执行文件
    int result = luaL_loadfile(L, filepath.c_str());
    if (result != LUA_OK) {
        lastError = GetLuaError(L);
        return false;
    }

    // 执行代码
    result = lua_pcall(L, 0, 0, 0);
    if (result != LUA_OK) {
        lastError = GetLuaError(L);
        return false;
    }

    // 记录已加载的脚本
    std::string filename = std::filesystem::path(filepath).filename().string();
    loadedScripts[filename] = filepath;

    return true;
}

bool LuaEngine::ExecuteString(const std::string& code) {
    return ExecuteString(code, "=string");
}

bool LuaEngine::ExecuteString(const std::string& code, const std::string& chunkName) {
    std::lock_guard<std::mutex> lock(mutex);
    
    if (!initialized || !L) {
        lastError = "Lua engine not initialized";
        return false;
    }

    // 加载代码
    int result = luaL_loadbuffer(L, code.c_str(), code.length(), chunkName.c_str());
    if (result != LUA_OK) {
        lastError = GetLuaError(L);
        return false;
    }

    // 执行代码
    result = lua_pcall(L, 0, 0, 0);
    if (result != LUA_OK) {
        lastError = GetLuaError(L);
        return false;
    }

    return true;
}

bool LuaEngine::ExecuteStringCapture(const std::string& code,
                                     const std::string& chunkName,
                                     std::string& output,
                                     int timeoutMs) {
    std::lock_guard<std::mutex> lock(mutex);

    if (!initialized || !L) {
        lastError = "Lua engine not initialized";
        return false;
    }

    // 保存原始 print
    lua_getglobal(L, "print");
    int printRef = luaL_ref(L, LUA_REGISTRYINDEX);

    LuaCaptureContext captureContext{&output, false};

    // 设置捕获 print → 写入 output
    lua_pushlightuserdata(L, &captureContext);
    lua_pushcclosure(L, [](lua_State* L) -> int {
        LuaCaptureContext* capture =
            static_cast<LuaCaptureContext*>(lua_touserdata(L, lua_upvalueindex(1)));
        int n = lua_gettop(L);
        for (int i = 1; i <= n; i++) {
            if (i > 1) AppendCapturedOutput(capture, "\t");
            const char* s = lua_tostring(L, i);
            if (s) AppendCapturedOutput(capture, s);
        }
        AppendCapturedOutput(capture, "\n");
        return 0;
    }, 1);
    lua_setglobal(L, "print");

    // 加载并执行代码
    int result = luaL_loadbuffer(L, code.c_str(), code.length(), chunkName.c_str());
    if (result != LUA_OK) {
        lastError = GetLuaError(L);
        // 恢复原始 print
        lua_rawgeti(L, LUA_REGISTRYINDEX, printRef);
        lua_setglobal(L, "print");
        luaL_unref(L, LUA_REGISTRYINDEX, printRef);
        return false;
    }

    LuaTimeoutContext timeoutContext{};
    lua_Hook previousHook = nullptr;
    int previousHookMask = 0;
    int previousHookCount = 0;
    const bool useTimeout = timeoutMs > 0;
    if (useTimeout) {
        timeoutContext.deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        previousHook = lua_gethook(L);
        previousHookMask = lua_gethookmask(L);
        previousHookCount = lua_gethookcount(L);

        lua_pushlightuserdata(L, &g_luaTimeoutRegistryKey);
        lua_pushlightuserdata(L, &timeoutContext);
        lua_settable(L, LUA_REGISTRYINDEX);
        lua_sethook(L, LuaTimeoutHook, LUA_MASKCOUNT, kLuaTimeoutInstructionInterval);
    }

    result = lua_pcall(L, 0, 0, 0);
    bool ok = (result == LUA_OK);
    if (!ok) {
        lastError = GetLuaError(L);
        if (timeoutContext.expired) {
            lastError = "Lua execution timed out";
        }
    }

    if (useTimeout) {
        lua_sethook(L, previousHook, previousHookMask, previousHookCount);
        lua_pushlightuserdata(L, &g_luaTimeoutRegistryKey);
        lua_pushnil(L);
        lua_settable(L, LUA_REGISTRYINDEX);
    }

    // 恢复原始 print
    lua_rawgeti(L, LUA_REGISTRYINDEX, printRef);
    lua_setglobal(L, "print");
    luaL_unref(L, LUA_REGISTRYINDEX, printRef);

    return ok;
}

bool LuaEngine::ReloadScript(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex);
    
    auto it = loadedScripts.find(name);
    if (it == loadedScripts.end()) {
        lastError = "Script not loaded: " + name;
        return false;
    }

    std::string filepath = it->second;
    return ExecuteFileLocked(filepath);
}

void LuaEngine::UnloadScript(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex);
    loadedScripts.erase(name);
}

bool LuaEngine::IsScriptLoaded(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex);
    return loadedScripts.find(name) != loadedScripts.end();
}

bool LuaEngine::RegisterCallback(const std::string& name, const std::string& luaFunctionName) {
    std::lock_guard<std::mutex> lock(mutex);
    callbacks[name] = luaFunctionName;
    return true;
}

bool LuaEngine::CallCallback(const std::string& name, int nargs, int nresults) {
    std::lock_guard<std::mutex> lock(mutex);
    
    if (!initialized || !L) {
        lastError = "Lua engine not initialized";
        return false;
    }

    auto it = callbacks.find(name);
    if (it == callbacks.end()) {
        lastError = "Callback not registered: " + name;
        return false;
    }

    // 获取Lua函数
    lua_getglobal(L, it->second.c_str());
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        lastError = "Lua function not found: " + it->second;
        return false;
    }

    // 调用函数（参数已经在栈上）
    int result = lua_pcall(L, nargs, nresults, 0);
    if (result != LUA_OK) {
        lastError = GetLuaError(L);
        return false;
    }

    return true;
}

void LuaEngine::AddScriptPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex);
    AddScriptPathLocked(path);
}

void LuaEngine::AddScriptPathLocked(const std::string& path) {
    if (!L) return;

    // 获取当前的package.path
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "path");
    
    std::string currentPath = lua_tostring(L, -1);
    std::string newPath = currentPath + ";" + path + "/?.lua;" + path + "/?/init.lua";
    
    lua_pop(L, 1);
    lua_pushstring(L, newPath.c_str());
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);
}

void LuaEngine::SetScriptBasePath(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex);
    scriptBasePath = path;
    AddScriptPathLocked(path);
}

std::vector<std::string> LuaEngine::GetLoadedScripts() const {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::string> result;
    for (const auto& pair : loadedScripts) {
        result.push_back(pair.first);
    }
    return result;
}

bool LuaEngine::CheckLuaError(int result) {
    if (result != LUA_OK) {
        lastError = GetLuaError(L);
        return false;
    }
    return true;
}

std::string LuaEngine::GetLuaError(lua_State* L) {
    const char* error = lua_tostring(L, -1);
    if (error) {
        std::string errorStr(error);
        lua_pop(L, 1);  // 移除错误消息
        return errorStr;
    }
    return "Unknown Lua error";
}

