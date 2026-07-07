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
import time
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
from content_serializer import prepare_content_for_daemon
from dependencies import get_daemon_client
from models import ContentPart
from providers import SessionProvider
from routes.artifact_extractor import extract_artifact_infos, publicize_artifact_urls, serialize_artifacts
from routes.conversations import get_store, ConversationRecord
from services import _pull_and_save_traces
from session_dependencies import get_session_provider

logger = logging.getLogger("agentflow.bridge.routes.chat_rest")

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


def _register_dialogue(d: _ActiveDialogue) -> None:
    _active_dialogues[_registry_key(d)] = d


def _unregister_dialogue(user_id: str, client_id: str) -> None:
    _active_dialogues.pop((user_id, client_id), None)


async def _cancel_dialogue(d: _ActiveDialogue) -> None:
    """Send DIALOGUE-INTERRUPT to the daemon for this dialogue.

    Idempotent: subsequent calls for the same dialogue are no-ops.
    """
    if d.cancelled:
        return
    d.cancelled = True
    session_id = d.session_id
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
    except Exception as exc:
        logger.warning(
            "cancel_dialogue.error agent=%s dialogue=%s err=%s",
            d.agent_id, d.dialogue_id, exc,
        )


# Cancel request schema
class CancelRequest(BaseModel):
    """Request body for POST /api/chat/{agent_id}/cancel."""
    user_id: str = Field(..., description="User identifier (matches the active SSE stream).")
    client_id: str = Field(..., description="Client/session identifier (matches the active SSE stream).")


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

    user_id: str = Field(..., description="User identifier.")
    client_id: str = Field(..., description="Client/session identifier.")
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
    base = settings.artifact_public_base_url.rstrip("/")
    if not base:
        return content
    return content.replace("(/api/artifacts/", f"({base}/api/artifacts/")


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
    this_turn_text = prepare_content_for_daemon(content, agent_id=agent_id)

    # Assemble the daemon payload per the messages-list contract:
    #   {stream, messages: history + [{"role":"user","content": this_turn_text}]}
    # JSON-stringify the whole object so it travels through the daemon's
    # single `content` string field. Nodes parse the object to extract
    # both the messages array and the stream flag.
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

    # --- Step 1: execute --------------------------------------------------
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
        daemon_client.discard_response_queue(dialogue_id)
        logger.warning(
            "chat_turn.timeout agent=%s dialogue=%s execute_timeout=%ss",
            agent_id, dialogue_id, turn_timeout,
        )
        yield {"type": "error", "message": f"agent.execute timed out after {turn_timeout}s", "code": -32000}
        yield {"type": "done", "final": True}
        return
    except DaemonError as exc:
        daemon_client.discard_response_queue(dialogue_id)
        yield {"type": "error", "message": str(exc), "code": getattr(exc, "code", -32000)}
        yield {"type": "done", "final": True}
        return

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
        yield {"type": "_session_info", "session_id": session_id}

    # --- Step 2: wait for response notifications --------------------------
    deadline = asyncio.get_running_loop().time() + turn_timeout
    chunk_count = 0
    turn_start = time.time()

    while True:
        remaining = deadline - asyncio.get_running_loop().time()
        if remaining <= 0:
            logger.warning(
                "chat_turn.timeout agent=%s dialogue=%s task=%s timeout=%ss elapsed=%.1fs",
                agent_id, dialogue_id, task_id, turn_timeout, time.time() - turn_start,
            )
            yield {"type": "error", "message": f"turn timed out after {turn_timeout}s", "code": -32000}
            daemon_client.discard_response_queue(dialogue_id)
            yield {"type": "done", "final": True}
            return

        try:
            notification = await asyncio.wait_for(
                daemon_client.wait_for_response(
                    agent_id, dialogue_id=dialogue_id, timeout=remaining,
                ),
                timeout=remaining,
            )
        except asyncio.TimeoutError:
            logger.warning(
                "chat_turn.wait_timeout agent=%s dialogue=%s remaining=%.1fs",
                agent_id, dialogue_id, remaining,
            )
            yield {"type": "error", "message": f"turn timed out after {turn_timeout}s", "code": -32000}
            daemon_client.discard_response_queue(dialogue_id)
            yield {"type": "done", "final": True}
            return
        except DaemonError as exc:
            logger.error(
                "chat_turn.error agent=%s dialogue=%s task=%s error=%s",
                agent_id, dialogue_id, task_id, exc,
            )
            yield {"type": "error", "message": f"daemon error: {exc}", "code": -32000}
            daemon_client.discard_response_queue(dialogue_id)
            yield {"type": "done", "final": True}
            return

        if not notification:
            continue

        is_final = _extract_final(notification)
        chunk_content = publicize_artifact_urls(_extract_content(notification))
        msg_type = _extract_metadata_value(notification, "type", "")
        is_progress = (msg_type == "progress")

        chunk_count += 1

        # Structured artifact metadata from the chunk content.
        artifacts = extract_artifact_infos(chunk_content) if is_final else []

        # task_id is attached to the first chunk only, so callers can
        # track the daemon task without scanning every frame.
        # 技能执行元数据（Python 节点 response_push 传入）
        event: dict[str, Any] = {
            "type": "chunk",
            "content": chunk_content,
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
    turn_timeout = settings.chat_turn_timeout_seconds

    logger.info(
        "chat_turn.start agent=%s user=%s client=%s dialogue=%s content_len=%d stream=%s",
        agent_id, body.user_id, body.client_id, dialogue_id, len(str(body.content)), stream,
    )

    # ── 上下文注入（设计文档 §2.4 / §3.1 Step 2-3）─────────────────
    # 对话前从 SessionProvider 取历史上下文，拼成 messages 数组传给 daemon。
    # 协议：Bridge 拼完整 messages 数组（历史 + 本次输入）→ JSON 字符串化
    # → 作为 content 传给 daemon → daemon 原样转发给 node → node 自行解析。
    # I-5: 历史上下文拉取失败不应阻塞对话主流程，静默降级为无历史。
    history_messages: list[dict] = []
    bsession: dict | None = None
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
                    # 同步路径只保留 final chunk 作为 full_content，非 final chunk 全丢弃。
                    # 这样持久化到 SessionProvider 的 agent_response、返回给前端的 content、
                    # 前端刷新加载历史的渲染 三者完全一致——都是 coordinator 的唯一最终回答。
                    # 非 final chunk（progress / skill 思辨 / llm_token）的过程展示
                    # 仅在 SSE 流式路径透传给前端，被 final 一到即覆盖。
                    if event.get("final"):
                        accumulated_content = [
                            _extract_token_for_persist(
                                event["content"], event.get("chunk_type"),
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
            raise
        except Exception as exc:
            logger.exception("chat_turn.unexpected agent=%s: %s", agent_id, exc)
            client.discard_response_queue(dialogue_id)
            raise HTTPException(
                status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
                detail=str(exc),
            ) from exc

        full_content = "".join(accumulated_content)
        elapsed = time.time() - start_ts
        logger.info(
            "chat_turn.complete agent=%s user=%s dialogue=%s elapsed=%.1fs content_len=%d",
            agent_id, body.user_id, dialogue_id, elapsed, len(full_content),
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
            artifacts_json = serialize_artifacts(full_content)
            await provider.save_dialogue(
                session_id=sess["id"],
                user_input=_content_to_text(body.content),
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

        return _ok(ChatRestResponse(
            task_id=task_id,
            content=full_content,
            artifacts=accumulated_artifacts,
        ))

    # --- SSE streaming mode ------------------------------------------------
    def _save_conversation(chunks: list[str]) -> None:
        """Persist completed conversation to the legacy in-memory store."""
        get_store().save(ConversationRecord(
            agent_id=agent_id, user_id=body.user_id,
            client_id=body.client_id, dialogue_id=dialogue_id,
            content="".join(chunks),
        ))

    async def _persist_to_provider(full_content: str) -> None:
        """Persist the completed dialogue to the durable SessionProvider.

        Runs the Bridge-session save + trace pull. I-5: silent on failure
        so a persistence outage never breaks the chat main flow. The
        user has already received their SSE stream by the time this runs.

        ``bsession`` was resolved during the context-injection step before
        the SSE stream opened; re-resolve only if that step failed.
        """
        try:
            sess = bsession if bsession is not None else await provider.get_or_create_session(
                body.user_id, body.client_id,
            )
            await provider.update_agentflow_session_ref(sess["id"], active.session_id)
            artifacts_json = serialize_artifacts(full_content)
            await provider.save_dialogue(
                session_id=sess["id"],
                user_input=_content_to_text(body.content),
                agent_response=full_content,
                dialogue_id=dialogue_id,
                agent_name=agent_id,
                artifacts=artifacts_json,
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

    # Register the active dialogue so the cancel endpoint can find it.
    active = _ActiveDialogue(
        agent_id=agent_id,
        dialogue_id=dialogue_id,
        user_id=body.user_id,
        client_id=body.client_id,
        daemon_client=client,
    )
    _register_dialogue(active)

    async def _sse_stream() -> AsyncIterator[str]:
        sse_chunks: list[str] = []
        try:
            async for event in event_gen:
                if event["type"] == "_session_info":
                    # Capture session_id from daemon and update the registry.
                    sid = event["session_id"]
                    if sid:
                        active.session_id = sid
                    continue  # internal event, not sent to client

                if event["type"] == "chunk":
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
                    # chunk 类型（progress / llm_chunk / text）— 前端据此分流
                    if chunk_type:
                        payload["_chunk_type"] = chunk_type
                    # 技能执行元数据（来自 response_push 的 source/skill/round/status）
                    for meta_key in ("_source", "_skill", "_round", "_status"):
                        meta_val = event.get(meta_key)
                        if meta_val:
                            payload[meta_key] = meta_val
                    yield _sse_data_json(payload)
                    # 累积到 sse_chunks 供首 chunk 检测（L887）及数据库持久化。
                    # 守不变量 I-1：仅 final chunk 的内容入库，非 final chunk 仅作 SSE 流式展示。
                    # 与同步路径（Fix 2, L690-695）逻辑一致。
                    sse_chunks.append(
                        _extract_token_for_persist(event["content"], event.get("chunk_type"))
                    )
                    if event["final"]:
                        # 仅 final chunk 的内容入库（非 final 的 progress / llm_token 仅 SSE 展示）
                        final_content = [_extract_token_for_persist(
                            event["content"], event.get("chunk_type"),
                        )]
                        yield _SSE_DONE
                        _save_conversation(final_content)
                        await _persist_to_provider("".join(final_content))
                        return
                elif event["type"] == "error":
                    yield _sse_data_json({
                        "error": {
                            "message": event["message"],
                            "code": event.get("code", -32000),
                        },
                    })
                    yield _SSE_DONE
                    _save_conversation(sse_chunks)
                    await _persist_to_provider("".join(sse_chunks))
                    return
                elif event["type"] == "done":
                    yield _SSE_DONE
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
            await _cancel_dialogue(active)
            raise
        finally:
            _unregister_dialogue(active.user_id, active.client_id)

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
