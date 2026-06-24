#include "LuaScriptWindow.h"
#include "ColorScheme.h"
#include "../lua/LuaEngine.h"
#include "../imgui/imgui.h"
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <cfloat>

LuaScriptWindow::LuaScriptWindow() {
    name = "Lua脚本管理器";
    refreshScriptList();
}

unsigned int LuaScriptWindow::getWindowFlags() const
{
    return ImGuiWindowFlags_NoDocking;
}

LuaScriptWindow::~LuaScriptWindow() {
}

void LuaScriptWindow::onDraw() {
    drawScriptControls();
    ImGui::Separator();

    // 整体改为左右两个 Child：左侧（列表+日志），右侧（脚本内容）
    float leftWidth = 320.0f;

    // 左侧：上半部分脚本列表，下半部分脚本日志
    ImGui::BeginChild("LeftPanel", ImVec2(leftWidth, 0), true);
    {
        ImVec2 leftSize = ImGui::GetContentRegionAvail();
        float listHeight = leftSize.y * 0.5f;

        ImGui::BeginChild("ScriptListRegion", ImVec2(0, listHeight), true);
        drawScriptList();
        ImGui::EndChild();

        ImGui::Separator();

        ImGui::BeginChild("ScriptLogRegion", ImVec2(0, 0), true);
        drawScriptOutput();
        ImGui::EndChild();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // 右侧：脚本内容展示（占满右侧全部高度）
    ImGui::BeginChild("RightPanel", ImVec2(0, 0), true);
    {
        drawScriptContent();
    }
    ImGui::EndChild();

    // 脚本浏览弹窗（基于 ImGui，实现跨平台）
    if (showScriptBrowser) {
        drawScriptBrowserPopup();
    }
}

void LuaScriptWindow::drawScriptControls() {
    if (ImGui::Button("刷新列表")) {
        refreshScriptList();
    }
    ImGui::SameLine();

    if (ImGui::Button("选择文件")) {
        // 打开基于 ImGui 的脚本浏览窗口，避免使用系统原生文件选择对话框
        showScriptBrowser = true;
        // 默认浏览目录为当前脚本目录
        std::snprintf(browserDirectoryBuf, sizeof(browserDirectoryBuf), "%s", scriptDirectory.c_str());
        refreshBrowserFiles();
        ImGui::OpenPopup("选择Lua脚本");
    }
    ImGui::SameLine();

    if (scriptRunning) {
        ImGui::PushStyleColor(ImGuiCol_Button, ColorScheme::ErrorBright);
        if (ImGui::Button("停止脚本")) {
            stopScript();
        }
        ImGui::PopStyleColor();
    } else {
        if (ImGui::Button("执行选中")) {
            if (selectedScriptIndex >= 0 && selectedScriptIndex < scriptFiles.size()) {
                std::string filepath;
                if (selectedScriptIndex < static_cast<int>(scriptFilePaths.size()) && 
                    !scriptFilePaths[selectedScriptIndex].empty()) {
                    // 使用完整路径（外部文件）
                    filepath = scriptFilePaths[selectedScriptIndex];
                } else {
                    // 使用相对路径（默认目录中的文件）
                    filepath = scriptDirectory + "/" + scriptFiles[selectedScriptIndex];
                }
                executeScript(filepath);
            }
        }
    }
    ImGui::SameLine();

    if (ImGui::Button("重载选中")) {
        if (selectedScriptIndex >= 0 && selectedScriptIndex < scriptFiles.size()) {
            reloadScript(scriptFiles[selectedScriptIndex]);
        }
    }

    ImGui::SameLine();
    ImGui::Text("脚本目录: %s", scriptDirectory.c_str());
}

void LuaScriptWindow::drawScriptList() {
    ImGui::Text("脚本列表 (%d)", static_cast<int>(scriptFiles.size()));
    ImGui::Separator();

    if (scriptFiles.empty()) {
        ImGui::TextColored(ColorScheme::TextSecondary, "没有找到脚本文件");
        ImGui::Text("请将.lua文件放在 %s 目录下", scriptDirectory.c_str());
        ImGui::Text("或点击'选择文件'按钮从其他目录选择");
        return;
    }

    for (size_t i = 0; i < scriptFiles.size(); ++i) {
        bool isSelected = (selectedScriptIndex == static_cast<int>(i));
        
        // 显示文件名，如果是外部文件则显示路径提示
        std::string displayName = scriptFiles[i];
        if (i < scriptFilePaths.size() && !scriptFilePaths[i].empty()) {
            std::filesystem::path filePath(scriptFilePaths[i]);
            std::filesystem::path dirPath = filePath.parent_path();
            std::string dirStr = dirPath.string();
            // 如果路径很长，只显示最后一部分
            if (dirStr.length() > 40) {
                dirStr = "..." + dirStr.substr(dirStr.length() - 20);
            }
            displayName += " [" + dirStr + "]";
        }
        
        if (ImGui::Selectable(displayName.c_str(), isSelected)) {
            selectedScriptIndex = static_cast<int>(i);

            // 选中脚本时读取脚本内容以便右侧编辑
            currentScriptDirty = false;
            if (i < scriptFilePaths.size() && !scriptFilePaths[i].empty()) {
                try {
                    std::ifstream ifs(scriptFilePaths[i], std::ios::in | std::ios::binary);
                    if (ifs) {
                        currentScriptContent.assign((std::istreambuf_iterator<char>(ifs)),
                                                    std::istreambuf_iterator<char>());
                    } else {
                        currentScriptContent = "-- 无法打开脚本文件: " + scriptFilePaths[i];
                    }
                } catch (const std::exception& e) {
                    currentScriptContent = std::string("-- 读取脚本失败: ") + e.what();
                }
            } else {
                currentScriptContent.clear();
            }
        }

        // 双击执行
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            if (!scriptRunning) {
                std::string filepath;
                if (i < scriptFilePaths.size() && !scriptFilePaths[i].empty()) {
                    filepath = scriptFilePaths[i];
                } else {
                    filepath = scriptDirectory + "/" + scriptFiles[i];
                }
                executeScript(filepath);
            }
        }
    }
}

void LuaScriptWindow::drawScriptContent() {
    ImGui::Text("脚本内容");
    ImGui::Separator();

    if (selectedScriptIndex < 0 || selectedScriptIndex >= static_cast<int>(scriptFiles.size())) {
        ImGui::TextColored(ColorScheme::TextSecondary, "未选中任何脚本");
        return;
    }

    // 标题和简单信息
    std::string title = scriptFiles[static_cast<size_t>(selectedScriptIndex)];
    ImGui::Text("当前脚本: %s", title.c_str());

    ImGui::SameLine();
    if (currentScriptDirty) {
        ImGui::TextColored(ColorScheme::WarningBright, "[已修改未保存]");
    }

    // 保存按钮
    ImGui::SameLine();
    if (ImGui::Button("保存")) {
        saveCurrentScript();
    }

    ImGui::Separator();

    ImGui::BeginChild("ScriptSourceView", ImVec2(0, 0), false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    // 可编辑多行文本框
    if (currentScriptContent.empty()) {
        // 保证有至少一个字符缓冲区，否则 ImGui 可能访问空指针
        currentScriptContent.reserve(1);
        currentScriptContent.assign("");
    }

    auto textEditCallback = [](ImGuiInputTextCallbackData* data) -> int {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            auto* str = static_cast<std::string*>(data->UserData);
            str->resize(data->BufTextLen);
            data->Buf = str->data();
        }
        return 0;
    };

    ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize;

    bool changed = ImGui::InputTextMultiline(
        "##ScriptEditor",
        currentScriptContent.data(),
        currentScriptContent.size() + 1,
        ImVec2(-FLT_MIN, -FLT_MIN),
        flags,
        textEditCallback,
        &currentScriptContent);

    if (changed) {
        currentScriptDirty = true;
    }

    ImGui::EndChild();
}

void LuaScriptWindow::saveCurrentScript() {
    if (selectedScriptIndex < 0 || selectedScriptIndex >= static_cast<int>(scriptFiles.size())) {
        return;
    }

    std::string path;
    if (selectedScriptIndex < static_cast<int>(scriptFilePaths.size()) &&
        !scriptFilePaths[static_cast<size_t>(selectedScriptIndex)].empty()) {
        path = scriptFilePaths[static_cast<size_t>(selectedScriptIndex)];
    } else {
        path = scriptDirectory + "/" + scriptFiles[static_cast<size_t>(selectedScriptIndex)];
    }

    try {
        std::ofstream ofs(path, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!ofs) {
            outputLog.push_back("[错误] 保存失败，无法打开文件: " + path);
        } else {
            ofs.write(currentScriptContent.data(), static_cast<std::streamsize>(currentScriptContent.size()));
            if (!ofs) {
                outputLog.push_back("[错误] 保存失败，写入数据出错: " + path);
            } else {
                outputLog.push_back("[保存] 脚本已保存: " + path);
                currentScriptDirty = false;
            }
        }
    } catch (const std::exception& e) {
        outputLog.push_back(std::string("[错误] 保存异常: ") + e.what());
    }

    if (outputLog.size() > MAX_LOG_LINES) {
        outputLog.erase(outputLog.begin(),
                        outputLog.begin() + (outputLog.size() - MAX_LOG_LINES));
    }
}

void LuaScriptWindow::drawScriptOutput() {
    ImGui::Text("脚本输出");
    ImGui::Separator();

    if (ImGui::Button("清空日志")) {
        outputLog.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("复制日志")) {
        std::string allLog;
        for (const auto& line : outputLog) {
            allLog += line + "\n";
        }
        ImGui::SetClipboardText(allLog.c_str());
    }

    ImGui::Separator();

    // 显示日志
    ImGui::BeginChild("LogContent", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& line : outputLog) {
        ImGui::TextUnformatted(line.c_str());
    }

    // 自动滚动到底部
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10) {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();
}

void LuaScriptWindow::refreshScriptList() {
    scriptFiles.clear();
    scriptFilePaths.clear();
    scriptSelected.clear();

    if (!std::filesystem::exists(scriptDirectory)) {
        std::filesystem::create_directories(scriptDirectory);
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(scriptDirectory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".lua") {
            try {
                std::string normalizedPath = std::filesystem::canonical(entry.path()).string();
                scriptFiles.push_back(entry.path().filename().string());
                scriptFilePaths.push_back(normalizedPath);  // 存储规范化完整路径
                scriptSelected.push_back(false);
            } catch (...) {
                // 如果无法规范化，使用原始路径
                scriptFiles.push_back(entry.path().filename().string());
                scriptFilePaths.push_back(entry.path().string());
                scriptSelected.push_back(false);
            }
        }
    }

    // 排序（同时保持 scriptFilePaths 同步）
    std::vector<std::pair<std::string, std::string>> pairs;
    for (size_t i = 0; i < scriptFiles.size(); ++i) {
        pairs.push_back({scriptFiles[i], scriptFilePaths[i]});
    }
    std::sort(pairs.begin(), pairs.end());
    
    scriptFiles.clear();
    scriptFilePaths.clear();
    for (const auto& pair : pairs) {
        scriptFiles.push_back(pair.first);
        scriptFilePaths.push_back(pair.second);
    }
}

void LuaScriptWindow::refreshBrowserFiles() {
    browserLuaFiles.clear();
    browserSelectedIndex = -1;

    try {
        std::filesystem::path dirPath(browserDirectoryBuf);
        if (!std::filesystem::exists(dirPath) || !std::filesystem::is_directory(dirPath)) {
            return;
        }

        for (const auto& entry : std::filesystem::directory_iterator(dirPath)) {
            if (entry.is_regular_file() && entry.path().extension() == ".lua") {
                browserLuaFiles.push_back(entry.path());
            }
        }

        // 按文件名排序，保证显示有序
        std::sort(browserLuaFiles.begin(), browserLuaFiles.end(),
                  [](const std::filesystem::path& a, const std::filesystem::path& b) {
                      return a.filename().string() < b.filename().string();
                  });
    } catch (...) {
        // 静默失败，避免因路径错误导致崩溃
    }
}

void LuaScriptWindow::drawScriptBrowserPopup() {
    if (!ImGui::BeginPopupModal("选择Lua脚本", &showScriptBrowser, ImGuiWindowFlags_AlwaysAutoResize)) {
        // 如果弹窗未真正打开，则关闭标记
        if (!ImGui::IsPopupOpen("选择Lua脚本")) {
            showScriptBrowser = false;
        }
        return;
    }

    ImGui::Text("从指定目录选择 Lua 脚本");
    ImGui::Separator();

    ImGui::Text("当前目录:");
    ImGui::SameLine();
    ImGui::InputText("##LuaBrowserDir", browserDirectoryBuf, sizeof(browserDirectoryBuf));
    ImGui::SameLine();
    if (ImGui::Button("刷新")) {
        refreshBrowserFiles();
    }

    ImGui::Separator();

    ImGui::BeginChild("LuaBrowserFileList", ImVec2(500, 300), true);
    if (browserLuaFiles.empty()) {
        ImGui::TextColored(ColorScheme::TextSecondary, "该目录下没有找到 .lua 脚本文件");
    } else {
        for (size_t i = 0; i < browserLuaFiles.size(); ++i) {
            bool isSelected = (static_cast<int>(i) == browserSelectedIndex);
            std::string displayName = browserLuaFiles[i].filename().string();
            std::string fullPath = browserLuaFiles[i].string();

            // 如果路径太长，截断中间部分
            const size_t maxDisplayLen = 80;
            if (fullPath.length() > maxDisplayLen) {
                fullPath = fullPath.substr(0, 30) + " ... " +
                           fullPath.substr(fullPath.length() - 30);
            }

            if (ImGui::Selectable((displayName + "##" + std::to_string(i)).c_str(), isSelected)) {
                browserSelectedIndex = static_cast<int>(i);
            }

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", browserLuaFiles[i].string().c_str());
            }
        }
    }
    ImGui::EndChild();

    ImGui::Separator();

    bool canConfirm = (browserSelectedIndex >= 0 &&
                       browserSelectedIndex < static_cast<int>(browserLuaFiles.size()));

    if (!canConfirm) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("添加到脚本列表")) {
        try {
            const auto& selectedPath = browserLuaFiles[static_cast<size_t>(browserSelectedIndex)];
            std::filesystem::path filePath = selectedPath;
            std::string normalizedPath = std::filesystem::canonical(filePath).string();
            std::string filename = filePath.filename().string();

            // 检查是否已存在（使用规范化路径比较）
            bool exists = false;
            for (size_t i = 0; i < scriptFilePaths.size(); ++i) {
                try {
                    std::filesystem::path existingPath(scriptFilePaths[i]);
                    std::string existingNormalized = std::filesystem::canonical(existingPath).string();
                    if (existingNormalized == normalizedPath) {
                        exists = true;
                        selectedScriptIndex = static_cast<int>(i);
                        outputLog.push_back("[提示] 文件已在列表中: " + filename);
                        break;
                    }
                } catch (...) {
                    if (scriptFilePaths[i] == normalizedPath ||
                        scriptFilePaths[i] == selectedPath.string()) {
                        exists = true;
                        selectedScriptIndex = static_cast<int>(i);
                        break;
                    }
                }
            }

            if (!exists) {
                scriptFiles.push_back(filename);
                scriptFilePaths.push_back(normalizedPath);
                scriptSelected.push_back(false);
                selectedScriptIndex = static_cast<int>(scriptFiles.size() - 1);
                outputLog.push_back("[添加] " + filename + " (" + normalizedPath + ")");

                if (outputLog.size() > MAX_LOG_LINES) {
                    outputLog.erase(outputLog.begin(),
                                    outputLog.begin() + (outputLog.size() - MAX_LOG_LINES));
                }
            }
        } catch (const std::exception& e) {
            outputLog.push_back("[错误] 无法处理文件: " + std::string(e.what()));
        }

        showScriptBrowser = false;
        ImGui::CloseCurrentPopup();
    }
    if (!canConfirm) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (ImGui::Button("取消")) {
        showScriptBrowser = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void LuaScriptWindow::executeScript(const std::string& filepath) {
    if (scriptRunning) {
        return;
    }

    auto& engine = LuaEngine::GetInstance();
    if (!engine.IsInitialized()) {
        if (!engine.Initialize()) {
            outputLog.push_back("[错误] Lua引擎初始化失败: " + engine.GetLastError());
            if (outputLog.size() > MAX_LOG_LINES) {
                outputLog.erase(outputLog.begin());
            }
            return;
        }
    }

    scriptRunning = true;
    currentScript = filepath;
    outputLog.push_back("[执行] " + std::filesystem::path(filepath).filename().string());

    bool success = engine.ExecuteFile(filepath);
    if (success) {
        outputLog.push_back("[成功] 脚本执行完成");
    } else {
        outputLog.push_back("[错误] " + engine.GetLastError());
    }

    // 限制日志行数
    if (outputLog.size() > MAX_LOG_LINES) {
        outputLog.erase(outputLog.begin(), outputLog.begin() + (outputLog.size() - MAX_LOG_LINES));
    }

    scriptRunning = false;
    currentScript.clear();
}

void LuaScriptWindow::stopScript() {
    // LuaJIT不支持直接停止正在执行的脚本
    // 这里只能标记状态，实际停止需要在脚本中检查标志
    scriptRunning = false;
    outputLog.push_back("[停止] 脚本执行已停止");
}

void LuaScriptWindow::reloadScript(const std::string& name) {
    auto& engine = LuaEngine::GetInstance();
    if (!engine.IsInitialized()) {
        return;
    }

    bool success = engine.ReloadScript(name);
    if (success) {
        outputLog.push_back("[重载] " + name + " 已重新加载");
    } else {
        outputLog.push_back("[错误] 重载失败: " + engine.GetLastError());
    }

    if (outputLog.size() > MAX_LOG_LINES) {
        outputLog.erase(outputLog.begin(), outputLog.begin() + (outputLog.size() - MAX_LOG_LINES));
    }
}

