#include "Gui.h"
#include "Window.h"
#include "CEWindow.h"
#include "ServerConnectWindow.h"
#include "ModulesWindow.h"
#include "LogWindow.h"
#include "../imgui/imgui.h"
#include <exception>
#include <functional>
#include <map>
#include <thread>
#include <vector>

#ifdef HAVE_LUAJIT
#include "LuaScriptWindow.h"
#endif

namespace Gui {
	std::list<std::unique_ptr<Window>> windows;
	std::list<std::pair<std::string, int>> logs;
	std::mutex logsMutex;

	namespace {
		std::mutex tasksMutex;
		std::vector<std::function<void()>> pendingTasks;
		std::thread::id guiThreadId;

		void runPendingTasks()
		{
			std::vector<std::function<void()>> tasks;
			{
				std::lock_guard<std::mutex> lock(tasksMutex);
				tasks.swap(pendingTasks);
			}
			for (auto& task : tasks) {
				try {
					task();
				} catch (const std::exception& e) {
					Gui::log("GUI任务执行失败: %s", e.what());
				} catch (...) {
					Gui::log("GUI任务执行失败: 未知异常");
				}
			}
		}
	}

	void postTask(std::function<void()> task)
	{
		if (!task)
			return;
		bool runImmediately = false;
		{
			std::lock_guard<std::mutex> lock(tasksMutex);
			runImmediately = guiThreadId != std::thread::id{} &&
				std::this_thread::get_id() == guiThreadId;
			if (!runImmediately) {
				pendingTasks.push_back(std::move(task));
			}
		}
		if (runImmediately) {
			task();
		}
	}

	std::vector<std::pair<std::string, int>> getLogsSnapshot()
	{
		std::lock_guard<std::mutex> lock(logsMutex);
		return {logs.begin(), logs.end()};
	}

	void addWindow(Window* window)
	{
		static std::map<std::string, int> totalWindows;
		if (!window)
			return;

		int& count = totalWindows[window->name];
		if (count > 0)
			window->name = window->name + " " + std::to_string(count + 1);
		count++;

		windows.emplace_back(window);
	}

	bool mainLoop(Mem::IMemService& service)
	{
		{
			std::lock_guard<std::mutex> lock(tasksMutex);
			guiThreadId = std::this_thread::get_id();
		}
		runPendingTasks();

		static bool bootstrapped = false;
		if (!bootstrapped) {
			if (windows.empty()) {
				Gui::addWindow(new CEWindow(service));
				Gui::addWindow(new ServerConnectWindow(service));
				Gui::addWindow(new LogWindow());
				
#ifdef HAVE_LUAJIT
				Gui::addWindow(new LuaScriptWindow(service));
#endif
			}
			Gui::log("欢迎使用 MiniMem，请先在「服务器连接」窗口连接设备");
			bootstrapped = true;
		}

		bool hasOpenWindow = false;
		for (auto it = windows.begin(); it != windows.end(); ++it)
		{
			Window* w = it->get();
			if (!w) {
				continue;
			}
			// 只绘制打开的窗口，但不删除关闭的窗口（保留状态和指针有效性）
			if (w->pOpen) {
				(*w)();
				hasOpenWindow = hasOpenWindow || w->pOpen;
			}
		}
		return hasOpenWindow;
	}

	void shutdown()
	{
		{
			std::lock_guard<std::mutex> lock(tasksMutex);
			guiThreadId = std::this_thread::get_id();
		}
		runPendingTasks();
		windows.clear();
		// LuaScriptWindow 析构会等待后台脚本退出，期间可能产生最后一批任务。
		runPendingTasks();
		windows.clear();
		std::lock_guard<std::mutex> lock(tasksMutex);
		pendingTasks.clear();
	}
}
