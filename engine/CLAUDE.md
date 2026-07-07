# CLAUDE.md — engine/（MiniMem Android 后端引擎）

运行在 Android ARM64 设备上的精简内存引擎，实现 Cheat Engine 协议的子集，通过 TCP Socket 与前端/MCP 通信。语言 C++23 / C11，需 Android NDK（r27c/r28c）。

## 构建

```bash
./build.sh                       # 默认 Release，仅 Socket 接口
./build.sh --debug --no-strip
# 或：
cmake -S . -B build -DANDROID_NDK=<ndk> -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

产物 `bin/socket_server`（设备端常驻服务，默认监听 0.0.0.0:52736）。内核模式还需把 newkernelmem 构建出的 `NI.ko` 放到设备上。

## 架构（分层）

```
客户端(GUI/MCP) ──TCP──▶ interface/socket/server.cpp (ALooper + 多连接)
                              ▼ DispatchCommand_V2
                         ceserver/CEServer.cpp  (单字节 opcode 分发)
                              ▼
                         ceserver/api.cpp  (CApi 各命令实现)
                              ▼
                         android/  (g_memIO: IMemoryOp 实现)
                              ▼
                  内核驱动(AndroidMemKernel) / syscall(AndroidMemorySys)
```

- **内核切换**：`CApi::InitReadWriteDriver`（`CMD_INITRWDRIVER`）尝试通过 newkernelmem anon_fd 连接 `NI` 驱动，失败时用 `finit_module` 加载 `NI.ko`，成功后把全局 `g_memIO` 热替换为 `AndroidMemKernel`；默认是 `AndroidMemorySys`（syscall）。`GetRWDriverType`（`CMD_GETMEMTYPE`）返回当前模式（IO/Syscall/Kernel/SysHook）。
- **断点**：`CMD_KERNEL_SETBREAKPOINT` 等支持双后端，**均为进程级逻辑断点 + 自动跟随新线程**，按 handle 归属分发（先 `PerfHwBreakpoint::Owns` 再 `KernelHwBreakpoint::Owns`，查 map 权威判别）——内核模式经 `android/KernelHwBreakpoint.hpp`（封装 `AndroidKernelDriver`，底层走 newkernelmem `NiDriver` 的 per-tid 断点事件流）；非内核模式经 `android/PerfHwBreakpoint.hpp`（用户态 `perf_event_open`，后台消费 ring buffer）。两引擎后台线程均周期 rescan `/proc/<pid>/task` 补下断新线程 / 回收退出线程，命中皆"被动累积 + `ReadHwBpInfo` 轮询"。
- **ELF 符号**：`android/AndroidElfScanner` 解析符号表（`CMD_SYMBOL_*`）。
- `ptrace_hw/`（可选，`BUILD_PTRACE_HW`）：基于 ptrace 的 ARM64 硬件断点底层支持。

## 保留的命令（DispatchCommand_V2）

GETVERSION / GETMEMTYPE / INITRWDRIVER / OPENPROCESS / CLOSEHANDLE / GETPROCESSLIST / GETMODULELIST / READPROCESSMEMORY / WRITEPROCESSMEMORY / READBRATCHMEMORY / READBRATCHADDR / KERNEL_SET|REMOVE|SUSPEND|RESUME_BREAKPOINT / KERNEL_READHWBPINFO / SYMBOL_INIT|GETLIST|FIND。

## 重要：已移除的能力

数据搜索（`MemSearchKit`/`newScan`/`AndroidScanner`）、指针扫描（`Point_Scan`）、冻结（`FreezeManager`）、SO 注入、`ResultMgr`，以及 pipe/stdio/uds/jni 接口与 V1 分发器（`DispatchCommand`/`DispatchCmd_V1`）均已删除。`api.h`/`api.cpp` 不再含 Scan/Freeze/Inject 函数。新增能力时不要重新引入这些模块。

## 约定

- 注释/提交用中文。编译标志 `-fvisibility=hidden -Wno-format`。
- `g_memIO` 等全局单例由 `g_globalMutex`（shared_mutex）保护，切换驱动时写锁。
- 头文件大量 `.hpp`（header-only）。`g_tracer`(AndroidTracer) 用于 OpenProcess 初始化，`g_sym`(AndroidElfScanner) 用于符号解析。
