#include "Window.h"
#include "AppContext.h"
#include "../imgui/imgui.h"

void Window::draw()
{
    if (!pOpen)
        return;

    ImGuiWindowFlags flags = static_cast<ImGuiWindowFlags>(getWindowFlags());
    if (shouldBringToFront) {
        ImGui::SetNextWindowFocus();
        shouldBringToFront = false;
    }

    if (ImGui::Begin(name.c_str(), &pOpen, flags))
        onDraw();
    ImGui::End();
}

void Window::operator()()
{
    draw();
}

bool Window::hasProcess() const {
    return AppContext::Get().hasProcess();
}

int Window::currentPid() const {
    return AppContext::Get().selectedPid.load();
}

std::string Window::currentProcessName() const {
    return AppContext::Get().getSelectedName();
}

bool Window::shouldRefresh(float& timer, float interval) {
    float dt = ImGui::GetIO().DeltaTime;
    timer += dt;
    if (timer >= interval) {
        timer = 0.0f;
        return true;
    }
    return false;
}
