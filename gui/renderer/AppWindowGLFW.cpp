#include "AppWindow.h"

#ifndef _WIN32

#include "StyleSetup.h"
#include "../imgui/imgui.h"
#include "../imgui/backends/imgui_impl_glfw.h"
#include "../imgui/backends/imgui_impl_opengl3.h"

#include <cstdio>

#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>

namespace {

void glfwErrorCallback(int error, const char* description) {
    std::fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

} // namespace

struct AppWindow::Impl {
    GLFWwindow* window = nullptr;
    const char* glslVersion = "#version 130";
    ImVec4 clearColor = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);
    bool closeRequested = false;

    static void windowCloseCallback(GLFWwindow* window) {
        auto* self = static_cast<Impl*>(glfwGetWindowUserPointer(window));
        if (self != nullptr) {
            self->closeRequested = true;
        }
        glfwHideWindow(window);
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    }

    bool init(const char* title, int width, int height) {
        glfwSetErrorCallback(glfwErrorCallback);
        if (!glfwInit()) {
            return false;
        }

#if defined(__APPLE__)
        glslVersion = "#version 150";
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

        float mainScale = 1.0f;
#if GLFW_VERSION_MAJOR > 3 || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3)
        if (GLFWmonitor* monitor = glfwGetPrimaryMonitor()) {
            mainScale = ImGui_ImplGlfw_GetContentScaleForMonitor(monitor);
        }
#endif

        window = glfwCreateWindow(static_cast<int>(width * mainScale),
                                  static_cast<int>(height * mainScale),
                                  title, nullptr, nullptr);
        if (window == nullptr) {
            shutdown();
            return false;
        }
        glfwSetWindowUserPointer(window, this);
        glfwSetWindowCloseCallback(window, windowCloseCallback);

        glfwMakeContextCurrent(window);
        glfwSwapInterval(1);

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

        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init(glslVersion);

        StyleSetup::loadFonts(io);
        return true;
    }

    void run(const std::function<void()>& frameCallback) {
        while (window != nullptr && !closeRequested && !glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (closeRequested || glfwWindowShouldClose(window)) {
                break;
            }
            if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0) {
                ImGui_ImplGlfw_Sleep(10);
                continue;
            }

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            frameCallback();

            ImGui::Render();
            int displayW = 0;
            int displayH = 0;
            glfwGetFramebufferSize(window, &displayW, &displayH);
            glViewport(0, 0, displayW, displayH);
            glClearColor(clearColor.x * clearColor.w,
                         clearColor.y * clearColor.w,
                         clearColor.z * clearColor.w,
                         clearColor.w);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            ImGuiIO& io = ImGui::GetIO();
            if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
                GLFWwindow* backupContext = glfwGetCurrentContext();
                ImGui::UpdatePlatformWindows();
                ImGui::RenderPlatformWindowsDefault();
                glfwMakeContextCurrent(backupContext);
            }

            glfwSwapBuffers(window);
        }
    }

    void requestClose() {
        if (window != nullptr) {
            closeRequested = true;
            glfwHideWindow(window);
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
    }

    void shutdown() {
        if (ImGui::GetCurrentContext() != nullptr) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
        }

        if (window != nullptr) {
            glfwSetWindowCloseCallback(window, nullptr);
            glfwSetWindowUserPointer(window, nullptr);
            glfwDestroyWindow(window);
            window = nullptr;
        }
        glfwTerminate();
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
