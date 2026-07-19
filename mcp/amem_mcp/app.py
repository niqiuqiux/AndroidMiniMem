"""MiniMem MCP Server 装配与 CLI 入口。"""

from __future__ import annotations

import argparse
import os
import sys

from mcp.server.fastmcp import FastMCP

from . import PROTOCOL_VERSION, __version__
from .ipc_client import IpcClient
from .tools import register_all


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 28100


def build_server(host: str = DEFAULT_HOST, port: int = DEFAULT_PORT) -> tuple[FastMCP, IpcClient]:
    """装配一个配置完成的 FastMCP 实例。"""
    mcp = FastMCP(
        "MiniMem",
        instructions=(
            f"MiniMem Android 内存调试 MCP 服务 v{__version__}（二进制协议 v{PROTOCOL_VERSION}）"
            " — 通过 GUI IPC 桥接，"
            "支持进程/模块列表、内存读写、硬件断点、ELF 符号、Lua 脚本执行"
            "（不含数据搜索/指针扫描/冻结）"
        ),
    )
    ipc = IpcClient(host, port)
    register_all(mcp, ipc)
    return mcp, ipc


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(
        prog="amem-mcp",
        description=f"MiniMem MCP Server v{__version__} (IPC 代理模式) — 桥接 GUI 的内嵌 IPC Server",
    )
    parser.add_argument(
        "--version", action="version",
        version=f"MiniMem MCP {__version__} (二进制协议 v{PROTOCOL_VERSION})",
    )
    parser.add_argument(
        "--ipc-host", default=os.environ.get("AMEM_IPC_HOST", DEFAULT_HOST),
        help=f"MiniMem GUI IPC Server 地址 (默认 {DEFAULT_HOST})",
    )
    parser.add_argument(
        "--ipc-port", type=int, default=int(os.environ.get("AMEM_IPC_PORT", DEFAULT_PORT)),
        help=f"MiniMem GUI IPC Server 端口 (默认 {DEFAULT_PORT})",
    )
    parser.add_argument(
        "--transport", choices=["stdio"], default="stdio",
        help="MCP 传输方式 (目前仅支持 stdio)",
    )
    args = parser.parse_args(argv)

    mcp, ipc = build_server(args.ipc_host, args.ipc_port)

    # 启动前探测 GUI 是否可达（不阻塞启动）
    probe = ipc.call("get_status")
    if probe.get("success"):
        print(f"[minimem-mcp] v{__version__} 已连接 GUI IPC Server ({ipc.base_url})", file=sys.stderr)
    else:
        print(
            f"[minimem-mcp] v{__version__} 警告: GUI 未启动或 IPC 不可达 ({ipc.base_url}) — "
            f"{probe.get('error', '')}",
            file=sys.stderr,
        )

    mcp.run(transport=args.transport)


if __name__ == "__main__":
    main()
