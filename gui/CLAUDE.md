# CLAUDE.md — gui/（MiniMem Windows 前端）

精简版 Windows 前端：Dear ImGui + DirectX 12。**前端只为方便 MCP 调用**，仅保留服务器连接、进程选择、模块列表、日志、Lua 脚本管理器，并在 `127.0.0.1:28100` 内嵌 IPC 服务供 MCP 桥接。

## 构建

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

产物 `bin/MiniMemClient.exe`。依赖：DirectX 12 SDK、Windows SDK，以及 `third_party/LuaJIT/`（`include/` + `lib/lua51.lib`，缺失则 `FATAL_ERROR`）。可选：Capstone（反汇编）、Keystone（汇编）——供 Lua 汇编 API 使用，缺失则相应功能禁用。`third_party/nlohmann/json.hpp` 供 IPC 使用。

## 架构

- `main.cpp` — Win32 窗口 + 渲染循环（`Gui::mainLoop()`），启动 IPC 服务（28100 端口）。
- `socket/client_singleton.h/.cpp` — 设备协议**单一真相源**，`WinSocketClientMgr` 管理三端口（MAIN/DEBUG/ERROR）。命令实现按域拆分：`ProcessCommands` / `MemoryCommands` / `BreakpointCommands` / `SymbolCommands`（**无** Scan/Freeze）。
- `ipc/IpcServer.cpp` — 手写 HTTP 服务（仅 127.0.0.1:28100），`RegisterBuiltinMethods()` 注册 21 个方法，是 MCP 的桥梁。
- `gui/` — 窗口：`CEWindow`(主控/进程选择) `ServerConnectWindow` `ModulesWindow` `LogWindow` `VersionWindow` `LuaScriptWindow` `LuaImGuiWindow`；`Window` 基类、`Gui` 命名空间、`AppContext`（进程状态 + 模块/符号缓存）、`EventBus`。
- `lua/` — `LuaEngine` + `LuaAPI`(进程/模块/断点) / `LuaAPI_Memory`(读写) / `LuaAPI_ImGui`(绘制) / `LuaAPI_Assembly`(汇编，Capstone/Keystone 门控)。Lua 用于复杂分析，可被 GUI 与 IPC `execute_lua` 触发。

## 重要：已移除的能力

数据搜索、指针扫描、冻结、SO 注入、内置 AI 聊天（`gui/ai/`）、扫描/内存查看器/断点显示窗口均已删除。`client_singleton.h` 不再声明任何 Scan/Freeze 函数；新增能力请遵循"`socket/*Commands.cpp` 实现 → `client_singleton.h` 声明 → IPC handler / GUI / Lua 暴露"的链路。

## 约定

- 单例用 Meyer's（`GetInstance()`/`Get()`）。Socket 收发必须持端口锁，用 `socket/SocketCommand.h` 的 `execute*` 模板。
- 后台线程不得直接碰 ImGui。可选功能用 `HAVE_LUAJIT`/`HAVE_CAPSTONE`/`HAVE_KEYSTONE` 门控。
