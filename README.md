# AndroidMiniMem

一个**精简版** Android 远程内存调试工具，由两份既有项目重构而来：

- 后端引擎 `android_mem_engine`（Android ARM64，Cheat Engine 协议服务端）
- 前端 GUI `AMem`（Windows + Dear ImGui，内置 IPC + Python MCP 桥接）

MiniMem 只保留"最基本"的能力，砍掉了数据搜索、指针扫描、冻结列表等重型功能，主要目的是**方便通过 MCP 让 AI 助手驱动内存调试**。

QQ交流群:1065266874

## 目录结构（Monorepo）

```
AndroidMiniMem/
├── engine/        Android ARM64 后端（设备端常驻 socket 服务）
├── gui/           Windows 前端（连接显示 + 日志 + Lua + 内嵌 IPC 服务）
└── mcp/           Python MCP 服务（把 IPC 能力暴露给外部 AI 助手）
```

## 数据流

```
外部 AI 助手 ──(MCP/stdio)──▶ mcp/ (Python)
                                  │ HTTP JSON
                                  ▼
              gui/ 内嵌 IpcServer (127.0.0.1:28100)
                                  │
                                  ▼
              gui/mem/ IMemService（校验、目标快照、复合事务）
                                  │ 调用 socket/client_singleton.h 协议层
                                  ▼
              gui/ WinSocketClientMgr ──(TCP)──▶ engine/ mini_server (Android 设备)
                                                      │
                                                      ▼
                    内存读写（内核 / syscall）与断点（内核 / perf）/ 符号
```

`socket/client_singleton.h` 中的自由函数是设备线协议的**单一真相源**；GUI、Lua、IPC(MCP) 统一依赖 `gui/mem/IMemService.h`，只有 `SystemMemService` 可以进入协议层。连接代际、目标 revision、复合事务和错误语义因此对所有入口保持一致。

## 保留 / 移除的能力

| 能力 | 状态 | 说明 |
|------|------|------|
| 进程列表 / 模块列表 | ✅ 保留 | `list_processes` / `list_modules` / `get_module_base` |
| 内存读 / 写 / 批量读 | ✅ 保留 | `read_memory` / `write_memory` / `read_batch` |
| 硬件断点（设置/删除/暂停/恢复/读命中） | ✅ 保留 | `set/remove/suspend/resume_breakpoint` / `read_bp_info` |
| 内核切换 | ✅ 保留 | `init_driver` 加载/连接内核驱动并热切换 `g_memIO`（IO/Syscall/Kernel/SysHook） |
| ELF 符号解析 | ✅ 保留 | `symbol_init` / `symbol_list` / `symbol_find` |
| 指针偏移链解析 | ✅ 保留 | `resolve_offset_chain` |
| Lua 脚本引擎 | ✅ 保留 | 用于复杂分析（`execute_lua` + GUI 脚本窗口） |
| 数据搜索（首次/再次/模糊/分组/HEX） | ❌ 移除 | — |
| 指针扫描 | ❌ 移除 | — |
| 冻结列表 | ❌ 移除 | — |
| SO 注入 / 远程 mmap | ❌ 移除 | — |
| GUI 数值扫描/内存查看器/断点显示窗口 | ❌ 移除 | 断点不再在前端显示 |
| 内置 AI 聊天（gui/ai） | ❌ 移除 | 改用 MCP 路径 |

前端只保留：主控制面板、服务器连接、模块列表、日志、Lua 脚本管理器。

## 构建

> **首次克隆**：`gui/imgui` 为 git 子模块（Dear ImGui，docking 分支），需初始化：
> ```bash
> git clone https://github.com/niqiuqiux/AndroidMiniMem.git
> cd AndroidMiniMem
> git submodule update --init --recursive
> # 或克隆时一步到位：git clone --recurse-submodules <url>
> ```

### 1. 后端引擎 `engine/`（Android ARM64）

需要 Android NDK（r27c/r28c）。产物为设备端可执行文件 `bin/mini_server`。

```bash
cd engine
./build.sh                 # Linux
# 或 build.bat            # Windows
# 或直接：
cmake -S . -B build -DANDROID_NDK=<ndk路径> -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

把 `mini_server`（及内核驱动 `Mem.ko`/`CFI.ko`，如使用内核模式）推送到设备并以 root 运行，监听端口（默认 52736）。

### 2. 前端 GUI `gui/`（Windows x64）

需要 Visual Studio 2022 / Clang、CMake、DirectX 12 SDK，以及 `third_party/LuaJIT`（必需）。Capstone/Keystone 可选（启用 Lua 反汇编/汇编）。

```bash
cd gui
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

产物 `bin/MiniMemClient.exe`，启动后自动在 `127.0.0.1:28100` 开启 IPC 服务。IPC 仅接受无非空 `Origin`、具有唯一 `Content-Length` 且 `Content-Type` 为 `application/json` 的 POST 请求，不开放浏览器 CORS，也不接受 `Transfer-Encoding`。

### 3. MCP 服务 `mcp/`（Python 3.10+）

```bash
cd mcp
pip install -e .       # 注册 amem-mcp 命令
amem-mcp               # 启动 stdio MCP，桥接 GUI 的 IPC 服务
```

仓库根的 `.mcp.json` 已为 Claude Code 配好（`python -m amem_mcp`，`PYTHONPATH` 指向 `mcp/`）。需先启动 GUI 并连上设备。

## MCP 工具一览

进程/模块：`list_processes` `open_process` `list_modules` `get_module_base` `resolve_offset_chain`
内存：`read_memory` `write_memory` `read_batch`
断点：`set_breakpoint` `remove_breakpoint` `suspend_breakpoint` `resume_breakpoint` `read_bp_info`
符号：`symbol_init` `symbol_list` `symbol_find`
脚本：`execute_lua`
状态：`get_status` `get_version` `get_architecture` `init_driver`

## 文档

- [engine/ceserver/cmd.md](engine/ceserver/cmd.md) — Socket 二进制协议：命令字、线格式、内存读写"连续前缀"语义
- [docs/api_design.md](docs/api_design.md) — API 设计契约与决策：连续前缀读/写三态、批量读、返回值/错误约定、模块基址/断点/符号/指针链要点、新增 API 规范
- [docs/amem_memservice_sync.md](docs/amem_memservice_sync.md) — AMem MemService 重构的同步范围、排除项与后续判断规则
- 各子项目另有 `CLAUDE.md`（`engine/`、`gui/`）说明构建与架构。
