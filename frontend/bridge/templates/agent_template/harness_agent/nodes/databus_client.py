"""
DataBus client for harness_agent_v2 — wraps official UDS SDK with dialogue_id support.

The framework reads next_skills/skill_prompt from /session/{sid}/sys/{dialogue_id}/{key},
so all puts/gets MUST include dialogue_id prefix. This wrapper ensures that.
"""

import json
import os
import sys
import traceback

_SDK_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "..", "..", "..", "src", "runtime", "databus", "clients", "python"
))
if _SDK_DIR not in sys.path:
    sys.path.insert(0, _SDK_DIR)

from agentflow_databus import DataBusClient as _RealDataBusClient
from agentflow_databus import DataBusError


def _log_warn(msg: str) -> None:
    print(f"[databus_client] WARN: {msg}", file=sys.stderr)


class DataBusClient:
    """Thin wrapper around the official UDS DataBusClient.

    All sys namespace access is dialogue-scoped: keys are written/read as
    {dialogue_id}/{key} so the framework (which reads /session/{sid}/sys/{dialogue_id}/{key})
    can find them.
    """

    def __init__(self):
        self._session_id = os.environ.get("AF_SESSION_ID", "")
        self._dialogue_id = os.environ.get("AF_DIALOGUE_ID", "")
        self._uds_path = os.environ.get("AF_DATABUS_UDS_FILE", "")
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
        try:
            raw = self._client.ctx().get("session_messages", "[]")
            if raw is None or raw == "":
                return []
            if isinstance(raw, bytes):
                raw = raw.decode("utf-8")
            return json.loads(raw) if isinstance(raw, str) else raw
        except Exception as exc:
            _log_warn(f"read_messages failed: {exc}\n{traceback.format_exc()}")
            return []

    def write_messages(self, messages: list) -> bool:
        """Write conversation messages (JSON list) to DataBus ctx namespace."""
        if not self._client:
            return False
        try:
            return self._client.ctx().put("session_messages", json.dumps(messages, ensure_ascii=False))
        except Exception as exc:
            _log_warn(f"write_messages failed: {exc}\n{traceback.format_exc()}")
            return False

    def put(self, key: str, value: str) -> bool:
        """Write to sys namespace WITH dialogue_id prefix so framework can find it."""
        if not self._client:
            return False
        # Use dialogue-scoped sys namespace: framework reads /session/{sid}/sys/{dialogue_id}/{key}
        scoped_key = f"{self._dialogue_id}/{key}" if self._dialogue_id else key
        return self._client.sys().put(scoped_key, value)

    def get(self, key: str) -> str | None:
        """Read from sys namespace WITH dialogue_id prefix."""
        if not self._client:
            return None
        scoped_key = f"{self._dialogue_id}/{key}" if self._dialogue_id else key
        try:
            raw = self._client.sys().get(scoped_key, None)
            if isinstance(raw, bytes):
                return raw.decode("utf-8", errors="replace")
            return raw
        except Exception as exc:
            _log_warn(f"get failed for key={scoped_key}: {exc}")
            return None

    def delete(self, key: str) -> bool:
        """Delete from sys namespace WITH dialogue_id prefix."""
        if not self._client:
            return False
        scoped_key = f"{self._dialogue_id}/{key}" if self._dialogue_id else key
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
        try:
            self._client.sys().put(key, value)
            return True
        except Exception as exc:
            _log_warn(f"put_global '{key}' failed: {exc}")
            return False

    def get_global(self, key: str) -> str | None:
        """Read from sys namespace WITHOUT dialogue_id prefix."""
        if not self._client:
            return None
        try:
            raw = self._client.sys().get(key)
            if raw is None:
                return None
            if isinstance(raw, bytes):
                return raw.decode("utf-8", errors="replace")
            return raw
        except Exception as exc:
            _log_warn(f"get_global '{key}' failed: {exc}")
            return None

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
        try:
            # 只传非空可选参数（守 SDK 行为：if param: params[param] = val）
            kwargs: dict = {"type": type}
            if source: kwargs["source"] = source
            if skill:   kwargs["skill"] = skill
            if round:   kwargs["round"] = round
            if status:  kwargs["status"] = status
            if self._dialogue_id:
                kwargs["dialogue_id"] = self._dialogue_id
            return self._client.response_push(content, **kwargs)
        except Exception as exc:
            _log_warn(f"response_push failed: {exc}")
            return False
