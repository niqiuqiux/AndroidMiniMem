# reference/

二进制协议参考实现，**MCP Server 不使用**。

## `amem_client.py`

精确复刻 `socket/client.hpp` + `socket/*Commands.cpp` 的协议实现，
可直接连接 Android 服务端，跳过 GUI 进行脚本化测试和调研。

典型用途：
- 协议调试、wire-level 抓包对拍
- 在没有 GUI 的环境下跑自动化脚本
- 向新语言移植时作为权威参考

如果你在写 MCP 工具或 AI 集成，直接用 `amem_mcp` 包即可，不需要这个文件。
