"""内存读写。"""

from __future__ import annotations

from mcp.server.fastmcp import FastMCP

from ..constants import DATA_TYPE_SIZE
from ..helpers import (
    clean_hex_string,
    decode_value,
    encode_value_hex,
    hex_dump,
    normalize_data_type,
    parse_int,
    parse_positive_int,
)
from ..ipc_client import IpcClient


def register(mcp: FastMCP, ipc: IpcClient) -> None:

    @mcp.tool()
    def read_memory(address: str, size: int = 256) -> str:
        """读取进程内存并以 hex dump 格式返回。

        Args:
            address: 内存地址，支持 0x 前缀（如 "0x7f12345000"）
            size: 读取字节数，默认 256，最大 65536
        """
        addr_int = parse_int(address)
        size = parse_positive_int(size, "size", 65536)
        r = ipc.call_or_raise("read_memory", {"address": address, "size": size})
        return hex_dump(r["hex"], addr_int)

    @mcp.tool()
    def read_value(address: str, data_type: str = "dword") -> str:
        """读取指定地址的单个值。

        Args:
            address: 内存地址
            data_type: 数据类型 - byte/word/dword/qword/float/double
        """
        addr_int = parse_int(address)
        data_type = normalize_data_type(data_type)
        sz = DATA_TYPE_SIZE[data_type]
        r = ipc.call_or_raise("read_memory", {"address": address, "size": sz})
        val = decode_value(r["hex"], data_type)
        if val is None:
            return "读取失败"
        if data_type in ("float", "double"):
            return f"[{addr_int:#x}] {data_type} = {val}"
        return f"[{addr_int:#x}] {data_type} = {val} ({val:#x})"

    @mcp.tool()
    def write_value(address: str, value: str, data_type: str = "dword") -> str:
        """向指定地址写入一个值。

        Args:
            address: 内存地址
            value: 要写入的值
            data_type: 数据类型 - byte/word/dword/qword/float/double
        """
        parse_int(address)
        hex_val = encode_value_hex(value, data_type)
        r = ipc.call_or_raise("write_memory", {"address": address, "hex": hex_val})
        return f"已写入 {r['written']} 字节到 {address}"

    @mcp.tool()
    def write_bytes(address: str, hex_string: str) -> str:
        """向指定地址写入原始字节。

        Args:
            address: 内存地址
            hex_string: 十六进制字节串，如 "90 90 90" 或 "909090"
        """
        parse_int(address)
        hex_clean = clean_hex_string(hex_string)
        r = ipc.call_or_raise("write_memory", {"address": address, "hex": hex_clean})
        return f"已写入 {r['written']} 字节到 {address}"
