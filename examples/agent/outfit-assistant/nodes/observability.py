#!/usr/bin/env python3
"""
Observability utilities for agent nodes.

Provides structured JSON logging to stderr for the coordinator and skill_executor
nodes. These structured log entries are parsed by the framework/daemon for
observability, monitoring, and debugging.

Usage:
    from observability import emit_event
    emit_event("coordinator.decision", {"round": 1, "tool_calls": ["run_skills"]})
"""

import json
import os
import sys
import time
from typing import Any


def _node_context() -> dict[str, str]:
    """Gather node context from env vars (consistent across all nodes)."""
    return {
        "dialogue_id": os.environ.get("AF_DIALOGUE_ID", ""),
        "session_id": os.environ.get("AF_SESSION_ID", ""),
        "node_id": os.environ.get("AF_NODE_ID", ""),
        "task_id": os.environ.get("AF_SELF_TASK_ID", ""),
    }


def emit_event(
    event: str,
    data: dict[str, Any] | None = None,
    *,
    level: str = "INFO",
    duration_ms: float | None = None,
) -> None:
    """Emit a structured observability event to stderr.

    Args:
        event: Event name, e.g. "coordinator.decision", "skill.tool_call"
        data: Event-specific key-value payload
        level: Log level (INFO, WARN, ERROR)
        duration_ms: Optional duration in milliseconds for timing events
    """
    payload = {
        "event": event,
        "level": level,
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime()),
    }
    if data:
        payload["data"] = data
    if duration_ms is not None:
        payload["duration_ms"] = round(duration_ms, 1)
    payload.update(_node_context())
    print(f"[obs] {json.dumps(payload, ensure_ascii=False)}", file=sys.stderr)
