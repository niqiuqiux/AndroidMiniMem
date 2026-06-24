"""符号表。"""

from __future__ import annotations

from mcp.server.fastmcp import FastMCP

from ..helpers import clamp_page, parse_int
from ..ipc_client import IpcClient


def register(mcp: FastMCP, ipc: IpcClient) -> None:

    @mcp.tool()
    def symbol_init(module_base: str) -> str:
        """初始化指定模块的符号表。

        Args:
            module_base: 模块基址，支持 0x 前缀
        """
        parse_int(module_base)
        r = ipc.call_or_raise("symbol_init", {"module_base": module_base})
        return f"符号表初始化完成，总符号数: {r['total_count']}"

    @mcp.tool()
    def symbol_list(offset: int = 0, count: int = 100, module_base: str = "") -> str:
        """分页列出当前符号表中的符号。

        Args:
            offset: 起始偏移
            count: 获取数量，默认 100，最大 1000
            module_base: 模块基址；如提供，则会先初始化该模块的符号表
        """
        offset, count = clamp_page(offset, count)
        if module_base:
            parse_int(module_base)
            ipc.call_or_raise("symbol_init", {"module_base": module_base})

        r = ipc.call_or_raise("symbol_list", {"offset": offset, "count": count})
        total = r.get("total", 0)
        symbols = r.get("symbols", [])
        off = r.get("offset", offset)
        if not symbols:
            return f"无符号结果（总数: {total}）"

        lines = [f"符号列表（总数: {total}, offset: {off}, 本页: {len(symbols)}）", ""]
        for item in symbols:
            lines.append(f"  {item['address']}  {item['name']}")
        if off + len(symbols) < total:
            lines.append(f"\n... 还有 {total - off - len(symbols)} 个符号未显示")
        return "\n".join(lines)

    @mcp.tool()
    def symbol_find(module_base: str, symbol_name: str) -> str:
        """在指定模块中按名称查找符号地址。

        Args:
            module_base: 模块基址，支持 0x 前缀
            symbol_name: 符号名称
        """
        parse_int(module_base)
        if not symbol_name.strip():
            raise ValueError("symbol_name must not be empty")
        r = ipc.call_or_raise("symbol_find", {
            "module_base": module_base,
            "name": symbol_name,
        })
        return f"符号 {symbol_name} 地址: {r['address']}"
