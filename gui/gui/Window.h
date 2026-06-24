#pragma once
#include "imgui.h"
#include <string>
#include <cstdint>

class Window {
public:
	bool pOpen = true;
	bool shouldBringToFront = false;

	const long long UID{(long long)this};
	std::string name = "Unnamed window";

	Window() = default;
	virtual ~Window() = default;

	virtual void onDraw() {}
	virtual unsigned int getWindowFlags() const { return 0; }
	virtual void draw();
	void operator()();

	// 便捷方法
	bool hasProcess() const;
	int currentPid() const;
	std::string currentProcessName() const;
	void navigateToAddress(uint64_t addr);

protected:
	bool shouldRefresh(float& timer, float interval);
};
