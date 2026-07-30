# MiniMem MCP Tool Catalog

Use this catalog for exact tool selection. The Python modules under `mcp/minimem_mcp/tools/` remain the source of truth when code and this document differ.

## Status and Connection

| Tool | Behavior and constraints |
| --- | --- |
| `get_mcp_version()` | Return the Python MCP and binary protocol versions; does not require a target. |
| `get_status()` | Return GUI device connection state and the current PID/process name. Call first. |
| `get_server_version()` | Query the connected Android server version. |
| `get_architecture()` | Query the connected target architecture. |

Driver initialization and its local configuration belong to the GUI and are intentionally not exposed as MCP tools.

The MCP resource `minimem://status` exposes the current GUI connection, PID, and process name.

## Processes and Modules

| Tool | Behavior and constraints |
| --- | --- |
| `list_processes()` | List observed Android processes and PIDs. |
| `open_process(pid)` | Select a positive PID as the global GUI target. Target switching can invalidate cached state and clean old-target breakpoints. |
| `list_modules(filter="", offset=0, count=200)` | List memory mappings with `rwxp` permissions. `offset` is non-negative; `count` is positive and capped at 1000. Use for `.so`, heap, anonymous, stack, and other mappings. |
| `get_module_base(module_name)` | Resolve a shared-object base. `module_name` must be non-empty and contain `.so`. |
| `resolve_offset_chain(module, base_offset, offsets=[], deref_final=true)` | Resolve an arm64 pointer chain from a `.so` base. Integers are non-negative; the offset list is capped at 1024 entries. |

## Memory

| Tool | Behavior and constraints |
| --- | --- |
| `read_memory(address, size=256)` | Read bytes and return a hex dump. `size` is positive and capped at 65536. |
| `read_value(address, data_type="dword")` | Read one little-endian scalar. Public types are `byte`, `word`, `dword`, `qword`, `float`, and `double`; the implementation also accepts `xor` as a 4-byte integer alias. |
| `write_value(address, value, data_type="dword")` | Encode and write one little-endian scalar. Integer input may be decimal or `0x`; finite decimal floating-point input is required for float types. |
| `write_bytes(address, hex_string)` | Write raw bytes. Whitespace is allowed; the normalized string must be non-empty, valid hexadecimal, and contain an even number of digits. |

All addresses must be non-negative integers or strings accepted by Python base-0 parsing, such as `0x7f1234`. Boolean values are not accepted as integers.

## Hardware Breakpoints

| Tool | Behavior and constraints |
| --- | --- |
| `set_breakpoint(address, bp_type=2, bp_size=4)` | Set a breakpoint by address. Types: `1/read`, `2/write`, `3/readwrite/access`, `4/execute`. Sizes: `1`, `2`, `4`, `8`; execute forces `4`. |
| `remove_breakpoint(address)` | Remove the address-keyed breakpoint and discard its cached MCP hit snapshot. |
| `suspend_breakpoint(address)` | Pause the breakpoint without removing it. |
| `resume_breakpoint(address)` | Resume a paused breakpoint. |
| `read_breakpoint_info(address)` | Pull pending device hits, return an aggregate summary, and cache a non-empty batch for sample drill-down. Device records are pull-and-clear. |
| `read_breakpoint_samples(address, offset=0, count=20)` | Page through the most recently cached hit batch. `count` is capped at 50. It does not fetch new device hits. |
| `query_hardware_breakpoint_slots()` | Query every TID in the selected process through the Kernel `hwbp_query_task` interface; returns per-thread summaries, errno for threads that exited or could not be queried, and occupied slot details. |

Breakpoint operations use addresses rather than handles. A summary distinguishes likely execute breakpoints from data watchpoints and reports hot LR or PC values. Treat this as evidence to correlate with mappings and symbols, not as a final semantic conclusion.

Track every successfully installed breakpoint address. Once no further hit data is needed, call `remove_breakpoint(address)` before switching targets, starting unrelated work, or ending the workflow. `suspend_breakpoint()` preserves the breakpoint and its hardware-slot usage, so it is only a temporary pause and does not satisfy cleanup. If removal has an ambiguous result, re-check status and report that the breakpoint may remain installed instead of claiming cleanup succeeded.

## UXN Exception Breakpoints

All UXN tools require the GUI to be in Kernel memory mode. The service rejects every UXN operation before device-command dispatch in other modes.

| Tool | Behavior and constraints |
| --- | --- |
| `install_uxn_breakpoint(address)` | Install at a non-zero, 4-byte-aligned ARM64 execution address; returns a driver-global slot from 0 to 15. |
| `wait_uxn_breakpoint(slot, timeout_ms=1000, last_sequence=0)` | Wait 1 to 60000 ms for a newer event. Never retries automatically. A successful result caches all X0-X30/SP/PC/PSTATE and FPSIMD data and leaves the target thread paused. |
| `resume_uxn_breakpoint(slot, set_x0=None)` | Resume a paused slot. Optional X0 writeback requires a cached event and revalidates PID, PAUSED state, and sequence before sending its full general-register context. |
| `query_uxn_breakpoint_status(slot)` | Read EMPTY/ARMED/PAUSED/STEPPING state, PID/TID, address, errno, sequence, and counters. Read-only and retryable at the HTTP bridge. |
| `remove_uxn_breakpoint(address)` | Remove the address-owned UXN breakpoint, release a paused thread, and discard matching MCP cache entries. |
| `clear_uxn_breakpoints()` | Clear all driver UXN slots, release all paused threads, and discard all MCP UXN caches. |

Do not leave a successful wait unresolved. Resume, remove, or clear before switching targets or ending the workflow. A wait timeout only means no newer event arrived during that interval.

## Symbols

| Tool | Behavior and constraints |
| --- | --- |
| `symbol_init(module_base)` | Initialize the symbol table for an observed non-negative module base. |
| `symbol_list(offset=0, count=100, module_base="")` | Page symbols; `count` is capped at 1000. Supplying `module_base` initializes and lists in one operation. |
| `symbol_find(module_base, symbol_name)` | Find a non-empty symbol name in the specified initialized module. |

## Lua

| Tool | Behavior and constraints |
| --- | --- |
| `execute_lua(code, timeout_seconds=30)` | Execute non-empty Lua in the GUI. Timeout is positive and capped at 30 seconds. Available domains include `mem`, `process`, `module`, `bp`, and `asm`; `package`, `debug`, `ffi`, and `imgui` are unavailable. |

Lua is a mutation-capable escape hatch. Prefer direct typed MCP tools, and use Lua only for scoped compound logic that the direct surface cannot express.

## Intentionally Unavailable

MiniMem does not expose value scans, fuzzy scans, scan refinement, pointer scans, freeze lists, SO injection, or full-project agent tools. Do not invent these calls. Use targeted reads, module/pointer-chain resolution, symbols, breakpoints, or carefully scoped Lua instead.
