"""Lua 脚本执行。"""

from __future__ import annotations

from mcp.server.fastmcp import FastMCP

from ..helpers import parse_positive_int
from ..ipc_client import IpcClient


def register(mcp: FastMCP, ipc: IpcClient) -> None:

    @mcp.tool()
    def execute_lua(code: str, timeout_seconds: int = 30) -> str:
        """在 MiniMem GUI 内执行 Lua 脚本。

        可使用 mem/process/module/bp/asm 等 MiniMem 保留的 Lua API。
        IPC 脚本运行在独立环境中，可使用 os/io；不提供 package/debug/ffi/imgui。
        Lua 引擎会在首次调用时自动初始化，无需手动打开 Lua 窗口。

        Args:
            code: Lua 脚本代码
            timeout_seconds: 执行超时秒数，范围 1-30
        """
        if not code:
            raise ValueError("code must not be empty")
        timeout_seconds = parse_positive_int(timeout_seconds, "timeout_seconds", 30)
        r = ipc.call_or_raise(
            "execute_lua",
            {"code": code, "timeout_seconds": timeout_seconds},
            timeout=timeout_seconds + 5.0,
        )
        output = r.get("output", "")
        return output if output else "(执行成功，无输出)"
