"""Bridge 最终响应的 fail-open SessionLog 观测。"""

from __future__ import annotations

import hashlib
import json
import logging
import os
import threading
import time
from pathlib import Path
from typing import Any

try:
    import fcntl
except ImportError:  # pragma: no cover - production harness is POSIX
    fcntl = None

_lock = threading.Lock()
_seq = 0
_pid = os.getpid()
_process_started_us = time.time_ns() // 1000
_producer_id = f"bridge:{_pid}:{_process_started_us}"
logger = logging.getLogger(__name__)
_PROJECT_ROOT = Path(__file__).resolve().parents[3]


def _sessionlog_path(agent_package_dir: Path, dialogue_id: str) -> Path:
    """显式 Hook 配置优先，随后查找两种本地默认布局。"""
    output_root = os.environ.get("AF_SESSIONLOG_DIR", "")
    if output_root:
        return Path(output_root) / f"{dialogue_id}.jsonl"
    trace_root = os.environ.get("AF_TRACE_OUTPUT_DIR", "")
    if trace_root:
        return Path(trace_root) / "sessionlogs" / f"{dialogue_id}.jsonl"
    candidates = (
        agent_package_dir / "trace_output" / "sessionlogs" / f"{dialogue_id}.jsonl",
        _PROJECT_ROOT / "trace_output" / "sessionlogs" / f"{dialogue_id}.jsonl",
    )
    return next((path for path in candidates if path.is_file()), candidates[0])


def _canonical_sha256(value: Any) -> str:
    encoded = json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), default=str,
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _latest_session_end(path: Path) -> tuple[str, str]:
    """读取最近的 Coordinator 候选终态，供跨进程因果链接。"""
    with path.open("rb") as source:
        if fcntl is not None:
            fcntl.flock(source.fileno(), fcntl.LOCK_SH)
        try:
            lines = source.read().splitlines()
        finally:
            if fcntl is not None:
                fcntl.flock(source.fileno(), fcntl.LOCK_UN)
    for line in reversed(lines):
        try:
            event = json.loads(line)
        except (json.JSONDecodeError, UnicodeDecodeError):
            continue
        if not isinstance(event, dict) or event.get("type") != "session.end":
            continue
        data = event.get("data")
        response_text = data.get("response_text", "") if isinstance(data, dict) else ""
        return str(event.get("event_id") or ""), str(response_text or "")
    return "", ""


def _append_line(path: Path, event: dict[str, Any]) -> None:
    record = (
        json.dumps(event, ensure_ascii=False, separators=(",", ":")) + "\n"
    ).encode("utf-8")
    with _lock:
        with path.open("a+b", buffering=0) as output:
            if fcntl is not None:
                fcntl.flock(output.fileno(), fcntl.LOCK_EX)
            try:
                output.seek(0, os.SEEK_END)
                if output.tell():
                    output.seek(-1, os.SEEK_END)
                    if output.read(1) != b"\n":
                        output.write(b"\n")
                view = memoryview(record)
                while view:
                    written = output.write(view)
                    if not written:
                        raise OSError("short write while appending bridge sessionlog")
                    view = view[written:]
            finally:
                if fcntl is not None:
                    fcntl.flock(output.fileno(), fcntl.LOCK_UN)


def _append_bridge_delivery(
    *,
    agent_package_dir: Path,
    dialogue_id: str,
    agent_id: str,
    session_id: str = "",
    task_id: str = "",
    transport: str,
    stage: str,
    status: str,
    raw_content: str,
    delivered_content: str,
    wire_payload: Any,
    artifacts: list | None = None,
) -> bool:
    """追加服务器侧交付事实；调用方负责 fail-open。"""
    if not dialogue_id:
        return False
    path = _sessionlog_path(Path(agent_package_dir), dialogue_id)
    if not path.is_file():
        return False
    candidate_event_id, candidate_response = _latest_session_end(path)
    raw_content_sha256 = hashlib.sha256(raw_content.encode("utf-8")).hexdigest()
    candidate_response_sha256 = (
        hashlib.sha256(candidate_response.encode("utf-8")).hexdigest() if candidate_event_id else ""
    )
    global _seq
    with _lock:
        _seq += 1
        sequence = _seq
    timestamp_us = time.time_ns() // 1000
    event_id = f"{_producer_id}:{sequence}"
    data = {
        "transport": transport,
        "stage": stage,
        "status": status,
        "raw_content": raw_content,
        "delivered_content": delivered_content,
        "raw_content_sha256": raw_content_sha256,
        "candidate_event_id": candidate_event_id,
        "candidate_response_sha256": candidate_response_sha256,
        "candidate_matches_raw_content": candidate_response_sha256 == raw_content_sha256 if candidate_event_id else None,
        "delivered_content_sha256": hashlib.sha256(delivered_content.encode("utf-8")).hexdigest(),
        "wire_payload": wire_payload,
        "wire_payload_sha256": _canonical_sha256(wire_payload),
        "artifacts": artifacts or [],
        "delivery_claim": "server_side_observation",
        "client_acknowledged": False,
    }
    event: dict[str, Any] = {
        "schema_version": 1,
        "event_id": event_id,
        "idempotency_key": event_id,
        "type": "client.delivery",
        "semantic_kind": "client",
        "lifecycle_phase": "delivery",
        "seq": sequence,
        "producer_sequence": sequence,
        "timestamp_us": timestamp_us,
        "ts": timestamp_us // 1000,
        "producer_id": _producer_id,
        "pid": _pid,
        "trace_id": _producer_id,
        "run_id": dialogue_id,
        "dialogue_id": dialogue_id,
        "session_id": session_id,
        "task_id": task_id,
        "node_id": "bridge.chat_rest",
        "primary_parent_event_id": candidate_event_id,
        "causal_parent_event_ids": [candidate_event_id] if candidate_event_id else [],
        "attributes": {"agent_id": agent_id, "transport": transport},
        "redaction": {"applied": False, "policy": "none"},
        "data": data,
    }
    try:
        _append_line(path, event)
    except Exception as exc:  # noqa: BLE001 - 响应主流程必须 fail-open
        logger.warning("bridge.sessionlog_append_failed dialogue=%s err=%s", dialogue_id, exc)
        return False
    return True


def append_bridge_delivery(
    *,
    agent_package_dir: Path,
    dialogue_id: str,
    agent_id: str,
    session_id: str = "",
    task_id: str = "",
    transport: str,
    stage: str,
    status: str,
    raw_content: str,
    delivered_content: str,
    wire_payload: Any,
    artifacts: list | None = None,
) -> bool:
    """Fail-open 地追加服务器侧交付事实。"""
    try:
        return _append_bridge_delivery(
            agent_package_dir=agent_package_dir,
            dialogue_id=dialogue_id,
            agent_id=agent_id,
            session_id=session_id,
            task_id=task_id,
            transport=transport,
            stage=stage,
            status=status,
            raw_content=raw_content,
            delivered_content=delivered_content,
            wire_payload=wire_payload,
            artifacts=artifacts,
        )
    except Exception as exc:  # noqa: BLE001 - 观测失败不得影响响应主流程
        logger.warning("bridge.sessionlog_append_failed dialogue=%s err=%s", dialogue_id, exc)
        return False
