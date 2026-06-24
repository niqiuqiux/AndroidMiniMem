#include "LogWindow.h"
#include "Gui.h"
#include "../imgui/imgui.h"
#include <string>
#include <cctype>

LogWindow::LogWindow() {
    name = "日志";
}

unsigned int LogWindow::getWindowFlags() const
{
    return ImGuiWindowFlags_NoDocking;
}

namespace {
// 大小写不敏感子串匹配；needle 为空视为全部匹配
bool containsCI(const std::string& haystack, const char* needle) {
    if (!needle || needle[0] == '\0')
        return true;
    std::string h = haystack;
    std::string n = needle;
    auto lower = [](std::string& s) {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    lower(h);
    lower(n);
    return h.find(n) != std::string::npos;
}

// 把一条日志格式化为带重复计数的显示文本
std::string formatLine(const std::string& msg, int dup) {
    if (dup > 0)
        return msg + "  (x" + std::to_string(dup + 1) + ")";
    return msg;
}
} // namespace

void LogWindow::onDraw() {
    const auto logs = Gui::getLogsSnapshot();

    // ===== 工具栏 =====
    if (ImGui::Button("清空")) {
        Gui::clearLogs();
    }
    ImGui::SameLine();
    if (ImGui::Button("复制")) {
        std::string all;
        for (const auto& [msg, dup] : logs) {
            if (!containsCI(msg, filterBuf))
                continue;
            all += formatLine(msg, dup);
            all += '\n';
        }
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    ImGui::Checkbox("自动滚动", &autoScroll);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##logfilter", "过滤...", filterBuf, IM_ARRAYSIZE(filterBuf));

    // 统计（过滤时显示 已显示/总数）
    int shown = 0;
    for (const auto& [msg, dup] : logs)
        if (containsCI(msg, filterBuf))
            ++shown;
    ImGui::SameLine();
    if (filterBuf[0] != '\0')
        ImGui::TextDisabled("显示 %d / %d 条", shown, (int)logs.size());
    else
        ImGui::TextDisabled("共 %d 条", (int)logs.size());

    ImGui::Separator();

    // ===== 日志列表（带边框滚动区，支持横向滚动）=====
    if (ImGui::BeginChild("log_scroll", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        if (logs.empty()) {
            ImGui::TextDisabled("暂无日志");
        } else {
            for (const auto& [msg, dup] : logs) {
                if (!containsCI(msg, filterBuf))
                    continue;
                ImGui::TextUnformatted(formatLine(msg, dup).c_str());
            }
            // 仅当用户已停在底部时才自动跟随，避免打断向上翻看
            if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
}
