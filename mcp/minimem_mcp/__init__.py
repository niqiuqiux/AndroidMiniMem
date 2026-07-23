"""MiniMem MCP Server — HTTP 桥接 GUI 的 IPC Server，把设备能力暴露给 AI 助手。"""

# 版本与 engine/gui 对齐：应用版本 MiniMem 1.0.1，二进制协议版本 1.0.0。
# 此处是包版本的单一真相源（pyproject.toml 通过 dynamic version 读取 __version__）。
__version__ = "1.0.1"
PROTOCOL_VERSION = "1.0.0"
