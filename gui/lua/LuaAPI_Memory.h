#pragma once

#include "LuaAPI.h"

#ifdef HAVE_LUAJIT
extern "C" {
#include "lua.hpp"
}
#endif

/**
 * Lua 内存操作 API 绑定
 */
class LuaAPI_Memory {
public:
    // 注册内存操作 API 到 Lua 状态机
    static void Register(lua_State* L);

    // ==================== 内存操作API ====================
    static int ReadMemory(lua_State* L);
    static int WriteMemory(lua_State* L);
    static int ReadMemoryBatch(lua_State* L);
    static int ReadInt(lua_State* L);
    static int ReadLong(lua_State* L);
    static int ReadShort(lua_State* L);
    static int ReadByte(lua_State* L);
    static int WriteInt(lua_State* L);
    static int WriteLong(lua_State* L);
    static int WriteShort(lua_State* L);
    static int WriteByte(lua_State* L);
    static int ReadFloat(lua_State* L);
    static int ReadDouble(lua_State* L);
    static int WriteFloat(lua_State* L);
    static int WriteDouble(lua_State* L);
    static int ReadString(lua_State* L);
    static int WriteString(lua_State* L);
};

