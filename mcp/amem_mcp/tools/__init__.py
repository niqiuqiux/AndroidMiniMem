"""所有工具模块的聚合注册入口。

每个子模块导出一个 `register(mcp, ipc)` 函数，统一由 `register_all` 调用。
"""

from __future__ import annotations

from mcp.server.fastmcp import FastMCP

from ..ipc_client import IpcClient
from . import breakpoint_, lua, memory, process, status, symbols


def register_all(mcp: FastMCP, ipc: IpcClient) -> None:
    """一次性注册所有工具到 FastMCP 实例上。"""
    status.register(mcp, ipc)
    process.register(mcp, ipc)
    memory.register(mcp, ipc)
    breakpoint_.register(mcp, ipc)
    lua.register(mcp, ipc)
    symbols.register(mcp, ipc)
