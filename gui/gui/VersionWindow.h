#pragma once

#include "Window.h"
#include <string>
namespace Mem { class IMemService; }

class VersionWindow : public Window {
public:
	explicit VersionWindow(Mem::IMemService& service);
	void onDraw() override;
	unsigned int getWindowFlags() const override;

private:
	bool hasData = false;
	int version = 0;
	std::string versionString;
	Mem::IMemService& service_;
};
