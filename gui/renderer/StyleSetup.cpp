#include "StyleSetup.h"

#include <array>

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
    constexpr float fontSize = 16.0f;
#ifdef _WIN32
    const std::array<const char*, 4> fontPaths = {
        "c:\\Windows\\Fonts\\msyh.ttc",
        "c:\\Windows\\Fonts\\SourceHanSansCN-Regular.otf",
        "c:\\Windows\\Fonts\\simsun.ttc",
        "c:\\Windows\\Fonts\\ArialUni.ttf",
    };
#else
    const std::array<const char*, 8> fontPaths = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.otf",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansSC-Regular.ttf",
        "/usr/share/fonts/opentype/source-han-sans/SourceHanSansCN-Regular.otf",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
#endif

    for (const char* path : fontPaths) {
        if (io.Fonts->AddFontFromFileTTF(path, fontSize, nullptr,
                                         io.Fonts->GetGlyphRangesChineseFull())) {
            return;
        }
    }

    io.Fonts->AddFontDefault();
}
