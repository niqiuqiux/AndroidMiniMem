#include "ServerConnectWindow.h"
#include "ColorScheme.h"
#include "../imgui/imgui.h"
#include "../mem/IMemService.h"
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

ServerConnectWindow::ServerConnectWindow(Mem::IMemService& service)
	: service_(service)
{
	name = "服务器连接";
	std::snprintf(hostBuf, sizeof(hostBuf), "%s", "127.0.0.1");
	port = 52736;
	autoReconnect = false;
	status = "空闲";

	// 初始化新增成员变量
	currentMemType = 0;
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
		auto versionInfo = service_.serverVersion(
			service_.captureContext(false));
		if (versionInfo.ok()) {
			status = std::string(action) + ": 已连接到 " + hostBuf + ":" + std::to_string(port);
			Gui::log("%s", status.c_str());
			Gui::log("服务器版本: %d", versionInfo.value().version);
			Gui::log("服务器版本字符串: %s",
			         versionInfo.value().versionString.c_str());
			status = status + "\n" + " (版本: " +
			         versionInfo.value().versionString + ")";
			
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
	if (service_.connectionSnapshot().connected) {
		auto memType = service_.memoryType(service_.captureContext(false));
		if (memType.ok()) {
			currentMemType = memType.value().type;
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

	if (cardKey.empty()) {
		driverStatus = "初始化失败: 卡密不能为空";
		Gui::log("驱动初始化失败: 卡密不能为空");
		return;
	}

	saveConfig();
	
	Gui::log("正在初始化驱动");
	cardKey = cardKey +"-"+ kernelVersion;
	Mem::DriverInitializeRequest request;
	request.card = cardKey;
	request.forceReclaimHardwareBreakpoints = kernelBreakpointForceReclaim;
	auto result = service_.initializeDriver(
		service_.captureContext(false), request);
	if (result.ok()) {
		driverStatus = "初始化成功: " + result.value().message;
		Gui::log("驱动初始化成功: %s", result.value().message.c_str());
		Gui::log("断点槽抢占: %s",
		         kernelBreakpointForceReclaim ? "已开启" : "已关闭");
		// 初始化成功后更新MemType
		updateMemType();
	} else {
		driverStatus = "初始化失败: " + result.error().message;
		Gui::log("驱动初始化失败: %s", result.error().message.c_str());
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
  if (ImGui::Checkbox("自动重连", &autoReconnect))
    saveConfig();
  ImGui::Text("状态: %s", status.c_str());

  if (!service_.connectionSnapshot().connected) {
    if (ImGui::Button("连接")) {
      if (!isValidPort(port)) {
        status = "连接: 失败 -> invalid port";
        Gui::log("连接失败: invalid port %d", port);
        return;
      }
      Mem::ConnectRequest request;
      request.host = hostBuf;
      request.port = static_cast<uint16_t>(port);
      bool ok = service_.connect(service_.captureContext(false), request).ok();
      updateStatus(ok, "连接");
      if (ok) saveConfig();  // 记住成功连接的主机/端口
    }
  } else {
    if (ImGui::Button("断开连接")) {
      (void)service_.disconnect(service_.captureContext(false));
      updateStatus(false, "断开连接");
    }
  }

  if (autoReconnect && !service_.connectionSnapshot().connected) {
    if (!isValidPort(port)) {
      status = "自动重连: 失败 -> invalid port";
      return;
    }
    Mem::ConnectRequest request;
    request.host = hostBuf;
    request.port = static_cast<uint16_t>(port);
    bool ok = service_.connect(service_.captureContext(false), request).ok();
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
  ImGui::InputText("卡密", cardKeyBuf, IM_ARRAYSIZE(cardKeyBuf),
                   ImGuiInputTextFlags_Password);
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    saveConfig();
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
  if (kernelVersionChanged) {
    saveConfig();
  }

  if (ImGui::Checkbox("断点槽抢占", &kernelBreakpointForceReclaim)) {
    saveConfig();
  }

  if (service_.connectionSnapshot().connected) {
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

	// 加载上次连接的主机 / 端口 / 自动重连（默认沿用构造函数里的值）
	std::string host = config.getString("host", hostBuf);
	std::snprintf(hostBuf, sizeof(hostBuf), "%s", host.c_str());
	port = config.getInt("port", port);
	autoReconnect = config.getInt("autoReconnect", autoReconnect ? 1 : 0) != 0;

	std::string cardKey = config.getString("cardKey", "");
	std::snprintf(cardKeyBuf, sizeof(cardKeyBuf), "%s", cardKey.c_str());

	// 加载内核版本
	KernelVersionBuf = config.getChar("kernelVersion", '6');
	kernelBreakpointForceReclaim =
		config.getInt("kernelBreakpointForceReclaim", 0) != 0;

	Gui::log("配置已加载 (host=%s:%d, 内核=%c系)", hostBuf, port, KernelVersionBuf);
}

void ServerConnectWindow::saveConfig()
{
	auto& config = ConfigManager::getInstance();

	// 保存主机 / 端口 / 自动重连
	config.setString("host", std::string(hostBuf));
	config.setInt("port", port);
	config.setInt("autoReconnect", autoReconnect ? 1 : 0);
	config.setString("cardKey", std::string(cardKeyBuf));

	// 保存内核版本
	config.setChar("kernelVersion", KernelVersionBuf);
	config.setInt("kernelBreakpointForceReclaim",
	              kernelBreakpointForceReclaim ? 1 : 0);

	// 保存到文件
	if (config.saveConfig("config.ini")) {
		Gui::log("配置已保存 (host=%s:%d, 内核=%c系)", hostBuf, port, KernelVersionBuf);
	} else {
		Gui::log("配置保存失败: 无法写入 config.ini");
	}
}

void ServerConnectWindow::onDraw()
{
	drawConnectionControls();
	drawDriverControls();
}
