#include "LogWindow.h"
#include "Gui.h"
#include "../imgui/imgui.h"
#include <string>

LogWindow::LogWindow() {
    name = "Logs";
}

unsigned int LogWindow::getWindowFlags() const
{
    return ImGuiWindowFlags_NoDocking;
}

void LogWindow::onDraw() {
    const auto logs = Gui::getLogsSnapshot();
    if (logs.empty()) {
        ImGui::TextDisabled("暂无日志");
    } else {
        for (const auto& [msg, dup] : logs) {
            if (dup > 0)
                ImGui::TextUnformatted((msg + "  (x" + std::to_string(dup + 1) + ")").c_str());
            else
                ImGui::TextUnformatted(msg.c_str());
        }
    }
}

