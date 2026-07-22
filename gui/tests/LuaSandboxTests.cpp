#include "../gui/Gui.h"
#include "../gui/Window.h"
#include "../imgui/imgui.h"
#include "../lua/LuaEngine.h"
#include "../mem/IMemService.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <stdexcept>
#include <thread>

void Window::draw() {}

namespace Gui {
std::list<std::unique_ptr<Window>> windows;
std::list<std::pair<std::string, int>> logs;
std::mutex logsMutex;

void addWindow(Window* window) {
    windows.emplace_back(window);
}

void postTask(std::function<void()> task) {
    if (task) {
        task();
    }
}
} // namespace Gui

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

template<typename T>
Mem::Result<T> unavailable() {
    return Mem::Result<T>::failure(
        Mem::ErrorCode::NotConnected, "test service is unavailable");
}

class StubMemService final : public Mem::IMemService {
public:
    Mem::OperationContext captureContext(bool includeTarget) const override {
        Mem::OperationContext context;
        context.connectionGeneration = 1;
        if (includeTarget) {
            context.target = Mem::TargetSnapshot{42, 7, 1, 1};
        }
        return context;
    }

    Mem::ConnectionSnapshot connectionSnapshot() const override {
        return {false, false, 1};
    }

    Mem::Result<Mem::Status> status(
        const Mem::OperationContext&) override {
        return unavailable<Mem::Status>();
    }
    Mem::Result<Mem::ServerVersion> serverVersion(
        const Mem::OperationContext&) override {
        return unavailable<Mem::ServerVersion>();
    }
    Mem::Result<Mem::MemoryTypeInfo> memoryType(
        const Mem::OperationContext&) override {
        return unavailable<Mem::MemoryTypeInfo>();
    }
    Mem::Result<Mem::ConnectionReceipt> connect(
        const Mem::OperationContext&,
        const Mem::ConnectRequest&) override {
        return unavailable<Mem::ConnectionReceipt>();
    }
    Mem::Result<Mem::DisconnectReceipt> disconnect(
        const Mem::OperationContext&) override {
        return unavailable<Mem::DisconnectReceipt>();
    }
    Mem::Result<Mem::DriverInitializationReceipt> initializeDriver(
        const Mem::OperationContext&,
        const Mem::DriverInitializeRequest&) override {
        return unavailable<Mem::DriverInitializationReceipt>();
    }
    Mem::Result<Mem::ProcessPage> listProcesses(
        const Mem::OperationContext&,
        const Mem::ProcessListRequest&) override {
        return unavailable<Mem::ProcessPage>();
    }
    Mem::Result<Mem::OpenProcessResult> openProcess(
        const Mem::OperationContext&,
        const Mem::OpenProcessRequest&) override {
        return unavailable<Mem::OpenProcessResult>();
    }
    Mem::Result<Mem::ModulePage> listModules(
        const Mem::OperationContext&,
        const Mem::ModuleListRequest&) override {
        return unavailable<Mem::ModulePage>();
    }
    Mem::Result<Mem::ResolvedModule> resolveModule(
        const Mem::OperationContext&,
        const Mem::ModuleResolveRequest&) override {
        return unavailable<Mem::ResolvedModule>();
    }
    Mem::Result<Mem::PointerResolution> resolvePointer(
        const Mem::OperationContext&,
        const Mem::PointerResolveRequest&) override {
        return unavailable<Mem::PointerResolution>();
    }
    Mem::Result<Mem::MemoryBlock> readMemory(
        const Mem::OperationContext&,
        const Mem::MemoryReadRequest& request) override {
        if (request.address == 0xC0FFEE) {
            throw std::runtime_error("synthetic host API failure");
        }
        return unavailable<Mem::MemoryBlock>();
    }
    Mem::Result<Mem::MemoryBatch> readMemoryBatch(
        const Mem::OperationContext&,
        const Mem::MemoryBatchReadRequest&) override {
        return unavailable<Mem::MemoryBatch>();
    }
    Mem::Result<Mem::WriteReceipt> writeMemory(
        const Mem::OperationContext&,
        const Mem::MemoryWriteRequest&) override {
        return unavailable<Mem::WriteReceipt>();
    }
    Mem::Result<Mem::BreakpointMutationReceipt> setBreakpoint(
        const Mem::OperationContext&,
        const Mem::BreakpointSetRequest&) override {
        return unavailable<Mem::BreakpointMutationReceipt>();
    }
    Mem::Result<Mem::BreakpointMutationReceipt> removeBreakpoint(
        const Mem::OperationContext&,
        const Mem::BreakpointAddressRequest&) override {
        return unavailable<Mem::BreakpointMutationReceipt>();
    }
    Mem::Result<Mem::BreakpointMutationReceipt> suspendBreakpoint(
        const Mem::OperationContext&,
        const Mem::BreakpointAddressRequest&) override {
        return unavailable<Mem::BreakpointMutationReceipt>();
    }
    Mem::Result<Mem::BreakpointMutationReceipt> resumeBreakpoint(
        const Mem::OperationContext&,
        const Mem::BreakpointAddressRequest&) override {
        return unavailable<Mem::BreakpointMutationReceipt>();
    }
    Mem::Result<Mem::BreakpointHitBatch> breakpointHits(
        const Mem::OperationContext&,
        const Mem::BreakpointHitBatchRequest&) override {
        return unavailable<Mem::BreakpointHitBatch>();
    }
    Mem::Result<Mem::SymbolTable> loadSymbolTable(
        const Mem::OperationContext&,
        const Mem::SymbolTableRequest&) override {
        return unavailable<Mem::SymbolTable>();
    }
    Mem::Result<Mem::SymbolPage> listSymbols(
        const Mem::OperationContext&,
        const Mem::SymbolListRequest&) override {
        return unavailable<Mem::SymbolPage>();
    }
    Mem::Result<Mem::ResolvedSymbol> resolveSymbol(
        const Mem::OperationContext&,
        const Mem::SymbolResolveRequest&) override {
        return unavailable<Mem::ResolvedSymbol>();
    }
};

LuaExecutionResult executeIpc(const std::string& code,
                              std::string& output,
                              int timeoutMs = 1000) {
    return LuaEngine::GetInstance().ExecuteStringCapture(
        code, "=sandbox_test", output,
        LuaEngine::Clock::now() + std::chrono::milliseconds(timeoutMs));
}

void testSandboxAndCapture() {
    std::string output;
    const LuaExecutionResult result = executeIpc(R"lua(
        assert(type(os) == "table" and type(io) == "table")
        assert(type(os.execute) == "function" and type(io.open) == "function")
        assert(package == nil and debug == nil)
        assert(coroutine == nil and require == nil and dofile == nil)
        assert(load == nil and loadfile == nil and loadstring == nil)
        assert(getfenv == nil and setfenv == nil and collectgarbage == nil)
        assert(ffi == nil and jit == nil and imgui == nil)
        assert(type(math) == "table" and type(string) == "table")
        assert(type(table) == "table" and type(mem) == "table")
        print("sandbox", true, 42)
    )lua", output);
    check(result.success, "IPC sandbox should execute safe Lua code");
    check(output == "sandbox\ttrue\t42\n",
          "IPC print output should be captured without changing globals");
}

void testEnvironmentIsolationAndCaptureLifetime() {
    std::string output;
    LuaExecutionResult result = executeIpc(R"lua(
        ipc_leak = 123
        math.ipc_leak = 456
        mem.saved_print = print
        local retained = print
        retained("retained", nil, false)
        return retained
    )lua", output);
    check(result.success && output == "retained\tnil\tfalse\n",
          "captured print should remain valid for the whole IPC call");

    output.clear();
    result = executeIpc(R"lua(
        assert(ipc_leak == nil)
        assert(math.ipc_leak == nil)
        assert(mem.saved_print == nil)
        print("isolated")
    )lua", output);
    check(result.success && output == "isolated\n",
          "IPC globals and copied API tables must not leak between calls");

    const LuaExecutionResult collection =
        LuaEngine::GetInstance().ExecuteString("collectgarbage('collect')");
    check(collection.success,
          "deactivated capture userdata should be safe to collect");
}

void testErrorStackCleanup() {
    std::string output;
    LuaExecutionResult result = executeIpc("error({ reason = 'test' })", output);
    check(!result.success &&
              result.error.find("Lua error object of type table") !=
                  std::string::npos &&
              result.error.find("stack traceback:") != std::string::npos,
          "non-string Lua errors should include a traceback and be popped");

    result = executeIpc("print('after_error')", output);
    check(result.success && output == "after_error\n",
          "a Lua error must not contaminate the next execution stack");
}

void testLuaTracebackAndHostExceptionBoundary() {
    std::string output;
    LuaExecutionResult result = executeIpc(R"lua(
        local function inner()
            error("nested Lua failure")
        end
        local function outer()
            inner()
        end
        outer()
    )lua", output);
    check(!result.success &&
              result.error.find("nested Lua failure") != std::string::npos &&
              result.error.find("stack traceback:") != std::string::npos &&
              result.error.find("sandbox_test") != std::string::npos,
          "nested Lua errors should report the chunk and Lua call stack");

    const std::string diagnostic = LuaEngine::GetCrashDiagnostic();
    check(diagnostic.find("sandbox_test") != std::string::npos &&
              diagnostic.find("nested Lua failure") != std::string::npos,
          "the crash snapshot should retain the latest Lua failure");

    result = executeIpc("mem.read(0xC0FFEE, 4)", output);
    check(!result.success &&
              result.error.find("mem.read") != std::string::npos &&
              result.error.find("synthetic host API failure") !=
                  std::string::npos &&
              result.error.find("stack traceback:") != std::string::npos,
          "C++ exceptions from host APIs should become Lua errors");
}

void testImGuiExecutionGuard() {
    const LuaExecutionResult result = LuaEngine::GetInstance().ExecuteString(
        "imgui.text('must not draw outside a GUI frame')");
    check(!result.success &&
              result.error.find("not available in this Lua execution context") !=
                  std::string::npos,
          "ImGui drawing should be rejected outside a GUI frame callback");
}

void testGuiCallbackTimeout() {
    LuaExecutionResult result = LuaEngine::GetInstance().ExecuteString(R"lua(
        function sandbox_hanging_callback()
            while true do end
        end
    )lua");
    check(result.success, "test GUI callback should be registered");
    if (!result.success) {
        return;
    }

    const auto started = LuaEngine::Clock::now();
    result = LuaEngine::GetInstance().InvokeGuiCallback(
        "sandbox_hanging_callback", 1);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        LuaEngine::Clock::now() - started);
    check(!result.success && result.error == "Lua execution timed out",
          "a hanging GUI callback should be interrupted");
    check(elapsed < std::chrono::seconds(2),
          "GUI callback timeout should keep the client responsive");
}

void testGuiCallbackScopeRecovery() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* fontPixels = nullptr;
    int fontWidth = 0;
    int fontHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(
        &fontPixels, &fontWidth, &fontHeight);
    ImGui::NewFrame();
    ImGui::Begin("Lua scope recovery host");

    LuaExecutionResult result = LuaEngine::GetInstance().ExecuteString(R"lua(
        function sandbox_extra_end_callback()
            imgui["end"]()
        end
        function sandbox_unclosed_window_callback()
            imgui.begin("Unclosed Lua window", true)
            error("failure after imgui.begin")
        end
    )lua", "=imgui_scope_test");
    if (!result.success) {
        std::cerr << "ImGui callback registration error: "
                  << result.error << '\n';
    }
    check(result.success, "test ImGui callbacks should be registered");

    if (result.success) {
        result = LuaEngine::GetInstance().InvokeGuiCallback(
            "sandbox_extra_end_callback", 1);
        check(!result.success &&
                  result.error.find("without a matching imgui.begin") !=
                      std::string::npos &&
                  result.error.find("stack traceback:") != std::string::npos,
              "Lua must not be able to close the host ImGui window");

        result = LuaEngine::GetInstance().InvokeGuiCallback(
            "sandbox_unclosed_window_callback", 1);
        check(!result.success &&
                  result.error.find("failure after imgui.begin") !=
                      std::string::npos &&
                  result.error.find("Unclosed Lua ImGui scopes") !=
                      std::string::npos &&
                  result.error.find("Missing End()") != std::string::npos,
              "Lua callback errors should recover unclosed ImGui scopes");
    }

    ImGui::End();
    ImGui::EndFrame();
    ImGui::DestroyContext();
}

void testDeadlineIncludesEngineLockWait() {
    LuaExecutionResult holderResult;
    std::thread holder([&holderResult] {
        holderResult = LuaEngine::GetInstance().ExecuteString(
            "log('sandbox_lock_acquired'); sleep(250)");
    });

    bool lockAcquired = false;
    const auto observationDeadline =
        LuaEngine::Clock::now() + std::chrono::seconds(1);
    while (LuaEngine::Clock::now() < observationDeadline) {
        {
            std::lock_guard<std::mutex> lock(Gui::logsMutex);
            for (const auto& entry : Gui::logs) {
                if (entry.first == "sandbox_lock_acquired") {
                    lockAcquired = true;
                    break;
                }
            }
        }
        if (lockAcquired) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(lockAcquired, "test worker should acquire the Lua engine lock");

    if (lockAcquired) {
        std::string output;
        const auto started = LuaEngine::Clock::now();
        const LuaExecutionResult result = executeIpc("print('late')", output, 50);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                LuaEngine::Clock::now() - started);
        check(!result.success &&
                  result.error ==
                      "Lua execution timed out while waiting for the engine",
              "Lua mutex waiting should consume the same deadline budget");
        check(elapsed < std::chrono::milliseconds(200),
              "Lua lock timeout should not wait for the running script");
    }

    holder.join();
    check(holderResult.success,
          "the script holding the Lua engine lock should complete");
}

void testCancellationInterruptsSleep() {
    auto cancellation = std::make_shared<std::atomic<bool>>(false);
    LuaExecutionResult result;
    const auto started = LuaEngine::Clock::now();
    std::thread worker([&result, cancellation] {
        result = LuaEngine::GetInstance().ExecuteString(
            "sleep(5000)", "=cancellation_test", cancellation);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    cancellation->store(true, std::memory_order_release);
    worker.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        LuaEngine::Clock::now() - started);
    check(!result.success && result.error == "Lua execution cancelled",
          "GUI cancellation should interrupt Lua sleep");
    check(elapsed < std::chrono::seconds(1),
          "Lua cancellation should be observed promptly");
}

void testPureLuaTimeout() {
    std::string output;
    const auto started = LuaEngine::Clock::now();
    const LuaExecutionResult result =
        executeIpc("while true do end", output, 100);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        LuaEngine::Clock::now() - started);
    check(!result.success && result.error == "Lua execution timed out",
          "pure Lua infinite loops should respect the absolute deadline");
    check(elapsed < std::chrono::seconds(2),
          "Lua timeout should stop execution promptly");
}

} // namespace

int main() {
    StubMemService service;
    auto& engine = LuaEngine::GetInstance();
    engine.Shutdown();
    const LuaExecutionResult initialization = engine.Initialize(service);
    check(initialization.success, "Lua engine should initialize for tests");
    if (initialization.success) {
        testSandboxAndCapture();
        testEnvironmentIsolationAndCaptureLifetime();
        testErrorStackCleanup();
        testLuaTracebackAndHostExceptionBoundary();
        testImGuiExecutionGuard();
        testGuiCallbackTimeout();
        testGuiCallbackScopeRecovery();
        testDeadlineIncludesEngineLockWait();
        testCancellationInterruptsSleep();
        testPureLuaTimeout();
    }
    engine.Shutdown();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "Lua sandbox tests passed\n";
    return 0;
}
