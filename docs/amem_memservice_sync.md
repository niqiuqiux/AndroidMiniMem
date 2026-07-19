# AMem MemService 重构同步决策

## 结论

MiniMem **需要同步 AMem 的业务边界与正确性约束，但不应同步完整版的功能面**。

判断标准不是“AMem 中是否存在这段代码”，而是该能力是否属于 MiniMem 已保留功能的共同基础：连接、目标进程、内存读写、断点、符号和 Lua 的一致性约束应同步；扫描、冻结、注入、内置 AI 及其审批/审计链路不得同步。

## 已同步的共同基础

| 领域 | MiniMem 决策 | 主要证据 |
|------|--------------|----------|
| 业务边界 | GUI、Lua、IPC 统一依赖注入的 `IMemService`，只有 `SystemMemService` 可调用设备协议 | `gui/mem/`、`minimem_mem_service_boundary` |
| 连接生命周期 | generation + `Disconnected/Connecting/Connected/Poisoned`；任意 I/O、EOF、超时或 malformed response 后只能重连 | `gui/socket/DeviceSession.*`、`client.hpp` |
| 复合事务 | 指针链、符号、进程切换等多命令操作持完整 `TransactionLease` | `SocketCommand.h`、`SystemMemService.cpp` |
| 目标一致性 | pid、handle、名称、revision 与缓存失效通过 `TargetMutation` 原子发布 | `gui/gui/AppContext.*` |
| 结果提交 | 只读结果在返回前复验 connection generation 与 target revision；状态采集优先报告连接失效，避免把重连或 poison 误报为目标切换 | `MemService::validateContext`、`testReadOnlyResultCommitValidation` |
| 副作用结果 | 驱动初始化、内存写入、断点变更区分未发送、响应丢失、明确拒绝/部分完成与确认成功 | `MemTypes.h`、`MemService.cpp` |
| 协议防御 | 数量、长度、总分配、地址溢出和响应对应关系均有上限；非法响应 poison 连接 | `socket/*Commands.cpp` |
| 符号 | 完整表与单页读取均在单个事务中完成；单页接口不再先加载全部符号 | `MemService::loadSymbolTable/listSymbols` |
| Lua | 每次顶层执行绑定一个目标上下文；仅 `process.attach()` 可推进脚本目标 | `LuaOperationBinding`、`LuaAPI::GetOperationContext` |
| IPC | 仅 loopback、JSON POST、无 CORS；拒绝非空 Origin、重复/折叠头和 Transfer-Encoding；有界工作队列与请求预算 | `IpcHttpRequest.*`、`IpcServer.cpp` |
| 敏感信息 | 卡密无默认值、不显示、不持久化、不写 GUI/engine 日志；旧配置自动清除 | `ServerConnectWindow.cpp`、`ConfigSecurityTests.cpp` |

## 明确不同步的 AMem 内容

- 数据搜索、再次搜索、模糊/分组搜索及扫描 session/epoch。
- 指针扫描、冻结列表与冻结写入事务。
- SO 注入、远程 mmap 和相关审批能力。
- 内置 AI 聊天、工具审批、审计记录、Native Agent runtime。
- 扫描/内存查看器/断点显示窗口及其后台读通道。
- 只为上述能力服务的类型、JSON 适配器、值编码和缓存。

AMem 当前的命名管道 IPC 属于其 Native Agent 架构。MiniMem 的既定链路是 Python MCP 通过 `127.0.0.1:28100` HTTP JSON 访问 GUI，因此不机械替换传输层；本项目选择保留 HTTP，并把浏览器访问面、请求 framing、并发和超时边界收紧到可测试模块。

## 后续同步规则

1. 先确认 AMem 变更是否影响 MiniMem 的六类保留能力：连接、目标、读写、断点、符号、Lua。
2. 同步契约和失败语义，不复制依赖已删除功能的类层次。
3. 新设备命令仍按 `engine -> socket -> IMemBackend/IMemService -> GUI/Lua/IPC/MCP` 链路接入。
4. 涉及副作用的命令必须提供“请求是否开始、响应是否确认、实际结果”三层状态。
5. 每次同步都扩展运行级测试和架构门禁；只靠静态搜索不能证明协议行为。
