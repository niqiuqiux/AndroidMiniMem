#pragma once

#include "Window.h"
#include "../socket/client_singleton.h"
#include <vector>
#include <string>
#include <tuple>

class ModulesWindow : public Window {
public:
	ModulesWindow();
	void onDraw() override;
	unsigned int getWindowFlags() const override;

	void triggerAutoRefresh() { autoRefreshOnce = true; }

private:
	void drawFilterModal();
	bool passesFilter(const ModuleInfoItem& module) const;
	const char* getModuleTypeName(int type) const;

private:
	bool hasData = false;
	bool autoRefreshOnce = false;
	std::vector<ModuleInfoItem> modules;

	// 过滤功能
	char nameFilter[256] = "";
	uint32_t selectedModuleTypes = static_cast<uint32_t>(-1);
	uint32_t selectedProtectionFlags = static_cast<uint32_t>(-1);
	bool showFilterModal = false;
}; 