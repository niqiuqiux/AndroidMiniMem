# MiniMem Socket 通信协议

精简版后端（`minimem_server`）的二进制通信协议。客户端通过 TCP Socket 连接，每条命令以**单字节 opcode** 开头，后跟对应的参数结构体/数据块。所有命令由 `CEServer.cpp` 的 `DispatchCommand_V2` 分发。

> 本精简版**不含**数据搜索 / 指针扫描 / 冻结 / SO注入 / 远程mmap / 线程上下文 / CE风格调试事件 / 旧式快照(Process32/Module32) 等命令。

## 1. 字节序与对齐

- 目标平台仅 **arm64**，字节序小端，指针恒为 8 字节。
- 结构体定义见 `ceserver.h`，含 `std::vector` 的结构体在 pack 之外，其余在 `#pragma pack(1)` 内。
- 命令字节 + 结构体顺序必须严格一致，否则解析失败。

## 2. 命令字一览

| 命令宏 | opcode | 功能 | 参数 / 返回 |
|--------|--------|------|-----------|
| CMD_GETVERSION | 0 | 获取服务端版本 | 返回 `CeVersion` + 版本字符串 |
| CMD_CLOSECONNECTION | 1 | 关闭当前连接 | 无返回 |
| CMD_TERMINATESERVER | 2 | 关闭服务端 | 无返回，触发优雅退出 |
| CMD_GETMEMTYPE | 3 | 查询当前读写模式（内核切换状态） | 返回: uint8 (0:null 1:io 2:syscall 3:kernel 4:syshook) |
| CMD_INITRWDRIVER | 4 | 初始化内核读写驱动并热切换 g_memIO | 参数: 授权卡密字符串；返回: 结果码 / 卡密时间 |
| CMD_OPENPROCESS | 5 | 打开进程，返回句柄 | 参数: int pid；返回: int handle（0=失败） |
| CMD_CLOSEHANDLE | 6 | 关闭句柄 | 参数: int handle；返回: int |
| CMD_GETPROCESSLIST | 7 | 获取进程列表 | 返回: int 进程数 + N×(pid/名长 + 进程名) |
| CMD_GETMODULELIST | 8 | 获取模块列表 | 返回: int 模块数 + N×(`CeModuleListEntry` + 模块名) |
| CMD_READPROCESSMEMORY | 9 | 读进程内存（**连续前缀**，见 §3） | 参数: `CeReadProcessMemoryInput`；返回: int read + size 字节数据 |
| CMD_WRITEPROCESSMEMORY | 10 | 写进程内存（**连续前缀**，见 §3） | 参数: `CeWriteProcessMemoryInput` + 数据；返回: `CeWriteProcessMemoryOutput`(int written) |
| CMD_READBRATCHMEMORY | 11 | 批量读取（按页返回有效页，见 §3） | 参数: `CeReadBratchMemory`；返回: int 页数 + N×(u64 addr + 整页数据) |
| CMD_READBRATCHADDR | 12 | 批量按地址读取（每条带有效长度，见 §3） | 参数: int N + N×`CeReadBratchAddr`；返回: int N + N×(u64 addr + u32 validLen + data[validLen]) |
| CMD_KERNEL_SETBREAKPOINT | 13 | 设置硬件断点 | 参数: int handle + u64 地址 + u32 类型 + u32 长度；返回: int(>0 成功 / 0 失败) |
| CMD_KERNEL_REMOVEBREAKPOINT | 14 | 删除硬件断点 | 参数: int handle + u64 地址；返回: int |
| CMD_KERNEL_SUSPENDBREAKPOINT | 15 | 暂停硬件断点 | 参数: int handle + u64 地址；返回: int |
| CMD_KERNEL_RESUMEBREAKPOINT | 16 | 恢复硬件断点 | 参数: int handle + u64 地址；返回: int |
| CMD_KERNEL_READHWBPINFO | 17 | 读取硬件断点命中记录 | 参数: int handle + u64 地址；返回: int 本次记录数（最多 100000）+ u64 设备累计命中数 + N×`HW_HIT_INFO` |
| CMD_SYMBOL_INIT | 18 | 初始化模块符号表 | 参数: `CeSymbolInitInput`；返回: `CeSymbolInitOutput`(result, totalCount) |
| CMD_SYMBOL_GETLIST | 19 | 分页获取符号列表 | 参数: `CeGetSymbolListInput`；返回: `CeGetSymbolListOutput` + N×(`CeSymbolEntry`+名称) |
| CMD_SYMBOL_FIND | 20 | 按名称查找 ELF 符号 | 参数: `CeFindSymbolInput` + 名称；返回: `CeFindSymbolOutput`(result, address) |
| CMD_GETSOBASE | 21 | 按 so 名称获取模块基址 | 参数: `CeGetSoBaseInput` + 名称；返回: `CeGetSoBaseOutput`(result, base) |
| CMD_SETKERNELHWBPRECLAIM | 22 | 设置 Kernel 断点槽抢占策略 | 参数: uint8 (0=关闭 / 非0=开启)；返回: int (1=已应用 / 0=当前非 Kernel 模式) |
| CMD_KERNEL_QUERYHWBPTHREADS | 23 | 查询当前进程每个线程的硬件断点槽位 | 参数: int handle + u32 每线程 capacity(1~64)；返回: int 成功标志 + u32 线程数，随后每线程固定摘要及 N×槽位条目；单线程失败保留 errno，不影响其它线程 |

> 断点类型 `type`：1=读 / 2=写 / 3=读写 / 4=执行（执行断点长度固定 4）。

## 3. 内存读写传输语义（连续前缀）

为消除"多页读写返回值无法定位有效区"的歧义，所有内存读写统一遵循**连续前缀**契约（详见 [api_design.md](../../docs/api_design.md) §2）：

- **CMD_READPROCESSMEMORY**：服务端始终回发 `size` 字节，其中 `read` = 从起始地址起**连续可读**的字节数。`buffer[0, read)` 为有效数据，`[read, size)` 已被服务端清零；`read < size` 表示在 `address+read` 处遇到不可读页。`read == 0` 表示起始即不可读。客户端据 `read` 截断即可，无需猜测。
- **CMD_WRITEPROCESSMEMORY**：`written` = **连续写入**的字节数。`written < size` 表示**部分写入**——前 `written` 字节已真实写入目标内存（有副作用），其余因不可写中断。客户端绝不能把部分写入当作"未写入"重试。
- **CMD_READBRATCHMEMORY**：按页返回**可读的页**；每条 = `u64 页地址 + 整页数据`（固定页大小）。不可读页直接不在结果中（页粒度），调用方据返回的页地址判断哪些页有效。
- **CMD_READBRATCHADDR**：对请求的每个地址返回一条 `u64 addr + u32 validLen + data[validLen]`，`validLen` = 该地址处连续可读字节数（≤请求 size，`0` = 该地址不可读）。返回条数与请求条数一致、顺序对应。

## 4. 内核切换

`CMD_INITRWDRIVER` 是内核切换的入口：服务端尝试通过 newkernelmem 的 anon_fd 通道连接已加载的 `NI` 驱动，并通过 `NI_IOCTL_GET_PROTOCOL_INFO` 校验协议；连接失败时用 `finit_module` 加载 `NI.ko`（优先当前目录，其次 `/data/local/tmp/NI.ko`）。成功后把全局内存读写实现 `g_memIO` 从默认的 `AndroidMemorySys`（syscall 模式）热替换为 `AndroidMemKernel`（内核模式）。`CMD_GETMEMTYPE` 查询当前所处模式。

> **硬件断点支持两种后端，对断点命令透明**：内核模式（已 `CMD_INITRWDRIVER` 切换）经内核驱动下发；非内核模式自动回退到**用户态 `perf_event_open`** 引擎（`android/PerfHwBreakpoint.hpp`）。两种后端的设置 / 删除 / 读取命令字、参数和返回一致，按断点 handle 归属自动分发。`CMD_SETKERNELHWBPRECLAIM` 是 Kernel 专用策略：开启后，per-TID 普通安装失败会使用 `NI_HWBP_F_FORCE_RECLAIM` 重试；Perf 模式不使用该策略。

## 5. 典型流程

1. `CMD_GETVERSION` → 校验版本
2. `CMD_GETMEMTYPE` / `CMD_INITRWDRIVER` → 查询 / 切换读写模式；Kernel 初始化成功后可发送 `CMD_SETKERNELHWBPRECLAIM`
3. `CMD_GETPROCESSLIST` → 选进程 → `CMD_OPENPROCESS` 取句柄
4. `CMD_GETSOBASE` 或 `CMD_GETMODULELIST` → 取模块基址
5. `CMD_READPROCESSMEMORY` / `CMD_WRITEPROCESSMEMORY` / `CMD_READBRATCHMEMORY` → 读写内存
6. `CMD_KERNEL_SETBREAKPOINT` → 下断点 → `CMD_KERNEL_READHWBPINFO` 轮询命中
7. `CMD_KERNEL_QUERYHWBPTHREADS` → Kernel 模式下查询当前进程各 TID 的断点槽位和占用状态
8. `CMD_SYMBOL_INIT` → `CMD_SYMBOL_FIND` / `CMD_SYMBOL_GETLIST` → 解析 ELF 符号

## 6. 错误处理

- 未实现 / 未知命令落入 `default` 分支（不响应或返回错误）。
- 客户端需检查每次 Send / Receive 的返回值，确保数据完整。
- 硬件断点需 root。两种后端（内核驱动 / 用户态 `perf_event_open`）均为进程级逻辑断点，后台周期 rescan `/proc/<pid>/task` **自动跟随目标新建线程**；命中均"被动累积 + `CMD_KERNEL_READHWBPINFO` 轮询"。perf 后端额外受 `perf_event_paranoid` 与每线程硬件断点 slot 数量限制、且可能被目标进程探测。
