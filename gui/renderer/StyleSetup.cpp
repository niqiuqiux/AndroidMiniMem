#include "StyleSetup.h"

void StyleSetup::setupImGuiStyle(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGuiIO& io = ImGui::GetIO();

    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    io.ConfigDpiScaleFonts = true;
    io.ConfigDpiScaleViewports = true;

    // 圆角和间距
    style.WindowRounding = 15.0f;
    style.ChildRounding = 12.0f;
    style.FrameRounding = 8.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;

    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 6.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.TouchExtraPadding = ImVec2(0.0f, 0.0f);
    style.IndentSpacing = 20.0f;
    style.ScrollbarSize = 14.0f;
    style.GrabMinSize = 12.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 1.0f;

    style.WindowMinSize = ImVec2(200.0f, 100.0f);
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.0f);
    style.DisplaySafeAreaPadding = ImVec2(3.0f, 3.0f);

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }
}

void StyleSetup::loadFonts(ImGuiIO& io) {
    ImFont* font = nullptr;
    font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\msyh.ttc", 16.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
    if (!font) {
        font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\SourceHanSansCN-Regular.otf", 16.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
        if (!font) {
            font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\simsun.ttc", 16.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
            if (!font) {
                font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf", 16.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
                if (!font) {
                    io.Fonts->AddFontDefault();
                    ImFontConfig config;
                    config.MergeMode = true;
                    io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\msyh.ttc", 16.0f, &config, io.Fonts->GetGlyphRangesChineseFull());
                }
            }
        }
    }
}
