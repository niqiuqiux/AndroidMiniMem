"""MiniMem MCP 的数据编码与断点常量。"""

# 基础内存读写的数据类型宽度和 struct 格式。
DATA_TYPE_SIZE: dict[str, int] = {
    "byte": 1,
    "word": 2,
    "dword": 4,
    "qword": 8,
    "float": 4,
    "double": 8,
    "xor": 4,
}

DATA_TYPE_FMT: dict[str, str] = {
    "byte": "<B",
    "word": "<H",
    "dword": "<I",
    "qword": "<Q",
    "float": "<f",
    "double": "<d",
    "xor": "<I",
}

# 硬件断点类型与 GUI、IPC 和 Lua 的语义保持一致。
BP_TYPE_READ = 1
BP_TYPE_WRITE = 2
BP_TYPE_READWRITE = 3
BP_TYPE_EXECUTE = 4

BP_TYPE_BY_NAME: dict[str, int] = {
    "read": BP_TYPE_READ,
    "write": BP_TYPE_WRITE,
    "readwrite": BP_TYPE_READWRITE,
    "access": BP_TYPE_READWRITE,
    "execute": BP_TYPE_EXECUTE,
}

BP_TYPE_NAMES: dict[int, str] = {
    BP_TYPE_READ: "read",
    BP_TYPE_WRITE: "write",
    BP_TYPE_READWRITE: "readwrite",
    BP_TYPE_EXECUTE: "execute",
}
