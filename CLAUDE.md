# CLAUDE.md

本文件为在此仓库工作的 Claude Code 提供指引。详尽说明见 [README.md](README.md)。

## 项目概述

AndroidMiniMem 是一个**精简版** Android 远程内存调试工具，由 `android_mem_engine`（后端）与 `AMem`（前端）重构而来。三个子项目：

- `engine/` — Android ARM64 设备端，CE 协议 socket 服务（C++23，需 NDK）
- `gui/` — 跨平台前端（C++17，ImGui），含内嵌 IPC 服务与 UXN 异常断点窗口
- `mcp/` — Python MCP 服务，把 IPC 能力暴露给外部 AI 助手

## 核心约束：本项目"只做基础功能"

**已被刻意移除**（不要重新引入）：数据搜索、指针扫描、冻结列表、SO 注入、内置 AI 聊天、GUI 的扫描/内存查看器/断点显示窗口。

**保留**：进程/模块列表、内存读写（含批量）、硬件断点、UXN 异常断点、内核切换（`init_driver`）、ELF 符号、指针偏移链解析、Lua 脚本引擎（用于复杂分析）。

## 架构要点

- 设备协议的**单一真相源**是 `gui/socket/client_singleton.h` 的自由函数。新增设备能力 = 在 `socket/*Commands.cpp` 实现 + 在 `client_singleton.h` 声明，再在需要的前端暴露（GUI 面板 / `ipc/IpcServer.cpp` 的 `RegisterBuiltinMethods()` → 对应 `mcp/minimem_mcp/tools/`）。
- 后端命令分发在 `engine/ceserver/CEServer.cpp` 的 `DispatchCommand_V2`（socket 接口专用，V1 已随 pipe/stdio 接口移除），具体实现在 `engine/ceserver/api.cpp` 的 `CApi`。
- **内核切换**：`CApi::InitReadWriteDriver` 尝试连接/加载内核驱动并把全局 `g_memIO` 热替换为 `AndroidMemKernel`（否则默认 `AndroidMemorySys` syscall 模式）；`GetRWDriverType` 返回当前模式。
- IPC 服务监听 `127.0.0.1:28100`，由 `gui/main.cpp` 启动；MCP（Python）通过 HTTP JSON 转发到它。

## 构建

- 后端：`cd engine && ./build.sh`（需 `-DANDROID_NDK=`），产物 `bin/minimem_server`。
- 前端：`cd gui && cmake -S . -B build -G Ninja && cmake --build build`，需 `third_party/LuaJIT`，产物 `bin/MiniMemClient.exe`。Capstone/Keystone 可选（Lua 反汇编/汇编）。
- MCP：`cd mcp && pip install -e .`，命令 `minimem-mcp`（包名为 `minimem_mcp`）。

## 代码规范

- 注释与提交信息用中文。后端 C++23 / C11，前端 C++17。
- 可选功能用宏门控：`HAVE_LUAJIT` / `HAVE_CAPSTONE` / `HAVE_KEYSTONE`（前端），`BUILD_PTRACE_HW`（后端）。
- 单例普遍采用 Meyer's（`GetInstance()`/`Get()`）。Socket 收发必须持端口锁（用 `SocketCommand::execute*` 模板）。
