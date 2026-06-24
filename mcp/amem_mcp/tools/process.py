"""进程与模块管理。"""

from __future__ import annotations

from mcp.server.fastmcp import FastMCP

from ..helpers import clamp_page, parse_int
from ..ipc_client import IpcClient


MAX_OFFSET_CHAIN_LENGTH = 1024


def register(mcp: FastMCP, ipc: IpcClient) -> None:

    @mcp.tool()
    def list_processes() -> str:
        """列出 Android 设备上所有运行中的进程。"""
        procs = ipc.call_or_raise("list_processes")
        if not procs:
            return "未获取到进程列表"
        lines = [f"共 {len(procs)} 个进程:", ""]
        for p in procs:
            lines.append(f"  PID {p['pid']:>6}  {p['name']}")
        return "\n".join(lines)

    @mcp.tool()
    def open_process(pid: int) -> str:
        """打开指定 PID 的进程，后续内存操作将针对此进程。

        Args:
            pid: 目标进程 PID
        """
        pid = parse_int(pid)
        if pid == 0:
            raise ValueError("pid must be positive")
        r = ipc.call_or_raise("open_process", {"pid": pid})
        return f"已打开进程 PID={pid}, handle={r['handle']}"

    @mcp.tool()
    def list_modules(filter: str = "", offset: int = 0, count: int = 200) -> str:
        """列出当前进程加载的模块。

        Args:
            filter: 模块名称过滤（大小写不敏感子串匹配），留空返回全部
            offset: 起始偏移，默认 0
            count: 获取数量，默认 200，最大 1000
        """
        offset, count = clamp_page(offset, count)
        params: dict = {"offset": offset, "count": count}
        if filter:
            params["filter"] = filter
        r = ipc.call_or_raise("list_modules", params)
        total = r.get("total", 0)
        mods = r.get("modules", [])
        off = r.get("offset", offset)
        if not mods:
            return f"未获取到模块（总数: {total}）"
        lines = [f"模块列表（总数: {total}, offset: {off}, 本页: {len(mods)}）:", ""]
        for m in mods:
            lines.append(f"  {m['base']}  size={m['size']:#010x}  {m['name']}")
        if off + len(mods) < total:
            lines.append(
                f"\n... 还有 {total - off - len(mods)} 个模块未显示，"
                f"使用 offset={off + len(mods)} 获取更多"
            )
        return "\n".join(lines)

    @mcp.tool()
    def get_module_base(module_name: str) -> str:
        """获取指定模块的基址。

        Args:
            module_name: 模块名称
        """
        if not module_name.strip():
            raise ValueError("module_name must not be empty")
        r = ipc.call_or_raise("get_module_base", {"name": module_name})
        return f"模块 {module_name} 基址: {r['base']}"

    @mcp.tool()
    def resolve_offset_chain(
        module: str,
        base_offset: str,
        offsets: list[int] | None = None,
        deref_final: bool = True,
    ) -> str:
        """解析模块偏移链（指针链），获取最终地址。

        Args:
            module: 模块名称
            base_offset: 基础偏移（支持 0x 前缀）
            offsets: 偏移链列表，如 [0x10, 0x20, 0x8]
            deref_final: 是否解引用最终地址，默认 True
        """
        if not module.strip():
            raise ValueError("module must not be empty")
        parse_int(base_offset)
        if offsets is not None and not isinstance(offsets, list):
            raise ValueError("offsets must be a list")
        if offsets is not None and len(offsets) > MAX_OFFSET_CHAIN_LENGTH:
            raise ValueError(
                f"offsets is too long (max {MAX_OFFSET_CHAIN_LENGTH})"
            )
        parsed_offsets = [parse_int(offset) for offset in (offsets or [])]
        r = ipc.call_or_raise("resolve_offset_chain", {
            "module": module,
            "base_offset": base_offset,
            "offsets": parsed_offsets,
            "deref_final": deref_final,
        })
        return f"最终地址: {r['address']}"
