#pragma once

#include <string>

struct lua_State;

namespace LuaDiagnostics {

// Lua 运行期间只写入线程本地的定长快照，崩溃处理器不直接访问 lua_State。
void BeginExecution(const char* chunkName, const char* mode);
void EndExecutionSuccess();
void EndExecutionFailure(const std::string& error);
void RecordFailure(const char* chunkName,
                   const char* mode,
                   const std::string& error);

void BeginHostCall(lua_State* state, const char* apiName);
void EndHostCall();
void UpdateLuaLocation(lua_State* state);
void RecordPanic(lua_State* state);

std::string BuildCrashReport();

} // namespace LuaDiagnostics
