# MiniMem API 设计文档

本文沉淀 MiniMem 内存调试 API 的设计契约与关键决策，面向维护者与二次开发。
逐条命令的线格式见 [engine/ceserver/cmd.md](../engine/ceserver/cmd.md)；本文只讲"为什么这样设计"与"必须遵守的契约"。

---

## 1. 分层与单一真相源

```
GUI 窗口 ─┐
Lua API  ─┼─▶ gui/mem/IMemService ──▶ SystemMemService
MCP ▶ IPC ┘         │ 校验/目标快照/复合事务      │
                    └───────────────────────────▶ gui/socket/client_singleton.h
                                                   │ 线协议单一真相源
                                                   ▼
                                             engine/minimem_server
```

- **设备协议的单一真相源是 `gui/socket/client_singleton.h` 的自由函数**（`ReadProcessMemoryBytes` / `WriteProcessMemoryBytes` / `ReadBratchAddr` / `SetKernelBreakpoint` / `ResolveModuleOffsetChain` / `SymbolFind` …）。
- GUI、Lua 与 IPC 不得直接调用协议函数；它们通过注入的 `IMemService` 使用同一套参数校验、错误码和目标约束。
- `SystemMemService` 是协议层适配器，也是唯一允许从业务层调用 `client_singleton.h` 的实现。
- `DeviceSession` 为连接分配单调 generation。任意发送/接收失败、超时或 EOF 都会 poison 当前连接；无帧协议不通过清空缓冲区恢复，只能显式重连。
- 后端命令真相源是 `engine/ceserver/CEServer.cpp` 的 `DispatchCommand_V2` + `engine/ceserver/api.cpp` 的 `CApi`。
- **新增设备能力的链路**：`engine` 实现命令 → `socket/*Commands.cpp` 实现客户端函数 + `client_singleton.h` 声明 → `IMemBackend`/`IMemService` 适配 → 在需要处暴露（GUI / Lua / IPC → MCP）。两端 opcode 必须一致（`ceserver.h` ↔ `client.hpp`，0 基连续编号）。

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
- 协议层保留轻量结构/out 参数；`MemService` 转换为结构化 `Result<T>`，区分 `connection_changed`、`connection_poisoned`、`target_changed`、`permission_denied`、`partial_write` 与 `completion_unknown`。IPC 继续保持 `{success, result|error}`，并附加 `error_code`/`retryable`。
- 驱动初始化、内存写入和断点变更都分别记录 `requestStarted`、`responseReceived` 与服务端结果。未发送可安全重试；已发送但响应不完整必须返回 `completion_unknown` 并要求重连；明确拒绝不得伪装成成功。
- **错误信息尽量可操作且不预设后端**：硬件断点既可走内核驱动，也可走用户态 `perf_event_open`。失败信息应保留结构化错误码，并提示 root、`perf_event_paranoid`、地址对齐、槽位耗尽等中性排查方向，不能一律要求切换内核模式。
- 断点命中读取 `read_bp_info` 返回 `{total_hits, returned, dropped, hits}`：`total_hits`=设备累计命中数（可大于单次返回上限），`returned`=本次返回记录数，`dropped`=与同一目标、同一地址上次成功轮询相比新增但未返回的条数。设备最多保留最近 100000 条待取记录。
- Kernel 断点槽位查询 `query_hwbp_slots` 通过 `CMD_KERNEL_QUERYHWBPTHREADS` 枚举当前目标 `/proc/<pid>/task` 下的 TID，并逐线程调用 `hwbp_query_task`。响应保留每个线程的计数、槽位条目和 errno；查询期间退出的线程只标记失败，不影响其它线程。服务边界在非 Kernel 模式明确返回 `permission_denied`。

---

## 4. 各域设计要点

### 进程 / 句柄
- `AppContext::TargetMutation` 在一把状态锁内发布 pid、handle、名称与缓存失效；revision 在变更中为奇数、稳定时为偶数。
- `TargetSnapshot` 同时包含 pid、handle、process revision 和 connection generation。每个目标操作必须在发送前及结果提交前验证快照。
- 不再惰性重开句柄。进程选择仅能通过 `IMemService::openProcess` 完成，清理旧断点、关闭旧句柄、打开新句柄和状态发布属于同一事务。

### 模块 / 基址
- `GetModuleBaseByName`：优先 **basename 精确匹配**（避免 `"libc.so"` 误命中 `"libc.so.6"`），一个模块多个段时取**最小基址**（即加载基址/ELF 头所在段）；无精确匹配再回退子串匹配。

### 指针偏移链
- `ResolveModuleOffsetChain`：模块基址 + baseOffset，逐级解引用 + 加偏移，可选解引用末级。
- **仅支持 arm64**：指针恒为 8 字节小端（不做 32 位适配）。每级解引用失败即返回 false（不产出垃圾地址），并做加法溢出检查。

### 硬件断点
- **按地址管理**：`set/remove/suspend/resume/read_bp_info` 均以断点地址为键；引擎 `mHwBpList` 为 `地址 → [句柄]`。
- 现实约束：同一地址至多有读写型与执行型两类、通常只下一个——故**未引入按句柄管理**（保持地址键简单够用）。
- 设备端支持内核驱动与用户态 `perf_event_open` 两种实现；两者均要求 root，实际可用性还受内核配置、`perf_event_paranoid` 和硬件槽位限制。
- GUI 在初始化 Kernel 驱动后同步“断点槽抢占”开关。该策略仅对 Kernel 后端生效：per-TID 普通下断失败后使用 `NI_HWBP_F_FORCE_RECLAIM` 重试，并对初始线程与后续新线程记录重试结果。
- 前端按当前连接跟踪已设置的断点地址：切换同一连接内的目标进程时先清理旧目标断点；断开、重连或连接失败时只重置本地跟踪，绝不向新连接发送旧地址的删除命令。
- 断点命中轮询使用独立 `PORT_DEBUG` 事务，避免高命中读取阻塞主命令通道。

### IPC HTTP 边界
- IPC 固定监听 loopback，只接受 `/` 上的 JSON POST；必须携带 `Content-Type: application/json`（允许 charset 参数）。
- 不开放 CORS、不处理 `OPTIONS`，并拒绝任何非空 `Origin` 请求头，避免网页借助本机端口发起内存写入或断点操作。
- 拒绝重复请求头、折叠头和 `Transfer-Encoding`，只接受唯一十进制 `Content-Length`；请求头与 body 合计上限 1 MiB。
- 固定 4 个工作线程和 64 个待处理连接。每个 IPC 请求有 30 秒总预算；未显式设置调用预算的同步 socket I/O 仍有 5 秒单次传输上限，静默设备不能无限占住 GUI 或工作线程。

### 符号
- `loadSymbolTable` 在一个端口事务中完成模块校验、`SymbolInit` 和所有分页读取；`listSymbols`/MCP 的 `symbol_list(module_base=...)` 在一个事务中初始化后只拉取目标页，避免为 100 条结果加载最多 100 万条符号。

### Lua
- 每次顶层 Lua 文件、字符串或回调执行都会绑定一个 `OperationContext`。同一脚本内的内存、模块、符号和断点操作共享连接 generation、目标 revision 与截止时间。
- 外部线程中途切换连接或进程时，后续 Lua 操作失败而不会悄悄转向新目标；只有脚本显式调用 `process.attach()` 才会把绑定上下文推进到新目标。

---

## 5. 版本

- 应用版本 / 协议版本统一 **MiniMem 1.0.0 / 协议 1.0.0**：后端运行时版本串 `"MiniMem 1.0.0"` + 协议主版本字节 1；前端 `PROJECT_VERSION` / `PROTOCOL_VERSION`；MCP `minimem_mcp.__version__`（`pyproject.toml` 动态读取，单一真相源）。
- `CMD_GETVERSION` 返回的版本字节即协议主版本，前端 `VersionWindow` 直接与 `PROTOCOL_VERSION_MAJOR` 比对。

---

## 6. 已知限制与未决项

- 设备端仍只返回部分细粒度状态，因此除驱动授权外的部分服务端拒绝仍归类为 `protocol_error`；连接、目标、参数、授权拒绝、部分写入和结果未知均已有稳定错误码。
- **同址多断点的精细管理**：见 §4，当前按地址键，未支持按句柄区分同址多个断点（现实少见）。
- **ReadBratchMemory 非页对齐起始**：以页大小分块，起始非页对齐时跨页块的页内有效长度未单独标注（仍遵循连续前缀+清零，地址可定位）。

---

## 7. 添加新 API 的规范

1. 引擎：在 `api.cpp`(`CApi`) 实现 + `CEServer.cpp DispatchCommand_V2` 加 case + `ceserver.h` 定义 opcode/结构体。
2. 协议契约：opcode 在 `ceserver.h` 与前端 `client.hpp` 必须一致（0 基连续）；变长数据带显式长度前缀，避免靠固定大小约定。
3. 客户端：`socket/*Commands.cpp` 实现 + `client_singleton.h` 声明；遵守 §2/§3 契约（连续前缀、状态字段而非哨兵、可操作错误）。
4. 服务：在 `IMemBackend`/`IMemService` 增加类型化操作，并在 `SystemMemService` 适配协议函数。
5. 暴露：按需在 GUI / Lua / `IpcServer::RegisterBuiltinMethods()`(→`mcp/minimem_mcp/tools/`) 注入服务，禁止直接包含 `client_singleton.h`。
6. 单次收发使用 `SocketCommand::execute*`；依赖多个命令共享远端状态时必须额外持 `TransactionLease`。
