"""AgentFlow Bridge — REST chat endpoint.

Provides both synchronous (request-response) and streaming (SSE) REST
alternatives to the WebSocket streaming chat in ``chat.py``.

Uses ``user_id`` + ``client_id`` (not ``session_id``) to identify
the caller, matching the daemon's ``SessionRouter`` design.

Cancellation support (two paths):

1. **Explicit cancel** via ``POST /api/chat/{agent_id}/cancel`` with
   ``{"dialogue_id": "..."}`` — sends ``DIALOGUE-INTERRUPT`` to the daemon
   so the session-level serialization lock is released.

2. **Auto-cancel on SSE disconnect** (incl. client-initiated close) —
   when the SSE stream generator is interrupted, Bridge sends
   ``DIALOGUE-INTERRUPT`` automatically to prevent the daemon from
   holding the lock indefinitely.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import time
import typing
import uuid
from collections.abc import AsyncIterator
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any

from fastapi import APIRouter, Depends, HTTPException, Query, Request, status
from pydantic import BaseModel, Field
from starlette.responses import StreamingResponse

from client import (
    DaemonClient,
    DaemonConnectionError,
    DaemonError,
    DaemonTimeoutError,
    InvalidParamsError,
    NotFoundError,
)
from config import settings
from content_serializer import (
    derive_history_user_messages,
    prepare_content_for_daemon,
    serialize_content_for_storage,
)
from dependencies import get_daemon_client
from models import ContentPart, SAFE_ID_PATTERN
from providers import SessionProvider
from routes.artifact_extractor import apply_artifact_output_mode, extract_artifact_infos, publicize_artifact_urls as _shared_publicize_artifact_urls, serialize_artifacts
from routes.conversations import get_store, ConversationRecord
from services import _pull_and_save_traces
from services.sessionlog_delivery import append_bridge_delivery
from services.secret_safety import sanitize_for_storage, SecretSanitizerUnavailable
from session_dependencies import get_session_provider

logger = logging.getLogger("agentflow.bridge.routes.chat_rest")

# ⚠ DEBUG: SSE capture fixture path (set via env; empty = disabled)
_SSE_CAPTURE_PATH = os.environ.get("AF_SSE_CAPTURE", "")

# Per-request SSE dump directory (set via env; empty = disabled).
# Each SSE streaming request dumps request body + all SSE payloads to
# {dir}/{timestamp}_{dialogue_id}.jsonl for debugging.
_SSE_DUMP_DIR = os.environ.get("AF_SSE_DUMP_DIR", "")
_sse_capture_file: typing.IO | None = None


def _sanitize_debug_payload(value: Any) -> Any:
    """Sanitize optional debug copies with a fail-closed fallback."""
    try:
        safe_value, _report = sanitize_for_storage(value)
        return safe_value
    except SecretSanitizerUnavailable:
        return {"redaction": "sanitizer_unavailable"}


def _capture_sse_payload(payload: dict[str, Any]) -> None:
    """如果 AF_SSE_CAPTURE 环境变量已设，将 SSE payload 追加到 fixture 文件。"""
    global _sse_capture_file
    if not _SSE_CAPTURE_PATH:
        return
    try:
        if _sse_capture_file is None:
            _sse_capture_file = open(_SSE_CAPTURE_PATH, "w")
        _sse_capture_file.write(json.dumps(_sanitize_debug_payload(payload), ensure_ascii=False) + "\n")
        _sse_capture_file.flush()
    except Exception:
        pass

router = APIRouter(prefix="/api/chat", tags=["chat"])


# ═══════════════════════════════════════════════════════════════════
# Active dialogue registry — tracks in-progress SSE streams so the
# cancel endpoint and disconnect handler can find the right dialogue.
# ═══════════════════════════════════════════════════════════════════

@dataclass
class _ActiveDialogue:
    """State held for an in-flight SSE-stream dialogue."""
    agent_id: str
    dialogue_id: str
    user_id: str
    client_id: str
    daemon_client: DaemonClient
    session_id: str = ""           # resolved from agent.execute result
    cancelled: bool = False        # set to True when cancel is requested


# Module-level registry.  Keyed by ``(user_id, client_id)`` so external
# callers can cancel without knowing the internal dialogue_id.
# At most one active dialogue per (user_id, client_id) due to the daemon's
# session-level serialization lock.
_active_dialogues: dict[tuple[str, str], _ActiveDialogue] = {}


def _registry_key(d: _ActiveDialogue) -> tuple[str, str]:
    return (d.user_id, d.client_id)


def _register_dialogue(d: _ActiveDialogue) -> bool:
    key = _registry_key(d)
    if key in _active_dialogues:
        return False
    _active_dialogues[key] = d
    return True


def _unregister_dialogue(user_id: str, client_id: str, dialogue_id: str = "") -> None:
    key = (user_id, client_id)
    current = _active_dialogues.get(key)
    if current is not None and (not dialogue_id or current.dialogue_id == dialogue_id):
        _active_dialogues.pop(key, None)


async def _cancel_dialogue(d: _ActiveDialogue) -> None:
    """Send DIALOGUE-INTERRUPT to the daemon for this dialogue.

    Idempotent: subsequent calls for the same dialogue are no-ops.
    """
    if d.cancelled:
        return
    d.cancelled = True
    session_id = d.session_id
    if not session_id:
        try:
            result = await d.daemon_client.session_list(d.agent_id, d.user_id)
            sessions = result if isinstance(result, list) else (
                result.get("sessions", []) if isinstance(result, dict) else []
            )
            matches = [
                str(item.get("session_id") or item.get("id") or "")
                for item in sessions
                if isinstance(item, dict)
                and str(item.get("user_id") or "") == d.user_id
                and str(item.get("client_id") or "") == d.client_id
                and (item.get("session_id") or item.get("id"))
            ]
            if len(matches) == 1:
                session_id = matches[0]
                d.session_id = session_id
            elif len(matches) > 1:
                logger.warning(
                    "cancel_dialogue.ambiguous_session agent=%s dialogue=%s matches=%d",
                    d.agent_id, d.dialogue_id, len(matches),
                )
        except Exception as exc:
            logger.warning(
                "cancel_dialogue.session_lookup_error agent=%s dialogue=%s err=%s",
                d.agent_id, d.dialogue_id, exc,
            )
    if not session_id:
        logger.warning("cancel_dialogue.no_session agent=%s dialogue=%s", d.agent_id, d.dialogue_id)
        return
    try:
        await d.daemon_client.agent_send_control(
            agent_id=d.agent_id,
            command="DIALOGUE-INTERRUPT",
            session_id=session_id,
            user_id=d.user_id,
            dialogue_id=d.dialogue_id,
        )
        logger.info(
            "cancel_dialogue.sent agent=%s dialogue=%s session=%s",
            d.agent_id, d.dialogue_id, session_id,
        )
        append_bridge_delivery(
            agent_package_dir=settings.agent_packages_path / d.agent_id,
            dialogue_id=d.dialogue_id,
            agent_id=d.agent_id,
            user_id=d.user_id,
            client_id=d.client_id,
            task_id="",
            transport="control",
            stage="cancel_acknowledged",
            status="cancelled",
            raw_content="",
            delivered_content="",
            wire_payload={"command": "DIALOGUE-INTERRUPT"},
        )
    except Exception as exc:
        logger.warning(
            "cancel_dialogue.error agent=%s dialogue=%s err=%s",
            d.agent_id, d.dialogue_id, exc,
        )


# Cancel request schema
class CancelRequest(BaseModel):
    """Request body for POST /api/chat/{agent_id}/cancel."""
    user_id: str = Field(
        ..., pattern=SAFE_ID_PATTERN, description="User identifier (matches the active SSE stream).",
    )
    client_id: str = Field(
        ..., pattern=SAFE_ID_PATTERN, description="Client/session identifier (matches the active SSE stream).",
    )


# Cancel endpoint
@router.post(
    "/{agent_id}/cancel",
    status_code=status.HTTP_200_OK,
    summary="Cancel an in-flight dialogue and release the session serialization lock.",
)
async def cancel_dialogue(
    agent_id: str,
    body: CancelRequest,
    client: DaemonClient = Depends(get_daemon_client),  # noqa: B008
) -> dict[str, Any]:
    """Cancel an in-flight SSE-stream dialogue.

    Looks up the active dialogue by ``(user_id, client_id)`` and sends
    ``DIALOGUE-INTERRUPT`` to the daemon, which interrupts any running
    skill tasks and releases the session-level serialization lock so the
    next queued dialogue can proceed.

    Idempotent: cancelling a completed or non-existent dialogue is a no-op
    (returns ``cancelled=false``).
    """
    key = (body.user_id, body.client_id)
    d = _active_dialogues.get(key)
    if not d or d.cancelled:
        logger.info(
            "cancel_dialogue.skip agent=%s user=%s client=%s reason=%s",
            agent_id, body.user_id, body.client_id,
            "not found" if not d else "already cancelled",
        )
        return {"success": True, "data": {"cancelled": False}}

    await _cancel_dialogue(d)
    return {"success": True, "data": {"cancelled": True, "dialogue_id": d.dialogue_id}}


class ChatRestRequest(BaseModel):
    """REST chat request body."""

    model_config = {"from_attributes": True}

    user_id: str = Field(..., pattern=SAFE_ID_PATTERN, description="User identifier.")
    client_id: str = Field(..., pattern=SAFE_ID_PATTERN, description="Client/session identifier.")
    content: str | list[ContentPart] = Field(
        ..., description="User message content (string or content array).",
    )


class ChatRestResponse(BaseModel):
    """REST chat response body."""

    model_config = {"from_attributes": True}

    task_id: str | None = None
    content: str = ""
    artifacts: list[dict] = Field(default_factory=list)


# --------------------------------------------------------------------------
# Envelope helpers
# --------------------------------------------------------------------------


def _ok(data: Any) -> dict[str, Any]:
    return {"success": True, "data": data, "error": None}


def _raise_daemon_error(exc: DaemonError) -> HTTPException:
    if isinstance(exc, InvalidParamsError):
        code = status.HTTP_400_BAD_REQUEST
    elif isinstance(exc, NotFoundError):
        code = status.HTTP_404_NOT_FOUND
    elif isinstance(exc, DaemonTimeoutError):
        code = status.HTTP_504_GATEWAY_TIMEOUT
    elif isinstance(exc, DaemonConnectionError):
        code = status.HTTP_502_BAD_GATEWAY
    else:
        code = status.HTTP_500_INTERNAL_SERVER_ERROR
    logger.warning(
        "daemon proxy error: %s -> HTTP %s",
        exc,
        code,
        exc_info=logger.isEnabledFor(logging.DEBUG),
    )
    return HTTPException(status_code=code, detail=str(exc))


# --------------------------------------------------------------------------
# Extractors
# --------------------------------------------------------------------------


def _extract_task_id(execute_result: Any) -> str | None:
    if isinstance(execute_result, dict):
        return execute_result.get("task_id") or execute_result.get("message_id")
    return None


def _extract_session_id(execute_result: Any) -> str:
    """Pull ``session_id`` out of an ``agent.execute`` result payload."""
    if isinstance(execute_result, dict):
        return execute_result.get("session_id") or ""
    return ""


def _extract_final(payload: Any) -> bool:
    """从通知 payload 中提取 final 标记。

    守设计文档 §3.2：``metadata.final`` 是唯一真相源（string "true"/"false"）。
    daemon ``build_response_notification`` 同时注入顶层 ``final``（bool）作历史遗留，
    但 Bridge 应只信任 ``metadata.final``。详见 streaming-response.md §3.2 历史遗留说明。
    """
    meta_val = _extract_metadata_value(payload, "final", "")
    if meta_val == "true":
        return True
    if meta_val == "false":
        return False
    # 降级：如果 metadata.final 不存在，读顶层 final（兼容老 daemon）
    if isinstance(payload, dict):
        return bool(payload.get("final", False))
    return False


def _extract_metadata_value(payload: Any, key: str, default: str = "") -> str:
    """Extract a value from a daemon notification's metadata dict."""
    if isinstance(payload, dict):
        meta = payload.get("metadata")
        if isinstance(meta, dict):
            val = meta.get(key)
            if val is not None:
                return str(val)
        elif isinstance(meta, str):
            try:
                parsed = json.loads(meta)
                if isinstance(parsed, dict):
                    val = parsed.get(key)
                    if val is not None:
                        return str(val)
            except json.JSONDecodeError:
                pass
    return default


def _extract_content(payload: Any) -> str:
    if isinstance(payload, dict):
        content = payload.get("content") or payload.get("text") or ""
        return str(content)
    return ""


def publicize_artifact_urls(content: str) -> str:
    return _shared_publicize_artifact_urls(content)


# --------------------------------------------------------------------------
# Shared turn-event generator (used by both synchronous and SSE paths)
# --------------------------------------------------------------------------

_TurnEvent = dict[str, Any]
"""Event dict shapes yielded by ``_chat_turn_events``:

* ``{"type": "chunk", "content": str, "final": bool, "task_id": str | None}`` — a content chunk. ``task_id`` is present on the first chunk, ``None`` thereafter.
* ``{"type": "error", "message": str, "code": int}`` — a non-recoverable error.
* ``{"type": "done", "final": true}`` — terminal signal.
"""


async def _chat_turn_events(
    *,
    daemon_client: DaemonClient,
    agent_id: str,
    user_id: str,
    client_id: str,
    content: str | list[ContentPart],
    dialogue_id: str,
    turn_timeout: float,
    history_messages: list[dict] | None = None,
    stream: bool = False,
) -> AsyncIterator[_TurnEvent]:
    """Execute one agent turn and yield raw event dicts.

    The caller decides whether to accumulate (synchronous mode) or
    serialize to SSE frames (streaming mode).

    Yields:
        ``chunk`` events for each ``agent.response`` notification,
        then a final ``done`` event. The first ``chunk`` carries
        ``task_id``; subsequent chunks have ``task_id=None``.
        On error yields ``error`` followed by ``done``.

    ``history_messages`` is the prior dialogue context as a list of
    ``{"role": "user"|"assistant", "content": "..."}`` dicts. When
    supplied, the daemon receives a JSON-stringified ``messages`` array
    (history + this turn's user input) per the Bridge↔daemon contract:
    the Bridge owns context assembly, the daemon forwards it verbatim
    to the node, and the node parses it as the LLM input. When ``None``
    or empty, the daemon receives the plain text content (backward
    compatible with single-turn callers).
    """
    # Serialize content for daemon. Large inline base64 images are
    # landed to disk and replaced with file_url here. The result is
    # the text-only representation of THIS turn's user input.
    this_turn_text = prepare_content_for_daemon(content, agent_id=agent_id,
                                                 user_id=user_id, client_id=client_id)

    # Assemble the daemon payload per the messages-list contract:
    #   {stream, messages: history + [{"role":"user","content": this_turn_text}]}
    # JSON-stringify the whole object so it travels through the daemon's
    # single `content` string field. Nodes parse the object to extract
    # both the messages array and the stream flag.
    # I-DERIV（03-bridge-session-persistence.md §1.3）：历史 user 消息存的是原始
    # content（含 base64 真相源），拼 daemon messages 前消费时派生路径文本。
    if history_messages:
        history_messages = derive_history_user_messages(
            history_messages, agent_id=agent_id,
            user_id=user_id, client_id=client_id,
        )
    if history_messages or stream:
        messages_payload = [*history_messages, {"role": "user", "content": this_turn_text}] if history_messages else [{"role": "user", "content": this_turn_text}]
        daemon_content = json.dumps({
            "stream": stream,
            "messages": messages_payload,
        }, ensure_ascii=False)
    else:
        # Single-turn, no history: still use JSON protocol for consistency
        daemon_content = json.dumps({
            "stream": False,
            "messages": [{"role": "user", "content": this_turn_text}],
        }, ensure_ascii=False)

    cancel_state = _active_dialogues.get((user_id, client_id))
    if cancel_state is None or cancel_state.dialogue_id != dialogue_id:
        # Sync REST turns are not stored in the SSE registry, but use the
        # same idempotent cancellation path when an execute or wait times out.
        cancel_state = _ActiveDialogue(
            agent_id=agent_id,
            dialogue_id=dialogue_id,
            user_id=user_id,
            client_id=client_id,
            daemon_client=daemon_client,
        )

    # --- Step 1: execute --------------------------------------------------
    daemon_exec_start = time.time()
    try:
        execute_result = await asyncio.wait_for(
            daemon_client.agent_execute(
                agent_id,
                session_id="",
                content=daemon_content,
                user_id=user_id,
                client_id=client_id,
                dialogue_id=dialogue_id,
                stream=stream,
            ),
            timeout=turn_timeout,
        )
    except asyncio.TimeoutError:
        logger.info(
            "chat_turn.daemon_execute agent=%s dialogue=%s elapsed=%.3fs status=timeout",
            agent_id, dialogue_id, time.time() - daemon_exec_start,
        )
        await _cancel_dialogue(cancel_state)
        daemon_client.discard_response_queue(dialogue_id)
        logger.warning(
            "chat_turn.timeout agent=%s dialogue=%s execute_timeout=%ss",
            agent_id, dialogue_id, turn_timeout,
        )
        yield {"type": "error", "message": f"agent.execute timed out after {turn_timeout}s", "code": -32000}
        yield {"type": "done", "final": True}
        return
    except DaemonError as exc:
        logger.info(
            "chat_turn.daemon_execute agent=%s dialogue=%s elapsed=%.3fs status=daemon_error",
            agent_id, dialogue_id, time.time() - daemon_exec_start,
        )
        daemon_client.discard_response_queue(dialogue_id)
        yield {"type": "error", "message": str(exc), "code": getattr(exc, "code", -32000)}
        yield {"type": "done", "final": True}
        return

    logger.info(
        "chat_turn.daemon_execute agent=%s dialogue=%s elapsed=%.3fs status=ok",
        agent_id, dialogue_id, time.time() - daemon_exec_start,
    )

    task_id = _extract_task_id(execute_result)
    if not task_id:
        daemon_client.discard_response_queue(dialogue_id)
        yield {"type": "error", "message": "agent.execute did not return a task_id", "code": -32000}
        yield {"type": "done", "final": True}
        return

    # Emit session_id as metadata so the caller (SSE streaming path)
    # can register the dialogue for cancellation support.
    session_id = _extract_session_id(execute_result)
    if session_id:
        cancel_state.session_id = session_id
        yield {"type": "_session_info", "session_id": session_id}

    # --- Step 2: wait for response notifications --------------------------
    deadline = asyncio.get_running_loop().time() + turn_timeout
    chunk_count = 0
    turn_start = time.time()
    ttfb_logged = False

    while True:
        remaining = deadline - asyncio.get_running_loop().time()
        if remaining <= 0:
            logger.warning(
                "chat_turn.timeout agent=%s dialogue=%s task=%s timeout=%ss elapsed=%.1fs",
                agent_id, dialogue_id, task_id, turn_timeout, time.time() - turn_start,
            )
            await _cancel_dialogue(cancel_state)
            yield {"type": "error", "message": f"turn timed out after {turn_timeout}s", "code": -32000}
            daemon_client.discard_response_queue(dialogue_id)
            yield {"type": "done", "final": True}
            return

        try:
            wait_once_start = time.time()
            notification = await asyncio.wait_for(
                daemon_client.wait_for_response(
                    agent_id, dialogue_id=dialogue_id, timeout=remaining,
                ),
                timeout=remaining,
            )
            if logger.isEnabledFor(logging.DEBUG):
                logger.debug(
                    "chat_turn.wait_once agent=%s dialogue=%s task=%s elapsed=%.3fs",
                    agent_id, dialogue_id, task_id, time.time() - wait_once_start,
                )
        except asyncio.TimeoutError:
            elapsed = time.time() - turn_start
            logger.warning(
                "chat_turn.wait_timeout agent=%s dialogue=%s task=%s "
                "remaining=%.1fs elapsed=%.1fs chunks=%d connected=%s",
                agent_id, dialogue_id, task_id,
                remaining, elapsed, chunk_count,
                daemon_client.is_connected,
            )
            await _cancel_dialogue(cancel_state)
            yield {"type": "error", "message": f"turn timed out after {turn_timeout}s", "code": -32000}
            daemon_client.discard_response_queue(dialogue_id)
            yield {"type": "done", "final": True}
            return
        except DaemonError as exc:
            elapsed = time.time() - turn_start
            logger.error(
                "chat_turn.error agent=%s dialogue=%s task=%s "
                "elapsed=%.1fs chunks=%d error=%s",
                agent_id, dialogue_id, task_id,
                elapsed, chunk_count, exc,
            )
            yield {"type": "error", "message": f"daemon error: {exc}", "code": -32000}
            daemon_client.discard_response_queue(dialogue_id)
            yield {"type": "done", "final": True}
            return

        if not notification:
            continue

        is_final = _extract_final(notification)
        chunk_content = publicize_artifact_urls(_extract_content(notification))

        if not ttfb_logged and chunk_content:
            logger.info(
                "chat_turn.ttfb agent=%s dialogue=%s task=%s elapsed=%.3fs",
                agent_id, dialogue_id, task_id, time.time() - turn_start,
            )
            ttfb_logged = True
        msg_type = _extract_metadata_value(notification, "type", "")
        # daemon 推送的 progress 通知仅设 metadata["final"]="false"，
        # 未设 metadata["type"]="progress"。
        # 当 type 为空且非终态时，判定为 progress。
        is_progress = (msg_type == "progress") or (not msg_type and not is_final)

        chunk_count += 1

        # Structured artifact metadata from the chunk content.
        artifacts = extract_artifact_infos(chunk_content) if is_final else []
        # 产物输出模式转换（artifact_reference / artifact_payload 两维正交开关）：
        # 仅 final chunk 应用（产物 URL 只出现在最终完整文本）；默认 link+url 时无转换，
        # 与历史行为完全一致（I-3 三路一致：SSE / 同步 / 历史回放共用同一转换后结果）。
        # 持久化保留替换前的原始 content（含真实产物路径）——placeholder/base64 模式下
        # 占位符会丢掉真实路径，LLM 下一轮历史将看不到产物路径 → 误判未生成 → 重复生成
        # （2026-08-17 复盘：生成视频轮重复生成效果图）。对外展示用替换后 content，
        # 持久化（save_dialogue）用替换前 raw content。
        raw_content = chunk_content
        if is_final:
            chunk_content, artifacts = apply_artifact_output_mode(chunk_content, artifacts)

        # task_id is attached to the first chunk only, so callers can
        # track the daemon task without scanning every frame.
        # 技能执行元数据（Python 节点 response_push 传入）
        event: dict[str, Any] = {
            "type": "chunk",
            "content": chunk_content,
            # 持久化专用：替换前的原始 content（含真实产物路径），供 save_dialogue 落库，
            # LLM 下一轮历史可见真实路径（I-1 不丢数据；2026-08-17 复盘修复）。
            # 仅 final chunk 有意义；非 final 恒为空串（同步/SSE 累积只认 final）。
            "raw_content": raw_content if is_final else "",
            "final": is_final,
            "progress": is_progress,
            "chunk_type": msg_type if msg_type else None,
            "artifacts": [a.model_dump() for a in artifacts] if artifacts else None,
            "task_id": task_id if chunk_count == 1 else None,
        }
        for meta_key in ("_source", "_skill", "_round", "_status"):
            meta_val = _extract_metadata_value(notification, meta_key.lstrip("_"))
            if meta_val:
                event[meta_key] = meta_val
        yield event

        if is_final:
            break

    elapsed = time.time() - turn_start
    logger.info(
        "chat_turn.complete agent=%s dialogue=%s task=%s elapsed=%.1fs chunks=%d",
        agent_id, dialogue_id, task_id, elapsed, chunk_count,
    )
    yield {"type": "done", "final": True}


# --------------------------------------------------------------------------
# SSE format helpers — OpenAI-compatible data-only SSE
# --------------------------------------------------------------------------


def _sse_data_json(data: Any) -> str:
    """Format an SSE ``data:`` frame with JSON payload."""
    return f"data: {json.dumps(data, ensure_ascii=False)}\n\n"


def _extract_token_for_persist(content: str, chunk_type: str | None) -> str:
    """剥壳 ``llm_chunk`` 的 raw OpenAI chunk JSON 取 ``delta.content`` token，
    供累积持久化 buffer（``sse_chunks`` / ``accumulated_content``）使用。

    协议改动后（streaming-response.md §3.4），节点推的 ``llm_chunk.content`` 是
    原始 OpenAI SSE chunk JSON 整串（``{"choices":[{"delta":{"content":"..."}}]}``），
    而非剥壳后的 token。bridge SSE 路径原样透传给前端没问题（前端剥壳渲染），
    但 bridge 内部累积 buffer（用于 ``provider.save_dialogue`` 持久化 + 前端刷新恢复）
    **必须剥壳**——否则 ``full_content`` 是 raw JSON 拼接，刷新后前端 Markdown 渲染崩坏。

    非 ``llm_chunk`` chunk（text/progress）原样返回，不受影响。
    解析失败时降级原样返回（守 I-1 不丢数据，节点契约偏离时不阻断流）。
    """
    if chunk_type != "llm_chunk" or not content:
        return content
    try:
        parsed = json.loads(content)
        delta = parsed.get("choices", [{}])[0].get("delta", {}) if parsed.get("choices") else {}
        token = delta.get("content", "")
        return token or ""
    except (json.JSONDecodeError, TypeError, IndexError):
        # 节点契约违反：content 不是合法 OpenAI JSON。降级原样追加守 I-1。
        logger.warning(
            "persist.llm_chunk_invalid_json content_len=%d (降级原样追加)",
            len(content),
        )
        return content


def _content_to_text(content: str | list[ContentPart]) -> str:
    """Flatten a chat request's ``content`` field to plain text.

    The SessionProvider stores the user input as a single string per
    dialogue round, so multimodal content arrays are reduced to their
    text parts. Image parts are represented by their ``name`` (filename)
    so the dialogue record stays human-readable without losing the fact
    that an image was sent.
    """
    if isinstance(content, str):
        return content
    parts: list[str] = []
    for p in content:
        # TextPart has a ``text`` field; ImagePart has ``image_url``.
        text = getattr(p, "text", None)
        if text:
            parts.append(text)
            continue
        # ImagePart / file part: record a placeholder so the persisted
        # dialogue entry does not silently drop the multimodal context.
        # ImagePart has image_url.url (not a name field), so extract the
        # filename from the URL path or use a short hash of the URL.
        url = getattr(p, "image_url", None)
        url_str = getattr(url, "url", "") if url else ""
        if url_str:
            # Extract the filename from the URL path, or truncate for
            # base64 data URLs.
            if url_str.startswith("data:"):
                # Base64 inline: use a short prefix from the MIME type
                name = url_str.split(";")[0].replace("data:", "")[:20]
            else:
                name = url_str.rstrip("/").split("/")[-1] or "image"
        else:
            name = "image"
        parts.append(f"[image:{name}]")
    return "".join(parts)


def _content_images(content: str | list[ContentPart]) -> str:
    """从聊天请求 content 中提取用户附加图片的 data URI 列表（JSON 编码）。

    仅收集 ``data:`` 前缀的内联图片（blob URL / 文件路径在会话历史中无法恢复，
    落库无意义）；纯文本消息返回 ``"[]"``。供 save_dialogue 持久化用户图片。
    """
    if isinstance(content, str):
        return "[]"
    images: list[str] = []
    for p in content:
        url = getattr(p, "image_url", None)
        url_str = getattr(url, "url", "") if url else ""
        if url_str.startswith("data:"):
            images.append(url_str)
    return json.dumps(images, ensure_ascii=False)


# Transport EOF only.  Business success requires a preceding payload with
# ``_final=true``; error streams carry an explicit negative terminal status.
_SSE_DONE = "data: [DONE]\n\n"


# --------------------------------------------------------------------------
# Endpoint: POST /api/chat/{agent_id}?stream=true|false
# --------------------------------------------------------------------------


@router.post(
    "/{agent_id}",
    status_code=status.HTTP_200_OK,
    summary="Execute a single agent turn (synchronous or SSE streaming).",
)
async def chat_rest(
    agent_id: str,
    body: ChatRestRequest,
    client: DaemonClient = Depends(get_daemon_client),  # noqa: B008
    provider: SessionProvider = Depends(get_session_provider),  # noqa: B008
    stream: bool = Query(False, description="If true, returns SSE stream instead of a single JSON response."),
) -> Any:
    """Execute a single agent turn.

    Two modes:

    **Synchronous mode** (default, ``?stream=false``):
    Returns the complete response as a single JSON body wrapped in the
    standard ``ApiResponse`` envelope.

    **SSE streaming mode** (``?stream=true``):
    Returns a ``text/event-stream`` response compatible with the OpenAI
    Chat Completions SSE format (``data:`` lines only, ``[DONE]`` termination).

    SSE data payloads:

    * Content chunk::

        data: {"choices":[{"delta":{"content":"..."},"index":0}]}

    * Terminal signal::

        data: [DONE]

    * Error (before terminal)::

        data: {"error":{"message":"...","code":-32000}}

    **Request body** (JSON):

    * ``user_id`` — user identifier (required).
    * ``client_id`` — client/session identifier (required).
    * ``content`` — the user message (required).
    """
    dialogue_id = (
        f"{datetime.now(timezone.utc).strftime('%m%d%H%M%S')}"
        f"-{uuid.uuid4().hex[:12]}"
    )
    active = _ActiveDialogue(
        agent_id=agent_id,
        dialogue_id=dialogue_id,
        user_id=body.user_id,
        client_id=body.client_id,
        daemon_client=client,
    )
    if not _register_dialogue(active):
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail="a dialogue is already active for this user and client",
        )
    turn_timeout = settings.chat_turn_timeout_seconds

    logger.info(
        "chat_turn.start agent=%s user=%s client=%s dialogue=%s content_len=%d stream=%s ts=%.3f",
        agent_id, body.user_id, body.client_id, dialogue_id, len(str(body.content)), stream, time.time(),
    )

    # ── 上下文注入（设计文档 §2.4 / §3.1 Step 2-3）─────────────────
    # 对话前从 SessionProvider 取历史上下文，拼成 messages 数组传给 daemon。
    # 协议：Bridge 拼完整 messages 数组（历史 + 本次输入）→ JSON 字符串化
    # → 作为 content 传给 daemon → daemon 原样转发给 node → node 自行解析。
    # I-5: 历史上下文拉取失败不应阻塞对话主流程，静默降级为无历史。
    history_messages: list[dict] = []
    bsession: dict | None = None
    context_load_start = time.time()
    try:
        bsession = await provider.get_or_create_session(body.user_id, body.client_id)
        ctx = await provider.get_context(
            bsession["id"], body.user_id,
            query=_content_to_text(body.content),
            limit_rounds=settings.session_context_limit_rounds,
        )
        history_messages = ctx.messages
        logger.info(
            "chat_turn.context agent=%s session=%s history_msgs=%d memories=%d",
            agent_id, bsession["id"], len(history_messages), len(ctx.memories),
        )
        # 长期记忆（仅 KMMSessionProvider 会返回）拼为 system 角色消息前置。
        if ctx.memories:
            memory_text = "用户相关信息：\n" + "\n".join(ctx.memories)
            history_messages = [
                {"role": "system", "content": memory_text},
                *history_messages,
            ]
    except Exception as exc:  # noqa: BLE101
        logger.warning(
            "chat_turn.context_failed agent=%s dialogue=%s: %s (proceeding without history)",
            agent_id, dialogue_id, exc,
        )
    logger.info(
        "chat_turn.context_load agent=%s dialogue=%s elapsed=%.3fs",
        agent_id, dialogue_id, time.time() - context_load_start,
    )

    event_gen = _chat_turn_events(
        daemon_client=client,
        agent_id=agent_id,
        user_id=body.user_id,
        client_id=body.client_id,
        content=body.content,
        dialogue_id=dialogue_id,
        turn_timeout=turn_timeout,
        history_messages=history_messages or None,
        stream=stream,
    )

    if not stream:
        # --- Synchronous mode: accumulate, return a single JSON response ----
        start_ts = time.time()
        accumulated_content: list[str] = []
        public_content: list[str] = []
        # Collect artifacts from the final chunk (only chunk with artifacts)
        accumulated_artifacts: list[dict] = []
        task_id: str | None = None
        agentflow_session_id: str = ""
        try:
            async for event in event_gen:
                if event["type"] == "_session_info":
                    # Capture the agentflow session_id for trace pulling.
                    sid = event.get("session_id") or ""
                    if sid:
                        agentflow_session_id = sid
                    continue
                if event["type"] == "chunk":
                    # task_id is attached to the first chunk only.
                    if event.get("task_id") is not None:
                        task_id = event["task_id"]
                    # 守不变量 I-1（final=true 的 chunk = 一个对话一个最终响应）：
                    # 同步路径只保留 final chunk，非 final chunk 全丢弃；同时分开保存
                    # 含真实产物路径的 raw_content（会话持久化）与输出模式转换后的
                    # content（HTTP 客户端），避免 placeholder/base64 模式退回原始 URL。
                    # 默认 link+url 模式下两者内容相同。
                    # 非 final chunk（progress / skill 思辨 / llm_token）的过程展示
                    # 仅在 SSE 流式路径透传给前端，被 final 一到即覆盖。
                    if event.get("final"):
                        # 持久化用替换前 raw_content（含真实产物路径），LLM 下一轮历史
                        # 可见路径避免重复生成（2026-08-17 复盘修复）；对外展示仍用 content。
                        accumulated_content = [
                            _extract_token_for_persist(
                                event.get("raw_content") or event["content"],
                                event.get("chunk_type"),
                            )
                        ]
                        public_content = [
                            _extract_token_for_persist(
                                event["content"],
                                event.get("chunk_type"),
                            )
                        ]
                        # artifacts on the final chunk only
                        event_arts = event.get("artifacts")
                        if event_arts:
                            accumulated_artifacts = event_arts
                    # 非 final chunk：同步路径不累积（SSE 路径透传作过程展示）
                elif event["type"] == "error":
                    # Generator has already cleaned up the response queue;
                    # do not double-discard here (single source of truth).
                    raise HTTPException(
                        status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
                        detail=event["message"],
                    )
                # "done" is the terminal signal — just break
        except HTTPException:
            _unregister_dialogue(body.user_id, body.client_id, dialogue_id)
            raise
        except Exception as exc:
            logger.exception("chat_turn.unexpected agent=%s: %s", agent_id, exc)
            client.discard_response_queue(dialogue_id)
            _unregister_dialogue(body.user_id, body.client_id, dialogue_id)
            raise HTTPException(
                status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
                detail=str(exc),
            ) from exc

        full_content = "".join(accumulated_content)
        client_content = "".join(public_content)
        elapsed = time.time() - start_ts
        logger.info(
            "chat_turn.complete agent=%s user=%s dialogue=%s elapsed=%.1fs content_len=%d",
            agent_id, body.user_id, dialogue_id, elapsed, len(client_content),
        )

        # Persist to the legacy in-memory conversation store so any caller
        # still on the old API keeps working until the frontend migrates.
        get_store().save(ConversationRecord(
            agent_id=agent_id,
            user_id=body.user_id,
            client_id=body.client_id,
            dialogue_id=dialogue_id,
            content=full_content,
        ))

        # Persist to the durable SessionProvider (Bridge session). I-5:
        # wrap in try/except so a persistence failure never breaks the
        # chat main flow. bsession was resolved during context injection
        # above; re-resolve only if that step failed (bsession is None).
        try:
            sess = bsession if bsession is not None else await provider.get_or_create_session(
                body.user_id, body.client_id,
            )
            await provider.update_agentflow_session_ref(sess["id"], agentflow_session_id)
            # 持久化使用转换后产物元数据（含 placeholder/data_base64，I-3 三路一致）；
            # 默认模式（link+url）下 accumulated_artifacts 即 extract 结果，行为与历史一致。
            artifacts_json = (
                json.dumps(accumulated_artifacts, ensure_ascii=False)
                if accumulated_artifacts else serialize_artifacts(full_content)
            )
            # Audit:产物清单即将落盘。extract_artifact_infos 已记 urls，此处补数量 + dialogue 关联
            logger.info(
                "chat_turn.persist_artifacts agent=%s dialogue=%s artifacts=%d json_len=%d",
                agent_id, dialogue_id,
                len(json.loads(artifacts_json)) if artifacts_json and artifacts_json != "[]" else 0,
                len(artifacts_json),
            )
            await provider.save_dialogue(
                session_id=sess["id"],
                user_input=serialize_content_for_storage(body.content),
                agent_response=full_content,
                dialogue_id=dialogue_id,
                agent_name=agent_id,
                artifacts=artifacts_json,
            )
            # Pull LLM traces asynchronously: the user has already
            # received their answer by the time we return, so we permit
            # the trace pull to run before responding. It is silent on
            # failure (see services/trace_puller.py).
            # We pass (user_id, client_id) because the daemon's
            # agent.execute does not return session_id; the daemon
            # resolves it internally via resolve_session_id().
            logger.info(
                "rest_chat.trace_pull agent=%s dialogue=%s user=%s client=%s sess=%s",
                agent_id, dialogue_id, body.user_id, body.client_id, sess["id"],
            )
            await _pull_and_save_traces(
                client, provider,
                agent_id=agent_id,
                user_id=body.user_id,
                client_id=body.client_id,
                dialogue_id=dialogue_id,
                session_id=sess["id"],
            )
        except Exception as exc:  # noqa: BLE101
            logger.warning(
                "chat_turn.persist_failed agent=%s dialogue=%s: %s",
                agent_id, dialogue_id, exc,
            )

        response_data = ChatRestResponse(
            task_id=task_id,
            content=client_content,
            artifacts=accumulated_artifacts,
        )
        response_payload = _ok(response_data)
        append_bridge_delivery(
            agent_package_dir=settings.agent_packages_path / agent_id,
            dialogue_id=dialogue_id,
            agent_id=agent_id,
            user_id=body.user_id,
            client_id=body.client_id,
            task_id=task_id or "",
            transport="http_json",
            stage="response_prepared",
            status="final",
            raw_content=full_content,
            delivered_content=client_content,
            wire_payload=_ok(response_data.model_dump(mode="json")),
            artifacts=accumulated_artifacts,
        )
        _unregister_dialogue(body.user_id, body.client_id, dialogue_id)
        return response_payload

    # --- SSE streaming mode ------------------------------------------------
    def _save_conversation(chunks: list[str]) -> None:
        """Persist completed conversation to the legacy in-memory store."""
        get_store().save(ConversationRecord(
            agent_id=agent_id, user_id=body.user_id,
            client_id=body.client_id, dialogue_id=dialogue_id,
            content="".join(chunks),
        ))

    async def _persist_to_provider(full_content: str, artifacts: list | None = None) -> None:
        """Persist the completed dialogue to the durable SessionProvider.

        Runs the Bridge-session save + trace pull. I-5: silent on failure
        so a persistence outage never breaks the chat main flow. The
        user has already received their SSE stream by the time this runs.

        ``bsession`` was resolved during the context-injection step before
        the SSE stream opened; re-resolve only if that step failed.

        ``artifacts`` 为 final chunk 的转换后产物元数据（含 placeholder/data_base64，
        I-3 三路一致）；为 None 时回退 ``serialize_artifacts(full_content)``。
        """
        try:
            sess = bsession if bsession is not None else await provider.get_or_create_session(
                body.user_id, body.client_id,
            )
            await provider.update_agentflow_session_ref(sess["id"], active.session_id)
            # 持久化使用转换后产物元数据（含 placeholder/data_base64，I-3 三路一致）；
            # 默认模式（link+url）下 artifacts 即 extract 结果，行为与历史一致。
            artifacts_json = (
                json.dumps(artifacts, ensure_ascii=False)
                if artifacts else serialize_artifacts(full_content)
            )
            # Audit:产物清单即将落盘。extract_artifact_infos 已记 urls，此处补数量 + dialogue 关联
            logger.info(
                "sse_chat.persist_artifacts agent=%s dialogue=%s artifacts=%d json_len=%d",
                agent_id, dialogue_id,
                len(json.loads(artifacts_json)) if artifacts_json and artifacts_json != "[]" else 0,
                len(artifacts_json),
            )
            await provider.save_dialogue(
                session_id=sess["id"],
                user_input=serialize_content_for_storage(body.content),
                agent_response=full_content,
                dialogue_id=dialogue_id,
                agent_name=agent_id,
                artifacts=artifacts_json,
                images=_content_images(body.content),
            )
            logger.info(
                "sse_chat.trace_pull agent=%s dialogue=%s user=%s client=%s sess=%s",
                agent_id, dialogue_id, body.user_id, body.client_id, sess["id"],
            )
            await _pull_and_save_traces(
                client, provider,
                agent_id=agent_id,
                user_id=body.user_id,
                client_id=body.client_id,
                dialogue_id=dialogue_id,
                session_id=sess["id"],
            )
        except Exception as exc:  # noqa: BLE101
            logger.warning(
                "chat_sse.persist_failed agent=%s dialogue=%s: %s",
                agent_id, dialogue_id, exc,
            )


    sse_dump_fh: typing.IO | None = None
    if _SSE_DUMP_DIR:
        try:
            os.makedirs(_SSE_DUMP_DIR, exist_ok=True)
            dump_name = f"{datetime.now(timezone.utc).strftime('%Y%m%d_%H%M%S')}_{dialogue_id}.jsonl"
            sse_dump_fh = open(os.path.join(_SSE_DUMP_DIR, dump_name), "w", encoding="utf-8")
            sse_dump_fh.write(json.dumps({
                "_meta": True,
                "timestamp": datetime.now(timezone.utc).isoformat(),
                "agent_id": agent_id,
                "user_id": body.user_id,
                "client_id": body.client_id,
                "dialogue_id": dialogue_id,
                "request": _sanitize_debug_payload(body.model_dump()),
            }, ensure_ascii=False, default=str) + "\n")
            sse_dump_fh.flush()
            logger.info("sse_dump.start dialogue=%s file=%s", dialogue_id, dump_name)
        except Exception as exc:
            logger.warning("sse_dump.init_failed dialogue=%s: %s", dialogue_id, exc)
            sse_dump_fh = None

    async def _sse_stream() -> AsyncIterator[str]:
        sse_chunks: list[str] = []
        stream_start = time.time()
        sse_chunk_count = 0
        stream_task_id = ""
        try:
            async for event in event_gen:
                if event["type"] == "_session_info":
                    # Capture session_id from daemon and update the registry.
                    sid = event["session_id"]
                    if sid:
                        active.session_id = sid
                    continue  # internal event, not sent to client

                if event["type"] == "chunk":
                    if event.get("task_id") is not None:
                        stream_task_id = str(event["task_id"])
                    chunk_type = event.get("chunk_type")
                    is_llm_chunk = (chunk_type == "llm_chunk")
                    # ── llm_chunk: 节点推的是原始 OpenAI SSE chunk JSON，原样透传 ──
                    # 设计文档 streaming-response.md §3.4：节点透传 LLM 返回的原始 chunk，
                    # bridge 不再二次包装 OpenAI 信封，前端自行解析 delta.content / delta.tool_calls。
                    if is_llm_chunk:
                        try:
                            raw_chunk = json.loads(event["content"])
                        except (json.JSONDecodeError, TypeError):
                            # 节点契约违反：content 不是合法 JSON。降级为空 chunk 不阻断流。
                            logger.warning(
                                "sse_stream.llm_chunk_invalid_json agent=%s dialogue=%s content_len=%d",
                                agent_id, dialogue_id, len(event["content"] or ""),
                            )
                            raw_chunk = {}
                        payload: dict[str, Any] = raw_chunk if raw_chunk else {}
                    else:
                        # ── text / progress: 包装 OpenAI 同步响应信封 ──
                        # 守 §3.2 核心原则 #2：text/progress 用 choices[0].message.content
                        # （同步响应格式，完整内容），区别于 llm_chunk 的 delta.content。
                        choice: dict[str, Any] = {
                            "message": {"content": event["content"]},
                            "index": 0,
                        }
                        payload: dict[str, Any] = {"choices": [choice]}
                    # 首个 chunk 携带 dialogue_id，供前端后续 trace 查询按 dialogue 过滤
                    if len(sse_chunks) == 0:
                        payload["_dialogue_id"] = dialogue_id
                    # 携带结构化 artifact 元数据，供前端 ArtifactList 渲染（非标准字段）
                    event_artifacts = event.get("artifacts")
                    if event_artifacts:
                        payload["_artifacts"] = event_artifacts
                    # 进度消息标记（技能执行期间）
                    if event.get("progress"):
                        payload["_progress"] = True
                    # 最终回复标记（metadata.final 唯一真相源 → 透传给下游，
                    # sidecar/前端据此区分"过程"与"最终回复"，避免完整块重复渲染）
                    if event.get("final"):
                        payload["_final"] = True
                    # chunk 类型（progress / llm_chunk / text）— 前端据此分流
                    if chunk_type:
                        payload["_chunk_type"] = chunk_type
                    # 技能执行元数据（来自 response_push 的 source/skill/round/status）
                    for meta_key in ("_source", "_skill", "_round", "_status"):
                        meta_val = event.get(meta_key)
                        if meta_val:
                            payload[meta_key] = meta_val
                    wire_frame = _sse_data_json(payload)
                    yield wire_frame
                    _capture_sse_payload(payload)
                    if sse_dump_fh:
                        try:
                            sse_dump_fh.write(json.dumps(_sanitize_debug_payload(payload), ensure_ascii=False, default=str) + "\n")
                            sse_dump_fh.flush()
                        except Exception:
                            pass
                    # 累积到 sse_chunks 供首 chunk 检测（L887）及数据库持久化。
                    # 守不变量 I-1：仅 final chunk 的内容入库，非 final chunk 仅作 SSE 流式展示。
                    # 与同步路径（Fix 2, L690-695）逻辑一致。
                    sse_chunks.append(
                        _extract_token_for_persist(
                            event.get("raw_content") or event["content"],
                            event.get("chunk_type"),
                        )
                    )
                    sse_chunk_count += 1
                    if event["final"]:
                        append_bridge_delivery(
                            agent_package_dir=settings.agent_packages_path / agent_id,
                            dialogue_id=dialogue_id,
                            agent_id=agent_id,
                            user_id=body.user_id,
                            client_id=body.client_id,
                            task_id=stream_task_id,
                            transport="sse",
                            stage="server_yielded",
                            status="final",
                            raw_content=_extract_token_for_persist(
                                event.get("raw_content") or event["content"],
                                event.get("chunk_type"),
                            ),
                            delivered_content=_extract_token_for_persist(
                                event["content"], event.get("chunk_type"),
                            ),
                            wire_payload={"frame": wire_frame, "data": payload},
                            artifacts=event.get("artifacts"),
                        )
                        # 仅 final chunk 的内容入库（非 final 的 progress / llm_token 仅 SSE 展示）
                        # 持久化用替换前 raw_content（含真实产物路径），LLM 下一轮历史可见
                        # 路径避免重复生成（2026-08-17 复盘修复）；对外展示仍用 content。
                        final_content = [_extract_token_for_persist(
                            event.get("raw_content") or event["content"],
                            event.get("chunk_type"),
                        )]
                        logger.info(
                            "chat_sse.stream agent=%s dialogue=%s elapsed=%.3fs chunks=%d",
                            agent_id, dialogue_id, time.time() - stream_start, sse_chunk_count,
                        )
                        yield _SSE_DONE
                        append_bridge_delivery(
                            agent_package_dir=settings.agent_packages_path / agent_id,
                            dialogue_id=dialogue_id,
                            agent_id=agent_id,
                            user_id=body.user_id,
                            client_id=body.client_id,
                            task_id=stream_task_id,
                            transport="sse",
                            stage="server_stream_completed",
                            status="done",
                            raw_content="",
                            delivered_content="",
                            wire_payload=_SSE_DONE,
                        )
                        _save_conversation(final_content)
                        await _persist_to_provider(
                            "".join(final_content), event.get("artifacts"),
                        )
                        return
                elif event["type"] == "error":
                    err_payload = {
                        "error": {
                            "message": event["message"],
                            "code": event.get("code", -32000),
                        },
                        "_dialogue_id": dialogue_id,
                        "_final": False,
                        "_terminal_status": "error",
                    }
                    logger.info(
                        "chat_sse.stream agent=%s dialogue=%s elapsed=%.3fs chunks=%d status=error",
                        agent_id, dialogue_id, time.time() - stream_start, sse_chunk_count,
                    )
                    error_frame = _sse_data_json(err_payload)
                    yield error_frame
                    append_bridge_delivery(
                        agent_package_dir=settings.agent_packages_path / agent_id,
                        dialogue_id=dialogue_id,
                        agent_id=agent_id,
                        user_id=body.user_id,
                        client_id=body.client_id,
                        task_id=stream_task_id,
                        transport="sse",
                        stage="server_yielded",
                        status="error",
                        raw_content=event["message"],
                        delivered_content=event["message"],
                        wire_payload={"frame": error_frame, "data": err_payload},
                    )
                    if sse_dump_fh:
                        try:
                            sse_dump_fh.write(json.dumps(_sanitize_debug_payload(err_payload), ensure_ascii=False, default=str) + "\n")
                            sse_dump_fh.flush()
                        except Exception:
                            pass
                    yield _SSE_DONE
                    append_bridge_delivery(
                        agent_package_dir=settings.agent_packages_path / agent_id,
                        dialogue_id=dialogue_id,
                        agent_id=agent_id,
                        user_id=body.user_id,
                        client_id=body.client_id,
                        task_id=stream_task_id,
                        transport="sse",
                        stage="server_stream_completed",
                        status="done_after_error",
                        raw_content="",
                        delivered_content="",
                        wire_payload=_SSE_DONE,
                    )
                    # Progress remains observable in SSE/client.delivery, but an
                    # incomplete turn must not become assistant conversation history.
                    return
                elif event["type"] == "done":
                    logger.info(
                        "chat_sse.stream agent=%s dialogue=%s elapsed=%.3fs chunks=%d status=done",
                        agent_id, dialogue_id, time.time() - stream_start, sse_chunk_count,
                    )
                    yield _SSE_DONE
                    append_bridge_delivery(
                        agent_package_dir=settings.agent_packages_path / agent_id,
                        dialogue_id=dialogue_id,
                        agent_id=agent_id,
                        user_id=body.user_id,
                        client_id=body.client_id,
                        task_id=stream_task_id,
                        transport="sse",
                        stage="server_stream_completed",
                        status="done",
                        raw_content="",
                        delivered_content="",
                        wire_payload=_SSE_DONE,
                    )
                    _save_conversation(sse_chunks)
                    await _persist_to_provider("".join(sse_chunks))
                    return
        except (asyncio.CancelledError, GeneratorExit):
            # Client disconnected (incl. client-initiated close) before
            # dialogue completed.  Send DIALOGUE-INTERRUPT to release
            # the session serialization lock so queued dialogues can run.
            logger.info(
                "sse_stream.disconnect agent=%s dialogue=%s (auto-cancelling)",
                agent_id, dialogue_id,
            )
            logger.info(
                "chat_sse.stream agent=%s dialogue=%s elapsed=%.3fs chunks=%d status=disconnect",
                agent_id, dialogue_id, time.time() - stream_start, sse_chunk_count,
            )
            await _cancel_dialogue(active)
            raise
        finally:
            _unregister_dialogue(active.user_id, active.client_id, active.dialogue_id)
            if sse_dump_fh:
                try:
                    sse_dump_fh.close()
                except Exception:
                    pass

    return StreamingResponse(
        _sse_stream(),
        media_type="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no",
            "Connection": "keep-alive",
        },
    )


__all__ = ["router"]
