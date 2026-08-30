"""Bridge 最终响应的 fail-open 轨迹日志观测。"""

from __future__ import annotations

import hashlib
import json
import logging
import os
import threading
import time
from pathlib import Path
from typing import Any

from services.secret_safety import (
    safe_error_text,
    sanitize_for_storage,
)

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


def _safe_path_component(value: str, fallback: str) -> str:
    """与采集 Hook 使用相同的安全文件名规则。"""
    try:
        original, _report = sanitize_for_storage(str(value or ""))
    except Exception:
        return fallback
    if not isinstance(original, str) or not original:
        return fallback
    component = "".join(
        char if char.isalnum() or char in "-_." else "_"
        for char in original
    ).strip(".")
    changed = component != original or len(component) > 48
    component = component[:48] or fallback
    if changed:
        digest = hashlib.sha256(original.encode("utf-8")).hexdigest()[:12]
        component = f"{component}-{digest}"
    return component


def _find_dialogue_file(root: Path, dialogue_id: str) -> Path:
    """定位新层级文件，并兼容历史平铺与旧投影目录。"""
    filename = f"{_safe_path_component(dialogue_id, 'unknown-dialogue')}.jsonl"
    direct = root / filename
    if direct.is_file():
        return direct
    for pattern in (
        f"by-user/*/*/{filename}",
        f"by-user/*/sessions/*/{filename}",
        f"by-user/*/conversations/*/{filename}",
    ):
        match = next((path for path in sorted(root.glob(pattern)) if path.is_file()), None)
        if match is not None:
            return match
    return direct


def _find_identity_dialogue_file(
    root: Path,
    dialogue_id: str,
    user_id: str,
    client_id: str,
) -> Path:
    """按采集身份定位文件，绝不回退到其他用户的同名 dialogue。"""
    filename = f"{_safe_path_component(dialogue_id, 'unknown-dialogue')}.jsonl"
    user_component = _safe_path_component(user_id, "unknown-user")
    client_component = _safe_path_component(client_id, "unknown-conversation")
    exact = root / "by-user" / user_component / client_component / filename
    candidates = (
        exact,
        root / "by-user" / user_component / "conversations" / client_component / filename,
        root / filename,
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    legacy_sessions = root / "by-user" / user_component / "sessions"
    match = next((
        path for path in sorted(legacy_sessions.glob(f"*/{filename}"))
        if path.is_file()
    ), None)
    return match or exact


def _sessionlog_path(
    agent_package_dir: Path,
    dialogue_id: str,
    user_id: str = "",
    client_id: str = "",
) -> Path:
    """显式 Hook 配置优先，随后查找本地新旧轨迹布局。"""
    finder = (
        lambda root: _find_identity_dialogue_file(
            root, dialogue_id, user_id, client_id,
        )
        if user_id and client_id else _find_dialogue_file(root, dialogue_id)
    )
    output_root = os.environ.get("AF_SESSIONLOG_DIR", "")
    if output_root:
        return finder(Path(output_root))
    trace_root = os.environ.get("AF_TRACE_OUTPUT_DIR", "")
    if trace_root:
        return finder(Path(trace_root) / "sessionlogs")
    roots = (
        agent_package_dir / "trace_output" / "sessionlogs",
        _PROJECT_ROOT / "trace_output" / "sessionlogs",
    )
    candidates = tuple(finder(root) for root in roots)
    return next((path for path in candidates if path.is_file()), candidates[0])


def _canonical_sha256(value: Any) -> str:
    encoded = json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), default=str,
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _latest_dialogue_end(path: Path) -> tuple[str, str]:
    """读取最近的 Coordinator 候选终态，兼容历史 session.end。"""
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
        if not isinstance(event, dict) or event.get("type") not in {
            "dialogue.end", "session.end",
        }:
            continue
        data = event.get("data")
        response_text = data.get("response_text", "") if isinstance(data, dict) else ""
        return str(event.get("event_id") or ""), str(response_text or "")
    return "", ""


def _append_line(path: Path, event: dict[str, Any]) -> None:
    safe_event, sink_report = sanitize_for_storage(event)
    if sink_report.applied:
        safe_event["redaction"] = sink_report.to_dict()
    record = (
        json.dumps(safe_event, ensure_ascii=False, separators=(",", ":")) + "\n"
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
    task_id: str = "",
    transport: str,
    stage: str,
    status: str,
    raw_content: str,
    delivered_content: str,
    wire_payload: Any,
    artifacts: list | None = None,
    user_id: str = "",
    client_id: str = "",
) -> bool:
    """追加服务器侧交付事实；调用方负责 fail-open。"""
    if not dialogue_id:
        return False
    safe_fields, redaction = sanitize_for_storage({
        "dialogue_id": dialogue_id,
        "agent_id": agent_id,
        "task_id": task_id,
        "transport": transport,
        "stage": stage,
        "status": status,
        "raw_content": raw_content,
        "delivered_content": delivered_content,
        "wire_payload": wire_payload,
        "artifacts": artifacts or [],
        "user_id": user_id,
        "client_id": client_id,
    })
    dialogue_id = str(safe_fields["dialogue_id"])
    agent_id = str(safe_fields["agent_id"])
    task_id = str(safe_fields["task_id"])
    transport = str(safe_fields["transport"])
    stage = str(safe_fields["stage"])
    status = str(safe_fields["status"])
    raw_content = str(safe_fields["raw_content"])
    delivered_content = str(safe_fields["delivered_content"])
    wire_payload = safe_fields["wire_payload"]
    artifacts = safe_fields["artifacts"]
    user_id = str(safe_fields["user_id"])
    client_id = str(safe_fields["client_id"])
    path = _sessionlog_path(
        Path(agent_package_dir), dialogue_id,
        user_id=user_id, client_id=client_id,
    )
    if not path.is_file():
        return False
    candidate_event_id, candidate_response = _latest_dialogue_end(path)
    safe_candidate, _candidate_redaction = sanitize_for_storage({
        "event_id": candidate_event_id,
        "response": candidate_response,
    })
    candidate_event_id = str(safe_candidate["event_id"])
    candidate_response = str(safe_candidate["response"])
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
        "schema_version": 2,
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
        "task_id": task_id,
        "node_id": "bridge.chat_rest",
        "primary_parent_event_id": candidate_event_id,
        "causal_parent_event_ids": [candidate_event_id] if candidate_event_id else [],
        "attributes": {"agent_id": agent_id, "transport": transport},
        "redaction": redaction.to_dict(),
        "data": data,
    }
    try:
        _append_line(path, event)
    except Exception as exc:  # noqa: BLE001 - 响应主流程必须 fail-open
        logger.warning("bridge.sessionlog_append_failed dialogue=%s err=%s", dialogue_id, safe_error_text(exc))
        return False
    return True


def append_bridge_delivery(
    *,
    agent_package_dir: Path,
    dialogue_id: str,
    agent_id: str,
    task_id: str = "",
    transport: str,
    stage: str,
    status: str,
    raw_content: str,
    delivered_content: str,
    wire_payload: Any,
    artifacts: list | None = None,
    user_id: str = "",
    client_id: str = "",
) -> bool:
    """Fail-open 地追加服务器侧交付事实。"""
    try:
        return _append_bridge_delivery(
            agent_package_dir=agent_package_dir,
            dialogue_id=dialogue_id,
            agent_id=agent_id,
            task_id=task_id,
            transport=transport,
            stage=stage,
            status=status,
            raw_content=raw_content,
            delivered_content=delivered_content,
            wire_payload=wire_payload,
            artifacts=artifacts,
            user_id=user_id,
            client_id=client_id,
        )
    except Exception as exc:  # noqa: BLE001 - 观测失败不得影响响应主流程
        logger.warning("bridge.sessionlog_append_failed dialogue=%s err=%s", dialogue_id, safe_error_text(exc))
        return False
