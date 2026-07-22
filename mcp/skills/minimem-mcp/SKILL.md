---
name: minimem-mcp
description: Operate MiniMem through its MCP server and reason about the MiniMem MCP bridge, including GUI and device status, Android process selection, module and pointer-chain resolution, targeted memory reads or writes, hardware breakpoints, Lua execution, and ELF symbol queries. Use for live MiniMem Android memory-debugging tasks or when editing or auditing the project's MCP integration; enforce workflow order, scope limits, parameter constraints, mutation authorization, and verification.
---

# MiniMem MCP

## Core Contract

Treat MiniMem MCP as a bridge into a live Android debugging session:

`Codex -> MCP stdio -> minimem_mcp -> MiniMem GUI HTTP IPC -> Android target`

Do not confuse the two transports. The MCP client starts `minimem-mcp` over stdio; the Python server connects to the GUI at `127.0.0.1:28100` by default.

Do not invent process IDs, module names, addresses, values, breakpoint hits, registers, symbol results, or Lua output. Derive each operation from user input or observed tool results.

## Required Workflow

1. Call `get_status()` before any target operation.
2. If the GUI IPC is unavailable, explain that MiniMem GUI must be running and stop target operations.
3. If the device is not connected, report the status. Driver initialization and configuration are GUI-only operations and are not part of the MCP workflow.
4. If no process is open, call `list_processes()`. Call `open_process(pid)` only when the user selected the process or the choice is unambiguous.
5. For module-relative work, call `list_modules()` or `get_module_base()` before calculating an address. Prefer `resolve_offset_chain()` for pointer chains.
6. Prefer small, targeted reads until the process, address, type, and expected effect are clear.
7. Perform writes, breakpoint changes, or Lua execution only when the user explicitly requested that class of action. Verify effects with a read-back or state query when possible.
8. Report the selected process and exact addresses alongside results so the user can detect stale-target mistakes.

`open_process()` changes global GUI target state and may clean up breakpoints belonging to the old target. Treat it as a state-changing operation, not simple discovery.

## Safety Rules

- Before writing memory, establish the target process, address or module expression, data type, encoded width, intended value, and expected effect.
- Treat a partial write as a real side effect. Report the written byte count and do not blindly retry the entire write.
- Use `read_value()` for scalars. Use `read_memory()` only when surrounding bytes matter, and keep ranges narrow.
- Use `execute_lua()` only when direct tools are insufficient or the user requests Lua automation. Keep code short, deterministic, and scoped to the selected target. Summarize generated code before execution.
- Do not repeatedly retry a mutating operation after timeout, connection loss, or an ambiguous response. Re-check status and current state first because completion may be unknown.
- Remove temporary breakpoints when the requested investigation is complete, unless the user asked to leave them active.

MiniMem intentionally has no value scanning, fuzzy scanning, pointer scanning, freeze list, or injection tools. Do not call or imply those capabilities. Use module discovery, pointer-chain resolution, targeted reads, symbols, breakpoints, or scoped Lua as applicable.

## Operation Guidance

### Processes, Modules, and Addresses

- Use `list_processes()` for discovery; do not guess a PID from a process name.
- Use `list_modules(filter, offset, count)` for both shared objects and anonymous or non-`.so` mappings.
- Use `get_module_base()` only for names containing `.so`. It resolves a module loading base, not an arbitrary matching segment.
- Treat addresses and offsets as non-negative decimal integers or `0x` strings. Check arithmetic provenance before dereferencing.
- Use `resolve_offset_chain(module, base_offset, offsets, deref_final)` for arm64 pointer chains instead of manually mixing base calculations and repeated reads.

### Memory

- Use `read_value(address, data_type)` when the expected type is known.
- Use `read_memory(address, size)` for byte context; request at most 65536 bytes and paginate analysis rather than making broad dumps.
- Use `write_value()` for typed scalar changes and `write_bytes()` for explicit patches.
- Read back the same type and width after a successful write. For raw bytes, compare the exact patched range.

### Hardware Breakpoints

- Use `write` to find writers, `read` to find readers, `readwrite` or `access` for both, and `execute` for control flow.
- Use sizes `1`, `2`, `4`, or `8`; execute breakpoints always use size `4`.
- Call `read_breakpoint_info(address)` once to pull and aggregate the pending hit batch. This device-side read drains pending records and refreshes the Python snapshot cache when hits exist.
- Use `read_breakpoint_samples(address, offset, count)` to inspect raw registers from that cached batch. Do not call `read_breakpoint_info()` again merely to paginate samples.
- Correlate hot PCs or LR values with `list_modules()` and symbol tools before assigning semantic meaning.
- Suspend or remove a breakpoint when it is no longer needed.
- Use `query_hardware_breakpoint_slots()` to inspect per-thread Kernel hardware-breakpoint slots. It is read-only, requires GUI Kernel mode, and must be interpreted as a point-in-time snapshot; a failed TID query is not evidence that other thread results are invalid.

### Symbols and Lua

- Resolve the module base first. Call `symbol_list(..., module_base=...)` to initialize and list in one operation, or call `symbol_init()` before later paginated lists.
- Use `symbol_find(module_base, symbol_name)` only with an observed module base and a non-empty exact query.
- Keep Lua within the documented `mem`, `process`, `module`, `bp`, and `asm` APIs. The IPC sandbox does not expose `package`, `debug`, `ffi`, or `imgui`.
- Include both the error and captured output when Lua execution fails.

## Error Handling

- Treat local validation errors as request mistakes; correct parameters before retrying.
- Treat connection refusal as a missing GUI or unavailable IPC endpoint.
- Preserve GUI IPC error text because it often carries the most precise device or protocol failure.
- On connection loss, target change, timeout, or unknown completion, call `get_status()` before any next operation and ask for confirmation before repeating a mutation.
- Do not interpret an empty breakpoint batch, empty symbol page, zero module base, or zero bytes written as success unless the tool explicitly reports success.

## Tool Reference

Read [references/tool-catalog.md](references/tool-catalog.md) when selecting exact tools, signatures, parameter limits, or stateful behavior.

When editing or auditing this MCP implementation, also read:

- `mcp/README.md` for installation, transport setup, and the public tool surface.
- `docs/api_design.md` for connection, target, partial-write, IPC, breakpoint, symbol, and Lua contracts.
- The relevant module in `mcp/minimem_mcp/tools/` for the executable validation rules.
- `mcp/reference/README.md` before using `mcp/reference/minimem_client.py`; it documents a historical binary client and is not the normal MCP path.

Preserve the implementation chain for new capabilities:

`engine -> socket client -> IMemBackend/IMemService -> GUI IPC -> mcp/minimem_mcp/tools`

Do not bypass `IMemService` from GUI IPC or add features removed from MiniMem merely because they exist in the upstream full project.
