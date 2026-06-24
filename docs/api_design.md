# MiniMem API 设计文档

本文沉淀 MiniMem 内存调试 API 的设计契约与关键决策，面向维护者与二次开发。
逐条命令的线格式见 [engine/ceserver/cmd.md](../engine/ceserver/cmd.md)；本文只讲"为什么这样设计"与"必须遵守的契约"。

---

## 1. 分层与单一真相源

```
GUI 窗口 ─┐
          ├─▶ gui/socket/client_singleton.h  ──▶ WinSocketClientMgr(3端口) ──▶ engine/socket_server
MCP ▶ IPC ┘        (协议层 = 单一真相源)
```

- **设备协议的单一真相源是 `gui/socket/client_singleton.h` 的自由函数**（`ReadProcessMemoryBytes` / `WriteProcessMemoryBytes` / `ReadBratchAddr` / `SetKernelBreakpoint` / `ResolveModuleOffsetChain` / `SymbolFind` …）。
- 两类调用方都经它说话：**GUI 窗口**直接调；**外部 AI** 经 MCP(Python) → IPC(`ipc/IpcServer.cpp` :28100) → 同一批函数。
- 后端命令真相源是 `engine/ceserver/CEServer.cpp` 的 `DispatchCommand_V2` + `engine/ceserver/api.cpp` 的 `CApi`。
- **新增设备能力的链路**：`engine` 实现命令 → `socket/*Commands.cpp` 实现客户端函数 + `client_singleton.h` 声明 → 在需要处暴露（GUI 面板 / `IpcServer::RegisterBuiltinMethods()` → `mcp/amem_mcp/tools/`）。两端 opcode 必须一致（`ceserver.h` ↔ `client.hpp`，0 基连续编号）。

---

## 2. 内存读写：连续前缀契约（核心）

### 2.1 背景：被消除的歧义
旧设计中读内存返回单个"真实读取量"标量。多页读取且中间有不可读页时，实现会**跳过空洞继续读后面的页**，返回值只是有效字节之和——同一个返回值对应完全不同的有效区分布，调用方无法定位哪些字节有效，前端按"前 N 字节有效"截断会把填充当数据、并丢掉尾部真实数据。

### 2.2 契约（`IMemoryOp::Read` / `Write`）
> **从 `address` 起连续读/写，遇到首个不可读/不可写页即停止。**
> - 返回值 = 从 `address` 起**连续**成功的字节数（有效区恒为 `buffer[0, 返回值)`）；
> - **不跳过空洞、不续读后续页**（稀疏/大范围读取用批量接口）；
> - 读：实现须将 `buffer[返回值, len)` 清零，调用方仅凭返回值即可定位有效区；
> - 返回值 `< len` 表示在 `address+返回值` 处遇到边界；`== 0` 表示起始即不可访问。

三个内存后端（`AndroidMemorySys` syscall / `AndroidMemoryIO` /proc/mem / `AndroidMemKernel` 内核）**统一遵循该契约**：多页循环遇错 `break`（而非跳过），`pread/pwrite` 负值归零（避免 `-1` 被当 `SIZE_MAX`）。

### 2.3 写：三态语义（写是破坏性的）
写内存可能"部分写入"——前 `written` 字节已真实落地（有副作用），其余中断。因此写 API 区分三态，**绝不能把部分写入当作未写入**：

| 状态 | 判定 | 含义 |
|------|------|------|
| 全部成功 | `written == size` | 完整写入 |
| 部分写入 | `0 < written < size` | 已改 `written` 字节（副作用），剩余因不可写中断 |
| 完全失败 | `written == 0` | 未写入任何字节 |

- 客户端 `WriteProcessMemoryBytes(..., int32_t* outWritten)`：返回值=是否全部写入；`*outWritten`=连续写入量。
- IPC `write_memory`：部分写入返回 `success:false` + 明确错误（含 `written`），让上层/AI 知道已有副作用、不要盲目重试。

### 2.4 批量读：两种接口，各自语义清晰
- **`ReadBratchMemory`（CMD 11）**：按**页**返回**可读的页**，每条 = 页地址 + 整页数据。不可读页不在结果中（页粒度无歧义，地址自证）。适合"读一大段，把可读的页都给我"。
- **`ReadBratchAddr`（CMD 12）**：对每个请求地址返回 `addr + validLen + data[validLen]`，`validLen`=该地址连续可读长度（`0`=不可读）。**每条自带有效长度**，与单次读的连续前缀语义一致，无多页歧义。适合"按一组地址各读一小块"。

---

## 3. 返回值与错误约定

- **哨兵歧义要避免**：用独立的状态字段区分"失败"与"恰好是该值"。范本是 `SymbolFind`——返回 `{result, address}`，`result==0` 才表示找到，从不拿 `address==0` 当"未找到"。新接口应仿此，不要用 `0/地址0/-1` 既表失败又表合法值。
- **客户端层**收敛为 `bool`（成功/失败）或带 out 参数；**IPC 层**统一包成 `{success: bool, result|error}`，是 MCP/AI 的稳定契约。
- **错误信息尽量可操作**：典型如硬件断点**仅内核模式可用**——断点类操作失败时若检测到当前非内核模式，IPC 返回"请先 `init_driver` 切换内核模式"，而非无信息的"失败"。
- 内核断点命中读取 `read_bp_info` 返回 `{total_hits, returned, hits}`：`total_hits`=设备累计命中数，`returned`=本次返回记录数（驱动环形缓冲可能已覆盖旧命中），便于判断是否漏读。

---

## 4. 各域设计要点

### 进程 / 句柄
- `OpenProcessHandle` 返回引擎句柄（`0`=失败）；`AppContext` 缓存当前 pid/handle，并以 `processRevision` 在切换进程时失效旧状态。
- `EnsureOpenHandle` 惰性重开句柄。句柄对应的远程进程若已退出，操作静默失败（返回 0/false）。

### 模块 / 基址
- `GetModuleBaseByName`：优先 **basename 精确匹配**（避免 `"libc.so"` 误命中 `"libc.so.6"`），一个模块多个段时取**最小基址**（即加载基址/ELF 头所在段）；无精确匹配再回退子串匹配。

### 指针偏移链
- `ResolveModuleOffsetChain`：模块基址 + baseOffset，逐级解引用 + 加偏移，可选解引用末级。
- **仅支持 arm64**：指针恒为 8 字节小端（不做 32 位适配）。每级解引用失败即返回 false（不产出垃圾地址），并做加法溢出检查。

### 硬件断点
- **按地址管理**：`set/remove/suspend/resume/read_bp_info` 均以断点地址为键；引擎 `mHwBpList` 为 `地址 → [句柄]`。
- 现实约束：同一地址至多有读写型与执行型两类、通常只下一个——故**未引入按句柄管理**（保持地址键简单够用）。
- **仅内核模式可用**（依赖内核驱动）。

### 符号
- `SymbolInit`（按模块解析并缓存）→ `SymbolGetList`（分页）/ `SymbolFind`（按名查）。分页与名长均有上限校验。

---

## 5. 版本

- 应用版本 / 协议版本统一 **MiniMem 1.0.0 / 协议 1.0.0**：后端运行时版本串 `"MiniMem 1.0.0"` + 协议主版本字节 1；前端 `PROJECT_VERSION` / `PROTOCOL_VERSION`；MCP `amem_mcp.__version__`（`pyproject.toml` 动态读取，单一真相源）。
- `CMD_GETVERSION` 返回的版本字节即协议主版本，前端 `VersionWindow` 直接与 `PROTOCOL_VERSION_MAJOR` 比对。

---

## 6. 已知限制与未决项

- **统一错误码模型**（区分 连接/句柄/权限/参数）：目前仅对最关键的"断点需内核模式"做了可操作提示，其余失败仍收敛为 bool/通用错误。按需增量细化，不做一次性大重构。
- **同址多断点的精细管理**：见 §4，当前按地址键，未支持按句柄区分同址多个断点（现实少见）。
- **ReadBratchMemory 非页对齐起始**：以页大小分块，起始非页对齐时跨页块的页内有效长度未单独标注（仍遵循连续前缀+清零，地址可定位）。

---

## 7. 添加新 API 的规范

1. 引擎：在 `api.cpp`(`CApi`) 实现 + `CEServer.cpp DispatchCommand_V2` 加 case + `ceserver.h` 定义 opcode/结构体。
2. 协议契约：opcode 在 `ceserver.h` 与前端 `client.hpp` 必须一致（0 基连续）；变长数据带显式长度前缀，避免靠固定大小约定。
3. 客户端：`socket/*Commands.cpp` 实现 + `client_singleton.h` 声明；遵守 §2/§3 契约（连续前缀、状态字段而非哨兵、可操作错误）。
4. 暴露：按需在 GUI / `IpcServer::RegisterBuiltinMethods()`(→`mcp/amem_mcp/tools/`) 接入。
5. 收发务必持端口锁（用 `SocketCommand::execute*` 模板）。
