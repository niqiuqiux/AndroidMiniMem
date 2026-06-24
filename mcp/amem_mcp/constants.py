"""AMem 扫描协议常量 — 与 C++ 端 MemoryTypes.h 保持一致。"""

# ── 数据类型 (MemoryTypes.h TYPE enum) ───────────────────────────
TYPE_BYTE = 1
TYPE_WORD = 2
TYPE_DWORD = 4
TYPE_XOR = 8
TYPE_FLOAT = 16
TYPE_QWORD = 32
TYPE_DOUBLE = 64

DATA_TYPE_MAP: dict[str, int] = {
    "byte": TYPE_BYTE,
    "word": TYPE_WORD,
    "dword": TYPE_DWORD,
    "qword": TYPE_QWORD,
    "float": TYPE_FLOAT,
    "double": TYPE_DOUBLE,
    "xor": TYPE_XOR,
}

# ── 扫描类型高位 flag (MemoryTypes.h SCAN_1_TYPE) ────────────────
SCAN1_GROUP = 1 << 31
SCAN1_UNKNOW = 1 << 30
SCAN1_ACCURATE = 1 << 29
SCAN1_LARGER = 1 << 28
SCAN1_LESS = 1 << 27
SCAN1_BETWEEN = 1 << 26
SCAN1_ADD_UNKNOW = 1 << 25
SCAN1_ADD_ACCURATE = 1 << 24
SCAN1_SUB_UNKNOW = 1 << 23
SCAN1_SUB_ACCURATE = 1 << 22
SCAN1_CHANGED = 1 << 21
SCAN1_UNCHANGED = 1 << 20

SCAN_TYPE_MAP: dict[str, int] = {
    "exact": SCAN1_ACCURATE,
    "unknown": SCAN1_UNKNOW,
    "greater": SCAN1_LARGER,
    "less": SCAN1_LESS,
    "between": SCAN1_BETWEEN,
    "increased": SCAN1_ADD_UNKNOW,
    "increased_by": SCAN1_ADD_ACCURATE,
    "decreased": SCAN1_SUB_UNKNOW,
    "decreased_by": SCAN1_SUB_ACCURATE,
    "changed": SCAN1_CHANGED,
    "unchanged": SCAN1_UNCHANGED,
}

# ── 内存区域类型 (MemoryType enum) ───────────────────────────────
MEMORY_TYPE_MAP: dict[str, int] = {
    "all": -1,
    "anonymous": 32,
    "c_alloc": 4,
    "c_heap": 1,
    "c_data": 8,
    "c_bss": 16,
    "java_heap": 2,
    "java": 65536,
    "stack": 64,
    "code_app": 16384,
    "code_system": 32768,
    "video": 1048576,
    "ashmem": 524288,
    "bad": 131072,
    "other": -2080896,
}

# ── 数据类型字节宽度和 struct 格式 ───────────────────────────────
DATA_TYPE_SIZE: dict[str, int] = {
    "byte": 1, "word": 2, "dword": 4, "qword": 8,
    "float": 4, "double": 8, "xor": 4,
}

DATA_TYPE_FMT: dict[str, str] = {
    "byte": "<B", "word": "<H", "dword": "<I", "qword": "<Q",
    "float": "<f", "double": "<d", "xor": "<I",
}

# ── 硬件断点类型 (与 gui/BreakpointWindow.h HW_BREAKPOINT_* 对齐) ──
# 语义与 AMem 内部（IPC / Lua / GUI）完全一致，MCP 层不做任何翻译
BP_TYPE_READ = 1          # HW_BREAKPOINT_R
BP_TYPE_WRITE = 2         # HW_BREAKPOINT_W
BP_TYPE_READWRITE = 3     # HW_BREAKPOINT_RW = R | W
BP_TYPE_EXECUTE = 4       # HW_BREAKPOINT_X

# 便于从字符串映射（与 LuaAPI.cpp SetBreakpoint 的字符串标签一致）
BP_TYPE_BY_NAME: dict[str, int] = {
    "read":      BP_TYPE_READ,
    "write":     BP_TYPE_WRITE,
    "readwrite": BP_TYPE_READWRITE,
    "access":    BP_TYPE_READWRITE,
    "execute":   BP_TYPE_EXECUTE,
}

BP_TYPE_NAMES: dict[int, str] = {
    BP_TYPE_READ:      "read",
    BP_TYPE_WRITE:     "write",
    BP_TYPE_READWRITE: "readwrite",
    BP_TYPE_EXECUTE:   "execute",
}
