"""
DataBus client for harness_agent_v2 — wraps official UDS SDK with dialogue_id support.

The framework reads next_skills/skill_prompt from /session/{sid}/sys/{dialogue_id}/{key},
so all puts/gets MUST include dialogue_id prefix. This wrapper ensures that.
"""

import json
import os
import sys
import time
import traceback

_SDK_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "..", "..", "..", "src", "runtime", "databus", "clients", "python"
))
if _SDK_DIR not in sys.path:
    sys.path.insert(0, _SDK_DIR)

from agentflow_databus import DataBusClient as _RealDataBusClient
from agentflow_databus import DataBusError

from agentflow_runtime.common.runtime_context import (
    databus_uds_file,
    dialogue_id,
    session_id,
)
from agentflow_runtime.security import redact_secrets


def _log_warn(msg: str) -> None:
    safe_msg, _ = redact_secrets(str(msg))
    print(f"[databus_client] WARN: {safe_msg}", file=sys.stderr)

def _safe_text(value) -> str:
    safe_value, _report = redact_secrets(str(value))
    return str(safe_value)


def _scoped_key(dialogue: str, key: str) -> str:
    safe_dialogue = _safe_text(dialogue)
    safe_key = _safe_text(key)
    return f"{safe_dialogue}/{safe_key}" if safe_dialogue else safe_key


class DataBusClient:
    """Thin wrapper around the official UDS DataBusClient.

    All sys namespace access is dialogue-scoped: keys are written/read as
    {dialogue_id}/{key} so the framework (which reads /session/{sid}/sys/{dialogue_id}/{key})
    can find them.
    """

    def __init__(self):
        self._session_id = session_id()
        self._dialogue_id = dialogue_id()
        self._uds_path = databus_uds_file()
        self._client: _RealDataBusClient | None = None

        if self._uds_path:
            try:
                self._client = _RealDataBusClient(
                    self._uds_path,
                    session_id=self._session_id,
                    timeout_seconds=30.0,
                )
            except (DataBusError, OSError) as exc:
                _log_warn(f"UDS init failed at {self._uds_path}: {exc}")
                self._client = None

    def is_connected(self) -> bool:
        return self._client is not None

    def read_messages(self) -> list:
        """Read conversation messages (JSON list) from DataBus ctx namespace."""
        if not self._client:
            return []
        _t0 = time.monotonic()
        try:
            raw = self._client.ctx().get("session_messages", "[]")
            if raw is None or raw == "":
                return []
            if isinstance(raw, bytes):
                raw = raw.decode("utf-8")
            messages = json.loads(raw) if isinstance(raw, str) else raw
            safe_messages, _ = redact_secrets(messages)
            return safe_messages
        except Exception as exc:
            _log_warn(f"read_messages failed: {exc}\n{traceback.format_exc()}")
            return []
        finally:
            _ms = (time.monotonic() - _t0) * 1000
            print(f"[timing] databus.read_messages session={_safe_text(self._session_id)} duration_ms={_ms:.1f}",
                  file=sys.stderr)

    def write_messages(self, messages: list) -> bool:
        """Write conversation messages (JSON list) to DataBus ctx namespace."""
        if not self._client:
            return False
        _t0 = time.monotonic()
        try:
            safe_messages, _ = redact_secrets(messages)
            payload = json.dumps(safe_messages, ensure_ascii=False)
            return self._client.ctx().put("session_messages", payload)
        except Exception as exc:
            _log_warn(f"write_messages failed: {exc}\n{traceback.format_exc()}")
            return False
        finally:
            _ms = (time.monotonic() - _t0) * 1000
            print(f"[timing] databus.write_messages session={_safe_text(self._session_id)} duration_ms={_ms:.1f}",
                  file=sys.stderr)

    def put(self, key: str, value: str) -> bool:
        """Write to sys namespace WITH dialogue_id prefix so framework can find it."""
        if not self._client:
            return False
        _t0 = time.monotonic()
        try:
            # Use dialogue-scoped sys namespace: framework reads /session/{sid}/sys/{dialogue_id}/{key}
            scoped_key = _scoped_key(self._dialogue_id, key)
            safe_value, _ = redact_secrets(value)
            return self._client.sys().put(scoped_key, safe_value)
        except Exception as exc:
            _log_warn(f"put failed for key={key}: {exc}")
            return False
        finally:
            _ms = (time.monotonic() - _t0) * 1000
            print(f"[timing] databus.put session={_safe_text(self._session_id)} duration_ms={_ms:.1f}",
                  file=sys.stderr)

    def get(self, key: str) -> str | None:
        """Read from sys namespace WITH dialogue_id prefix."""
        if not self._client:
            return None
        _t0 = time.monotonic()
        scoped_key = _scoped_key(self._dialogue_id, key)
        try:
            raw = self._client.sys().get(scoped_key, None)
            if isinstance(raw, bytes):
                raw = raw.decode("utf-8", errors="replace")
            safe_raw, _ = redact_secrets(raw)
            return safe_raw
        except Exception as exc:
            _log_warn(f"get failed for key={scoped_key}: {exc}")
            return None
        finally:
            _ms = (time.monotonic() - _t0) * 1000
            print(f"[timing] databus.get session={_safe_text(self._session_id)} duration_ms={_ms:.1f}",
                  file=sys.stderr)

    def delete(self, key: str) -> bool:
        """Delete from sys namespace WITH dialogue_id prefix."""
        if not self._client:
            return False
        scoped_key = _scoped_key(self._dialogue_id, key)
        try:
            self._client.sys().remove(scoped_key)
            return True
        except Exception as exc:
            _log_warn(f"delete failed for key={scoped_key}: {exc}")
            return False

    def put_global(self, key: str, value: str) -> bool:
        """Write to sys namespace WITHOUT dialogue_id prefix.

        Used for cross-dialogue data sharing like dispatch manifests
        (coordinator writes → skill-runner reads with different dialogue_ids).
        Uses sys() directly (not raw()) because raw() lacks session context.
        Key is stored as /session/{sid}/sys/{key} (no dialogue prefix).
        """
        if not self._client:
            return False
        _t0 = time.monotonic()
        safe_key = _safe_text(key)
        try:
            safe_value, _ = redact_secrets(value)
            self._client.sys().put(safe_key, safe_value)
            return True
        except Exception as exc:
            _log_warn(f"put_global '{safe_key}' failed: {exc}")
            return False
        finally:
            _ms = (time.monotonic() - _t0) * 1000
            print(f"[timing] databus.put_global session={_safe_text(self._session_id)} duration_ms={_ms:.1f}",
                  file=sys.stderr)

    def get_global(self, key: str) -> str | None:
        """Read from sys namespace WITHOUT dialogue_id prefix."""
        if not self._client:
            return None
        _t0 = time.monotonic()
        safe_key = _safe_text(key)
        try:
            raw = self._client.sys().get(safe_key)
            if raw is None:
                return None
            if isinstance(raw, bytes):
                raw = raw.decode("utf-8", errors="replace")
            safe_raw, _ = redact_secrets(raw)
            return safe_raw
        except Exception as exc:
            _log_warn(f"get_global '{safe_key}' failed: {exc}")
            return None
        finally:
            _ms = (time.monotonic() - _t0) * 1000
            print(f"[timing] databus.get_global session={_safe_text(self._session_id)} duration_ms={_ms:.1f}",
                  file=sys.stderr)

    def response_push(self, content: str, type: str = "progress",
                      source: str = "", skill: str = "",
                      round: str = "", status: str = "") -> bool:
        """Push a streaming chunk to the Bridge via the SDK's response_push.

        Delegates to the underlying ``_RealDataBusClient.response_push()``.
        See streaming-response.md §3.2 / §3.6 for the full protocol contract.

        Returns:
            True if the message was successfully enqueued.
        """
        if not self._client:
            return False
        _t0 = time.monotonic()
        content, _ = redact_secrets(content)
        safe_fields, _ = redact_secrets({
            "type": type,
            "source": source,
            "skill": skill,
            "round": round,
            "status": status,
            "dialogue_id": self._dialogue_id,
        })
        type = safe_fields["type"]
        source = safe_fields["source"]
        skill = safe_fields["skill"]
        round = safe_fields["round"]
        status = safe_fields["status"]
        safe_dialogue_id = safe_fields["dialogue_id"]
        try:
            # 只传非空可选参数（守 SDK 行为：if param: params[param] = val）
            kwargs: dict = {"type": type}
            if source:
                kwargs["source"] = source
            if skill:
                kwargs["skill"] = skill
            if round:
                kwargs["round"] = round
            if status:
                kwargs["status"] = status
            if safe_dialogue_id:
                kwargs["dialogue_id"] = safe_dialogue_id
            ok = self._client.response_push(content, **kwargs)
            # W2 集中审计日志(2026-08-14):调用点分散在 5 个模块,审计行含
            # source/type/skill/status 上下文,全链路推送可追溯(F7 不截断)。
            print(
                f"[audit] databus.response_push session={_safe_text(self._session_id)} "
                f"source={source or '(unset)'} type={type} "
                f"skill={skill or '-'} round={round or '-'} status={status or '-'} "
                f"ok={ok}",
                file=sys.stderr,
            )
            return ok
        except Exception as exc:
            # 失败也打印审计行(含调用模块),再打 WARN 带异常根因——两条信息互补,
            # 定位"哪个模块推送失败 + 为什么失败"(F7.1 请求/响应同范式)。
            print(
                f"[audit] databus.response_push session={_safe_text(self._session_id)} "
                f"source={source or '(unset)'} type={type} "
                f"skill={skill or '-'} round={round or '-'} status={status or '-'} "
                f"ok=False",
                file=sys.stderr,
            )
            _log_warn(f"response_push failed: {exc}")
            return False
        finally:
            _ms = (time.monotonic() - _t0) * 1000
            print(f"[timing] databus.response_push session={_safe_text(self._session_id)} duration_ms={_ms:.1f}",
                  file=sys.stderr)
