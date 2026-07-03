"""
DataBus client for agentflow_assistant — wraps official UDS SDK.

The official SDK (src/runtime/databus/clients/python/agentflow_databus.py) provides
the full UDS DataBus client with namespace support, subscriptions, async calls, etc.
This module is a thin wrapper that preserves the standard DataBusClient API
while delegating all UDS communication to the official SDK.

Environment variables (injected by the worker):
  AF_DATABUS_UDS_FILE    — UDS socket for runtime DataBus access
  AF_SESSION_ID          — Current session ID
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

    Public methods:
      is_connected()      -> bool
      read_messages()     -> list  (ctx/session_messages, JSON list)
      write_messages(msg) -> bool  (ctx/session_messages, JSON list)
      put(key, value)     -> bool  (sys namespace)
      get(key)            -> str | None (sys namespace)
    """

    def __init__(self):
        self._session_id = os.environ.get("AF_SESSION_ID", "")
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
        if not self._client:
            return False
        return self._client.sys().put(key, value)

    def get(self, key: str) -> str | None:
        if not self._client:
            return None
        return self._client.sys().get(key, None)
