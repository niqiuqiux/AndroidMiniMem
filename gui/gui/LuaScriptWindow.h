#pragma once

#include "Window.h"
#include <vector>
#include <string>
#include <filesystem>

class LuaScriptWindow : public Window {
public:
    LuaScriptWindow();
    ~LuaScriptWindow();

    void onDraw() override;
    unsigned int getWindowFlags() const override;

private:
    void drawScriptList();
    void drawScriptOutput();
    void drawScriptContent();
    void drawScriptControls();
    void refreshScriptList();
    void drawScriptBrowserPopup();
    void refreshBrowserFiles();
    void executeScript(const std::string& filepath);
    void stopScript();
    void reloadScript(const std::string& name);
    void saveCurrentScript();

    // 脚本列表
    std::vector<std::string> scriptFiles;
    std::vector<std::string> scriptFilePaths;  // 存储完整路径
    std::vector<bool> scriptSelected;
    int selectedScriptIndex = -1;

    // 脚本输出日志
    std::vector<std::string> outputLog;
    static const int MAX_LOG_LINES = 1000;

    // 脚本执行状态
    bool scriptRunning = false;
    std::string currentScript;
    std::string currentScriptContent;
    bool currentScriptDirty = false;

    // 脚本目录
    std::string scriptDirectory = "./scripts";

    // 使用 ImGui 的脚本浏览弹窗状态
    bool showScriptBrowser = false;
    char browserDirectoryBuf[260] = "./scripts";
    std::vector<std::filesystem::path> browserLuaFiles;
    int browserSelectedIndex = -1;
};

