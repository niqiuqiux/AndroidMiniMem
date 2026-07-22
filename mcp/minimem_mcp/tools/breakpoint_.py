"""硬件断点。

断点类型编号与 MiniMem 内部（LuaAPI.cpp / IpcServer）保持一致：
    1 = 读 (HW_BREAKPOINT_R)
    2 = 写 (HW_BREAKPOINT_W)
    3 = 读写 (HW_BREAKPOINT_RW = R | W)
    4 = 执行 (HW_BREAKPOINT_X)

MCP 层不做任何翻译，整数值直接透传给 C++ 层。也可传入语义字符串
(read/write/readwrite/access/execute)，由工具层解析。

命中读取采用「聚合摘要 + 按需钻取」两层接口:
  - read_breakpoint_info  默认返回聚合摘要(命中总数、热点 PC/调用来源 Top-N、
    参数分布、代表样本)。高频断点几秒可累积上万条命中且高度冗余,逐条返回会
    淹没 AI、塞爆上下文,聚合后信息密度更高。
  - read_breakpoint_samples 从最近一次 read_breakpoint_info 拉取的批次中分页
    取原始逐条记录(engine 侧「拉取即清空」,故钻取基于该快照缓存)。
"""

from __future__ import annotations

from collections import Counter

from mcp.server.fastmcp import FastMCP

from ..constants import BP_TYPE_BY_NAME, BP_TYPE_EXECUTE, BP_TYPE_NAMES
from ..helpers import clamp_page, parse_int
from ..ipc_client import IpcClient

# 最近一次 read_breakpoint_info 拉取的命中批次缓存: addr_int -> {"hits": [...], "total": int}
# 供 read_breakpoint_samples 钻取。engine 侧 read 是「拉取即清空」,故钻取只能基于此快照。
_HIT_CACHE: dict[int, dict] = {}
_SAMPLE_MAX = 50

_QUERY_TYPE_NAMES = {
    1: "read",
    2: "write",
    3: "readwrite",
    4: "execute",
}
_QUERY_SOURCE_NAMES = {
    0: "perf",
    1: "ptrace",
    2: "module",
}
_QUERY_STATE_NAMES = {
    0: "unknown",
    1: "dead",
    2: "exit",
    3: "error",
    4: "off",
    5: "inactive",
    6: "active",
}
_QUERY_FLAG_NAMES = (
    (1 << 0, "enabled"),
    (1 << 1, "active"),
    (1 << 2, "pinned"),
    (1 << 3, "inherited"),
    (1 << 4, "sigtrap"),
)


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


def _to_int(value) -> int:
    """把命中记录里的地址字段(可能是 '0x..' 字符串或整数)统一成 int。"""
    if isinstance(value, int):
        return value
    return int(str(value), 0)


def _reg(h: dict, i: int):
    """安全取第 i 个通用寄存器(X0..X30)，越界返回 None。"""
    regs = h.get("regs") or []
    return regs[i] if i < len(regs) else None


def _fmt_reg_lines(h: dict, indent: str = "  ") -> list[str]:
    """把一条命中的 X0..X30 格式化成每行 4 个寄存器。"""
    regs = h.get("regs") or []
    out = []
    for j in range(0, min(len(regs), 31), 4):
        parts = [f"X{j + k}={regs[j + k]:#x}" for k in range(4) if j + k < len(regs)]
        out.append(indent + " ".join(parts))
    return out


def _summarize(address: str, hits: list, total: int) -> str:
    """把一批命中记录聚合成高信息密度的摘要文本。"""
    addr_int = _to_int(address)
    n = len(hits)
    pcs = [_to_int(h.get("pc", "0x0")) for h in hits]
    # 类型推断: 执行断点命中 PC 恒等于断点地址; 数据监视点的 PC 是各访问指令(≠数据地址)
    is_exec = n > 0 and all(pc == addr_int for pc in pcs)

    lines = [
        f"断点 {address} 命中摘要",
        f"  本次拉取 {n} 条 · 设备累计 {total} 次 · 推断【{'执行断点' if is_exec else '数据监视点'}】",
    ]
    times = [h["hit_time"] for h in hits
             if isinstance(h.get("hit_time"), int) and h["hit_time"] > 0]
    if len(times) >= 2:
        lines.append(f"  采样时间跨度 {(max(times) - min(times)) / 1e6:.1f} ms")
    lines.append("")

    if is_exec:
        # 执行断点: PC 恒定无信息量, 改按「调用来源 LR(X30)」聚合 + 首参 X0 分布
        lrs = Counter(_reg(h, 30) for h in hits if _reg(h, 30) is not None)
        lines.append(f"▼ 调用来源 Top10 (LR/X30, 共 {len(lrs)} 个不同来源)")
        for lr, c in lrs.most_common(10):
            lines.append(f"  ×{c} ({c * 100 // max(n, 1)}%)  LR={lr:#x}")
        x0s = Counter(_reg(h, 0) for h in hits if _reg(h, 0) is not None)
        lines.append("")
        lines.append(f"▼ 首参 X0 常见值 Top8 (共 {len(x0s)} 种)")
        lines.append("  " + " · ".join(f"{v:#x}×{c}" for v, c in x0s.most_common(8)))
    else:
        # 数据监视点: 按「访问指令 PC」聚合 — 哪些指令在读写被监控地址
        pcc = Counter(pcs)
        lines.append(f"▼ 访问指令 Top10 (PC, 共 {len(pcc)} 个不同指令)")
        for pc, c in pcc.most_common(10):
            lines.append(f"  ×{c} ({c * 100 // max(n, 1)}%)  PC={pc:#x}")

    s = hits[0]
    lines.append("")
    lines.append("▼ 代表样本 (第 1 条)")
    lines.append(f"  命中地址={s.get('hit_addr')}  PC={s.get('pc')}  SP={s.get('sp')}")
    lines.extend(_fmt_reg_lines(s))
    lines.append("")
    lines.append(f'ℹ 钻取原始记录: read_breakpoint_samples("{address}", offset, count)')
    lines.append("  热点地址可用 list_modules / symbol_find 定位所属模块/函数")
    return "\n".join(lines)


def register(mcp: FastMCP, ipc: IpcClient) -> None:

    @mcp.tool()
    def query_hardware_breakpoint_slots() -> str:
        """查询当前进程所有线程的 Kernel 硬件断点槽位。

        仅适用于 GUI 已初始化的 Kernel 模式。结果保留查询期间退出线程的
        TID 和 errno，并显示每个已占用槽位的地址、类型、来源、状态及标志。
        """
        data = ipc.call_or_raise("query_hwbp_slots")
        threads = data.get("threads", [])
        lines = [
            f"PID {data.get('pid')} Kernel 硬件断点槽位",
            "  线程 {total} 个 · 成功 {ok} · 失败 {failed} · 返回槽位 {slots}".format(
                total=data.get("thread_count", len(threads)),
                ok=data.get("successful_threads", 0),
                failed=data.get("failed_threads", 0),
                slots=data.get("returned_slots", 0),
            ),
        ]
        for thread in threads:
            tid = thread.get("tid")
            if not thread.get("query_succeeded", False):
                code = thread.get("error_code", 0)
                message = thread.get("error", "query failed")
                lines.append(f"TID {tid}: 查询失败 errno={code} ({message})")
                continue

            slots = thread.get("slots") or []
            lines.append(
                "TID {tid}: {count}/{total} 槽位 · BRP={brp} WRP={wrp} "
                "enabled={enabled} active={active} · perf={perf} ptrace={ptrace} module={module}".format(
                    tid=tid,
                    count=thread.get("count", len(slots)),
                    total=thread.get("total_count", len(slots)),
                    brp=thread.get("brp_count", 0),
                    wrp=thread.get("wrp_count", 0),
                    enabled=thread.get("enabled_count", 0),
                    active=thread.get("active_count", 0),
                    perf=thread.get("perf_count", 0),
                    ptrace=thread.get("ptrace_count", 0),
                    module=thread.get("module_count", 0),
                )
            )
            for index, slot in enumerate(slots):
                type_value = slot.get("type", 0)
                source_value = slot.get("source", 0)
                state_value = slot.get("state", 0)
                flags_value = slot.get("flags", 0)
                flag_names = [
                    name for mask, name in _QUERY_FLAG_NAMES
                    if flags_value & mask
                ]
                lines.append(
                    "  [{index}] addr={address} type={type_name}({type_value}) len={length} "
                    "source={source_name}({source_value}) state={state_name}({state_value}) "
                    "flags={flags:#x}[{flag_names}] event={event_id} module={module_handle} "
                    "tid={slot_tid} on_cpu={on_cpu}".format(
                        index=index,
                        address=slot.get("address"),
                        type_name=_QUERY_TYPE_NAMES.get(type_value, "unknown"),
                        type_value=type_value,
                        length=slot.get("length"),
                        source_name=_QUERY_SOURCE_NAMES.get(source_value, "unknown"),
                        source_value=source_value,
                        state_name=_QUERY_STATE_NAMES.get(state_value, "unknown"),
                        state_value=state_value,
                        flags=flags_value,
                        flag_names=",".join(flag_names) or "none",
                        event_id=slot.get("event_id"),
                        module_handle=slot.get("module_handle"),
                        slot_tid=slot.get("tid"),
                        on_cpu=slot.get("on_cpu"),
                    )
                )
        return "\n".join(lines)

    @mcp.tool()
    def set_breakpoint(address: str, bp_type: int = 2, bp_size: int = 4) -> str:
        """设置硬件断点（类型编号与 MiniMem 内部保持一致）。

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
        _HIT_CACHE.pop(parse_int(address), None)  # 清掉钻取缓存,避免陈旧
        return f"断点 {address} 已移除"

    @mcp.tool()
    def read_breakpoint_info(address: str) -> str:
        """读取断点命中信息，返回【聚合摘要】(命中总数、热点 PC/调用来源 Top-N、参数分布、代表样本)。

        高频断点(如 malloc、活跃栈)几秒可累积上万条命中且高度冗余,本工具聚合后返回
        以保持信息密度;需要逐条原始寄存器时用 read_breakpoint_samples 钻取。

        Args:
            address: 断点地址
        """
        addr_int = parse_int(address)
        data = ipc.call_or_raise("read_bp_info", {"address": address})
        # 兼容新(对象 {total_hits, returned, hits})与旧(数组)两种返回形状
        if isinstance(data, dict):
            hits = data.get("hits", [])
            total = data.get("total_hits", len(hits))
        else:
            hits = data or []
            total = len(hits)
        # 仅在有命中时刷新缓存,避免一次空读清掉上一批可钻取的样本
        if hits:
            _HIT_CACHE[addr_int] = {"hits": hits, "total": total}
        if not hits:
            return f"断点 {address} 无命中记录（设备累计命中 {total} 次）"
        return _summarize(address, hits, total)

    @mcp.tool()
    def read_breakpoint_samples(address: str, offset: int = 0, count: int = 20) -> str:
        """钻取最近一次 read_breakpoint_info 拉取批次的原始逐条命中记录（分页）。

        engine 侧命中「拉取即清空」,故只能钻取最近一次 read_breakpoint_info 缓存的批次;
        若期间又调用了 read_breakpoint_info,缓存会刷新为新批次。

        Args:
            address: 断点地址
            offset: 起始索引,默认 0
            count: 返回条数,默认 20,最大 50
        """
        addr_int = parse_int(address)
        off, cnt = clamp_page(offset, count, _SAMPLE_MAX)
        cached = _HIT_CACHE.get(addr_int)
        if not cached or not cached.get("hits"):
            return f"断点 {address} 无缓存命中记录，请先调用 read_breakpoint_info 拉取一批"
        hits = cached["hits"]
        page = hits[off:off + cnt]
        if not page:
            return f"断点 {address} offset={off} 超出缓存范围（缓存 {len(hits)} 条）"
        lines = [
            f"断点 {address} 原始命中 [{off}, {off + len(page)}) / 缓存 {len(hits)} 条"
            f"（设备累计 {cached.get('total')} 次）:",
            "",
        ]
        for i, h in enumerate(page):
            lines.append(f"--- #{off + i + 1} ---")
            lines.append(f"  命中地址={h.get('hit_addr')}  PC={h.get('pc')}  SP={h.get('sp')}")
            lines.extend(_fmt_reg_lines(h))
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
