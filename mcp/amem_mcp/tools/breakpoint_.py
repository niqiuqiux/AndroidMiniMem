"""硬件断点。

断点类型编号与 AMem 内部（gui/BreakpointWindow.h / LuaAPI.cpp / IpcServer）保持一致:
    1 = 读 (HW_BREAKPOINT_R)
    2 = 写 (HW_BREAKPOINT_W)
    3 = 读写 (HW_BREAKPOINT_RW = R | W)
    4 = 执行 (HW_BREAKPOINT_X)

MCP 层不做任何翻译，整数值直接透传给 C++ 层。也可传入语义字符串
(read/write/readwrite/access/execute)，由工具层解析。
"""

from __future__ import annotations

from mcp.server.fastmcp import FastMCP

from ..constants import BP_TYPE_BY_NAME, BP_TYPE_EXECUTE, BP_TYPE_NAMES
from ..helpers import parse_int
from ..ipc_client import IpcClient


def _resolve_bp_type(bp_type: int | str) -> int:
    """接受整数 (1/2/3/4) 或语义字符串，返回统一的整数类型。"""
    if isinstance(bp_type, bool):
        raise ValueError("bp_type must not be boolean")
    if isinstance(bp_type, int):
        if bp_type not in BP_TYPE_NAMES:
            raise ValueError(
                f"bp_type 非法: {bp_type}，合法取值: 1=读 / 2=写 / 3=读写 / 4=执行"
            )
        return bp_type
    key = str(bp_type).strip().lower()
    if key not in BP_TYPE_BY_NAME:
        raise ValueError(
            f"bp_type 非法: {bp_type}，合法字符串: {sorted(BP_TYPE_BY_NAME.keys())}"
        )
    return BP_TYPE_BY_NAME[key]


def register(mcp: FastMCP, ipc: IpcClient) -> None:

    @mcp.tool()
    def set_breakpoint(address: str, bp_type: int = 2, bp_size: int = 4) -> str:
        """设置硬件断点（类型编号与 AMem 内部保持一致）。

        Args:
            address: 断点地址
            bp_type: 1=读, 2=写, 3=读写, 4=执行
                     也接受字符串: "read" / "write" / "readwrite" / "access" / "execute"
                     默认 2 (写入)
            bp_size: 监控大小 (1/2/4/8 字节)；执行断点强制为 4
        """
        parse_int(address)
        resolved = _resolve_bp_type(bp_type)
        size = 4 if resolved == BP_TYPE_EXECUTE else parse_int(bp_size)
        if size not in (1, 2, 4, 8):
            raise ValueError("bp_size must be 1, 2, 4, or 8")
        ipc.call_or_raise("set_breakpoint", {
            "address": address, "bp_type": resolved, "bp_size": size,
        })
        return f"断点 {address} 设置成功 ({BP_TYPE_NAMES[resolved]}, size={size})"

    @mcp.tool()
    def remove_breakpoint(address: str) -> str:
        """移除硬件断点。

        Args:
            address: 断点地址
        """
        parse_int(address)
        ipc.call_or_raise("remove_breakpoint", {"address": address})
        return f"断点 {address} 已移除"

    @mcp.tool()
    def read_breakpoint_info(address: str) -> str:
        """读取断点命中信息（ARM64 寄存器状态）。

        Args:
            address: 断点地址
        """
        parse_int(address)
        hits = ipc.call_or_raise("read_bp_info", {"address": address})
        if not hits:
            return f"断点 {address} 无命中记录"
        lines = [f"断点 {address} 共 {len(hits)} 次命中:", ""]
        for i, h in enumerate(hits):
            lines.append(f"--- 命中 #{i + 1} ---")
            lines.append(f"  命中地址: {h['hit_addr']}")
            lines.append(f"  PC: {h['pc']}  SP: {h['sp']}")
            regs = h.get("regs", [])
            for j in range(0, min(len(regs), 31), 4):
                parts = [f"X{j+k}={regs[j+k]:#x}" for k in range(4) if j + k < len(regs)]
                lines.append(f"  {' '.join(parts)}")
            lines.append("")
        return "\n".join(lines)

    @mcp.tool()
    def suspend_breakpoint(address: str) -> str:
        """暂停硬件断点（不删除）。

        Args:
            address: 断点地址
        """
        parse_int(address)
        ipc.call_or_raise("suspend_breakpoint", {"address": address})
        return f"断点 {address} 已暂停"

    @mcp.tool()
    def resume_breakpoint(address: str) -> str:
        """恢复已暂停的硬件断点。

        Args:
            address: 断点地址
        """
        parse_int(address)
        ipc.call_or_raise("resume_breakpoint", {"address": address})
        return f"断点 {address} 已恢复"
