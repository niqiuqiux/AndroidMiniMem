#include "LuaEngine.h"
#include "LuaAPI.h"
#include "LuaAPI_ImGui.h"
#include "LuaDiagnostics.h"
#include "../mem/IMemService.h"
#include "../imgui/imgui_internal.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <new>
#include <sstream>
#include <utility>

#ifdef HAVE_LUAJIT
extern "C" {
#include "lua.hpp"
#include "luajit.h"
}
#else
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#endif

namespace {

constexpr size_t kMaxCapturedOutputBytes = 1024 * 1024;
constexpr int kLuaHookInstructionInterval = 10000;
constexpr int kLuaGuiCallbackTimeoutMs = 250;
constexpr const char* kGuiScriptModeName = "GUI 脚本";
constexpr const char* kGuiFrameModeName = "GUI 帧回调";
constexpr const char* kIpcModeName = "IPC";

char g_luaExecutionControlRegistryKey;
char g_luaExecutionModeRegistryKey;

int LuaPanic(lua_State* state) {
    LuaDiagnostics::RecordPanic(state);
    return 0;
}

int LuaTraceback(lua_State* state) {
    const char* message = lua_tostring(state, 1);
    if (!message) {
        lua_pushfstring(state, "Lua error object of type %s",
                        luaL_typename(state, 1));
        message = lua_tostring(state, -1);
    }
    luaL_traceback(state, state, message, 1);
    return 1;
}

int ProtectedLuaCall(lua_State* state, int argumentCount, int resultCount) {
    const int functionIndex = lua_gettop(state) - argumentCount;
    lua_pushcfunction(state, LuaTraceback);
    lua_insert(state, functionIndex);
    const int status = lua_pcall(
        state, argumentCount, resultCount, functionIndex);
    lua_remove(state, functionIndex);
    return status;
}

struct LuaExecutionControl {
    LuaEngine::Deadline deadline = (LuaEngine::Deadline::max)();
    Mem::CancellationToken cancellation;
    bool timedOut = false;
    bool cancelled = false;
};

struct LuaCaptureContext {
    std::string* output = nullptr;
    bool truncated = false;
};

class LuaDiagnosticBinding {
public:
    LuaDiagnosticBinding(const char* chunkName, const char* mode) {
        LuaDiagnostics::BeginExecution(chunkName, mode);
    }

    ~LuaDiagnosticBinding() {
        if (!completed_) {
            try {
                LuaDiagnostics::EndExecutionFailure(
                    "Lua execution aborted by an uncaught host exception");
            } catch (...) {
            }
        }
    }

    void Success() {
        if (!completed_) {
            LuaDiagnostics::EndExecutionSuccess();
            completed_ = true;
        }
    }

    void Failure(const std::string& error) {
        if (!completed_) {
            LuaDiagnostics::EndExecutionFailure(error);
            completed_ = true;
        }
    }

private:
    bool completed_ = false;
};

struct ImGuiErrorCapture {
    std::string messages;
    ImGuiErrorCallback previousCallback = nullptr;
    void* previousUserData = nullptr;
};

void CaptureImGuiError(ImGuiContext* context,
                       void* userData,
                       const char* message) {
    auto* capture = static_cast<ImGuiErrorCapture*>(userData);
    if (!capture) {
        return;
    }
    try {
        if (!capture->messages.empty()) {
            capture->messages += "; ";
        }
        capture->messages += message ? message : "unknown ImGui error";
    } catch (...) {
    }
    if (capture->previousCallback) {
        try {
            capture->previousCallback(
                context, capture->previousUserData, message);
        } catch (...) {
        }
    }
}

class LuaImGuiErrorBoundary {
public:
    LuaImGuiErrorBoundary() {
        context_ = ImGui::GetCurrentContext();
        if (!context_ || !context_->CurrentWindow) {
            context_ = nullptr;
            return;
        }

        ImGui::ErrorRecoveryStoreState(&initialState_);
        previousAssertEnabled_ = context_->IO.ConfigErrorRecoveryEnableAssert;
        capture_.previousCallback = context_->ErrorCallback;
        capture_.previousUserData = context_->ErrorCallbackUserData;
        context_->IO.ConfigErrorRecoveryEnableAssert = false;
        context_->ErrorCallback = CaptureImGuiError;
        context_->ErrorCallbackUserData = &capture_;
    }

    ~LuaImGuiErrorBoundary() {
        if (!finished_) {
            try {
                Finish();
            } catch (...) {
                Restore();
            }
        }
    }

    const std::string& Finish() {
        if (!context_ || finished_) {
            return capture_.messages;
        }
        ImGui::ErrorRecoveryTryToRecoverState(&initialState_);
        Restore();
        finished_ = true;
        return capture_.messages;
    }

private:
    void Restore() {
        if (!context_) {
            return;
        }
        context_->ErrorCallback = capture_.previousCallback;
        context_->ErrorCallbackUserData = capture_.previousUserData;
        context_->IO.ConfigErrorRecoveryEnableAssert = previousAssertEnabled_;
    }

    ImGuiContext* context_ = nullptr;
    ImGuiErrorRecoveryState initialState_;
    ImGuiErrorCapture capture_;
    bool previousAssertEnabled_ = true;
    bool finished_ = false;
};

class LuaImGuiScopeBinding {
public:
    explicit LuaImGuiScopeBinding(lua_State* state) : state_(state) {
        LuaAPI_ImGui::BeginFrameExecution(state_);
    }

    ~LuaImGuiScopeBinding() {
        if (!finished_) {
            try {
                LuaAPI_ImGui::EndFrameExecution(state_);
            } catch (...) {
            }
        }
    }

    std::string Finish() {
        if (finished_) {
            return {};
        }
        finished_ = true;
        return LuaAPI_ImGui::EndFrameExecution(state_);
    }

private:
    lua_State* state_ = nullptr;
    bool finished_ = false;
};

LuaExecutionResult Success() {
    return {true, false, {}};
}

LuaExecutionResult Failure(std::string error) {
    return {false, false, std::move(error)};
}

LuaExecutionResult Busy() {
    return {false, true, {}};
}

LuaExecutionResult LockTimeout() {
    return Failure("Lua execution timed out while waiting for the engine");
}

bool AcquireLock(std::unique_lock<std::timed_mutex>& lock,
                 LuaEngine::Deadline deadline) {
    if (deadline == (LuaEngine::Deadline::max)()) {
        lock.lock();
        return true;
    }
    if (LuaEngine::Clock::now() >= deadline) {
        return false;
    }
    return lock.try_lock_until(deadline);
}

bool DeadlineExpired(LuaEngine::Deadline deadline) {
    return deadline != (LuaEngine::Deadline::max)() &&
           LuaEngine::Clock::now() >= deadline;
}

bool PrepareInterruptibleChunk(lua_State* state,
                               int chunkIndex,
                               bool interruptible) {
    if (!interruptible) {
        return true;
    }
#ifdef HAVE_LUAJIT
    // LuaJIT 的已编译紧循环不会可靠触发计数 hook，因此带超时或取消能力的
    // chunk 及其子函数必须保持解释执行。
    return luaJIT_setmode(
               state, chunkIndex,
               LUAJIT_MODE_ALLFUNC | LUAJIT_MODE_OFF) != 0;
#else
    (void)state;
    (void)chunkIndex;
    return true;
#endif
}

class LuaStackGuard {
public:
    explicit LuaStackGuard(lua_State* state)
        : state_(state), top_(lua_gettop(state)) {}

    ~LuaStackGuard() {
        lua_settop(state_, top_);
    }

private:
    lua_State* state_ = nullptr;
    int top_ = 0;
};

class LuaOperationBinding {
public:
    LuaOperationBinding(lua_State* state,
                        Mem::IMemService& service,
                        const Mem::CancellationToken& cancellation,
                        LuaEngine::Deadline deadline)
        : state_(state), context_(service.captureContext(true)) {
        context_.cancellation = cancellation;
        context_.deadline = deadline;
        previous_ = LuaAPI::BindOperationContext(state_, &context_);
    }

    ~LuaOperationBinding() {
        LuaAPI::BindOperationContext(state_, previous_);
    }

    LuaOperationBinding(const LuaOperationBinding&) = delete;
    LuaOperationBinding& operator=(const LuaOperationBinding&) = delete;

private:
    lua_State* state_ = nullptr;
    Mem::OperationContext context_;
    Mem::OperationContext* previous_ = nullptr;
};

void SetExecutionMode(lua_State* state, LuaEngine::ExecutionMode mode) {
    lua_pushlightuserdata(state, &g_luaExecutionModeRegistryKey);
    lua_pushinteger(state, static_cast<lua_Integer>(mode));
    lua_settable(state, LUA_REGISTRYINDEX);
}

class LuaExecutionModeBinding {
public:
    LuaExecutionModeBinding(lua_State* state, LuaEngine::ExecutionMode mode)
        : state_(state), previous_(LuaEngine::CurrentExecutionMode(state)) {
        SetExecutionMode(state_, mode);
    }

    ~LuaExecutionModeBinding() {
        SetExecutionMode(state_, previous_);
    }

private:
    lua_State* state_ = nullptr;
    LuaEngine::ExecutionMode previous_ = LuaEngine::ExecutionMode::None;
};

void LuaExecutionHook(lua_State* state, lua_Debug*) {
    LuaDiagnostics::UpdateLuaLocation(state);
    lua_pushlightuserdata(state, &g_luaExecutionControlRegistryKey);
    lua_gettable(state, LUA_REGISTRYINDEX);
    auto* control = static_cast<LuaExecutionControl*>(
        lua_touserdata(state, -1));
    lua_pop(state, 1);

    if (!control) {
        return;
    }
    if (control->cancellation &&
        control->cancellation->load(std::memory_order_acquire)) {
        control->cancelled = true;
        luaL_error(state, "Lua execution cancelled");
        return;
    }
    if (DeadlineExpired(control->deadline)) {
        control->timedOut = true;
        luaL_error(state, "Lua execution timed out");
    }
}

class LuaHookBinding {
public:
    LuaHookBinding(lua_State* state, LuaExecutionControl& control)
        : state_(state) {
        if (control.deadline == (LuaEngine::Deadline::max)() &&
            !control.cancellation) {
            return;
        }

        enabled_ = true;
        previousHook_ = lua_gethook(state_);
        previousMask_ = lua_gethookmask(state_);
        previousCount_ = lua_gethookcount(state_);

        lua_pushlightuserdata(state_, &g_luaExecutionControlRegistryKey);
        lua_gettable(state_, LUA_REGISTRYINDEX);
        previousControl_ = lua_touserdata(state_, -1);
        lua_pop(state_, 1);

        lua_pushlightuserdata(state_, &g_luaExecutionControlRegistryKey);
        lua_pushlightuserdata(state_, &control);
        lua_settable(state_, LUA_REGISTRYINDEX);
        lua_sethook(state_, LuaExecutionHook, LUA_MASKCOUNT,
                    kLuaHookInstructionInterval);
    }

    ~LuaHookBinding() {
        if (!enabled_) {
            return;
        }
        lua_sethook(state_, previousHook_, previousMask_, previousCount_);
        lua_pushlightuserdata(state_, &g_luaExecutionControlRegistryKey);
        if (previousControl_) {
            lua_pushlightuserdata(state_, previousControl_);
        } else {
            lua_pushnil(state_);
        }
        lua_settable(state_, LUA_REGISTRYINDEX);
    }

private:
    lua_State* state_ = nullptr;
    lua_Hook previousHook_ = nullptr;
    int previousMask_ = 0;
    int previousCount_ = 0;
    void* previousControl_ = nullptr;
    bool enabled_ = false;
};

class LuaCaptureDeactivation {
public:
    explicit LuaCaptureDeactivation(LuaCaptureContext* context)
        : context_(context) {}

    ~LuaCaptureDeactivation() {
        if (context_) {
            context_->output = nullptr;
        }
    }

private:
    LuaCaptureContext* context_ = nullptr;
};

void AppendCapturedOutput(LuaCaptureContext* context,
                          const char* text,
                          size_t length) {
    if (!context || !context->output || !text || context->truncated) {
        return;
    }

    const size_t current = context->output->size();
    const size_t remaining = current < kMaxCapturedOutputBytes
        ? kMaxCapturedOutputBytes - current
        : 0;
    if (remaining == 0) {
        context->truncated = true;
        context->output->append("\n[output truncated]\n");
        return;
    }

    const size_t copied = (std::min)(length, remaining);
    context->output->append(text, copied);
    if (copied != length) {
        context->truncated = true;
        context->output->append("\n[output truncated]\n");
    }
}

void AppendCapturedOutput(LuaCaptureContext* context, const char* text) {
    AppendCapturedOutput(context, text, std::strlen(text));
}

int CapturePrintImpl(lua_State* state) {
    auto* capture = static_cast<LuaCaptureContext*>(
        lua_touserdata(state, lua_upvalueindex(1)));
    const int count = lua_gettop(state);
    for (int index = 1; index <= count; ++index) {
        if (index > 1) {
            AppendCapturedOutput(capture, "\t");
        }

        size_t length = 0;
        const char* value = lua_tolstring(state, index, &length);
        if (value) {
            AppendCapturedOutput(capture, value, length);
        } else if (lua_isnil(state, index)) {
            AppendCapturedOutput(capture, "nil");
        } else if (lua_isboolean(state, index)) {
            AppendCapturedOutput(
                capture, lua_toboolean(state, index) ? "true" : "false");
        } else {
            AppendCapturedOutput(
                capture, lua_typename(state, lua_type(state, index)));
        }
    }
    AppendCapturedOutput(capture, "\n");
    return 0;
}

int CapturePrint(lua_State* state) {
    lua_pushvalue(state, lua_upvalueindex(1));
    return LuaAPI::InvokeProtected(
        state, CapturePrintImpl, "print", 1);
}

int AbsoluteIndex(lua_State* state, int index) {
    return index < 0 ? lua_gettop(state) + index + 1 : index;
}

void PushShallowTableCopy(lua_State* state, int sourceIndex) {
    sourceIndex = AbsoluteIndex(state, sourceIndex);
    lua_newtable(state);
    const int copyIndex = lua_gettop(state);
    lua_pushnil(state);
    while (lua_next(state, sourceIndex) != 0) {
        lua_pushvalue(state, -2);
        lua_pushvalue(state, -2);
        lua_settable(state, copyIndex);
        lua_pop(state, 1);
    }
}

void CopyGlobalValue(lua_State* state, int environmentIndex, const char* name) {
    lua_getglobal(state, name);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return;
    }
    lua_setfield(state, environmentIndex, name);
}

void CopyGlobalTable(lua_State* state, int environmentIndex, const char* name) {
    lua_getglobal(state, name);
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return;
    }
    PushShallowTableCopy(state, -1);
    lua_setfield(state, environmentIndex, name);
    lua_pop(state, 1);
}

LuaCaptureContext* PushIpcEnvironment(lua_State* state,
                                      std::string& output) {
    lua_newtable(state);
    const int environmentIndex = lua_gettop(state);

    constexpr const char* kSafeBaseValues[] = {
        "_VERSION", "assert", "error", "ipairs", "next", "pairs",
        "pcall", "rawequal", "rawget", "rawset", "select", "setmetatable",
        "tonumber", "tostring", "type", "unpack", "xpcall",
        "log", "sleep", "time",
    };
    for (const char* name : kSafeBaseValues) {
        CopyGlobalValue(state, environmentIndex, name);
    }

    constexpr const char* kAllowedLibraryTables[] = {
        "math", "string", "table", "bit", "os", "io",
    };
    for (const char* name : kAllowedLibraryTables) {
        CopyGlobalTable(state, environmentIndex, name);
    }

    constexpr const char* kMiniMemApiTables[] = {
        "mem", "process", "module", "bp", "uxn", "asm",
    };
    for (const char* name : kMiniMemApiTables) {
        CopyGlobalTable(state, environmentIndex, name);
    }

    lua_pushvalue(state, environmentIndex);
    lua_setfield(state, environmentIndex, "_G");

    void* storage = lua_newuserdata(state, sizeof(LuaCaptureContext));
    auto* capture = new (storage) LuaCaptureContext{&output, false};
    lua_pushcclosure(state, CapturePrint, 1);
    lua_setfield(state, environmentIndex, "print");
    return capture;
}

std::string ExecutionInterruptionMessage(
    const LuaExecutionControl& control) {
    if (control.cancelled ||
        (control.cancellation &&
         control.cancellation->load(std::memory_order_acquire))) {
        return "Lua execution cancelled";
    }
    if (control.timedOut || DeadlineExpired(control.deadline)) {
        return "Lua execution timed out";
    }
    return {};
}

std::string ExecutionFailureMessage(const LuaExecutionControl& control,
                                    std::string luaError) {
    const std::string interruption =
        ExecutionInterruptionMessage(control);
    if (!interruption.empty()) {
        return interruption;
    }
    return luaError;
}

std::string AppendImGuiDiagnostics(std::string error,
                                   const std::string& openScopes,
                                   const std::string& imguiErrors) {
    if (openScopes.empty() && imguiErrors.empty()) {
        return error;
    }
    if (error.empty()) {
        error = "Lua ImGui callback left ImGui in an invalid state";
    }
    if (!openScopes.empty()) {
        error += "\nUnclosed Lua ImGui scopes: ";
        error += openScopes;
    }
    if (!imguiErrors.empty()) {
        error += "\nImGui recovery: ";
        error += imguiErrors;
    }
    return error;
}

} // namespace

LuaEngine& LuaEngine::GetInstance() {
    static LuaEngine instance;
    return instance;
}

LuaExecutionResult LuaEngine::Initialize(Mem::IMemService& service,
                                         Deadline deadline) {
    std::unique_lock<std::timed_mutex> lock(mutex, std::defer_lock);
    if (!AcquireLock(lock, deadline)) {
        return LockTimeout();
    }

    if (initialized) {
        if (memService_ == &service) {
            return Success();
        }
        return Failure("Lua engine is already bound to another memory service");
    }
    if (DeadlineExpired(deadline)) {
        return LockTimeout();
    }

    L = luaL_newstate();
    if (!L) {
        return Failure("Failed to create Lua state");
    }
    lua_atpanic(L, LuaPanic);

    RegisterStandardLibs();
    memService_ = &service;
    RegisterAPIs();
    AddScriptPathLocked(scriptBasePath);
    initialized = true;
    return Success();
}

void LuaEngine::Shutdown() {
    std::lock_guard<std::timed_mutex> lock(mutex);
    if (L) {
        lua_close(L);
        L = nullptr;
    }
    initialized = false;
    memService_ = nullptr;
    loadedScripts.clear();
}

LuaEngine::~LuaEngine() {
    Shutdown();
}

void LuaEngine::RegisterStandardLibs() {
    if (L) {
        luaL_openlibs(L);
    }
}

void LuaEngine::RegisterAPIs() {
    if (L && memService_) {
        LuaAPI::RegisterAll(L, *memService_);
    }
}

LuaExecutionResult LuaEngine::ExecuteFile(
    const std::string& filepath,
    Mem::CancellationToken cancellation,
    Deadline deadline) {
    std::unique_lock<std::timed_mutex> lock(mutex, std::defer_lock);
    if (!AcquireLock(lock, deadline)) {
        return LockTimeout();
    }
    return ExecuteFileLocked(filepath, cancellation, deadline);
}

LuaExecutionResult LuaEngine::ExecuteFileLocked(
    const std::string& filepath,
    const Mem::CancellationToken& cancellation,
    Deadline deadline) {
    if (!initialized || !L || !memService_) {
        return Failure("Lua engine not initialized");
    }
    if (cancellation &&
        cancellation->load(std::memory_order_acquire)) {
        return Failure("Lua execution cancelled");
    }
    if (DeadlineExpired(deadline)) {
        return Failure("Lua execution timed out");
    }

    std::error_code filesystemError;
    if (!std::filesystem::exists(filepath, filesystemError)) {
        if (filesystemError) {
            return Failure("Failed to inspect Lua file: " +
                           filesystemError.message());
        }
        return Failure("File not found: " + filepath);
    }

    LuaStackGuard stackGuard(L);
    LuaDiagnosticBinding diagnostics(filepath.c_str(), kGuiScriptModeName);
    int result = luaL_loadfile(L, filepath.c_str());
    if (result != LUA_OK) {
        const std::string error = GetLuaError(L);
        diagnostics.Failure(error);
        return Failure(error);
    }
    if (!PrepareInterruptibleChunk(
            L, -1,
            cancellation || deadline != (Deadline::max)())) {
        const std::string error = "Failed to make Lua chunk interruptible";
        diagnostics.Failure(error);
        return Failure(error);
    }
    if (cancellation &&
        cancellation->load(std::memory_order_acquire)) {
        const std::string error = "Lua execution cancelled";
        diagnostics.Failure(error);
        return Failure(error);
    }
    if (DeadlineExpired(deadline)) {
        const std::string error = "Lua execution timed out";
        diagnostics.Failure(error);
        return Failure(error);
    }

    LuaExecutionControl control{deadline, cancellation};
    LuaExecutionModeBinding mode(L, ExecutionMode::GuiScript);
    LuaOperationBinding operation(L, *memService_, cancellation, deadline);
    LuaHookBinding hook(L, control);
    result = ProtectedLuaCall(L, 0, 0);
    if (result != LUA_OK) {
        const std::string error =
            ExecutionFailureMessage(control, GetLuaError(L));
        diagnostics.Failure(error);
        return Failure(error);
    }
    const std::string interruption =
        ExecutionInterruptionMessage(control);
    if (!interruption.empty()) {
        diagnostics.Failure(interruption);
        return Failure(interruption);
    }

    const std::string filename =
        std::filesystem::path(filepath).filename().string();
    loadedScripts[filename] = filepath;
    diagnostics.Success();
    return Success();
}

LuaExecutionResult LuaEngine::ExecuteString(
    const std::string& code,
    const std::string& chunkName,
    Mem::CancellationToken cancellation,
    Deadline deadline) {
    std::unique_lock<std::timed_mutex> lock(mutex, std::defer_lock);
    if (!AcquireLock(lock, deadline)) {
        return LockTimeout();
    }
    return ExecuteStringLocked(code, chunkName, cancellation, deadline);
}

LuaExecutionResult LuaEngine::ExecuteStringLocked(
    const std::string& code,
    const std::string& chunkName,
    const Mem::CancellationToken& cancellation,
    Deadline deadline) {
    if (!initialized || !L || !memService_) {
        return Failure("Lua engine not initialized");
    }
    if (cancellation &&
        cancellation->load(std::memory_order_acquire)) {
        return Failure("Lua execution cancelled");
    }
    if (DeadlineExpired(deadline)) {
        return Failure("Lua execution timed out");
    }

    LuaStackGuard stackGuard(L);
    LuaDiagnosticBinding diagnostics(chunkName.c_str(), kGuiScriptModeName);
    int result = luaL_loadbuffer(
        L, code.data(), code.size(), chunkName.c_str());
    if (result != LUA_OK) {
        const std::string error = GetLuaError(L);
        diagnostics.Failure(error);
        return Failure(error);
    }
    if (!PrepareInterruptibleChunk(
            L, -1,
            cancellation || deadline != (Deadline::max)())) {
        const std::string error = "Failed to make Lua chunk interruptible";
        diagnostics.Failure(error);
        return Failure(error);
    }
    if (cancellation &&
        cancellation->load(std::memory_order_acquire)) {
        const std::string error = "Lua execution cancelled";
        diagnostics.Failure(error);
        return Failure(error);
    }
    if (DeadlineExpired(deadline)) {
        const std::string error = "Lua execution timed out";
        diagnostics.Failure(error);
        return Failure(error);
    }

    LuaExecutionControl control{deadline, cancellation};
    LuaExecutionModeBinding mode(L, ExecutionMode::GuiScript);
    LuaOperationBinding operation(L, *memService_, cancellation, deadline);
    LuaHookBinding hook(L, control);
    result = ProtectedLuaCall(L, 0, 0);
    if (result != LUA_OK) {
        const std::string error =
            ExecutionFailureMessage(control, GetLuaError(L));
        diagnostics.Failure(error);
        return Failure(error);
    }
    const std::string interruption =
        ExecutionInterruptionMessage(control);
    if (!interruption.empty()) {
        diagnostics.Failure(interruption);
        return Failure(interruption);
    }
    diagnostics.Success();
    return Success();
}

LuaExecutionResult LuaEngine::ExecuteStringCapture(
    const std::string& code,
    const std::string& chunkName,
    std::string& output,
    Deadline deadline) {
    output.clear();
    std::unique_lock<std::timed_mutex> lock(mutex, std::defer_lock);
    if (!AcquireLock(lock, deadline)) {
        return LockTimeout();
    }
    if (!initialized || !L || !memService_) {
        return Failure("Lua engine not initialized");
    }
    if (DeadlineExpired(deadline)) {
        return Failure("Lua execution timed out");
    }

    LuaStackGuard stackGuard(L);
    LuaDiagnosticBinding diagnostics(chunkName.c_str(), kIpcModeName);
    int result = luaL_loadbuffer(
        L, code.data(), code.size(), chunkName.c_str());
    if (result != LUA_OK) {
        const std::string error = GetLuaError(L);
        diagnostics.Failure(error);
        return Failure(error);
    }

    const int chunkIndex = lua_gettop(L);
    if (!PrepareInterruptibleChunk(L, chunkIndex, true)) {
        const std::string error =
            "Failed to make IPC Lua chunk interruptible";
        diagnostics.Failure(error);
        return Failure(error);
    }
    LuaCaptureContext* capture = PushIpcEnvironment(L, output);
    LuaCaptureDeactivation deactivateCapture(capture);
    if (lua_setfenv(L, chunkIndex) == 0) {
        const std::string error = "Failed to create IPC Lua environment";
        diagnostics.Failure(error);
        return Failure(error);
    }
    if (DeadlineExpired(deadline)) {
        const std::string error = "Lua execution timed out";
        diagnostics.Failure(error);
        return Failure(error);
    }

    LuaExecutionControl control{deadline, {}};
    LuaExecutionModeBinding mode(L, ExecutionMode::Ipc);
    LuaOperationBinding operation(L, *memService_, {}, deadline);
    LuaHookBinding hook(L, control);
    result = ProtectedLuaCall(L, 0, 0);
    capture->output = nullptr;
    if (result != LUA_OK) {
        const std::string error =
            ExecutionFailureMessage(control, GetLuaError(L));
        diagnostics.Failure(error);
        return Failure(error);
    }
    const std::string interruption =
        ExecutionInterruptionMessage(control);
    if (!interruption.empty()) {
        diagnostics.Failure(interruption);
        return Failure(interruption);
    }
    diagnostics.Success();
    return Success();
}

LuaExecutionResult LuaEngine::ReloadScript(
    const std::string& name,
    Mem::CancellationToken cancellation,
    Deadline deadline) {
    std::unique_lock<std::timed_mutex> lock(mutex, std::defer_lock);
    if (!AcquireLock(lock, deadline)) {
        return LockTimeout();
    }
    const auto found = loadedScripts.find(name);
    if (found == loadedScripts.end()) {
        return Failure("Script not loaded: " + name);
    }
    return ExecuteFileLocked(found->second, cancellation, deadline);
}

void LuaEngine::UnloadScript(const std::string& name) {
    std::lock_guard<std::timed_mutex> lock(mutex);
    loadedScripts.erase(name);
}

bool LuaEngine::IsScriptLoaded(const std::string& name) const {
    std::lock_guard<std::timed_mutex> lock(mutex);
    return loadedScripts.find(name) != loadedScripts.end();
}

LuaExecutionResult LuaEngine::InvokeGuiCallback(
    const std::string& luaFunctionName,
    int windowId) {
    std::unique_lock<std::timed_mutex> lock(mutex, std::defer_lock);
    if (!lock.try_lock()) {
        return Busy();
    }
    if (!initialized || !L || !memService_) {
        return Failure("Lua engine not initialized");
    }

    LuaStackGuard stackGuard(L);
    LuaDiagnosticBinding diagnostics(
        luaFunctionName.c_str(), kGuiFrameModeName);
    lua_getglobal(L, luaFunctionName.c_str());
    if (!lua_isfunction(L, -1)) {
        const std::string error =
            "Lua function not found: " + luaFunctionName;
        diagnostics.Failure(error);
        return Failure(error);
    }
    if (!PrepareInterruptibleChunk(L, -1, true)) {
        const std::string error =
            "Failed to make Lua callback interruptible";
        diagnostics.Failure(error);
        return Failure(error);
    }
    lua_pushinteger(L, windowId);

    const Deadline deadline =
        Clock::now() + std::chrono::milliseconds(kLuaGuiCallbackTimeoutMs);
    LuaExecutionControl control{deadline, {}};
    LuaExecutionModeBinding mode(L, ExecutionMode::GuiFrame);
    LuaOperationBinding operation(L, *memService_, {}, deadline);
    LuaHookBinding hook(L, control);
    LuaImGuiScopeBinding scopes(L);
    LuaImGuiErrorBoundary imguiBoundary;
    const int result = ProtectedLuaCall(L, 1, 0);

    std::string error;
    if (result != LUA_OK) {
        error = ExecutionFailureMessage(control, GetLuaError(L));
    } else {
        error = ExecutionInterruptionMessage(control);
    }

    const std::string openScopes = scopes.Finish();
    const std::string imguiErrors = imguiBoundary.Finish();
    error = AppendImGuiDiagnostics(
        std::move(error), openScopes, imguiErrors);
    if (!error.empty()) {
        diagnostics.Failure(error);
        return Failure(error);
    }

    diagnostics.Success();
    return Success();
}

void LuaEngine::AddScriptPath(const std::string& path) {
    std::lock_guard<std::timed_mutex> lock(mutex);
    AddScriptPathLocked(path);
}

void LuaEngine::AddScriptPathLocked(const std::string& path) {
    if (!L) {
        return;
    }

    LuaStackGuard stackGuard(L);
    lua_getglobal(L, "package");
    if (!lua_istable(L, -1)) {
        return;
    }
    lua_getfield(L, -1, "path");
    const char* current = lua_tostring(L, -1);
    std::string newPath = current ? current : "";
    newPath += ";" + path + "/?.lua;" + path + "/?/init.lua";
    lua_pop(L, 1);
    lua_pushlstring(L, newPath.data(), newPath.size());
    lua_setfield(L, -2, "path");
}

void LuaEngine::SetScriptBasePath(const std::string& path) {
    std::lock_guard<std::timed_mutex> lock(mutex);
    scriptBasePath = path;
    AddScriptPathLocked(path);
}

std::vector<std::string> LuaEngine::GetLoadedScripts() const {
    std::lock_guard<std::timed_mutex> lock(mutex);
    std::vector<std::string> result;
    result.reserve(loadedScripts.size());
    for (const auto& script : loadedScripts) {
        result.push_back(script.first);
    }
    return result;
}

LuaEngine::ExecutionMode LuaEngine::CurrentExecutionMode(lua_State* state) {
    if (!state) {
        return ExecutionMode::None;
    }
    lua_pushlightuserdata(state, &g_luaExecutionModeRegistryKey);
    lua_gettable(state, LUA_REGISTRYINDEX);
    const lua_Integer raw = lua_isnumber(state, -1)
        ? lua_tointeger(state, -1)
        : static_cast<lua_Integer>(ExecutionMode::None);
    lua_pop(state, 1);
    if (raw < static_cast<lua_Integer>(ExecutionMode::None) ||
        raw > static_cast<lua_Integer>(ExecutionMode::Ipc)) {
        return ExecutionMode::None;
    }
    return static_cast<ExecutionMode>(raw);
}

std::string LuaEngine::GetCrashDiagnostic() {
    return LuaDiagnostics::BuildCrashReport();
}

std::string LuaEngine::GetLuaError(lua_State* state) {
    if (!state || lua_gettop(state) == 0) {
        return "Unknown Lua error";
    }

    size_t length = 0;
    const char* error = lua_tolstring(state, -1, &length);
    std::string message;
    if (error) {
        message.assign(error, length);
    } else {
        message = "Lua error object of type ";
        message += lua_typename(state, lua_type(state, -1));
    }
    lua_pop(state, 1);
    return message;
}
