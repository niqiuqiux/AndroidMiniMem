#pragma once

#include "../imgui/imgui.h"

// 莫奈黑白色系颜色配置
// 所有颜色值都经过调整，以适配深色背景主题，使用柔和的低饱和度色调

namespace ColorScheme {
    // ========== 基础颜色 ==========
    // 文本颜色（增强对比度）
    inline const ImVec4 TextPrimary = ImVec4(0.95f, 0.95f, 0.95f, 1.0f);      // 主要文本（白色）
    inline const ImVec4 TextSecondary = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);      // 次要文本（灰色，增强可见性）
    inline const ImVec4 TextDisabled = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);     // 禁用文本（深灰，增强可见性）
    
    // ========== 状态颜色 ==========
    // 成功/活动状态 - 淡灰绿色系（增强对比度，提高深度）
    inline const ImVec4 Success = ImVec4(0.65f, 0.8f, 0.7f, 1.0f);          // 成功/活动（增强可见性）
    inline const ImVec4 SuccessLight = ImVec4(0.7f, 0.85f, 0.7f, 1.0f);     // 成功（浅色，提高深度）
    inline const ImVec4 SuccessBright = ImVec4(0.5f, 0.8f, 0.5f, 1.0f);      // 成功（亮色，用于强调，增强对比度）
    
    // 警告/暂停状态 - 淡灰黄色系（增强对比度，提高深度）
    inline const ImVec4 Warning = ImVec4(0.92f, 0.85f, 0.55f, 1.0f);         // 警告/暂停
    inline const ImVec4 WarningLight = ImVec4(0.90f, 0.88f, 0.65f, 1.0f);   // 警告（浅色）
    inline const ImVec4 WarningBright = ImVec4(0.95f, 0.75f, 0.0f, 1.0f);     // 警告（亮色，用于强调）
    
    // 错误状态 - 淡灰红色系（增强对比度，提高深度）
    inline const ImVec4 Error = ImVec4(0.85f, 0.7f, 0.7f, 1.0f);            // 错误（增强可见性）
    inline const ImVec4 ErrorLight = ImVec4(0.85f, 0.75f, 0.75f, 1.0f);     // 错误（浅色，提高深度）
    inline const ImVec4 ErrorBright = ImVec4(0.9f, 0.2f, 0.2f, 1.0f);       // 错误（亮色，用于强调，增强对比度）
    
    // 信息状态 - 淡灰蓝色系（增强对比度，提高深度）
    inline const ImVec4 Info = ImVec4(0.55f, 0.78f, 0.95f, 1.0f);           // 信息
    inline const ImVec4 InfoLight = ImVec4(0.65f, 0.82f, 0.92f, 1.0f);        // 信息（浅色）
    inline const ImVec4 InfoBright = ImVec4(0.4f, 0.85f, 0.95f, 1.0f);       // 信息（亮色，用于强调）
    
    // ========== 特殊用途颜色 ==========
    // 地址/数值显示 - 淡蓝色系（增强对比度，提高深度）
    inline const ImVec4 Address = ImVec4(0.7f, 0.8f, 0.85f, 1.0f);          // 地址显示（增强可见性）
    inline const ImVec4 AddressBright = ImVec4(0.5f, 0.8f, 0.9f, 1.0f);     // 地址（亮色，增强对比度）
    
    // 寄存器颜色 - 淡蓝色系（增强对比度，提高深度）
    inline const ImVec4 Register = ImVec4(0.7f, 0.8f, 0.85f, 1.0f);         // 通用寄存器（增强可见性）
    inline const ImVec4 RegisterSpecial = ImVec4(0.85f, 0.7f, 0.7f, 1.0f);  // 特殊寄存器（SP/PC，增强可见性）
    inline const ImVec4 RegisterStatus = ImVec4(0.8f, 0.8f, 0.7f, 1.0f);     // 状态寄存器（增强可见性）
    
    // 浮点寄存器 - 淡青绿色系（增强对比度，提高可读性，提高深度）
    inline const ImVec4 FloatRegister = ImVec4(0.6f, 0.85f, 0.75f, 1.0f);    // 浮点寄存器（增强可见性）
    inline const ImVec4 FloatValue = ImVec4(0.8f, 0.85f, 0.55f, 1.0f);       // 浮点数值（增强对比度，更清晰可见）
    
    // 排名颜色 - 金银铜色调（增强对比度，提高深度）
    inline const ImVec4 RankGold = ImVec4(0.85f, 0.85f, 0.65f, 1.0f);        // 第一名（增强可见性）
    inline const ImVec4 RankSilver = ImVec4(0.75f, 0.75f, 0.75f, 1.0f);      // 第二名（增强可见性）
    inline const ImVec4 RankBronze = ImVec4(0.8f, 0.7f, 0.65f, 1.0f);         // 第三名（增强可见性）
    
    // 反汇编相关（增强对比度，提高深度）
    inline const ImVec4 DisassemblyMnemonic = ImVec4(0.7f, 0.85f, 0.8f, 1.0f);  // 助记符（增强可见性）
    inline const ImVec4 DisassemblyHex = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);       // 十六进制（增强可见性）
    inline const ImVec4 DisassemblyPC = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);        // 当前PC（黄色高亮）
    inline const ImVec4 DisassemblyPCBg = ImVec4(0.3f, 0.5f, 0.3f, 0.4f);      // 当前PC背景
    
    // 统计/数据颜色（增强对比度，提高深度）
    inline const ImVec4 StatCount = ImVec4(0.7f, 0.8f, 0.85f, 1.0f);          // 计数（增强可见性）
    inline const ImVec4 StatHighlight = ImVec4(0.8f, 0.7f, 0.85f, 1.0f);     // 高亮统计（增强可见性）
    inline const ImVec4 StatAverage = ImVec4(0.7f, 0.85f, 0.7f, 1.0f);         // 平均值（增强可见性）
    
    // 缓存/状态指示（增强对比度，提高深度）
    inline const ImVec4 CacheValid = ImVec4(0.7f, 0.85f, 0.7f, 1.0f);         // 缓存有效（增强可见性）
    inline const ImVec4 CacheLoading = ImVec4(0.85f, 0.85f, 0.7f, 1.0f);     // 加载中（增强可见性）
    
    // ========== 背景颜色（用于表格行高亮等） ==========
    inline const ImVec4 RowHighlight = ImVec4(0.75f, 0.85f, 0.75f, 0.3f);   // 行高亮背景
    
    // ========== 按钮颜色 ==========
    // 选中状态按钮
    inline const ImVec4 ButtonSelected = ImVec4(0.3f, 0.5f, 0.8f, 1.0f);
    inline const ImVec4 ButtonSelectedHovered = ImVec4(0.4f, 0.6f, 0.9f, 1.0f);
    inline const ImVec4 ButtonSelectedActive = ImVec4(0.2f, 0.4f, 0.7f, 1.0f);
    
    // 高亮状态按钮（目标地址等）
    inline const ImVec4 ButtonHighlight = ImVec4(0.6f, 0.5f, 0.2f, 0.6f);
    inline const ImVec4 ButtonHighlightHovered = ImVec4(0.7f, 0.6f, 0.3f, 0.8f);
    inline const ImVec4 ButtonHighlightActive = ImVec4(0.5f, 0.4f, 0.1f, 0.7f);
    
    // 默认状态按钮
    inline const ImVec4 ButtonDefault = ImVec4(0.2f, 0.2f, 0.2f, 0.4f);
    inline const ImVec4 ButtonDefaultHovered = ImVec4(0.3f, 0.3f, 0.3f, 0.6f);
    inline const ImVec4 ButtonDefaultActive = ImVec4(0.25f, 0.25f, 0.25f, 0.5f);
}

