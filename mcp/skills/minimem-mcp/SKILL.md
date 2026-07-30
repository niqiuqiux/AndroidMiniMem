---
name: minimem-mcp
description: Operate MiniMem through its MCP server and reason about the MiniMem MCP bridge, including GUI and device status, Android process selection, module and pointer-chain resolution, targeted memory reads or writes, hardware and UXN exception breakpoints, Lua execution, and ELF symbol queries. Use for live MiniMem Android memory-debugging tasks or when editing or auditing the project's MCP integration; enforce workflow order, scope limits, parameter constraints, mutation authorization, and verification.
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
8. Track every breakpoint address installed during the workflow. As soon as no further hit data is needed, call `remove_breakpoint()` for each temporary breakpoint before switching targets, starting unrelated work, or finishing the task.
9. Report the selected process and exact addresses alongside results so the user can detect stale-target mistakes.

`open_process()` changes global GUI target state and may clean up breakpoints belonging to the old target. Treat it as a state-changing operation, not simple discovery.

## Safety Rules

- Before writing memory, establish the target process, address or module expression, data type, encoded width, intended value, and expected effect.
- Treat a partial write as a real side effect. Report the written byte count and do not blindly retry the entire write.
- Use `read_value()` for scalars. Use `read_memory()` only when surrounding bytes matter, and keep ranges narrow.
- Use `execute_lua()` only when direct tools are insufficient or the user requests Lua automation. Keep code short, deterministic, and scoped to the selected target. Summarize generated code before execution.
- Do not repeatedly retry a mutating operation after timeout, connection loss, or an ambiguous response. Re-check status and current state first because completion may be unknown.
- Treat breakpoint cleanup as part of the requested operation. Do not leave a temporary breakpoint installed after its data is no longer needed unless the user explicitly asked to keep it.

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
- Keep a list of every address passed successfully to `set_breakpoint()` so cleanup does not depend on memory or rediscovery.
- When no more hits or samples are needed from a breakpoint, call `remove_breakpoint(address)` immediately and before continuing to unrelated operations. Remove all temporary breakpoints before changing the selected process or completing the workflow.
- Use `suspend_breakpoint()` only for a deliberate short pause followed by `resume_breakpoint()`. Suspension is not cleanup and does not release the hardware slot; use `remove_breakpoint()` when observation has ended.
- If removal fails or completion is ambiguous, call `get_status()`, report the address as potentially still installed, and do not claim cleanup succeeded or blindly repeat the mutation.
- Use `query_hardware_breakpoint_slots()` to inspect per-thread Kernel hardware-breakpoint slots. It is read-only, requires GUI Kernel mode, and must be interpreted as a point-in-time snapshot; a failed TID query is not evidence that other thread results are invalid.

### UXN Exception Breakpoints

- UXN tools require GUI Kernel memory mode. If the service returns `permission_denied`, initialize or switch the driver from the GUI before trying again.
- Use `install_uxn_breakpoint(address)` only with a non-zero, 4-byte-aligned ARM64 execution address observed for the selected target.
- Call `wait_uxn_breakpoint(slot, timeout_ms, last_sequence)` once per desired event. It does not retry automatically. A successful return means the target thread is paused.
- While an event is paused, inspect it promptly and then call `resume_uxn_breakpoint`, `remove_uxn_breakpoint`, or `clear_uxn_breakpoints`. Do not switch targets or begin unrelated work first.
- Use `resume_uxn_breakpoint(slot, set_x0)` only to modify X0 from the most recent event cached by the same MCP process. The tool verifies that PID, slot state, and sequence still match before writing the full register context.
- Use `query_uxn_breakpoint_status(slot)` for read-only state and statistics. State values are EMPTY, ARMED, PAUSED, and STEPPING.
- Track each installed UXN address and slot. Remove temporary UXN breakpoints when finished; use `clear_uxn_breakpoints()` as recovery cleanup when ownership is uncertain or several slots must be released.
- Treat timeout as “no newer event observed,” not proof that the breakpoint is absent. Treat ambiguous resume/remove/clear completion as potentially still paused and re-check connection and status before any retry.

#### Default: One-Shot Read-Only Register Capture

Unless the user explicitly requests a register modification, UXN inspection defaults to
one read-only sample. Do not supply `set_x0` to `resume_uxn_breakpoint`, and do not
send raw register data through the GUI IPC. The one-shot flow is:

1. Call `get_status()` and `get_architecture()`; continue only with the already
   selected target in Kernel mode.
2. Derive a non-zero, 4-byte-aligned execution address from observed target data,
   then call `install_uxn_breakpoint(address)` and record its returned slot.
3. Call `wait_uxn_breakpoint(slot, timeout_ms, last_sequence=0)` exactly once.
   On success, collect the returned PID, TID, sequence, PC/FAR/ESR, X0-X30, SP,
   PSTATE, and FPSIMD validity as the single register snapshot.
4. While the slot is `PAUSED`, call `resume_uxn_breakpoint(slot)` with no
   `set_x0` argument. This only releases the thread; it does not write registers.
5. Call `remove_uxn_breakpoint(address)` and confirm the recorded slot is `EMPTY`.

If the wait times out, report that no sample was observed and remove the temporary
breakpoint. If any step after a successful wait has an ambiguous result, first query
the slot and then remove that address; use `clear_uxn_breakpoints()` only when the
workflow owns every slot being cleared. Register writeback, including `set_x0`, is a
separate mutation workflow and requires explicit user authorization.

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
