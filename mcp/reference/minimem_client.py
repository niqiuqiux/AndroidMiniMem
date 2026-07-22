"""
MiniMem 二进制协议参考客户端
精确复刻 engine/ceserver + gui/socket/client.hpp 的协议实现（精简版）。
仅覆盖 MiniMem 保留的命令：版本/内核切换/进程·模块/读写/内核断点/ELF符号。
（数据搜索 / 指针扫描 / 冻结 已移除）
"""

import socket
import struct
import threading
from dataclasses import dataclass, field
from typing import Optional

# ── 命令码（与 engine/ceserver/ceserver.h 一致，opcode 从 0 连续编号）──
CMD_GETVERSION               = 0
CMD_CLOSECONNECTION          = 1
CMD_TERMINATESERVER          = 2
CMD_GETMEMTYPE               = 3
CMD_INITRWDRIVER             = 4
CMD_OPENPROCESS              = 5
CMD_CLOSEHANDLE              = 6
CMD_GETPROCESSLIST           = 7
CMD_GETMODULELIST            = 8
CMD_READPROCESSMEMORY        = 9
CMD_WRITEPROCESSMEMORY       = 10
CMD_READBRATCHMEMORY         = 11
CMD_READBRATCHADDR           = 12
CMD_KERNEL_SETBREAKPOINT     = 13
CMD_KERNEL_REMOVEBREAKPOINT  = 14
CMD_KERNEL_SUSPENDBREAKPOINT = 15
CMD_KERNEL_RESUMEBREAKPOINT  = 16
CMD_KERNEL_READHWBPINFO      = 17
CMD_SYMBOL_INIT              = 18
CMD_SYMBOL_GETLIST           = 19
CMD_SYMBOL_FIND              = 20
CMD_GETSOBASE                = 21

# ── 基础内存读写数据类型 ─────────────────────────────────────────
TYPE_BYTE   = 1
TYPE_WORD   = 2
TYPE_DWORD  = 4
TYPE_XOR    = 8
TYPE_FLOAT  = 16
TYPE_QWORD  = 32
TYPE_DOUBLE = 64


def require_so_name(name: str) -> str:
    if not name or ".so" not in name:
        raise ValueError("so name must include '.so'")
    return name


def encode_value(value, data_type: str) -> bytes:
    """将 Python 值编码为对应数据类型的 bytes（便于 write_memory 写入定型值）"""
    fmt = {"byte": "<B", "word": "<H", "dword": "<I", "qword": "<Q",
           "float": "<f", "double": "<d", "xor": "<I"}
    return struct.pack(fmt.get(data_type, "<I"), int(value) if data_type not in ("float", "double") else float(value))


# ── 数据结构 ─────────────────────────────────────────────────────
@dataclass
class ServerVersion:
    version: int = 0
    version_string: str = ""

@dataclass
class ProcessInfo:
    pid: int = 0
    name: str = ""

@dataclass
class ModuleInfo:
    base: int = 0
    size: int = 0
    type: int = 0
    flag: int = 0
    name: str = ""

@dataclass
class BreakpointHit:
    hit_addr: int = 0
    hit_time: int = 0
    regs: list = field(default_factory=list)   # x0-x30
    sp: int = 0
    pc: int = 0
    pstate: int = 0


@dataclass
class SymbolInfo:
    address: int = 0
    name: str = ""


# ── TCP 客户端 ───────────────────────────────────────────────────
class MiniMemClient:
    """与 MiniMem Android 服务端通信的 TCP 客户端"""

    def __init__(self):
        self._sock: Optional[socket.socket] = None
        self._lock = threading.Lock()
        self._handle: int = 0
        self._pid: int = 0

    # ── 连接管理 ──────────────────────────────────────────────────

    @property
    def connected(self) -> bool:
        return self._sock is not None

    def connect(self, host: str, port: int, timeout: float = 5.0) -> bool:
        self.disconnect()
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(timeout)
            s.connect((host, port))
            s.settimeout(None)
            self._sock = s
            return True
        except OSError:
            return False

    def disconnect(self):
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
        self._handle = 0
        self._pid = 0

    # ── 底层收发 ──────────────────────────────────────────────────

    def _send_all(self, data: bytes):
        sock = self._sock
        if not sock:
            raise ConnectionError("未连接")
        total = 0
        while total < len(data):
            sent = sock.send(data[total:])
            if sent == 0:
                raise ConnectionError("发送失败")
            total += sent

    def _recv_all(self, size: int) -> bytes:
        sock = self._sock
        if not sock:
            raise ConnectionError("未连接")
        buf = bytearray()
        while len(buf) < size:
            chunk = sock.recv(size - len(buf))
            if not chunk:
                raise ConnectionError("连接已断开")
            buf.extend(chunk)
        return bytes(buf)

    def _send_cmd(self, cmd: int):
        self._send_all(struct.pack("<B", cmd))

    def _send_cmd_handle(self, cmd: int):
        self._send_all(struct.pack("<Bi", cmd, self._handle))

    # ── 系统命令 (无需 handle) ────────────────────────────────────

    def get_version(self) -> ServerVersion:
        with self._lock:
            self._send_cmd(CMD_GETVERSION)
            data = self._recv_all(5)  # CeVersion: int + uchar
            ver, slen = struct.unpack("<iB", data)
            s = self._recv_all(slen).decode("utf-8", errors="replace") if slen else ""
            return ServerVersion(ver, s)

    def get_mem_type(self) -> int:
        """查询当前读写模式 (0:null 1:io 2:syscall 3:kernel 4:syshook)"""
        with self._lock:
            self._send_cmd(CMD_GETMEMTYPE)
            return struct.unpack("<B", self._recv_all(1))[0]

    def init_driver(self, card: str) -> tuple[int, str]:
        with self._lock:
            self._send_cmd(CMD_INITRWDRIVER)
            card_bytes = card.encode("utf-8")
            self._send_all(struct.pack("<i", len(card_bytes)))
            self._send_all(card_bytes)
            ret = struct.unpack("<i", self._recv_all(4))[0]
            slen = struct.unpack("<i", self._recv_all(4))[0]
            s = self._recv_all(slen).decode("utf-8", errors="replace") if slen > 0 else ""
            return ret, s

    # ── 进程命令 ──────────────────────────────────────────────────

    def list_processes(self) -> list[ProcessInfo]:
        with self._lock:
            self._send_cmd(CMD_GETPROCESSLIST)
            count = struct.unpack("<i", self._recv_all(4))[0]
            result = []
            for _ in range(count):
                pid, nlen = struct.unpack("<ii", self._recv_all(8))
                name = self._recv_all(nlen).decode("utf-8", errors="replace")
                result.append(ProcessInfo(pid, name))
            return result

    def open_process(self, pid: int) -> int:
        with self._lock:
            self._send_all(struct.pack("<Bi", CMD_OPENPROCESS, pid))
            handle = struct.unpack("<i", self._recv_all(4))[0]
            self._handle = handle
            self._pid = pid
            return handle

    def list_modules(self) -> list[ModuleInfo]:
        with self._lock:
            self._send_cmd_handle(CMD_GETMODULELIST)
            count = struct.unpack("<i", self._recv_all(4))[0]
            if count < 0:
                raise RuntimeError(f"模块列表数量非法: {count}")
            result = []
            for _ in range(count):
                data = self._recv_all(24)
                mtype, mflag, mbase, msize, nlen = struct.unpack("<iiQii", data)
                if nlen < 0:
                    raise RuntimeError(f"模块名长度非法: {nlen}")
                name = self._recv_all(nlen).decode("utf-8", errors="replace") if nlen > 0 else ""
                result.append(ModuleInfo(mbase, msize, mtype, mflag, name))
            return result

    def get_so_base(self, name: str) -> int:
        with self._lock:
            name = require_so_name(name)
            name_bytes = name.encode("utf-8")
            self._send_cmd(CMD_GETSOBASE)
            self._send_all(struct.pack("<Ii", self._handle, len(name_bytes)))
            if name_bytes:
                self._send_all(name_bytes)
            result, base = struct.unpack("<iQ", self._recv_all(12))
            if result != 0:
                return 0
            return base

    def symbol_init(self, module_base: int) -> int:
        with self._lock:
            self._send_cmd(CMD_SYMBOL_INIT)
            self._send_all(struct.pack("<IQ", self._handle, module_base))
            result, total_count = struct.unpack("<ii", self._recv_all(8))
            if result != 0:
                raise RuntimeError(f"符号初始化失败: result={result}")
            return total_count

    def symbol_get_list(self, offset: int = 0, count: int = 100) -> tuple[int, list[SymbolInfo]]:
        with self._lock:
            self._send_cmd(CMD_SYMBOL_GETLIST)
            self._send_all(struct.pack("<ii", offset, count))
            total_count, actual_count = struct.unpack("<ii", self._recv_all(8))
            if actual_count < 0:
                raise RuntimeError(f"符号数量非法: {actual_count}")
            result: list[SymbolInfo] = []
            for _ in range(actual_count):
                address, name_size = struct.unpack("<Qi", self._recv_all(12))
                if name_size < 0:
                    raise RuntimeError(f"符号名长度非法: {name_size}")
                name = self._recv_all(name_size).decode("utf-8", errors="replace") if name_size > 0 else ""
                result.append(SymbolInfo(address, name))
            return total_count, result

    def symbol_find(self, module_base: int, name: str) -> int:
        with self._lock:
            name_bytes = name.encode("utf-8")
            self._send_cmd(CMD_SYMBOL_FIND)
            self._send_all(struct.pack("<IQi", self._handle, module_base, len(name_bytes)))
            if name_bytes:
                self._send_all(name_bytes)
            result, address = struct.unpack("<iQ", self._recv_all(12))
            if result != 0:
                return 0
            return address

    # ── 内存命令 ──────────────────────────────────────────────────

    def read_memory(self, address: int, size: int) -> bytes:
        with self._lock:
            # pack: cmd(1) + handle(4) + address(8) + size(4) + compress(1) = 18
            pkt = struct.pack("<BiQIB", CMD_READPROCESSMEMORY,
                              self._handle, address, size, 0)
            self._send_all(pkt)
            read_count = struct.unpack("<i", self._recv_all(4))[0]
            data = self._recv_all(size)
            if read_count <= 0:
                return b""
            return data[:read_count]

    def write_memory(self, address: int, data: bytes) -> int:
        with self._lock:
            size = len(data)
            pkt = struct.pack("<BiQI", CMD_WRITEPROCESSMEMORY,
                              self._handle, address, size)
            self._send_all(pkt)
            self._send_all(data)
            written = struct.unpack("<i", self._recv_all(4))[0]
            return written

    def read_batch_addr(self, addrs: list[tuple[int, int]]) -> list[tuple[int, bytes]]:
        """批量读取: addrs = [(address, size), ...]"""
        with self._lock:
            self._send_cmd_handle(CMD_READBRATCHADDR)
            count = len(addrs)
            self._send_all(struct.pack("<i", count))
            for addr, sz in addrs:
                self._send_all(struct.pack("<QI", addr, sz))
            _result = struct.unpack("<i", self._recv_all(4))[0]
            out = []
            for addr, sz in addrs:
                raddr = struct.unpack("<Q", self._recv_all(8))[0]
                rdata = self._recv_all(sz)
                out.append((raddr, rdata))
            return out

    # ── 断点命令 ──────────────────────────────────────────────────

    def set_breakpoint(self, address: int, bp_type: int, bp_size: int) -> bool:
        with self._lock:
            self._send_cmd_handle(CMD_KERNEL_SETBREAKPOINT)
            self._send_all(struct.pack("<QII", address, bp_type, bp_size))
            return struct.unpack("<i", self._recv_all(4))[0] != 0

    def remove_breakpoint(self, address: int) -> bool:
        with self._lock:
            self._send_cmd_handle(CMD_KERNEL_REMOVEBREAKPOINT)
            self._send_all(struct.pack("<Q", address))
            return struct.unpack("<i", self._recv_all(4))[0] != 0

    def suspend_breakpoint(self, address: int) -> bool:
        with self._lock:
            self._send_cmd_handle(CMD_KERNEL_SUSPENDBREAKPOINT)
            self._send_all(struct.pack("<Q", address))
            return struct.unpack("<i", self._recv_all(4))[0] != 0

    def resume_breakpoint(self, address: int) -> bool:
        with self._lock:
            self._send_cmd_handle(CMD_KERNEL_RESUMEBREAKPOINT)
            self._send_all(struct.pack("<Q", address))
            return struct.unpack("<i", self._recv_all(4))[0] != 0

    def read_breakpoint_info(self, address: int) -> list[BreakpointHit]:
        with self._lock:
            self._send_cmd_handle(CMD_KERNEL_READHWBPINFO)
            self._send_all(struct.pack("<Q", address))
            hit_count = struct.unpack("<i", self._recv_all(4))[0]
            _total = struct.unpack("<Q", self._recv_all(8))[0]
            hits = []
            if hit_count <= 0:
                return hits
            # HW_HIT_INFO: hit_addr(8) + hit_time(8) + _user_pt_regs(288)（已去除 fpsimd_info）
            # _user_pt_regs: 31*Q + sp + pc + pstate + orig_x0 + syscallno = 36*8 = 288
            HIT_SIZE = 8 + 8 + 288
            raw = self._recv_all(hit_count * HIT_SIZE)
            for i in range(hit_count):
                off = i * HIT_SIZE
                hit_addr, hit_time = struct.unpack_from("<QQ", raw, off)
                off += 16
                regs = list(struct.unpack_from("<31Q", raw, off))
                off += 31 * 8
                sp, pc, pstate = struct.unpack_from("<QQQ", raw, off)
                off += 24
                # orig_x0, syscallno (跳过)
                hits.append(BreakpointHit(hit_addr, hit_time, regs, sp, pc, pstate))
            return hits
