#include "LuaAPI_Assembly.h"

#ifdef HAVE_LUAJIT

#include "LuaAPI.h"
#include "../gui/AssemblyHelper.h"
#include "../gui/DisassemblyHelper.h"
#include "../socket/client_singleton.h"
#include "../gui/Gui.h"

namespace {
constexpr size_t kMaxLuaAssemblyBytes = 65536;
constexpr size_t kMaxLuaDisassemblyInstructions = 4096;

uint64_t checkOptionalAddress(lua_State* L, int index, uint64_t defaultValue = 0) {
    if (lua_isnoneornil(L, index)) {
        return defaultValue;
    }
    return LuaAPI::CheckAddress(L, index);
}

size_t checkOptionalCount(lua_State* L, int index, size_t defaultValue, size_t maxValue, const char* name) {
    if (lua_isnoneornil(L, index)) {
        return defaultValue;
    }

    lua_Integer raw = luaL_checkinteger(L, index);
    if (raw < 0 || static_cast<uint64_t>(raw) > maxValue) {
        luaL_error(L, "%s out of range", name);
        return 0;
    }
    return static_cast<size_t>(raw);
}

size_t checkRequiredCount(lua_State* L, int index, size_t maxValue, const char* name) {
    lua_Integer raw = luaL_checkinteger(L, index);
    if (raw <= 0 || static_cast<uint64_t>(raw) > maxValue) {
        luaL_error(L, "%s out of range", name);
        return 0;
    }
    return static_cast<size_t>(raw);
}

std::vector<uint8_t> readByteTable(lua_State* L, int index) {
    const size_t len = lua_objlen(L, index);
    if (len == 0 || len > kMaxLuaAssemblyBytes) {
        luaL_error(L, "byte table length out of range");
        return {};
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        lua_rawgeti(L, index, static_cast<int>(i + 1));
        lua_Integer value = luaL_checkinteger(L, -1);
        if (value < 0 || value > 0xFF) {
            luaL_error(L, "byte value out of range at index %d", static_cast<int>(i + 1));
            return {};
        }
        bytes.push_back(static_cast<uint8_t>(value));
        lua_pop(L, 1);
    }
    return bytes;
}
} // namespace

// Lua 专用的 AssemblyHelper 单例
static AssemblyHelper& GetLuaAssemblyHelper() {
    static AssemblyHelper helper;
    static bool init = false;
    if (!init && AssemblyHelper::isKeystoneAvailable()) {
        if (helper.initialize(DisassemblyHelper::Architecture::ARM64)) {
            init = true;
        }
    }
    return helper;
}

// Lua 专用的 DisassemblyHelper 单例
static DisassemblyHelper& GetLuaDisassemblyHelper() {
    static DisassemblyHelper helper;
    static bool init = false;
    if (!init && DisassemblyHelper::isCapstoneAvailable()) {
        if (helper.initialize(DisassemblyHelper::Architecture::ARM64)) {
            init = true;
        }
    }
    return helper;
}

void LuaAPI_Assembly::Register(lua_State* L) {
    lua_newtable(L);

    lua_pushcfunction(L, Assemble);
    lua_setfield(L, -2, "assemble");

    lua_pushcfunction(L, Disassemble);
    lua_setfield(L, -2, "disassemble");

    lua_pushcfunction(L, Patch);
    lua_setfield(L, -2, "patch");

    lua_pushcfunction(L, IsAvailable);
    lua_setfield(L, -2, "isAvailable");

    lua_setglobal(L, "asm");
}

int LuaAPI_Assembly::IsAvailable(lua_State* L) {
    lua_pushboolean(L, AssemblyHelper::isKeystoneAvailable() ? 1 : 0);
    return 1;
}

int LuaAPI_Assembly::Assemble(lua_State* L) {
    const char* asmStr = luaL_checkstring(L, 1);
    uint64_t address = checkOptionalAddress(L, 2);

    auto& helper = GetLuaAssemblyHelper();
    AssemblyResult result = helper.assemble(asmStr, address);

    if (!result.success) {
        LuaAPI::PushError(L, result.errorMessage);
        return 2;
    }

    // 返回 bytes table
    lua_newtable(L);
    for (size_t i = 0; i < result.bytes.size(); ++i) {
        lua_pushinteger(L, static_cast<lua_Integer>(i + 1));
        lua_pushinteger(L, result.bytes[i]);
        lua_settable(L, -3);
    }
    lua_pushinteger(L, static_cast<lua_Integer>(result.statementCount));
    return 2;
}

int LuaAPI_Assembly::Patch(lua_State* L) {
    uint64_t address = LuaAPI::CheckAddress(L, 1);
    const char* asmStr = luaL_checkstring(L, 2);

    auto& helper = GetLuaAssemblyHelper();
    AssemblyResult result = helper.assemble(asmStr, address);

    if (!result.success) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, result.errorMessage.c_str());
        return 2;
    }

    // 转换为 unsigned char vector
    std::vector<unsigned char> data(result.bytes.begin(), result.bytes.end());
    bool writeOk = WriteProcessMemoryBytes(address,
        static_cast<uint32_t>(data.size()), data);

    lua_pushboolean(L, writeOk ? 1 : 0);
    return 1;
}

int LuaAPI_Assembly::Disassemble(lua_State* L) {
    auto& helper = GetLuaDisassemblyHelper();
    if (!helper.isInitialized()) {
        LuaAPI::PushError(L, "反汇编引擎未初始化 (Capstone 不可用)");
        return 2;
    }

    std::vector<uint8_t> codeBytes;
    uint64_t address = 0;
    size_t maxInstructions = 0;

    if (lua_istable(L, 1)) {
        // 用法1: asm.disassemble(bytes_table, address, [maxInstructions])
        codeBytes = readByteTable(L, 1);
        address = checkOptionalAddress(L, 2);
        maxInstructions = checkOptionalCount(
            L, 3, 0, kMaxLuaDisassemblyInstructions, "max instructions");
    } else {
        // 用法2: asm.disassemble(address, size, [maxInstructions])
        // 从进程内存读取字节
        address = LuaAPI::CheckAddress(L, 1);
        size_t size = checkRequiredCount(L, 2, kMaxLuaAssemblyBytes, "read size");
        maxInstructions = checkOptionalCount(
            L, 3, 0, kMaxLuaDisassemblyInstructions, "max instructions");

        std::vector<unsigned char> buffer(size);
        if (!ReadProcessMemoryBytes(address, static_cast<uint32_t>(size), buffer)) {
            LuaAPI::PushError(L, "读取进程内存失败");
            return 2;
        }
        codeBytes.assign(buffer.begin(), buffer.end());
    }

    if (codeBytes.empty()) {
        LuaAPI::PushError(L, "字节数据为空");
        return 2;
    }

    DisassemblyResult result = helper.disassembleMultiple(
        address, codeBytes.data(), codeBytes.size(), maxInstructions);

    if (!result.success) {
        LuaAPI::PushError(L, result.errorMessage);
        return 2;
    }

    // 返回 instructions table
    lua_newtable(L);
    for (size_t i = 0; i < result.instructions.size(); ++i) {
        const auto& inst = result.instructions[i];
        lua_pushinteger(L, static_cast<lua_Integer>(i + 1));
        lua_newtable(L);

        lua_pushnumber(L, static_cast<lua_Number>(inst.address));
        lua_setfield(L, -2, "address");

        lua_pushstring(L, inst.mnemonic.c_str());
        lua_setfield(L, -2, "mnemonic");

        lua_pushstring(L, inst.operands.c_str());
        lua_setfield(L, -2, "operands");

        lua_pushstring(L, inst.fullInstruction.c_str());
        lua_setfield(L, -2, "text");

        lua_pushstring(L, inst.hexBytes.c_str());
        lua_setfield(L, -2, "hex");

        lua_pushinteger(L, inst.size);
        lua_setfield(L, -2, "size");

        lua_settable(L, -3);
    }
    return 1;
}

#endif
