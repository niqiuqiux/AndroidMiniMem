#include "CEWindow.h"
#include "AppContext.h"
#include "ColorScheme.h"
#include "Gui.h"
#include "../imgui/imgui.h"
#include "../mem/IMemService.h"
#include "ModulesWindow.h"
#include "LogWindow.h"

#ifdef HAVE_LUAJIT
#include "LuaScriptWindow.h"
#endif
#include "ServerConnectWindow.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cctype>
#include <vector>

namespace {
bool loadAllProcesses(Mem::IMemService& service,
                      std::vector<Mem::ProcessInfo>& output,
                      std::string& error) {
    const Mem::OperationContext context = service.captureContext(false);
    std::vector<Mem::ProcessInfo> loaded;
    size_t offset = 0;
    while (true) {
        Mem::ProcessListRequest request;
        request.offset = offset;
        request.limit = Mem::kMaxProcessPageSize;
        auto result = service.listProcesses(context, request);
        if (!result.ok()) {
            error = result.error().message;
            return false;
        }
        auto& page = result.value();
        loaded.insert(loaded.end(), page.items.begin(), page.items.end());
        if (!page.nextOffset) {
            output.swap(loaded);
            error.clear();
            return true;
        }
        if (*page.nextOffset <= offset) {
            error = "process pagination did not advance";
            return false;
        }
        offset = *page.nextOffset;
    }
}
} // namespace

CEWindow::CEWindow(Mem::IMemService& service)
    : service_(service)
{
    name = "MiniMem";
}

unsigned int CEWindow::getWindowFlags() const
{
    return ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
}

void CEWindow::drawMenuBar()
{
    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu("文件"))
        {
            if (ImGui::MenuItem("打开进程", "Ctrl+P"))
                openProcessModal = true;
            ImGui::MenuItem("退出", nullptr, false);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("窗口"))
        {
            if (ImGui::MenuItem("Lua脚本管理器", "Ctrl+L"))
                openLuaScriptWindow();
            if (ImGui::MenuItem("模块列表"))
                openModulesWindow();
            ImGui::Separator();
            if (ImGui::MenuItem("服务器连接"))
                openServerConnectWindow();
            if (ImGui::MenuItem("日志"))
                openLogWindow();
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

void CEWindow::drawTopProcessBar()
{
    auto& ctx = AppContext::Get();
    ImGui::PushItemWidth(260.0f);
    if (ImGui::Button("选择进程")) { openProcessModal = true; }
    ImGui::SameLine();
    ImGui::TextDisabled("PID: %d", ctx.selectedPid.load());
    ImGui::PopItemWidth();
    ImGui::Separator();
}

void CEWindow::drawSelectedProcessBanner()
{
    auto& ctx = AppContext::Get();
    int pid = ctx.selectedPid.load();
    if (pid != 0) {
        const std::string processName = ctx.getSelectedName();
        ImGui::TextColored(ColorScheme::SuccessBright, "已附加: %s (PID %d)", processName.c_str(), pid);
        ImGui::SameLine();
        if (ImGui::Button("模块列表")) {
            openModulesWindow();
        }
    } else {
        ImGui::TextDisabled("未附加进程");
    }
}

void CEWindow::drawProcessSelectModal()
{
    static std::vector<Mem::ProcessInfo> list;
    static char filterText[256] = "";
    static bool listLoadAttempted = false;

    if (openProcessModal)
        ImGui::OpenPopup("进程列表");

    if (ImGui::BeginPopupModal("进程列表", &openProcessModal, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (ImGui::Button("刷新")) {
            list.clear();
            listLoadAttempted = true;
            std::string error;
            if (!loadAllProcesses(service_, list, error))
                Gui::log("获取进程列表失败: %s", error.c_str());
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(450.0f);
        ImGui::InputTextWithHint("##filter", "输入进程名称进行过滤...", filterText, sizeof(filterText));

        ImGui::Separator();
        if (!listLoadAttempted) {
            list.clear();
            listLoadAttempted = true;
            std::string error;
            if (!loadAllProcesses(service_, list, error))
                Gui::log("获取进程列表失败: %s", error.c_str());
        }

        if (ImGui::BeginChild("proc_modal", ImVec2(600, 400), ImGuiChildFlags_Borders))
        {
            if (ImGui::BeginTable("proc_modal_tbl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("进程名", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                // 应用过滤器
                std::string filterStr(filterText);
                for (size_t i = 0; i < filterStr.length(); i++) {
                    filterStr[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(filterStr[i])));
                }

                for (auto& it : list) {
                    if (filterStr.length() > 0) {
                        std::string nameLower = it.name;
                        for (size_t i = 0; i < nameLower.length(); i++) {
                            nameLower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(nameLower[i])));
                        }
                        if (nameLower.find(filterStr) == std::string::npos) {
                            continue;
                        }
                    }

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%d", it.pid);
                    ImGui::TableSetColumnIndex(1);
                    if (ImGui::Selectable(it.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                        Mem::OpenProcessRequest request;
                        request.pid = it.pid;
                        request.name = it.name;
                        auto result = service_.openProcess(
                            service_.captureContext(true), request);
                        if (!result.ok()) {
                            Gui::log("打开进程失败: %s",
                                     result.error().message.c_str());
                            continue;
                        }
                        Gui::log("进程已打开，句柄 %d",
                                 result.value().target.processHandle);
                        ImGui::CloseCurrentPopup();
                        openProcessModal = false;
                        listLoadAttempted = false;
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
        if (ImGui::Button("关闭")) {
            ImGui::CloseCurrentPopup();
            openProcessModal = false;
            listLoadAttempted = false;
        }
        ImGui::EndPopup();
    } else if (!openProcessModal) {
        listLoadAttempted = false;
    }
}

void CEWindow::onDraw()
{
    auto& ctx = AppContext::Get();
    ImGuiStyle& style = ImGui::GetStyle();
    float old_window_rounding = style.WindowRounding;
    float old_frame_rounding = style.FrameRounding;
    style.WindowRounding = 2.0f;
    style.FrameRounding = 2.0f;

    ImGui::SetNextWindowSize(ImVec2(800, 400), ImGuiCond_Once);

    // 全局键盘快捷键（不在文本输入框中时生效）
    if (!ImGui::GetIO().WantTextInput) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl) {
            if (ImGui::IsKeyPressed(ImGuiKey_P)) { openProcessModal = true; }
            else if (ImGui::IsKeyPressed(ImGuiKey_L)) { openLuaScriptWindow(); }
        }
    }

    drawMenuBar();
    drawSelectedProcessBanner();
    drawTopProcessBar();

    ImGui::Text("MiniMem 主控制面板");
    ImGui::Separator();

    const std::string processName = ctx.getSelectedName();
    ImGui::Text("当前进程: %s (PID: %d)", processName.c_str(), ctx.selectedPid.load());
    ImGui::Spacing();

    ImGui::TextWrapped("精简版前端：仅提供 服务器连接 / 进程选择 / 模块列表 / 日志 / Lua 脚本。"
                       "内存读写、断点、ELF符号等能力通过 MCP (IPC 端口 28100) 对外提供。");
    ImGui::Spacing();

    // IPC 服务端点（本前端的核心用途：供外部 MCP/AI 桥接）
    ImGui::TextColored(ColorScheme::InfoLight, "IPC 服务 (供 MCP 桥接): 127.0.0.1:28100");
    ImGui::SameLine();
    if (ImGui::SmallButton("复制端点"))
        ImGui::SetClipboardText("127.0.0.1:28100");
    ImGui::Spacing();

    ImGui::Text("可用功能窗口:");
    if (ImGui::Button("模块列表", ImVec2(200, 40))) {
        openModulesWindow();
    }
    ImGui::SameLine();
    if (ImGui::Button("Lua 脚本", ImVec2(200, 40))) {
        openLuaScriptWindow();
    }
    ImGui::SameLine();
    if (ImGui::Button("服务器连接", ImVec2(200, 40))) {
        openServerConnectWindow();
    }

    drawProcessSelectModal();

    style.WindowRounding = old_window_rounding;
    style.FrameRounding = old_frame_rounding;
}

// 窗口管理方法 — 使用 Gui::getOrCreate 简化
void CEWindow::openModulesWindow()
{
    auto* mw = Gui::getOrCreate<ModulesWindow>(service_);
    if (mw) mw->triggerAutoRefresh();
}

void CEWindow::openLuaScriptWindow()
{
#ifdef HAVE_LUAJIT
    Gui::getOrCreate<LuaScriptWindow>(service_);
#endif
}

void CEWindow::openServerConnectWindow()
{
    Gui::getOrCreate<ServerConnectWindow>(service_);
}

void CEWindow::openLogWindow()
{
    Gui::getOrCreate<LogWindow>();
}
