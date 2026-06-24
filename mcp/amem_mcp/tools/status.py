"""状态、版本、架构、驱动初始化。"""

from __future__ import annotations

from mcp.server.fastmcp import FastMCP

from .. import PROTOCOL_VERSION, __version__
from ..ipc_client import IpcClient


def register(mcp: FastMCP, ipc: IpcClient) -> None:

    @mcp.tool()
    def get_mcp_version() -> str:
        """获取 MiniMem MCP 服务自身的版本与协议版本。"""
        return f"MiniMem MCP v{__version__}（二进制协议 v{PROTOCOL_VERSION}）"

    @mcp.tool()
    def get_status() -> str:
        """获取 GUI 当前状态（连接、进程信息）。"""
        r = ipc.call_or_raise("get_status")
        connected = "已连接" if r["connected"] else "未连接"
        pid = r.get("pid", 0)
        name = r.get("process_name", "")
        return f"服务端: {connected}, PID: {pid}, 进程: {name}"

    @mcp.tool()
    def get_server_version() -> str:
        """获取 Android 服务端版本信息。"""
        r = ipc.call_or_raise("get_version")
        return f"版本号: {r['version']}, 版本字符串: {r['version_string']}"

    @mcp.tool()
    def get_architecture() -> str:
        """获取目标设备的内存架构类型。"""
        r = ipc.call_or_raise("get_architecture")
        return f"架构类型: {r['type']} ({r['name']})"

    @mcp.tool()
    def init_driver(card_name: str) -> str:
        """初始化内核读写驱动。

        Args:
            card_name: 卡密/授权字符串
        """
        card_name = str(card_name)
        if not card_name:
            raise ValueError("card_name must not be empty")
        if len(card_name) > 4096:
            raise ValueError("card_name is too long")
        r = ipc.call_or_raise("init_driver", {"card": card_name})
        return f"结果: {r['message']}"

    @mcp.resource("amem://status")
    def resource_status() -> str:
        """当前 AMem GUI 状态。"""
        try:
            r = ipc.call_or_raise("get_status")
            connected = "已连接" if r["connected"] else "未连接"
            return f"状态: {connected}\nPID: {r.get('pid', 0)}\n进程: {r.get('process_name', '')}"
        except Exception as e:
            return f"状态: GUI 未连接 ({e})"
