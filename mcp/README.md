# AMem MCP Server

AMem MCP Server 是一个基于 [Model Context Protocol](https://modelcontextprotocol.io/) 的服务。它对 AI 助手暴露 MCP 工具，对内通过 HTTP JSON 代理到 AMem GUI 内嵌的 IPC Server，从而把 GUI 的 C++ 能力暴露给 Claude Code / Claude Desktop / Codex CLI / Cursor / VS Code Copilot / Continue 等客户端。

## 架构

```
AI 助手  ←── stdio ──→  amem-mcp (Python)  ←── HTTP JSON ──→  AMem GUI (C++ IPC :28100)
                                                                       ↕
                                                                 Android 设备
```

MCP Server 本身不直接与 Android 设备通信，所有操作都委托给 AMem GUI 的 IPC Server（默认监听 `127.0.0.1:28100`）。默认业务路径是 HTTP JSON 代理路径：MCP 工具收到调用后，会把请求转成 HTTP JSON 发给 GUI IPC Server。

### 协议分层说明

这里有两层通信，容易混淆：

| 连接 | 协议 / 传输 | 说明 |
|------|-------------|------|
| AI 助手 ↔ `amem-mcp` | MCP over `stdio` | IDE/Codex 启动本地 Python 进程，通过标准输入输出交换 MCP 消息 |
| `amem-mcp` ↔ AMem GUI IPC Server | HTTP JSON | 默认业务路径。MCP 工具内部把请求转发到 GUI 的 IPC Server，默认地址 `http://127.0.0.1:28100` |

因此 `.mcp.json` / `~/.codex/config.toml` 里配置的是第一层：用 `command` 启动 `amem-mcp`，不是填 HTTP URL。真正访问 AMem 的默认路径是第二层 HTTP JSON，地址通过 `AMEM_IPC_HOST` / `AMEM_IPC_PORT` 或 `--ipc-host` / `--ipc-port` 指定。

当前 Python MCP 入口仅支持 `stdio`：

```bash
amem-mcp --transport stdio
python -m amem_mcp --transport stdio
```

## 环境要求

- Python 3.10+
- AMem GUI 已启动（IPC Server 随 GUI 自动启动）

## 安装

两种方式任选其一。

### A. 可编辑安装（推荐）

```bash
cd mcp
pip install -e .
```

安装后会注册 `amem-mcp` 命令，无需再写绝对路径。

### B. 仅安装依赖

```bash
cd mcp
pip install -r requirements.txt
```

用 `python -m amem_mcp` 或 `python server.py` 启动。

## 启动 / 命令行

```bash
amem-mcp                                       # pip install 后
python -m amem_mcp                             # 模块方式
python server.py                               # 兼容入口（任意 cwd）

amem-mcp --ipc-host 127.0.0.1 --ipc-port 28100 # 指定 IPC 地址
```

也支持环境变量 `AMEM_IPC_HOST` / `AMEM_IPC_PORT`（对 IDE 配置很有用）。

### 路径与环境变量

如果已经执行 `pip install -e .`，推荐在 IDE 配置中直接使用：

```json
{ "command": "amem-mcp" }
```

如果没有安装包，需要用 `PYTHONPATH` 指向本项目的 `mcp` 目录，然后用模块方式启动：

```json
{
  "command": "python",
  "args": ["-m", "amem_mcp"],
  "env": {
    "PYTHONPATH": "D:/AndroidMEM/AndroidMiniMem/mcp",
    "AMEM_IPC_HOST": "127.0.0.1",
    "AMEM_IPC_PORT": "28100"
  }
}
```

路径要按运行客户端的系统填写：

| 环境 | `PYTHONPATH` 示例 |
|------|-------------------|
| Windows 原生 IDE | `D:/AndroidMEM/AndroidMiniMem/mcp` |
| WSL / Linux Codex | `/home/qiu/桌面/MEMTool/AndroidMiniMem/mcp` |

`AMEM_IPC_HOST` / `AMEM_IPC_PORT` 指的是 AMem GUI IPC Server 地址，不是 MCP Server 的监听地址。

---

## IDE 接入

每个 IDE 需要的配置格式不同。`configs/` 目录下提供了全部样例，复制后按本机路径调整即可用。

> 以下示例假设用的是**可编辑安装**，推荐把 `command: "python", args: ["-m", "amem_mcp"]` 改为 `command: "amem-mcp"` 并删除 `args`、`env.PYTHONPATH`。

> 注意：这些 IDE 配置都是 `stdio` MCP。不要把 `http://127.0.0.1:28100` 写成 MCP URL；它只是 `amem-mcp` 内部访问 GUI IPC 的地址。

### Claude Code

项目根目录创建 `.mcp.json`（或合并到 `~/.claude.json`）：

```json
{
  "mcpServers": {
    "amem": {
      "command": "amem-mcp",
      "env": {
        "AMEM_IPC_HOST": "127.0.0.1",
        "AMEM_IPC_PORT": "28100"
      }
    }
  }
}
```

如果没做 pip install：

```json
{
  "mcpServers": {
    "amem": {
      "command": "python",
      "args": ["-m", "amem_mcp"],
      "env": {
        "PYTHONPATH": "D:/AndroidMEM/AndroidMiniMem/mcp",
        "AMEM_IPC_HOST": "127.0.0.1",
        "AMEM_IPC_PORT": "28100"
      }
    }
  }
}
```

模板：[`configs/claude-code.json`](./configs/claude-code.json)

### Claude Desktop

合并到 `%APPDATA%/Claude/claude_desktop_config.json`（macOS 是 `~/Library/Application Support/Claude/claude_desktop_config.json`）：

```json
{
  "mcpServers": {
    "amem": { "command": "amem-mcp" }
  }
}
```

模板：[`configs/claude-desktop.json`](./configs/claude-desktop.json)

### Codex CLI

合并到 `~/.codex/config.toml`（Codex 只支持用户级配置，不支持项目级）：

```toml
[mcp_servers.amem]
command = "amem-mcp"

[mcp_servers.amem.env]
AMEM_IPC_HOST = "127.0.0.1"
AMEM_IPC_PORT = "28100"
```

如果没有安装包：

```toml
[mcp_servers.amem]
command = "python"
args = ["-m", "amem_mcp"]

[mcp_servers.amem.env]
PYTHONPATH = "/home/qiu/桌面/MEMTool/AndroidMiniMem/mcp"
AMEM_IPC_HOST = "127.0.0.1"
AMEM_IPC_PORT = "28100"
```

注意 Codex 的 key 是 `mcp_servers`（下划线），不是 JSON 系列的 `mcpServers`。Codex 目前读取用户级 `~/.codex/config.toml`，不会自动读取项目根目录的 `.mcp.json`。

模板：[`configs/codex.toml`](./configs/codex.toml)

### Cursor

放置于 `~/.cursor/mcp.json` 或项目内 `.cursor/mcp.json`：

```json
{
  "mcpServers": {
    "amem": { "command": "amem-mcp" }
  }
}
```

模板：[`configs/cursor.json`](./configs/cursor.json)

### VS Code (GitHub Copilot)

VS Code 的 MCP 配置 key 是 `servers` 而不是 `mcpServers`。放置于项目 `.vscode/mcp.json`：

```json
{
  "servers": {
    "amem": {
      "type": "stdio",
      "command": "amem-mcp"
    }
  }
}
```

模板：[`configs/vscode.json`](./configs/vscode.json)

### Continue (VS Code / JetBrains 插件)

合并到 `~/.continue/config.json` 的 `experimental.modelContextProtocolServers` 数组。模板：[`configs/continue.json`](./configs/continue.json)

---

## 通信协议

本节描述默认业务通信路径：`amem-mcp` 内部通过 HTTP POST 向 AMem GUI IPC Server 发送 JSON 请求。MCP 客户端本身仍然通过 `stdio` 调用 `amem-mcp`，但所有进程、模块、内存、断点、Lua、符号等工具最终默认都会走这条 HTTP JSON 代理路径。

```json
// 请求
{ "method": "read_memory", "params": { "address": "0x7f12345000", "size": 256 } }

// 响应
{ "success": true, "result": { "hex": "48656c6c6f...", "size": 256 } }
```

默认 30 秒超时，扫描类操作 60 秒。仅支持本地回环地址。

`reference/` 目录里的二进制协议客户端只作为历史/参考实现保留，MCP Server 默认不使用它。

---

## 工具列表

### 状态与连接

| 工具 | 说明 |
|------|------|
| `get_status()` | 获取 GUI 当前状态（连接状态、PID、进程名） |
| `get_server_version()` | 获取 Android 服务端版本信息 |
| `get_architecture()` | 获取目标设备内存架构类型 |
| `init_driver(card_name)` | 初始化内核读写驱动，需传入授权卡密 |

### 进程与模块

| 工具 | 说明 |
|------|------|
| `list_processes()` | 列出 Android 设备上所有运行中的进程 |
| `open_process(pid)` | 打开指定 PID 的进程，后续操作针对此进程 |
| `list_modules(filter, offset, count)` | 列出当前进程加载的模块 |
| `get_module_base(module_name)` | 获取指定 `.so` 模块的基址（名称必须包含 `.so`） |
| `resolve_offset_chain(module, base_offset, offsets, deref_final)` | 基于指定 `.so` 解析指针链 |

### 内存读写

| 工具 | 参数 | 说明 |
|------|------|------|
| `read_memory(address, size=256)` | size 最大 65536 | 读取内存并返回 hex dump |
| `read_value(address, data_type="dword")` | byte/word/dword/qword/float/double | 读取单个值 |
| `write_value(address, value, data_type="dword")` | — | 写入单个值 |
| `write_bytes(address, hex_string)` | 如 `"90 90 90"` | 写入原始字节 |

> 注：精简版**不含**数据搜索 / 指针扫描 / 冻结工具。`.so` 定位请用 `get_module_base` + `resolve_offset_chain`；非 `.so` 或匿名段（例如同名同权限的 heap/anon 段）请用 `list_modules` 查看具体段信息，避免把首个匹配段误当作模块基址。

### 硬件断点

断点类型编号与 AMem 内部（GUI / Lua / IPC）完全一致，MCP 层不做任何翻译：

| `bp_type` | 语义 | 字符串别名 |
|-----------|------|-----------|
| `1` | 读 | `"read"` |
| `2` | 写 | `"write"` |
| `3` | 读写 | `"readwrite"` / `"access"` |
| `4` | 执行 | `"execute"` |

| 工具 | 参数 | 说明 |
|------|------|------|
| `set_breakpoint(address, bp_type=2, bp_size=4)` | `bp_type`: 1~4 整数或字符串别名; `bp_size`: 1/2/4/8（执行断点强制 4） | 设置硬件断点 |
| `remove_breakpoint(address)` | — | 移除断点 |
| `read_breakpoint_info(address)` | — | 读取断点命中信息（含 ARM64 寄存器状态） |
| `suspend_breakpoint(address)` | — | 暂停断点（不删除） |
| `resume_breakpoint(address)` | — | 恢复已暂停的断点 |

### Lua 脚本

| 工具 | 说明 |
|------|------|
| `execute_lua(code)` | 在 GUI 内执行 Lua，可使用 mem/process/bp 等 API（用于复杂分析） |

### 符号

| 工具 | 说明 |
|------|------|
| `symbol_init(module_base)` | 初始化指定模块的符号表 |
| `symbol_list(offset, count, module_base="")` | 分页列出符号 |
| `symbol_find(module_base, symbol_name)` | 按名称查找符号地址 |

---

## MCP Resources

| URI | 说明 |
|-----|------|
| `amem://status` | 当前 AMem GUI 状态（连接、PID、进程名） |

---

## 典型使用流程

```
1. get_status()                          # 确认 GUI 已连接设备
2. list_processes()                      # 查看进程列表
3. open_process(pid=12345)               # 打开目标进程
4. list_modules()                        # 查看模块列表
5. get_module_base("libxxx.so")          # 取模块基址
6. resolve_offset_chain("libxxx.so", "0x1234", [0x10, 0x8])  # 解析指针偏移链
7. read_memory("0x7f1234", 64)           # 读取内存
8. write_value("0x7f1234", "999")        # 修改内存值
9. set_breakpoint("0x7f1234", 2, 4)      # 设置写入断点 (2=写)
10. read_breakpoint_info("0x7f1234")     # 查看谁修改了这个地址
```

---

## 目录结构

```
mcp/
├── amem_mcp/                # 主包
│   ├── __init__.py
│   ├── __main__.py          # python -m amem_mcp 入口
│   ├── app.py               # FastMCP 装配 + main()
│   ├── constants.py         # 扫描 flag / 数据类型 / 内存类型
│   ├── helpers.py           # hex_dump / encode_value / make_scan_flags
│   ├── ipc_client.py        # HTTP JSON 客户端
│   └── tools/               # 工具按域拆分
│       ├── status.py        # 状态、版本、架构、驱动
│       ├── process.py       # 进程、模块、指针链
│       ├── memory.py        # 内存读写
│       ├── breakpoint_.py   # 硬件断点
│       ├── lua.py           # Lua 执行
│       └── symbols.py       # 符号表
├── configs/                 # 各 IDE 配置样例
│   ├── claude-code.json
│   ├── claude-desktop.json
│   ├── codex.toml
│   ├── continue.json
│   ├── cursor.json
│   └── vscode.json
├── reference/               # 二进制协议参考实现（MCP 不用）
│   ├── README.md
│   └── amem_client.py
├── pyproject.toml           # pip 安装入口，注册 amem-mcp 命令
├── requirements.txt
├── server.py                # 兼容入口
└── README.md
```

## 开发

添加新工具：在 `amem_mcp/tools/` 下新建或编辑模块，实现 `register(mcp, ipc)` 函数，并在 `tools/__init__.py` 的 `register_all` 里注册。所有扫描/类型常量都在 `amem_mcp/constants.py`，复用已有 helper 可避免重复的编码逻辑。
