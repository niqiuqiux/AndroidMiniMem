#include "ServerConnectWindow.h"
#include "ColorScheme.h"
#include "../imgui/imgui.h"
#include "../socket/client_singleton.h"
#include "../socket/client.hpp"
#include "Gui.h"
#include "version.h"
#include "ConfigManager.h"

namespace {
bool isValidPort(int port)
{
	return port > 0 && port <= 65535;
}

const char* getMemTypeName(const std::string (&names)[5], int memType)
{
	return (memType >= 0 && memType < 5) ? names[memType].c_str() : "Unknown";
}
} // namespace

ServerConnectWindow::ServerConnectWindow()
{
	name = "服务器连接";
	std::snprintf(hostBuf, sizeof(hostBuf), "%s", "127.0.0.1");
	port = 52736;
	autoReconnect = false;
	status = "空闲";
	lastConnected = GetSocketMgr().GetClient(PORT_MAIN)->IsConnected();
	
	// 初始化新增成员变量
	currentMemType = 0;
	//std::snprintf(cardKeyBuf, sizeof(cardKeyBuf), "%s", "");
	driverStatus = "未初始化";
	memTypeNames[0] = "空";
	memTypeNames[1] = "IO";
	memTypeNames[2] = "系统调用";
	memTypeNames[3] = "内核";
	memTypeNames[4] = "系统钩子";
	
	// 加载配置
	loadConfig();
	
	updateMemType();
}

void ServerConnectWindow::updateStatus(bool ok, const char* action)
{
	if (ok) {
		ServerVersionInfo versionInfo;
		if (FetchServerVersion(versionInfo)) {
			status = std::string(action) + ": 已连接到 " + hostBuf + ":" + std::to_string(port);
			Gui::log("%s", status.c_str());
			Gui::log("服务器版本: %d", versionInfo.version);
			Gui::log("服务器版本字符串: %s", versionInfo.versionString.c_str());
			status = status  +"\n" + " (版本: " + versionInfo.versionString + ")";
			
			// 连接成功后更新MemType
			updateMemType();
		} else {
			status = std::string(action) + ": 失败 -> 未知服务器";
			Gui::log("获取服务器版本失败，服务端可能不兼容");
		}
	} else {
		status = std::string(action) + ": 失败";
		Gui::log("%s", status.c_str());
	}
}

void ServerConnectWindow::updateMemType()
{
	auto client = GetSocketMgr().GetClient(PORT_MAIN);
	if (client->IsConnected()) {
		int memType = 0;
		if (GetMemType(memType)) {
			currentMemType = memType;
			Gui::log("当前内存类型: %s (%d)", getMemTypeName(memTypeNames, currentMemType), currentMemType);
		} else {
			Gui::log("获取内存类型失败，请检查连接状态");
			currentMemType = 0;
		}
	}
}

void ServerConnectWindow::initializeDriver()
{
	std::string cardKey = std::string(cardKeyBuf);
	std::string kernelVersion = std::string(1, KernelVersionBuf);
	std::string resultStr;
	
	if (cardKey.empty()) {
		driverStatus = "初始化失败: 卡密不能为空";
		Gui::log("驱动初始化失败: 卡密不能为空");
		return;
	}
	
	Gui::log("正在初始化驱动，卡密: %s", cardKey.c_str());
	cardKey = cardKey +"-"+ kernelVersion;
	if (InitDriver(cardKey, resultStr)) {
		driverStatus = "初始化成功: " + resultStr;
		Gui::log("驱动初始化成功: %s", resultStr.c_str());
		// 初始化成功后更新MemType
		updateMemType();
	} else {
		driverStatus = "初始化失败: " + resultStr;
		Gui::log("驱动初始化失败: %s", resultStr.c_str());
	}
}

void ServerConnectWindow::drawConnectionControls() {
  // ==================== 客户端版本信息 ====================
  ImGui::SeparatorText("客户端版本");
  ImGui::Text("程序版本: %s", PROJECT_VERSION);
  ImGui::Text("协议版本: %s", PROTOCOL_VERSION);
  ImGui::Text("构建日期: %s  构建时间: %s", BUILD_DATE, BUILD_TIME);
  ImGui::Text("Git 提交: %s  分支: %s", GIT_COMMIT_HASH, GIT_BRANCH);
  ImGui::Spacing();
  ImGui::Separator();

  ImGui::InputText("主机", hostBuf, IM_ARRAYSIZE(hostBuf));
  ImGui::InputInt("端口", &port);
  ImGui::Checkbox("自动重连", &autoReconnect);
  ImGui::Text("状态: %s", status.c_str());

  auto client = GetSocketMgr().GetClient(PORT_MAIN);
  if (!client->IsConnected()) {
    if (ImGui::Button("连接")) {
      if (!isValidPort(port)) {
        status = "连接: 失败 -> invalid port";
        Gui::log("连接失败: invalid port %d", port);
        return;
      }
      bool ok = GetSocketMgr().ConnectMultiPort(hostBuf, static_cast<uint16_t>(port));
      updateStatus(ok, "连接");
    }
  } else {
    if (ImGui::Button("断开连接")) {
      GetSocketMgr().DisconnectMultiPort();
      updateStatus(false, "断开连接");
    }
  }

  if (autoReconnect && !client->IsConnected()) {
    if (!isValidPort(port)) {
      status = "自动重连: 失败 -> invalid port";
      return;
    }
    bool ok = GetSocketMgr().ConnectMultiPort(hostBuf, static_cast<uint16_t>(port));
    if (ok)
      updateStatus(true, "自动重连");
  }
}

void ServerConnectWindow::drawDriverControls() {
  ImGui::Separator();
  ImGui::Text("驱动控制");

  // 显示当前内存类型
  ImGui::Text("当前内存类型: %s", getMemTypeName(memTypeNames, currentMemType));

  ImGui::SameLine();
  if (ImGui::Button("刷新类型")) {
    updateMemType();
  }

  // 卡密输入和驱动初始化
  bool cardKeyChanged = false;
  if (ImGui::InputText("卡密", cardKeyBuf, IM_ARRAYSIZE(cardKeyBuf))) {
    cardKeyChanged = true;
  }
  // 列表显示5 6
  const char *kernelVersionList[] = {"5系", "6系"};
  int currentKernelVersion = (KernelVersionBuf == '5') ? 0 : 1;
  ImGui::PushItemWidth(100);
  bool kernelVersionChanged = false;
  if (ImGui::Combo("##kernelVersion", &currentKernelVersion, kernelVersionList,
                   IM_ARRAYSIZE(kernelVersionList))) {
    KernelVersionBuf = currentKernelVersion == 0 ? '5' : '6';
    kernelVersionChanged = true;
  }
  ImGui::PopItemWidth();
  
  // 如果配置改变，保存配置
  if (cardKeyChanged || kernelVersionChanged) {
    saveConfig();
  }

  auto client = GetSocketMgr().GetClient(PORT_MAIN);
  if (client->IsConnected()) {
    if (ImGui::Button("初始化驱动")) {
      initializeDriver();
    }
  } else {
    ImGui::BeginDisabled();
    ImGui::Button("初始化驱动");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextColored(ColorScheme::Warning, "(需要先连接服务器)");
  }

  ImGui::Text("驱动状态: %s", driverStatus.c_str());
}

void ServerConnectWindow::loadConfig()
{
	auto& config = ConfigManager::getInstance();
	config.loadConfig("config.ini");
	
	// 加载卡密
	std::string cardKey = config.getString("cardKey", "1142192691366763");
	std::snprintf(cardKeyBuf, sizeof(cardKeyBuf), "%s", cardKey.c_str());
	
	// 加载内核版本
	KernelVersionBuf = config.getChar("kernelVersion", '6');
	
	Gui::log("配置已加载: 卡密=%s, 内核版本=%c", cardKeyBuf, KernelVersionBuf);
}

void ServerConnectWindow::saveConfig()
{
	auto& config = ConfigManager::getInstance();
	
	// 保存卡密
	config.setString("cardKey", std::string(cardKeyBuf));
	
	// 保存内核版本
	config.setChar("kernelVersion", KernelVersionBuf);
	
	// 保存到文件
	if (config.saveConfig("config.ini")) {
		Gui::log("配置已保存: 卡密=%s, 内核版本=%c", cardKeyBuf, KernelVersionBuf);
	} else {
		Gui::log("配置保存失败: 无法写入 config.ini");
	}
}

void ServerConnectWindow::onDraw()
{
	drawConnectionControls();
	drawDriverControls();
}
