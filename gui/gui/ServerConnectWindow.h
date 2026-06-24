#pragma once

#include "Window.h"
#include "ConfigManager.h"
#include <string>
#include <functional>

class ServerConnectWindow : public Window {
public:
	ServerConnectWindow();
	~ServerConnectWindow() override = default;

	void onDraw() override;
	unsigned int getWindowFlags() const override { return ImGuiWindowFlags_NoDocking; }

	std::function<void()> onConnected; // callback after successful connect

private:
	char hostBuf[128];
	int port;
	bool autoReconnect;
	std::string status;
	bool lastConnected;
	
	// 新增成员变量
	int currentMemType;
	char cardKeyBuf[256] = "1142192691366763";
	char KernelVersionBuf = '6';
	std::string driverStatus;
	std::string memTypeNames[5];

	void drawConnectionControls();
	void drawDriverControls();
	void updateStatus(bool ok, const char* action);
	void updateMemType();
	void initializeDriver();
	
	// 配置相关方法
	void loadConfig();
	void saveConfig();
}; 