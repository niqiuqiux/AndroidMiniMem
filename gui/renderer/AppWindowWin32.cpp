#include "AppWindow.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "DX12Renderer.h"
#include "StyleSetup.h"
#include "../imgui/imgui.h"
#include "../imgui/backends/imgui_impl_win32.h"
#include "../imgui/backends/imgui_impl_dx12.h"

#include <windows.h>

#include <iterator>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

struct AppWindow::Impl {
    DX12Renderer renderer;
    HWND hwnd = nullptr;
    WNDCLASSEXW windowClass = {};

    static Impl* fromWindow(HWND hwnd) {
        return reinterpret_cast<Impl*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    static LRESULT WINAPI wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
            return true;
        }

        Impl* self = fromWindow(hwnd);
        switch (msg) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_SIZE:
            if (self != nullptr && wParam != SIZE_MINIMIZED) {
                self->renderer.onResize(static_cast<UINT>(LOWORD(lParam)),
                                        static_cast<UINT>(HIWORD(lParam)));
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) {
                return 0;
            }
            break;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        }
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    bool init(const char* title, int width, int height) {
        ImGui_ImplWin32_EnableDpiAwareness();
        float mainScale = ImGui_ImplWin32_GetDpiScaleForMonitor(
            ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

        windowClass = {
            sizeof(windowClass), CS_CLASSDC, wndProc, 0L, 0L,
            GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
            L"MiniMem", nullptr
        };
        ::RegisterClassExW(&windowClass);

        wchar_t wideTitle[256] = {};
        ::MultiByteToWideChar(CP_UTF8, 0, title, -1, wideTitle,
                              static_cast<int>(std::size(wideTitle)));

        hwnd = ::CreateWindowW(windowClass.lpszClassName, wideTitle,
                               WS_OVERLAPPEDWINDOW, 100, 100,
                               static_cast<int>(width * mainScale),
                               static_cast<int>(height * mainScale),
                               nullptr, nullptr, windowClass.hInstance, this);
        if (hwnd == nullptr) {
            shutdown();
            return false;
        }

        if (!renderer.init(hwnd)) {
            shutdown();
            return false;
        }

        ::ShowWindow(hwnd, SW_SHOWDEFAULT);
        ::UpdateWindow(hwnd);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        io.ConfigViewportsNoAutoMerge = true;
        io.ConfigViewportsNoTaskBarIcon = false;
        ImGui::StyleColorsDark();

        StyleSetup::setupImGuiStyle(mainScale);

        ImGui_ImplWin32_Init(hwnd);

        ImGui_ImplDX12_InitInfo initInfo = {};
        initInfo.Device = renderer.getDevice();
        initInfo.CommandQueue = renderer.getCommandQueue();
        initInfo.NumFramesInFlight = APP_NUM_FRAMES_IN_FLIGHT;
        initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
        initInfo.UserData = this;
        initInfo.SrvDescriptorHeap = renderer.getSrvHeap();
        initInfo.SrvDescriptorAllocFn =
            [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
               D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
                auto* self = static_cast<Impl*>(info->UserData);
                self->renderer.getSrvHeapAlloc().Alloc(outCpu, outGpu);
            };
        initInfo.SrvDescriptorFreeFn =
            [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
               D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
                auto* self = static_cast<Impl*>(info->UserData);
                self->renderer.getSrvHeapAlloc().Free(cpu, gpu);
            };
        ImGui_ImplDX12_Init(&initInfo);

        StyleSetup::loadFonts(io);
        return true;
    }

    void run(const std::function<void()>& frameCallback) {
        bool done = false;
        while (!done) {
            MSG msg;
            while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
                ::TranslateMessage(&msg);
                ::DispatchMessage(&msg);
                if (msg.message == WM_QUIT) {
                    done = true;
                }
            }
            if (done) {
                break;
            }

            if (!renderer.beginFrame()) {
                continue;
            }

            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            frameCallback();

            renderer.endFrame();
        }
    }

    void requestClose() {
        if (hwnd != nullptr) {
            ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
    }

    void shutdown() {
        renderer.waitForPendingOperations();

        if (ImGui::GetCurrentContext() != nullptr) {
            ImGui_ImplDX12_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }

        renderer.shutdown();
        if (hwnd != nullptr) {
            ::DestroyWindow(hwnd);
            hwnd = nullptr;
        }
        if (windowClass.lpszClassName != nullptr) {
            ::UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
            windowClass = {};
        }
    }
};

AppWindow::AppWindow() : impl_(std::make_unique<Impl>()) {
}

AppWindow::~AppWindow() {
    shutdown();
}

bool AppWindow::init(const char* title, int width, int height) {
    return impl_->init(title, width, height);
}

void AppWindow::run(const std::function<void()>& frameCallback) {
    impl_->run(frameCallback);
}

void AppWindow::requestClose() {
    impl_->requestClose();
}

void AppWindow::shutdown() {
    if (impl_) {
        impl_->shutdown();
    }
}

#endif
