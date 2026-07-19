#pragma once

#include "Window.h"
#include "../mem/MemTypes.h"
#include <vector>
#include <string>
#include <tuple>

namespace Mem { class IMemService; }

class ModulesWindow : public Window {
public:
	explicit ModulesWindow(Mem::IMemService& service);
	void onDraw() override;
	unsigned int getWindowFlags() const override;

	void triggerAutoRefresh() { autoRefreshOnce = true; }

private:
	void drawFilterModal();
	bool refreshModules();
	bool passesFilter(const Mem::ModuleInfo& module) const;
	const char* getModuleTypeName(int type) const;

private:
	bool hasData = false;
	bool autoRefreshOnce = false;
	std::vector<Mem::ModuleInfo> modules;
	Mem::IMemService& service_;

	// 过滤功能
	char nameFilter[256] = "";
	uint32_t selectedModuleTypes = static_cast<uint32_t>(-1);
	uint32_t selectedProtectionFlags = static_cast<uint32_t>(-1);
	bool showFilterModal = false;
};
