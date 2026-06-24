#pragma once

#include "Window.h"
#include <string>

class VersionWindow : public Window {
public:
	VersionWindow();
	void onDraw() override;
	unsigned int getWindowFlags() const override;

private:
	bool hasData = false;
	int version = 0;
	std::string versionString;
}; 