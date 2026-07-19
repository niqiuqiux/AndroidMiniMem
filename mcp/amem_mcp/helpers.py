"""Shared helpers for MCP value encoding, validation, and formatting."""

from __future__ import annotations

import math
import struct

from .constants import (
    DATA_TYPE_FMT,
    DATA_TYPE_SIZE,
)


def parse_int(value: str | int) -> int:
    """Parse a non-negative decimal or 0x-prefixed integer."""
    if isinstance(value, bool):
        raise ValueError("integer value must not be boolean")
    if isinstance(value, int):
        parsed = value
    else:
        text = str(value).strip()
        if not text:
            raise ValueError("integer value is empty")
        parsed = int(text, 0)
    if parsed < 0:
        raise ValueError("integer value must be non-negative")
    return parsed


def parse_positive_int(value: str | int, name: str, max_value: int | None = None) -> int:
    """Parse a positive integer, optionally capping it to max_value."""
    parsed = parse_int(value)
    if parsed <= 0:
        raise ValueError(f"{name} must be positive")
    if max_value is not None and parsed > max_value:
        return max_value
    return parsed


def normalize_data_type(data_type: str) -> str:
    key = str(data_type).strip().lower()
    if key not in DATA_TYPE_FMT:
        raise ValueError(f"unsupported data_type '{data_type}'")
    return key


def clean_hex_string(hex_string: str) -> str:
    hex_clean = "".join(str(hex_string).split())
    if not hex_clean:
        raise ValueError("hex string is empty")
    if len(hex_clean) % 2 != 0:
        raise ValueError("hex string must contain an even number of digits")
    try:
        bytes.fromhex(hex_clean)
    except ValueError as exc:
        raise ValueError("hex string contains non-hex characters") from exc
    return hex_clean


def clamp_page(offset: int, count: int, max_count: int = 1000) -> tuple[int, int]:
    return parse_int(offset), parse_positive_int(count, "count", max_count)


def format_module_perms(flag: int) -> str:
    """将模块段的 flag 位掩码格式化为 rwxp 权限字符串。

    位定义与设备端一致（engine/common/LinuxProc.hpp、AndroidMemKernel.hpp）：
    1=读 2=写 4=执行 8=私有(p) 16=共享(s)。第四位私有优先于共享。
    """
    flag = int(flag)
    r = "r" if flag & 1 else "-"
    w = "w" if flag & 2 else "-"
    x = "x" if flag & 4 else "-"
    if flag & 8:
        sp = "p"
    elif flag & 16:
        sp = "s"
    else:
        sp = "-"
    return r + w + x + sp


def encode_value_hex(value: str, data_type: str) -> str:
    """Encode a typed scalar value as little-endian hex.

    Integer types accept signed input (e.g. "-1"): a negative value is wrapped
    to its two's-complement representation for the selected type width.
    """
    data_type = normalize_data_type(data_type)
    fmt = DATA_TYPE_FMT[data_type]
    if data_type in ("float", "double"):
        if isinstance(value, bool):
            raise ValueError(f"{data_type} value must not be boolean")
        parsed_float = float(value)
        if not math.isfinite(parsed_float):
            raise ValueError(f"{data_type} value must be finite")
        return struct.pack(fmt, parsed_float).hex()

    if isinstance(value, bool):
        raise ValueError(f"{data_type} value must not be boolean")
    parsed = int(str(value).strip(), 0)
    bit_width = DATA_TYPE_SIZE[data_type] * 8
    signed_min = -(1 << (bit_width - 1))
    unsigned_max = (1 << bit_width) - 1
    if parsed < signed_min or parsed > unsigned_max:
        raise ValueError(
            f"{data_type} value {parsed} is out of range for a "
            f"{bit_width}-bit integer"
        )
    if parsed < 0:
        parsed &= unsigned_max  # two's-complement wrap to match the C++ side
    return struct.pack(fmt, parsed).hex()


def decode_value(hex_data: str, data_type: str):
    """Decode little-endian hex bytes into a typed scalar value."""
    data_type = normalize_data_type(data_type)
    size = DATA_TYPE_SIZE[data_type]
    fmt = DATA_TYPE_FMT[data_type]
    data = bytes.fromhex(hex_data)
    if len(data) < size:
        return None
    return struct.unpack(fmt, data[:size])[0]


def hex_dump(hex_str: str, base_addr: int, width: int = 16) -> str:
    """Render hex bytes as an xxd-like dump."""
    data = bytes.fromhex(hex_str)
    lines = []
    for i in range(0, len(data), width):
        chunk = data[i:i + width]
        hex_part = " ".join(f"{b:02X}" for b in chunk)
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{base_addr + i:012X}  {hex_part:<{width * 3}}  {ascii_part}")
    return "\n".join(lines)
