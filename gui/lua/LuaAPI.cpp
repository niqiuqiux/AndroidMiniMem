#include "LuaAPI.h"
#include "LuaAPI_Memory.h"
#include "LuaAPI_ImGui.h"
#include "LuaAPI_Assembly.h"
#include "LuaAPI_Uxn.h"
#include "LuaDiagnostics.h"
#include "../mem/IMemService.h"
#include "../socket/socket_io_timeout.h"
#include "../gui/Gui.h"
#include <atomic>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <cstdio>
#include <exception>
#include <new>

// 辅助函数：将 Lua 值转换为字符串（Lua 5.1 兼容版本）
static const char* luaL_tolstring_compat(lua_State* L, int idx, size_t* len) {
    // Lua 5.1 兼容：手动计算绝对索引
    int absidx = (idx < 0) ? lua_gettop(L) + idx + 1 : idx;
    int type = lua_type(L, absidx);
    
    switch (type) {
        case LUA_TSTRING:
            return lua_tolstring(L, absidx, len);
        case LUA_TNUMBER: {
            lua_Number num = lua_tonumber(L, absidx);
            std::ostringstream oss;
            oss << num;
            std::string str = oss.str();
            lua_pushlstring(L, str.c_str(), str.length());
            if (len) *len = str.length();
            return lua_tostring(L, -1);
        }
        case LUA_TBOOLEAN: {
            int b = lua_toboolean(L, absidx);
            const char* str = b ? "true" : "false";
            lua_pushstring(L, str);
            if (len) *len = strlen(str);
            return str;
        }
        case LUA_TNIL: {
            const char* str = "nil";
            lua_pushstring(L, str);
            if (len) *len = strlen(str);
            return str;
        }
        case LUA_TTABLE: {
            const char* str = "table";
            lua_pushstring(L, str);
            if (len) *len = strlen(str);
            return str;
        }
        case LUA_TFUNCTION: {
            const char* str = "function";
            lua_pushstring(L, str);
            if (len) *len = strlen(str);
            return str;
        }
        case LUA_TUSERDATA: {
            const char* str = "userdata";
            lua_pushstring(L, str);
            if (len) *len = strlen(str);
            return str;
        }
        case LUA_TTHREAD: {
            const char* str = "thread";
            lua_pushstring(L, str);
            if (len) *len = strlen(str);
            return str;
        }
        case LUA_TLIGHTUSERDATA: {
            const char* str = "lightuserdata";
            lua_pushstring(L, str);
            if (len) *len = strlen(str);
            return str;
        }
        default: {
            const char* str = lua_typename(L, type);
            lua_pushstring(L, str);
            if (len) *len = strlen(str);
            return str;
        }
    }
}

// 辅助函数：检查地址参数
namespace {
constexpr size_t kMaxLuaOffsetCount = 1024;
constexpr int kLuaSleepPollMs = 50;
char g_memServiceRegistryKey;
char g_operationContextRegistryKey;

struct ProtectedLuaFunction {
    lua_CFunction function = nullptr;
    const char* apiName = nullptr;
};

int ProtectedLuaDispatch(lua_State* L) {
    auto* binding = static_cast<ProtectedLuaFunction*>(
        lua_touserdata(L, lua_upvalueindex(1)));
    if (!binding || !binding->function) {
        return luaL_error(L, "invalid MiniMem Lua API binding");
    }
    return LuaAPI::InvokeProtected(L, binding->function, binding->apiName);
}

void RegisterTableFunction(lua_State* L,
                           const char* fieldName,
                           lua_CFunction function,
                           const char* apiName) {
    LuaAPI::PushProtectedFunction(L, function, apiName);
    lua_setfield(L, -2, fieldName);
}

bool isValidBreakpointSize(uint32_t size) {
    return size == 1 || size == 2 || size == 4 || size == 8;
}

uint32_t checkBreakpointSize(lua_State* L, int index, uint32_t defaultValue) {
    lua_Integer raw = luaL_optinteger(L, index, defaultValue);
    if (raw < 0 || raw > (std::numeric_limits<uint32_t>::max)()) {
        luaL_error(L, "Breakpoint size out of range");
        return 0;
    }

    const uint32_t size = static_cast<uint32_t>(raw);
    if (!isValidBreakpointSize(size)) {
        luaL_error(L, "Breakpoint size must be 1, 2, 4, or 8");
        return 0;
    }
    return size;
}
} // namespace

uint64_t LuaAPI::CheckAddress(lua_State* L, int index) {
    if (lua_isnumber(L, index)) {
        lua_Number number = lua_tonumber(L, index);
        if (!std::isfinite(static_cast<double>(number)) || number < 0) {
            luaL_error(L, "Address must be a non-negative finite number");
            return 0;
        }
        return static_cast<uint64_t>(number);
    } else if (lua_isstring(L, index)) {
        // 支持十六进制字符串 "0x12345678"
        const char* str = lua_tostring(L, index);
        const char* begin = str;
        while (*begin == ' ' || *begin == '\t' || *begin == '\r' || *begin == '\n') {
            ++begin;
        }
        if (*begin == '-' || *begin == '\0') {
            luaL_error(L, "Invalid address string");
            return 0;
        }

        char* end = nullptr;
        errno = 0;
        uint64_t address = std::strtoull(begin, &end, 0);
        if (end == begin || errno == ERANGE) {
            luaL_error(L, "Invalid address string");
            return 0;
        }
        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
            ++end;
        }
        if (*end != '\0') {
            luaL_error(L, "Invalid address string");
            return 0;
        }
        return address;
    }
    luaL_error(L, "Expected number or hex string for address");
    return 0;
}

void LuaAPI::PushError(lua_State* L, const std::string& msg) {
    lua_pushnil(L);
    lua_pushstring(L, msg.c_str());
}

void LuaAPI::PushProtectedFunction(lua_State* L,
                                   lua_CFunction function,
                                   const char* apiName) {
    void* storage = lua_newuserdata(L, sizeof(ProtectedLuaFunction));
    new (storage) ProtectedLuaFunction{function, apiName};
    lua_pushcclosure(L, ProtectedLuaDispatch, 1);
}

int LuaAPI::InvokeProtected(lua_State* L,
                            lua_CFunction function,
                            const char* apiName,
                            int forwardedUpvalueCount) {
    if (!function) {
        return luaL_error(L, "invalid MiniMem Lua API function");
    }
    if (forwardedUpvalueCount < 0 ||
        forwardedUpvalueCount > lua_gettop(L)) {
        return luaL_error(L, "invalid MiniMem Lua API upvalue count");
    }

    LuaDiagnostics::BeginHostCall(L, apiName);
    int resultCount = 0;
    char exceptionMessage[1024]{};
    bool failed = false;
#ifndef _WIN32
    const int argumentCount = lua_gettop(L) - forwardedUpvalueCount;
    lua_pushcclosure(L, function, forwardedUpvalueCount);
    lua_insert(L, 1);
    int status = LUA_OK;
#else
    lua_pop(L, forwardedUpvalueCount);
#endif
    try {
#ifndef _WIN32
        // 先让 Lua 错误在内层 pcall 收敛，避免 Linux LuaJIT 的外部
        // 展开对象进入 catch (...)；真正的 C++ 异常仍会穿出 pcall。
        status = lua_pcall(L, argumentCount, LUA_MULTRET, 0);
#else
        resultCount = function(L);
#endif
    } catch (const std::exception& exception) {
        const char* what = exception.what();
        std::snprintf(exceptionMessage, sizeof(exceptionMessage),
                      "MiniMem host API '%s' raised a C++ exception: %s",
                      apiName ? apiName : "<unknown>",
                      what ? what : "<no message>");
        failed = true;
    } catch (...) {
        std::snprintf(exceptionMessage, sizeof(exceptionMessage),
                      "MiniMem host API '%s' raised an unknown C++ exception",
                      apiName ? apiName : "<unknown>");
        failed = true;
    }

    if (failed) {
#ifndef _WIN32
        lua_settop(L, 0);
#endif
        lua_pushstring(L, exceptionMessage);
        return lua_error(L);
    }

#ifndef _WIN32
    if (status != LUA_OK) {
        return lua_error(L);
    }
    resultCount = lua_gettop(L);
#endif

    LuaDiagnostics::EndHostCall();
    return resultCount;
}

// ==================== 注册所有API ====================
void LuaAPI::RegisterAll(lua_State* L, Mem::IMemService& service) {
    lua_pushlightuserdata(L, &g_memServiceRegistryKey);
    lua_pushlightuserdata(L, &service);
    lua_settable(L, LUA_REGISTRYINDEX);

    // 注册内存操作 API
    LuaAPI_Memory::Register(L);

    // 创建process表
    lua_newtable(L);
    RegisterTableFunction(L, "list", GetProcessList, "process.list");
    RegisterTableFunction(L, "attach", AttachProcess, "process.attach");
    RegisterTableFunction(
        L, "getCurrent", GetCurrentPid, "process.getCurrent");
    lua_setglobal(L, "process");

    // 创建module表
    lua_newtable(L);
    RegisterTableFunction(L, "list", GetModuleList, "module.list");
    RegisterTableFunction(L, "getBase", GetModuleBase, "module.getBase");
    RegisterTableFunction(L, "resolveOffsetChain", ResolveOffsetChain,
                          "module.resolveOffsetChain");
    lua_setglobal(L, "module");


    // 创建bp表
    lua_newtable(L);
    RegisterTableFunction(L, "set", SetBreakpoint, "bp.set");
    RegisterTableFunction(L, "remove", RemoveBreakpoint, "bp.remove");
    RegisterTableFunction(L, "suspend", SuspendBreakpoint, "bp.suspend");
    RegisterTableFunction(L, "resume", ResumeBreakpoint, "bp.resume");
    RegisterTableFunction(L, "getInfo", GetBreakpointInfo, "bp.getInfo");
    lua_setglobal(L, "bp");

    LuaAPI_Uxn::Register(L);

    // 全局函数
    PushProtectedFunction(L, Log, "log");
    lua_setglobal(L, "log");
    PushProtectedFunction(L, Sleep, "sleep");
    lua_setglobal(L, "sleep");
    PushProtectedFunction(L, GetTime, "time");
    lua_setglobal(L, "time");

    // 注册 ImGui API
    LuaAPI_ImGui::Register(L);

    // 注册汇编 API
    LuaAPI_Assembly::Register(L);
}

Mem::IMemService& LuaAPI::Service(lua_State* L) {
    lua_pushlightuserdata(L, &g_memServiceRegistryKey);
    lua_gettable(L, LUA_REGISTRYINDEX);
    auto* service =
        static_cast<Mem::IMemService*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!service) {
        luaL_error(L, "memory service is not bound to this Lua state");
    }
    return *service;
}

Mem::OperationContext* LuaAPI::BoundOperationContext(lua_State* L) {
    lua_pushlightuserdata(L, &g_operationContextRegistryKey);
    lua_gettable(L, LUA_REGISTRYINDEX);
    auto* context = static_cast<Mem::OperationContext*>(
        lua_touserdata(L, -1));
    lua_pop(L, 1);
    return context;
}

Mem::OperationContext LuaAPI::GetOperationContext(
    lua_State* L, bool includeTarget) {
    Mem::OperationContext* bound = BoundOperationContext(L);
    if (!bound)
        return Service(L).captureContext(includeTarget);

    Mem::OperationContext context = *bound;
    if (!includeTarget)
        context.target.reset();
    return context;
}

Mem::OperationContext* LuaAPI::BindOperationContext(
    lua_State* L, Mem::OperationContext* context) {
    Mem::OperationContext* previous = BoundOperationContext(L);
    lua_pushlightuserdata(L, &g_operationContextRegistryKey);
    if (context)
        lua_pushlightuserdata(L, context);
    else
        lua_pushnil(L);
    lua_settable(L, LUA_REGISTRYINDEX);
    return previous;
}

// ==================== 内存操作API实现 ====================
// 已移至 LuaAPI_Memory.cpp

// ==================== 进程和模块API实现 ====================
int LuaAPI::GetProcessList(lua_State* L) {
    auto& service = Service(L);
    const Mem::OperationContext context = GetOperationContext(L, false);
    std::vector<Mem::ProcessInfo> processes;
    size_t offset = 0;
    while (true) {
        Mem::ProcessListRequest request;
        request.offset = offset;
        request.limit = Mem::kMaxProcessPageSize;
        auto result = service.listProcesses(context, request);
        if (!result.ok()) {
            PushError(L, result.error().message);
            return 2;
        }
        auto& page = result.value();
        processes.insert(processes.end(), page.items.begin(), page.items.end());
        if (!page.nextOffset) {
            break;
        }
        offset = *page.nextOffset;
    }
    lua_newtable(L);
    for (size_t i = 0; i < processes.size(); ++i) {
        lua_pushinteger(L, i + 1);
        lua_newtable(L);
        lua_pushinteger(L, processes[i].pid);
        lua_setfield(L, -2, "pid");
        lua_pushstring(L, processes[i].name.c_str());
        lua_setfield(L, -2, "name");
        lua_settable(L, -3);
    }
    return 1;
}

int LuaAPI::AttachProcess(lua_State* L) {
    lua_Integer rawPid = luaL_checkinteger(L, 1);
    if (rawPid <= 0 || rawPid > (std::numeric_limits<int>::max)()) {
        luaL_error(L, "PID out of range");
        return 0;
    }

    auto& service = Service(L);
    Mem::OpenProcessRequest request;
    request.pid = static_cast<int>(rawPid);
    Mem::OperationContext* bound = BoundOperationContext(L);
    const Mem::OperationContext expected =
        bound ? *bound : service.captureContext(true);
    auto result = service.openProcess(expected, request);
    if (result.ok()) {
        if (bound) {
            const Mem::OperationContext current =
                service.captureContext(true);
            if (current.connectionGeneration !=
                    bound->connectionGeneration ||
                !current.target ||
                *current.target != result.value().target) {
                PushError(L,
                          "target changed while Lua process selection was being committed");
                return 2;
            }
            bound->target = result.value().target;
        }
        lua_pushboolean(L, 1);
        return 1;
    }
    PushError(L, result.error().message);
    return 2;
}

int LuaAPI::GetCurrentPid(lua_State* L) {
    const auto context = GetOperationContext(L, true);
    lua_pushinteger(L, context.target ? context.target->pid : 0);
    return 1;
}

int LuaAPI::GetModuleList(lua_State* L) {
    auto& service = Service(L);
    const Mem::OperationContext context = GetOperationContext(L, true);
    std::vector<Mem::ModuleInfo> modules;
    size_t offset = 0;
    while (true) {
        Mem::ModuleListRequest request;
        request.offset = offset;
        request.limit = Mem::kMaxModulePageSize;
        auto result = service.listModules(context, request);
        if (!result.ok()) {
            PushError(L, result.error().message);
            return 2;
        }
        auto& page = result.value();
        modules.insert(modules.end(), page.items.begin(), page.items.end());
        if (!page.nextOffset) {
            break;
        }
        offset = *page.nextOffset;
    }
    lua_newtable(L);
    for (size_t i = 0; i < modules.size(); ++i) {
        lua_pushinteger(L, i + 1);
        lua_newtable(L);
        lua_pushstring(L, modules[i].name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushnumber(L, static_cast<lua_Number>(modules[i].base));
        lua_setfield(L, -2, "base");
        lua_pushnumber(L, static_cast<lua_Number>(modules[i].size));
        lua_setfield(L, -2, "size");
        const int flag = modules[i].flag;
        lua_pushinteger(L, flag);
        lua_setfield(L, -2, "flag");
        char perms[5] = {
            (flag & 1) ? 'r' : '-',
            (flag & 2) ? 'w' : '-',
            (flag & 4) ? 'x' : '-',
            (flag & 8) ? 'p' : ((flag & 16) ? 's' : '-'),
            '\0'};
        lua_pushstring(L, perms);
        lua_setfield(L, -2, "perms");
        lua_settable(L, -3);
    }
    return 1;
}

int LuaAPI::GetModuleBase(lua_State* L) {
    const char* moduleName = luaL_checkstring(L, 1);
    auto& service = Service(L);
    auto result = service.resolveModule(
        GetOperationContext(L, true), Mem::ModuleResolveRequest{moduleName});
    if (result.ok()) {
        lua_pushnumber(L,
                       static_cast<lua_Number>(result.value().module.base));
        return 1;
    }
    PushError(L, result.error().message);
    return 2;
}

int LuaAPI::ResolveOffsetChain(lua_State* L) {
    const char* moduleName = luaL_checkstring(L, 1);
    if (!lua_istable(L, 2)) {
        luaL_error(L, "Expected table for offsets");
    }

    const size_t len = lua_objlen(L, 2);
    if (len > kMaxLuaOffsetCount) {
        luaL_error(L, "Offset chain is too long");
        return 0;
    }

    std::vector<uint64_t> offsets;
    offsets.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        lua_rawgeti(L, 2, static_cast<int>(i + 1));
        offsets.push_back(LuaAPI::CheckAddress(L, -1));
        lua_pop(L, 1);
    }

    Mem::PointerResolveRequest request;
    request.moduleName = moduleName;
    request.offsets = std::move(offsets);
    auto& service = Service(L);
    auto result = service.resolvePointer(GetOperationContext(L, true), request);
    if (result.ok()) {
        lua_pushnumber(L, static_cast<lua_Number>(result.value().address));
        return 1;
    }
    PushError(L, result.error().message);
    return 2;
}

// ==================== 断点API实现 ====================
int LuaAPI::SetBreakpoint(lua_State* L) {
    uint64_t address = LuaAPI::CheckAddress(L, 1);
    const char* bpType = luaL_checkstring(L, 2);
    uint32_t bpSize = checkBreakpointSize(L, 3, 4);

    uint32_t typeFlag = 0;
    if (strcmp(bpType, "execute") == 0) typeFlag = 4;
    else if (strcmp(bpType, "write") == 0) typeFlag = 2;
    else if (strcmp(bpType, "read") == 0) typeFlag = 1;
    else if (strcmp(bpType, "access") == 0 || strcmp(bpType, "readwrite") == 0) typeFlag = 3;
    else {
        LuaAPI::PushError(L, "Invalid breakpoint type");
        return 2;
    }

    if (typeFlag == 4) bpSize = 4;

    Mem::BreakpointSetRequest request;
    request.address = address;
    request.access = static_cast<Mem::BreakpointAccess>(typeFlag);
    request.size = bpSize;
    auto& service = Service(L);
    auto result = service.setBreakpoint(GetOperationContext(L, true), request);
    if (!result.ok()) {
        PushError(L, result.error().message);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int LuaAPI::RemoveBreakpoint(lua_State* L) {
    uint64_t address = LuaAPI::CheckAddress(L, 1);
    auto& service = Service(L);
    auto result = service.removeBreakpoint(
        GetOperationContext(L, true), Mem::BreakpointAddressRequest{address});
    if (!result.ok()) {
        PushError(L, result.error().message);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int LuaAPI::SuspendBreakpoint(lua_State* L) {
    uint64_t address = LuaAPI::CheckAddress(L, 1);
    auto& service = Service(L);
    auto result = service.suspendBreakpoint(
        GetOperationContext(L, true), Mem::BreakpointAddressRequest{address});
    if (!result.ok()) {
        PushError(L, result.error().message);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int LuaAPI::ResumeBreakpoint(lua_State* L) {
    uint64_t address = LuaAPI::CheckAddress(L, 1);
    auto& service = Service(L);
    auto result = service.resumeBreakpoint(
        GetOperationContext(L, true), Mem::BreakpointAddressRequest{address});
    if (!result.ok()) {
        PushError(L, result.error().message);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int LuaAPI::GetBreakpointInfo(lua_State* L) {
    uint64_t address = LuaAPI::CheckAddress(L, 1);
    auto& service = Service(L);
    auto result = service.breakpointHits(
        GetOperationContext(L, true),
        Mem::BreakpointHitBatchRequest{address,
                                       Mem::kMaxBreakpointHitCount});
    if (result.ok()) {
        const auto& infos = result.value().items;
        lua_newtable(L);
        for (size_t i = 0; i < infos.size(); ++i) {
            lua_pushinteger(L, i + 1);
            lua_newtable(L);
            lua_pushnumber(L, static_cast<lua_Number>(infos[i].hitAddress));
            lua_setfield(L, -2, "addr");
            lua_pushnumber(L, static_cast<lua_Number>(infos[i].hitTime));
            lua_setfield(L, -2, "time");
            lua_settable(L, -3);
        }
        return 1;
    }
    PushError(L, result.error().message);
    return 2;
}

// ==================== 工具API实现 ====================
int LuaAPI::Log(lua_State* L) {
    int n = lua_gettop(L);
    std::string msg;
    for (int i = 1; i <= n; ++i) {
        if (i > 1) msg += " ";
        if (lua_isstring(L, i)) {
            msg += lua_tostring(L, i);
        } else {
            size_t len;
            const char* str = luaL_tolstring_compat(L, i, &len);
            msg += str;
            lua_pop(L, 1);  // 弹出转换后的字符串
        }
    }
    Gui::log("%s", msg.c_str());
    return 0;
}

int LuaAPI::Sleep(lua_State* L) {
    lua_Integer rawMilliseconds = luaL_checkinteger(L, 1);
    if (rawMilliseconds < 0 || rawMilliseconds > (std::numeric_limits<int>::max)()) {
        luaL_error(L, "sleep duration must be non-negative");
        return 0;
    }
    int milliseconds = static_cast<int>(rawMilliseconds);
    Mem::OperationContext* operation = BoundOperationContext(L);
    std::atomic<bool>* cancellation =
        operation && operation->cancellation
            ? operation->cancellation.get()
            : nullptr;
    const auto deadline = operation
        ? operation->deadline
        : SocketIoTimeout::GetThreadDeadline();
    const bool hasDeadline = deadline !=
        (std::chrono::steady_clock::time_point::max)();

    if (!cancellation && !hasDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        return 0;
    }

    int remainingSleepMs = milliseconds;
    while (remainingSleepMs > 0) {
        if (cancellation && cancellation->load(std::memory_order_acquire)) {
            luaL_error(L, "Lua execution cancelled");
            return 0;
        }
        const auto now = std::chrono::steady_clock::now();
        if (hasDeadline && now >= deadline) {
            luaL_error(L, "Lua execution timed out");
            return 0;
        }

        int sliceMs = (std::min)(remainingSleepMs, kLuaSleepPollMs);
        if (hasDeadline) {
            auto remainingDeadlineMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now).count();
            if (remainingDeadlineMs <= 0) {
                remainingDeadlineMs = 1;
            }
            sliceMs = (std::min)(
                sliceMs, static_cast<int>(remainingDeadlineMs));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sliceMs));
        remainingSleepMs -= sliceMs;
    }

    if (cancellation && cancellation->load(std::memory_order_acquire)) {
        luaL_error(L, "Lua execution cancelled");
    }
    if (hasDeadline && std::chrono::steady_clock::now() >= deadline) {
        luaL_error(L, "Lua execution timed out");
    }
    return 0;
}

int LuaAPI::GetTime(lua_State* L) {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    lua_pushnumber(L, static_cast<lua_Number>(time));
    return 1;
}

// ==================== ImGui API实现 ====================
// 已移至 LuaAPI_ImGui.cpp
