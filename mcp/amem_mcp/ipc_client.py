"""HTTP JSON 客户端：与 MiniMem GUI 内嵌的 IpcServer 通信。"""

from __future__ import annotations

import json
import time
import urllib.error
import urllib.request
from typing import Any, Optional


# 耗时操作使用更长超时
_SLOW_METHODS = frozenset({
    "list_processes", "list_modules", "execute_lua",
})

_RETRYABLE_METHODS = frozenset({
    "get_status", "get_version", "get_architecture",
    "list_processes", "list_modules", "get_module_base",
    "read_memory", "resolve_offset_chain",
    "read_bp_info",
    "symbol_list", "symbol_find",
})

_DEFAULT_RETRIES = 2

_MAX_ERROR_PREVIEW_CHARS = 4096


def _effective_retries(method: str, retries: Optional[int]) -> int:
    if retries is None:
        return _DEFAULT_RETRIES if method in _RETRYABLE_METHODS else 0
    return max(0, retries)


def _preview_json(value: Any) -> str:
    try:
        text = json.dumps(value, ensure_ascii=False)
    except TypeError:
        text = repr(value)
    if len(text) > _MAX_ERROR_PREVIEW_CHARS:
        text = text[:_MAX_ERROR_PREVIEW_CHARS - 3] + "..."
    return text


def _protocol_error(message: str, value: Any | None = None) -> dict:
    if value is not None:
        message = f"{message}: {_preview_json(value)}"
    return {"success": False, "error": message}


def _normalize_success_response(parsed: Any) -> dict:
    if not isinstance(parsed, dict):
        return _protocol_error("IPC response must be a JSON object", parsed)

    success = parsed.get("success")
    if not isinstance(success, bool):
        return _protocol_error("IPC response missing boolean success field", parsed)

    if success and "result" not in parsed:
        return _protocol_error("IPC success response missing result field", parsed)

    return parsed


def _json_error_from_http_error(e: urllib.error.HTTPError) -> dict:
    try:
        body = e.read().decode("utf-8")
    except Exception:
        body = ""

    if body:
        try:
            parsed = json.loads(body)
            if isinstance(parsed, dict):
                parsed["success"] = False
                if "error" not in parsed:
                    parsed["error"] = f"IPC HTTP {e.code}: request failed"
                return parsed
        except json.JSONDecodeError:
            pass

    return {
        "success": False,
        "error": f"IPC HTTP {e.code}: {getattr(e, 'reason', '') or body or 'request failed'}",
    }


class IpcClient:
    """通过 HTTP JSON 协议与 MiniMem GUI 通信。"""

    def __init__(self, host: str = "127.0.0.1", port: int = 28100):
        self.host = host
        self.port = port
        self.base_url = f"http://{host}:{port}"

    def call(
        self,
        method: str,
        params: Optional[dict] = None,
        *,
        retries: Optional[int] = None,
        timeout: Optional[float] = None,
    ) -> dict:
        """调用 IPC 方法，返回响应 dict（保证含 success 字段）。"""
        if timeout is None:
            timeout = 60.0 if method in _SLOW_METHODS else 30.0
        retries = _effective_retries(method, retries)

        payload = json.dumps({
            "method": method,
            "params": params or {},
        }).encode("utf-8")

        last_error: Optional[str] = None
        for attempt in range(1 + retries):
            if attempt > 0:
                time.sleep(0.5)

            req = urllib.request.Request(
                self.base_url,
                data=payload,
                headers={"Content-Type": "application/json"},
                method="POST",
            )

            try:
                with urllib.request.urlopen(req, timeout=timeout) as resp:
                    parsed = json.loads(resp.read().decode("utf-8"))
                    return _normalize_success_response(parsed)
            except urllib.error.HTTPError as e:
                return _json_error_from_http_error(e)
            except ConnectionRefusedError:
                return {
                    "success": False,
                    "error": "MiniMem GUI 未启动或 IPC 端口未监听，请先启动 MiniMem GUI",
                }
            except urllib.error.URLError as e:
                reason = getattr(e, "reason", e)
                if isinstance(reason, ConnectionRefusedError):
                    return {
                        "success": False,
                        "error": "MiniMem GUI 未启动或 IPC 端口未监听，请先启动 MiniMem GUI",
                    }
                if isinstance(reason, TimeoutError):
                    last_error = f"操作超时 ({timeout}s)，方法: {method}"
                    continue
                last_error = f"连接 MiniMem GUI 失败: {reason}"
                continue
            except TimeoutError:
                last_error = f"操作超时 ({timeout}s)，方法: {method}"
                continue
            except json.JSONDecodeError as e:
                return {"success": False, "error": f"GUI 返回了无效的 JSON 响应: {e}"}

        return {"success": False, "error": last_error or "未知错误"}

    def call_or_raise(self, method: str, params: Optional[dict] = None, **kwargs) -> Any:
        """调用并检查 success，失败时抛 RuntimeError。"""
        resp = self.call(method, params, **kwargs)
        if not resp.get("success"):
            error_msg = resp.get("error", "未知错误")
            output = resp.get("output", "")
            if output:
                error_msg += f"\n--- 输出 ---\n{output}"
            raise RuntimeError(error_msg)
        return resp.get("result")
