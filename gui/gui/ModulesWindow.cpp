#include "ModulesWindow.h"
#include "AppContext.h"
#include "ColorScheme.h"
#include "AppContext.h"
#include "../imgui/imgui.h"
#include "../socket/client_singleton.h"
#include "Gui.h"


enum ProtectionFlags {
	PROT_NONE=0x0,
	PROT_READ=0x1,
	PROT_WRITE=0x2,
	PROT_EXEC=0x4,//可执行
	PROT_PRIVATE=0x8,//私有映射
	PROT_SHARED=0x10,//共享映射
};

enum _ModuleType {
    All = -1,
    Anonymous = 1 << 5,//32
    C_Alloc = 1 << 2, //4
    C_Heap = 1 << 0, //1
    C_Data = 1 << 3, //8
    C_Bss = 1 << 4, //16
    Java_Heap = 1 << 1, //2
    Java = 1 << 16, //65536
    Stack = 1 << 6, //64
    Video = 1 << 20, //1048576
    Code_App = 1 << 14, //16384
    Code_System = 1 << 15, //32768
    Ashmem = 1 << 19, //524288
    Bad = 1 << 17, //131072
    Other = -2080896 //-2080896
};


unsigned int ModulesWindow::getWindowFlags() const
{
    return ImGuiWindowFlags_NoDocking;
}

ModulesWindow::ModulesWindow()
{
	name = "模块列表";
}

const char* ModulesWindow::getModuleTypeName(int type) const
{
	switch (type) {
		case _ModuleType::C_Heap: return "C_Heap";
		case _ModuleType::Java_Heap: return "Java_Heap";
		case _ModuleType::C_Alloc: return "C_Alloc";
		case _ModuleType::C_Data: return "C_Data";
		case _ModuleType::C_Bss: return "C_Bss";
		case _ModuleType::Anonymous: return "Anonymous";
		case _ModuleType::Stack: return "Stack";
		case _ModuleType::Code_App: return "Code_App";
		case _ModuleType::Code_System: return "Code_System";
		case _ModuleType::Java: return "Java";
		case _ModuleType::Ashmem: return "Ashmem";
		case _ModuleType::Video: return "Video";
		case _ModuleType::Bad: return "Bad";
		case _ModuleType::Other: return "Other";
		default: return "Unknown";
	}
}

bool ModulesWindow::passesFilter(const ModuleInfoItem& module) const
{
	// 模块名过滤
	if (strlen(nameFilter) > 0) {
		if (module.name.find(nameFilter) == std::string::npos) {
			return false;
		}
	}
	
	// 模块类型过滤
	if (selectedModuleTypes != static_cast<uint32_t>(-1)) {
		if (!(selectedModuleTypes & module.type)) {
			return false;
		}
	}
	
	// 权限过滤
	if (selectedProtectionFlags != static_cast<uint32_t>(-1)) {
		if (!(selectedProtectionFlags & module.flag)) {
			return false;
		}
	}
	
	return true;
}

void ModulesWindow::onDraw()
{
	// 第一行：刷新按钮和进程信息
	if (ImGui::Button("刷新模块列表"))
	{
		std::vector<ModuleInfoItem> list;
		if (FetchModuleList(list)) {
			hasData = true;
			modules = std::move(list);
			Gui::log("获取到 %d 个模块", (int)modules.size());
		} else {
			hasData = false;
			Gui::log("获取模块列表失败 (请确保已连接并打开进程)");
		}
	}
	
	ImGui::SameLine();
	int pid = GetCurrentPid();
	if (pid)
		ImGui::Text("当前进程 PID: %d", pid);
	else
		ImGui::TextDisabled("未选择进程");

	if (autoRefreshOnce) {
		autoRefreshOnce = false;
		std::vector<ModuleInfoItem> list;
		if (FetchModuleList(list)) {
			hasData = true;
			modules = std::move(list);
		}
	}

	// 过滤控件区域
	ImGui::Separator();
	ImGui::Text("过滤:");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(200);
	ImGui::InputTextWithHint("##NameFilter", "模块名搜索...", nameFilter, IM_ARRAYSIZE(nameFilter));
	
	ImGui::SameLine();
	if (ImGui::Button("高级过滤")) {
		showFilterModal = true;
	}
	
	ImGui::SameLine();
	if (ImGui::Button("清除过滤")) {
		nameFilter[0] = '\0';
		selectedModuleTypes = static_cast<uint32_t>(-1);
		selectedProtectionFlags = static_cast<uint32_t>(-1);
		}
	
	// 显示过滤状态
	if (strlen(nameFilter) > 0 || selectedModuleTypes != static_cast<uint32_t>(-1) || selectedProtectionFlags != static_cast<uint32_t>(-1)) {
		ImGui::SameLine();
		ImGui::TextColored(ColorScheme::WarningBright, "(已启用过滤)");
	}

	if (hasData) {
		// 统计过滤结果
		std::vector<const ModuleInfoItem*> filteredModules;
		for (const auto& module : modules) {
			if (passesFilter(module)) {
				filteredModules.push_back(&module);
			}
		}
		
		ImGui::Text("显示 %d / %d 个模块", (int)filteredModules.size(), (int)modules.size());
		
		if (ImGui::BeginTable("mods", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
			ImGui::TableSetupColumn("基址", ImGuiTableColumnFlags_WidthFixed, 120.0f);
			ImGui::TableSetupColumn("大小", ImGuiTableColumnFlags_WidthFixed, 80.0f);
			ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, 100.0f);
			ImGui::TableSetupColumn("权限", ImGuiTableColumnFlags_WidthFixed, 80.0f);
			ImGui::TableSetupColumn("模块名", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();
			
			for (const auto* modulePtr : filteredModules) {
				const auto& module = *modulePtr;
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("0x%llX", (unsigned long long)module.base);
				
				ImGui::TableSetColumnIndex(1);
				if (module.size >= 1024 * 1024) {
					ImGui::Text("%.1fM", module.size / (1024.0f * 1024.0f));
				} else if (module.size >= 1024) {
					ImGui::Text("%.1fK", module.size / 1024.0f);
				} else {
					ImGui::Text("%d", module.size);
				}
				
				ImGui::TableSetColumnIndex(2);
				const char* typeName = getModuleTypeName(module.type);
				
				// 根据模块类型着色
				ImVec4 typeColor = ColorScheme::TextPrimary; // 默认白色
				if (module.type & (_ModuleType::Code_App | _ModuleType::Code_System)) {
					typeColor = ColorScheme::ErrorLight; // 代码段
				} else if (module.type & (_ModuleType::C_Heap | _ModuleType::Java_Heap)) {
					typeColor = ColorScheme::SuccessLight; // 堆内存
				} else if (module.type & _ModuleType::Stack) {
					typeColor = ColorScheme::InfoLight; // 栈内存
				} else if (module.type & _ModuleType::Anonymous) {
					typeColor = ColorScheme::WarningLight; // 匿名内存
				}
				
				ImGui::TextColored(typeColor, "%s", typeName);
				
				ImGui::TableSetColumnIndex(3);
				// 显示保护标志
				std::string protStr;
				if (module.flag & PROT_READ) protStr += "R";
				if (module.flag & PROT_WRITE) protStr += "W";
				if (module.flag & PROT_EXEC) protStr += "X";
				if (module.flag & PROT_PRIVATE) protStr += "P";
				if (module.flag & PROT_SHARED) protStr += "S";
				if (protStr.empty()) protStr = "---";
				
				// 根据权限着色
				if (module.flag & PROT_EXEC) {
					ImGui::TextColored(ColorScheme::ErrorLight, "%s", protStr.c_str());
				} else if (module.flag & PROT_WRITE) {
					ImGui::TextColored(ColorScheme::SuccessLight, "%s", protStr.c_str());
				} else {
					ImGui::TextColored(ColorScheme::InfoLight, "%s", protStr.c_str());
				}
				
				ImGui::TableSetColumnIndex(4);
				ImGui::TextUnformatted(module.name.c_str());
				
				// 添加工具提示
				if (ImGui::IsItemHovered()) {
					ImGui::BeginTooltip();
					ImGui::Text("模块: %s", module.name.c_str());
					ImGui::Text("基址: 0x%llX", (unsigned long long)module.base);
					ImGui::Text("大小: %d 字节 (0x%X)", module.size, module.size);
					ImGui::Text("类型: %s (%d)", typeName, module.type);
					ImGui::Text("标志: 0x%X", module.flag);
					ImGui::Separator();
					ImGui::Text("权限:");
					if (module.flag & PROT_READ) ImGui::Text("  - 可读");
					if (module.flag & PROT_WRITE) ImGui::Text("  - 可写");
					if (module.flag & PROT_EXEC) ImGui::Text("  - 可执行");
					if (module.flag & PROT_PRIVATE) ImGui::Text("  - 私有映射");
					if (module.flag & PROT_SHARED) ImGui::Text("  - 共享映射");
					ImGui::Separator();
					ImGui::TextColored(ColorScheme::SuccessLight, "右键菜单可浏览内存");
					ImGui::EndTooltip();
				}
				
				// 右键菜单 - 使用唯一ID避免断言失败
				char popup_id[64];
				std::snprintf(popup_id, sizeof(popup_id), "ModulePopup_%llX", (unsigned long long)module.base);
				if (ImGui::BeginPopupContextItem(popup_id)) {
					ImGui::TextColored(ColorScheme::InfoLight, "模块: %s", module.name.c_str());
					ImGui::Separator();
					
					if (ImGui::MenuItem("复制模块名")) {
						ImGui::SetClipboardText(module.name.c_str());
					}
					
					if (ImGui::MenuItem("复制基址")) {
						char addrBuf[32];
						std::snprintf(addrBuf, sizeof(addrBuf), "0x%llX", (unsigned long long)module.base);
						ImGui::SetClipboardText(addrBuf);
					}
					
					if (ImGui::MenuItem("在内存查看器中打开")) {
						navigateToAddress(module.base);
						Gui::log("跳转到模块基址: 0x%llX (%s)", (unsigned long long)module.base, module.name.c_str());
					}
					
					ImGui::EndPopup();
				}
			}
			ImGui::EndTable();
		}
	}
	
	// 绘制过滤弹窗
	drawFilterModal();
}

void ModulesWindow::drawFilterModal()
{
	if (showFilterModal)
		ImGui::OpenPopup("Advanced Filter");

	if (ImGui::BeginPopupModal("Advanced Filter", &showFilterModal, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("高级过滤设置");
		ImGui::Separator();
		
		// 模块名过滤
		ImGui::Text("模块名过滤:");
		ImGui::SetNextItemWidth(300);
		ImGui::InputTextWithHint("##NameFilterModal", "输入模块名或路径关键词...", nameFilter, IM_ARRAYSIZE(nameFilter));
		
		ImGui::Spacing();
		
		// 模块类型过滤
		ImGui::Text("模块类型过滤:");
		
		// 快捷按钮
		if (ImGui::Button("全选##ModType")) {
			selectedModuleTypes = static_cast<uint32_t>(-1);
		}
		ImGui::SameLine();
		if (ImGui::Button("全不选##ModType")) {
			selectedModuleTypes = 0;
		}
		ImGui::SameLine();
		if (ImGui::Button("常用##ModType")) {
			selectedModuleTypes = _ModuleType::C_Heap | _ModuleType::Java_Heap | 
			                    _ModuleType::C_Data | _ModuleType::C_Bss | 
			                    _ModuleType::Anonymous;
		}
		ImGui::SameLine();
		if (ImGui::Button("代码##ModType")) {
			selectedModuleTypes = _ModuleType::Code_App | _ModuleType::Code_System;
		}
		
		// 模块类型选择
		struct ModuleTypeInfo {
			_ModuleType type;
			const char* name;
			const char* description;
		};
		
		ModuleTypeInfo moduleTypes[] = {
			{_ModuleType::C_Heap, "C_Heap", "C程序堆内存"},
			{_ModuleType::Java_Heap, "Java_Heap", "Java虚拟机堆内存"},
			{_ModuleType::C_Alloc, "C_Alloc", "C malloc分配的内存"},
			{_ModuleType::C_Data, "C_Data", "程序数据段"},
			{_ModuleType::C_Bss, "C_Bss", "程序BSS段"},
			{_ModuleType::Anonymous, "Anonymous", "匿名映射内存"},
			{_ModuleType::Stack, "Stack", "程序栈内存"},
			{_ModuleType::Code_App, "Code_App", "应用程序代码段"},
			{_ModuleType::Code_System, "Code_System", "系统库代码段"},
			{_ModuleType::Java, "Java", "Java相关内存"},
			{_ModuleType::Ashmem, "Ashmem", "Android共享内存"},
			{_ModuleType::Video, "Video", "显卡内存"},
			{_ModuleType::Bad, "Bad", "无效内存区域"},
			{_ModuleType::Other, "Other", "其他类型内存"},
		};
		
		ImGui::Columns(2, "ModuleTypeColumns", false);
		for (int i = 0; i < IM_ARRAYSIZE(moduleTypes); i++) {
			bool isSelected = (selectedModuleTypes == static_cast<uint32_t>(-1)) || 
			                 (selectedModuleTypes & moduleTypes[i].type);
			
			if (ImGui::Checkbox(moduleTypes[i].name, &isSelected)) {
				if (selectedModuleTypes == static_cast<uint32_t>(-1)) {
					selectedModuleTypes = isSelected ? moduleTypes[i].type : 0;
				} else {
					if (isSelected) {
						selectedModuleTypes |= moduleTypes[i].type;
					} else {
						selectedModuleTypes &= ~moduleTypes[i].type;
					}
				}
			}
			
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", moduleTypes[i].description);
			}
			
			if (i == (IM_ARRAYSIZE(moduleTypes) - 1) / 2) {
				ImGui::NextColumn();
			}
		}
		ImGui::Columns(1);
		
		ImGui::Spacing();
		ImGui::Separator();
		
		// 权限过滤
		ImGui::Text("权限过滤:");
		
		// 权限快捷按钮
		if (ImGui::Button("全选##Prot")) {
			selectedProtectionFlags = static_cast<uint32_t>(-1);
		}
		ImGui::SameLine();
		if (ImGui::Button("全不选##Prot")) {
			selectedProtectionFlags = 0;
		}
		ImGui::SameLine();
		if (ImGui::Button("可执行##Prot")) {
			selectedProtectionFlags = PROT_EXEC;
		}
		ImGui::SameLine();
		if (ImGui::Button("可写##Prot")) {
			selectedProtectionFlags = PROT_WRITE;
		}
		
		// 权限选择
		struct ProtectionInfo {
			ProtectionFlags flag;
			const char* name;
			const char* description;
		};
		
		ProtectionInfo protections[] = {
			{PROT_READ, "可读 (R)", "内存可读"},
			{PROT_WRITE, "可写 (W)", "内存可写"},
			{PROT_EXEC, "可执行 (X)", "内存可执行"},
			{PROT_PRIVATE, "私有映射 (P)", "私有内存映射"},
			{PROT_SHARED, "共享映射 (S)", "共享内存映射"},
		};
		
		for (int i = 0; i < IM_ARRAYSIZE(protections); i++) {
			bool isSelected = (selectedProtectionFlags == static_cast<uint32_t>(-1)) || 
			                 (selectedProtectionFlags & protections[i].flag);
			
			if (ImGui::Checkbox(protections[i].name, &isSelected)) {
				if (selectedProtectionFlags == static_cast<uint32_t>(-1)) {
					selectedProtectionFlags = isSelected ? protections[i].flag : 0;
				} else {
					if (isSelected) {
						selectedProtectionFlags |= protections[i].flag;
					} else {
						selectedProtectionFlags &= ~protections[i].flag;
					}
				}
			}
			
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", protections[i].description);
			}
		}
		
		ImGui::Separator();
		ImGui::Spacing();
		
		// 按钮
		if (ImGui::Button("应用", ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
			showFilterModal = false;
		}
		ImGui::SameLine();
		if (ImGui::Button("重置", ImVec2(120, 0))) {
			nameFilter[0] = '\0';
			selectedModuleTypes = static_cast<uint32_t>(-1);
			selectedProtectionFlags = static_cast<uint32_t>(-1);
		}
		ImGui::SameLine();
		if (ImGui::Button("取消", ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
			showFilterModal = false;
		}
		
		ImGui::EndPopup();
	}
} 