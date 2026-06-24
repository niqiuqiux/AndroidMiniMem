#pragma once

#ifdef HAVE_LUAJIT
extern "C" {
#include "lua.hpp"
}

class LuaAPI_Assembly {
public:
    static void Register(lua_State* L);

    // asm.assemble("MOV X0, #1", address) -> bytes_table, count
    static int Assemble(lua_State* L);

    // asm.disassemble(bytes_table_or_address, size_or_address, [maxInstructions])
    // 用法1: asm.disassemble(bytes_table, address) -> instructions_table
    // 用法2: asm.disassemble(address, size) -> instructions_table（从进程内存读取）
    static int Disassemble(lua_State* L);

    // asm.patch(address, "MOV X0, #1") -> bool, [error]
    static int Patch(lua_State* L);

    // asm.isAvailable() -> bool
    static int IsAvailable(lua_State* L);
};

#endif
