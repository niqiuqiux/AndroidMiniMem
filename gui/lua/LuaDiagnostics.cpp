#include "LuaDiagnostics.h"

#ifdef HAVE_LUAJIT
extern "C" {
#include "lua.hpp"
}
#else
extern "C" {
#include <lua.h>
}
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace LuaDiagnostics {
namespace {

constexpr size_t kChunkCapacity = 512;
constexpr size_t kModeCapacity = 64;
constexpr size_t kHostApiCapacity = 192;
constexpr size_t kCallerCapacity = 512;
constexpr size_t kErrorCapacity = 8192;

struct DiagnosticState {
    bool active = false;
    char chunk[kChunkCapacity]{};
    char mode[kModeCapacity]{};
    char hostApi[kHostApiCapacity]{};
    char caller[kCallerCapacity]{};

    bool hasLastFailure = false;
    char lastChunk[kChunkCapacity]{};
    char lastMode[kModeCapacity]{};
    char lastHostApi[kHostApiCapacity]{};
    char lastCaller[kCallerCapacity]{};
    char lastError[kErrorCapacity]{};
};

thread_local DiagnosticState g_state;

template <size_t Capacity>
void CopyText(char (&destination)[Capacity], const char* source) {
    if (!source) {
        destination[0] = '\0';
        return;
    }
    std::snprintf(destination, Capacity, "%s", source);
}

template <size_t Capacity>
void CopyText(char (&destination)[Capacity], const std::string& source) {
    const size_t copied = (std::min)(source.size(), Capacity - 1);
    std::memcpy(destination, source.data(), copied);
    destination[copied] = '\0';
}

void ClearCurrentExecution() {
    g_state.active = false;
    g_state.chunk[0] = '\0';
    g_state.mode[0] = '\0';
    g_state.hostApi[0] = '\0';
    g_state.caller[0] = '\0';
}

void ClearLastFailure() {
    g_state.hasLastFailure = false;
    g_state.lastChunk[0] = '\0';
    g_state.lastMode[0] = '\0';
    g_state.lastHostApi[0] = '\0';
    g_state.lastCaller[0] = '\0';
    g_state.lastError[0] = '\0';
}

void SaveCurrentFailure(const char* error) {
    g_state.hasLastFailure = true;
    CopyText(g_state.lastChunk, g_state.chunk);
    CopyText(g_state.lastMode, g_state.mode);
    CopyText(g_state.lastHostApi, g_state.hostApi);
    CopyText(g_state.lastCaller, g_state.caller);
    CopyText(g_state.lastError, error);
}

void SaveCurrentFailure(const std::string& error) {
    g_state.hasLastFailure = true;
    CopyText(g_state.lastChunk, g_state.chunk);
    CopyText(g_state.lastMode, g_state.mode);
    CopyText(g_state.lastHostApi, g_state.hostApi);
    CopyText(g_state.lastCaller, g_state.caller);
    CopyText(g_state.lastError, error);
}

void AppendField(std::ostringstream& report,
                 const char* label,
                 const char* value) {
    if (value && value[0] != '\0') {
        report << label << value << '\n';
    }
}

void CaptureLuaLocation(lua_State* state, int stackLevel) {
    g_state.caller[0] = '\0';
    if (!state) {
        return;
    }

    lua_Debug frame{};
    if (lua_getstack(state, stackLevel, &frame) == 0 ||
        lua_getinfo(state, "Sln", &frame) == 0) {
        return;
    }

    const char* source = frame.short_src[0] != '\0'
        ? frame.short_src
        : (frame.source ? frame.source : "<unknown>");
    const char* functionName = frame.name ? frame.name : "<chunk>";
    std::snprintf(g_state.caller, sizeof(g_state.caller),
                  "%s:%d (%s)", source, frame.currentline, functionName);
}

} // namespace

void BeginExecution(const char* chunkName, const char* mode) {
    ClearLastFailure();
    g_state.active = true;
    CopyText(g_state.chunk, chunkName ? chunkName : "<unknown>");
    CopyText(g_state.mode, mode ? mode : "<unknown>");
    g_state.hostApi[0] = '\0';
    g_state.caller[0] = '\0';
}

void EndExecutionSuccess() {
    ClearCurrentExecution();
    ClearLastFailure();
}

void EndExecutionFailure(const std::string& error) {
    SaveCurrentFailure(error);
    ClearCurrentExecution();
}

void RecordFailure(const char* chunkName,
                   const char* mode,
                   const std::string& error) {
    BeginExecution(chunkName, mode);
    EndExecutionFailure(error);
}

void BeginHostCall(lua_State* state, const char* apiName) {
    CopyText(g_state.hostApi, apiName ? apiName : "<unknown>");
    CaptureLuaLocation(state, 1);
}

void EndHostCall() {
    g_state.hostApi[0] = '\0';
    g_state.caller[0] = '\0';
}

void UpdateLuaLocation(lua_State* state) {
    if (g_state.active && g_state.hostApi[0] == '\0') {
        CaptureLuaLocation(state, 0);
    }
}

void RecordPanic(lua_State* state) {
    const char* message = state && lua_type(state, -1) == LUA_TSTRING
        ? lua_tostring(state, -1)
        : nullptr;
    char panic[kErrorCapacity]{};
    std::snprintf(panic, sizeof(panic), "Lua panic%s%s",
                  message && message[0] != '\0' ? ": " : "",
                  message && message[0] != '\0' ? message : "");
    if (!g_state.active) {
        BeginExecution("<panic>", "Lua panic");
    }
    SaveCurrentFailure(panic);
}

std::string BuildCrashReport() {
    if (!g_state.active && !g_state.hasLastFailure) {
        return {};
    }

    std::ostringstream report;
    if (g_state.active) {
        report << "状态: Lua 正在执行\n";
        AppendField(report, "执行模式: ", g_state.mode);
        AppendField(report, "脚本块: ", g_state.chunk);
        AppendField(report, "宿主 API: ", g_state.hostApi);
        AppendField(report, "Lua 调用位置: ", g_state.caller);
    }
    if (g_state.hasLastFailure) {
        if (g_state.active) {
            report << '\n';
        }
        report << "状态: 最近一次 Lua 执行失败\n";
        AppendField(report, "执行模式: ", g_state.lastMode);
        AppendField(report, "脚本块: ", g_state.lastChunk);
        AppendField(report, "宿主 API: ", g_state.lastHostApi);
        AppendField(report, "Lua 调用位置: ", g_state.lastCaller);
        AppendField(report, "Lua 错误与调用栈:\n", g_state.lastError);
    }
    return report.str();
}

} // namespace LuaDiagnostics
