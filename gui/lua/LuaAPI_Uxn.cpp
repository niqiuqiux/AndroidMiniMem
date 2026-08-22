#include "LuaAPI_Uxn.h"

#include "LuaAPI.h"
#include "../mem/IMemService.h"

#include <chrono>
#include <cstdio>

namespace {

constexpr uint32_t kMaxUxnSlot = Mem::kMaxUxnSlots - 1;

uint32_t checkUxnSlot(lua_State* L, int index) {
    const lua_Integer raw = luaL_checkinteger(L, index);
    if (raw < 0 || raw > static_cast<lua_Integer>(kMaxUxnSlot)) {
        luaL_error(L, "UXN slot must be between 0 and 15");
        return 0;
    }
    return static_cast<uint32_t>(raw);
}

uint32_t checkUxnWaitTimeout(lua_State* L, int index) {
    const lua_Integer raw = luaL_optinteger(L, index, 1000);
    if (raw < 1 || raw > static_cast<lua_Integer>(Mem::kMaxUxnWaitTimeoutMs)) {
        luaL_error(L, "UXN wait timeout must be between 1 and 60000 ms");
        return 0;
    }
    return static_cast<uint32_t>(raw);
}

void pushHex64(lua_State* L, uint64_t value) {
    char text[19]{};
    std::snprintf(text, sizeof(text), "0x%016llX",
                  static_cast<unsigned long long>(value));
    lua_pushstring(L, text);
}

void pushHex32(lua_State* L, uint32_t value) {
    char text[11]{};
    std::snprintf(text, sizeof(text), "0x%08X", value);
    lua_pushstring(L, text);
}

void setIntegerField(lua_State* L, const char* name, uint64_t value) {
    lua_pushinteger(L, static_cast<lua_Integer>(value));
    lua_setfield(L, -2, name);
}

void setHex64Field(lua_State* L, const char* name, uint64_t value) {
    pushHex64(L, value);
    lua_setfield(L, -2, name);
}

void setHex32Field(lua_State* L, const char* name, uint32_t value) {
    pushHex32(L, value);
    lua_setfield(L, -2, name);
}

void pushUxnRegisters(lua_State* L, const Mem::UxnRegisters& registers) {
    lua_newtable(L);
    lua_newtable(L);
    for (size_t index = 0; index < registers.general.size(); ++index) {
        pushHex64(L, registers.general[index]);
        lua_rawseti(L, -2, static_cast<int>(index + 1));
    }
    lua_setfield(L, -2, "general");
    setHex64Field(L, "sp", registers.stackPointer);
    setHex64Field(L, "pc", registers.programCounter);
    setHex64Field(L, "pstate", registers.pstate);
}

void pushUxnFpsimdRegisters(lua_State* L,
                            const Mem::UxnFpsimdRegisters& registers) {
    lua_newtable(L);
    lua_pushboolean(L, registers.valid ? 1 : 0);
    lua_setfield(L, -2, "valid");
    setHex32Field(L, "fpsr", registers.fpsr);
    setHex32Field(L, "fpcr", registers.fpcr);
    lua_newtable(L);
    for (size_t index = 0; index < registers.vector.size(); ++index) {
        lua_newtable(L);
        setHex64Field(L, "low", registers.vector[index].low);
        setHex64Field(L, "high", registers.vector[index].high);
        lua_rawseti(L, -2, static_cast<int>(index + 1));
    }
    lua_setfield(L, -2, "vector");
}

void pushUxnEvent(lua_State* L, const Mem::UxnEvent& event) {
    lua_newtable(L);
    setIntegerField(L, "slot", event.slot);
    setIntegerField(L, "pid", event.pid);
    setIntegerField(L, "tid", event.tid);
    setIntegerField(L, "state", static_cast<uint32_t>(event.state));
    setHex64Field(L, "sequence", event.sequence);
    setHex64Field(L, "address", event.address);
    setHex64Field(L, "page", event.page);
    setHex64Field(L, "fault_address", event.faultAddress);
    setHex64Field(L, "esr", event.esr);
    setHex64Field(L, "hits", event.hits);
    setHex64Field(L, "false_hits", event.falseHits);
    pushUxnRegisters(L, event.registers);
    lua_setfield(L, -2, "registers");
    pushUxnFpsimdRegisters(L, event.fpsimd);
    lua_setfield(L, -2, "fpsimd");
}

void pushUxnStatus(lua_State* L, const Mem::UxnStatus& status) {
    lua_newtable(L);
    setIntegerField(L, "slot", status.slot);
    lua_pushboolean(L, status.used ? 1 : 0);
    lua_setfield(L, -2, "used");
    setIntegerField(L, "pid", status.pid);
    setIntegerField(L, "tid", status.tid);
    setIntegerField(L, "state", static_cast<uint32_t>(status.state));
    lua_pushinteger(L, status.lastError);
    lua_setfield(L, -2, "last_error");
    setHex64Field(L, "address", status.address);
    setHex64Field(L, "page", status.page);
    setHex64Field(L, "hits", status.hits);
    setHex64Field(L, "false_hits", status.falseHits);
    setHex64Field(L, "step_hits", status.stepHits);
    setHex64Field(L, "resumes", status.resumes);
    setHex64Field(L, "sequence", status.sequence);
}

bool readUxnRegisters(lua_State* L, int index, Mem::UxnRegisters& output) {
    if (!lua_istable(L, index)) {
        luaL_error(L, "UXN registers must be a table");
        return false;
    }

    const int tableIndex = index < 0 ? lua_gettop(L) + index + 1 : index;
    lua_getfield(L, tableIndex, "general");
    if (!lua_istable(L, -1) ||
        lua_objlen(L, -1) != output.general.size()) {
        lua_pop(L, 1);
        luaL_error(L, "UXN general registers must contain exactly 31 values");
        return false;
    }
    for (size_t item = 0; item < output.general.size(); ++item) {
        lua_rawgeti(L, -1, static_cast<int>(item + 1));
        output.general[item] = LuaAPI::CheckAddress(L, -1);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    lua_getfield(L, tableIndex, "sp");
    output.stackPointer = LuaAPI::CheckAddress(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, tableIndex, "pc");
    output.programCounter = LuaAPI::CheckAddress(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, tableIndex, "pstate");
    output.pstate = LuaAPI::CheckAddress(L, -1);
    lua_pop(L, 1);
    return true;
}

int install(lua_State* L) {
    Mem::UxnInstallRequest request;
    request.address = LuaAPI::CheckAddress(L, 1);
    const auto result = LuaAPI::Service(L).installUxnBreakpoint(
        LuaAPI::GetOperationContext(L, true), request);
    if (!result.ok()) {
        LuaAPI::PushError(L, result.error().message);
        return 2;
    }

    lua_newtable(L);
    setIntegerField(L, "slot", result.value().slot);
    setIntegerField(L, "pid", result.value().target.pid);
    setHex64Field(L, "address", result.value().address);
    return 1;
}

int remove(lua_State* L) {
    const Mem::UxnRemoveRequest request{LuaAPI::CheckAddress(L, 1)};
    const auto result = LuaAPI::Service(L).removeUxnBreakpoint(
        LuaAPI::GetOperationContext(L, true), request);
    if (!result.ok()) {
        LuaAPI::PushError(L, result.error().message);
        return 2;
    }

    lua_newtable(L);
    setIntegerField(L, "slot", result.value().slot);
    setIntegerField(L, "pid", result.value().target.pid);
    setHex64Field(L, "address", result.value().address);
    return 1;
}

int wait(lua_State* L) {
    Mem::UxnWaitRequest request;
    request.slot = checkUxnSlot(L, 1);
    request.timeoutMs = checkUxnWaitTimeout(L, 2);
    request.lastSequence = lua_isnoneornil(L, 3)
                               ? 0
                               : LuaAPI::CheckAddress(L, 3);

    const Mem::OperationContext context = LuaAPI::GetOperationContext(L, true);
    if (context.deadline != (std::chrono::steady_clock::time_point::max)()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            context.deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            LuaAPI::PushError(L, "Lua execution deadline expired before UXN wait");
            return 2;
        }
        if (request.timeoutMs > static_cast<uint64_t>(remaining.count())) {
            LuaAPI::PushError(
                L, "UXN wait timeout exceeds the remaining Lua execution deadline");
            return 2;
        }
    }

    const auto result = LuaAPI::Service(L).waitUxnBreakpoint(context, request);
    if (!result.ok()) {
        LuaAPI::PushError(L, result.error().message);
        return 2;
    }
    pushUxnEvent(L, result.value());
    return 1;
}

int resume(lua_State* L) {
    Mem::UxnResumeRequest request;
    request.slot = checkUxnSlot(L, 1);
    if (!lua_isnoneornil(L, 2)) {
        request.writeRegisters = true;
        if (!readUxnRegisters(L, 2, request.registers)) {
            return 0;
        }
    }

    const auto result = LuaAPI::Service(L).resumeUxnBreakpoint(
        LuaAPI::GetOperationContext(L, true), request);
    if (!result.ok()) {
        LuaAPI::PushError(L, result.error().message);
        return 2;
    }

    lua_newtable(L);
    setIntegerField(L, "slot", result.value().slot);
    setIntegerField(L, "pid", result.value().target.pid);
    lua_pushboolean(L, request.writeRegisters ? 1 : 0);
    lua_setfield(L, -2, "registers_written");
    return 1;
}

int status(lua_State* L) {
    const Mem::UxnStatusRequest request{checkUxnSlot(L, 1)};
    const auto result = LuaAPI::Service(L).queryUxnBreakpointStatus(
        LuaAPI::GetOperationContext(L, true), request);
    if (!result.ok()) {
        LuaAPI::PushError(L, result.error().message);
        return 2;
    }
    pushUxnStatus(L, result.value());
    return 1;
}

int clear(lua_State* L) {
    const auto result = LuaAPI::Service(L).clearUxnBreakpoints(
        LuaAPI::GetOperationContext(L, false));
    if (!result.ok()) {
        LuaAPI::PushError(L, result.error().message);
        return 2;
    }

    lua_newtable(L);
    setIntegerField(L, "pid", result.value().target.pid);
    return 1;
}

} // namespace

void LuaAPI_Uxn::Register(lua_State* L) {
    lua_newtable(L);
    const auto registerFunction = [L](const char* field,
                                      lua_CFunction function,
                                      const char* apiName) {
        LuaAPI::PushProtectedFunction(L, function, apiName);
        lua_setfield(L, -2, field);
    };
    registerFunction("install", install, "uxn.install");
    registerFunction("remove", remove, "uxn.remove");
    registerFunction("wait", wait, "uxn.wait");
    registerFunction("resume", resume, "uxn.resume");
    registerFunction("status", status, "uxn.status");
    registerFunction("clear", clear, "uxn.clear");
    lua_setglobal(L, "uxn");
}
