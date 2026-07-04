"""AgentFlow Bridge — REST chat endpoint.

Provides both synchronous (request-response) and streaming (SSE) REST
alternatives to the WebSocket streaming chat in ``chat.py``.

Uses ``user_id`` + ``client_id`` (not ``session_id``) to identify
the caller, matching the daemon's ``SessionRouter`` design.
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
import uuid
from collections.abc import AsyncIterator
from datetime import datetime, timezone
from typing import Any

from fastapi import APIRouter, Depends, HTTPException, Query, status
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
from routes.conversations import get_store, ConversationRecord

logger = logging.getLogger("agentflow.bridge.routes.chat_rest")

router = APIRouter(prefix="/api/chat", tags=["chat"])


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


def _extract_final(payload: Any) -> bool:
    if isinstance(payload, dict):
        return bool(payload.get("final", False))
    return False


def _extract_content(payload: Any) -> str:
    if isinstance(payload, dict):
        content = payload.get("content") or payload.get("text") or ""
        return str(content)
    return ""


def _publicize_artifact_urls(content: str) -> str:
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
) -> AsyncIterator[_TurnEvent]:
    """Execute one agent turn and yield raw event dicts.

    The caller decides whether to accumulate (synchronous mode) or
    serialize to SSE frames (streaming mode).

    Yields:
        ``chunk`` events for each ``agent.response`` notification,
        then a final ``done`` event. The first ``chunk`` carries
        ``task_id``; subsequent chunks have ``task_id=None``.
        On error yields ``error`` followed by ``done``.
    """
    # Serialize content for daemon. Large inline base64 images are
    # landed to disk and replaced with file_url here.
    daemon_content = prepare_content_for_daemon(content, agent_id=agent_id)

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
        chunk_content = _publicize_artifact_urls(_extract_content(notification))
        chunk_count += 1

        # task_id is attached to the first chunk only, so callers can
        # track the daemon task without scanning every frame.
        yield {
            "type": "chunk",
            "content": chunk_content,
            "final": is_final,
            "task_id": task_id if chunk_count == 1 else None,
        }

        if is_final:
            break

    elapsed = time.time() - turn_start
    logger.info(
        "chat_turn.complete agent=%s dialogue=%s task=%s elapsed=%.1fs chunks=%d",
        agent_id, dialogue_id, task_id, elapsed, chunk_count,
    )
    yield {"type": "done", "final": True}


# --------------------------------------------------------------------------
# SSE format helpers
# --------------------------------------------------------------------------


def _sse_format(event: str, data: Any) -> str:
    """Format an SSE frame."""
    return f"event: {event}\ndata: {json.dumps(data, ensure_ascii=False)}\n\n"


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
    stream: bool = Query(False, description="If true, returns SSE stream instead of a single JSON response."),
) -> Any:
    """Execute a single agent turn.

    Two modes:

    **Synchronous mode** (default, ``?stream=false``):
    Returns the complete response as a single JSON body wrapped in the
    standard ``ApiResponse`` envelope.

    **SSE streaming mode** (``?stream=true``):
    Returns a ``text/event-stream`` response.  Each ``agent.response``
    notification from the daemon is pushed as a separate SSE event.

    SSE event types:

    * ``event: message`` — a content chunk::

        event: message
        data: {"content": "...", "final": false}

    * ``event: done`` — terminal signal::

        event: done
        data: {"final": true}

    * ``event: error`` — a non-recoverable error (will be followed by ``done``)::

        event: error
        data: {"message": "...", "code": -32000}

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

    event_gen = _chat_turn_events(
        daemon_client=client,
        agent_id=agent_id,
        user_id=body.user_id,
        client_id=body.client_id,
        content=body.content,
        dialogue_id=dialogue_id,
        turn_timeout=turn_timeout,
    )

    if not stream:
        # --- Synchronous mode: accumulate, return a single JSON response ----
        start_ts = time.time()
        accumulated_content: list[str] = []
        task_id: str | None = None
        try:
            async for event in event_gen:
                if event["type"] == "chunk":
                    accumulated_content.append(event["content"])
                    # task_id is attached to the first chunk only.
                    if event.get("task_id") is not None:
                        task_id = event["task_id"]
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

        # Persist to conversation store so the frontend can list past
        # conversations independently of the daemon's dialogue lifecycle.
        get_store().save(ConversationRecord(
            agent_id=agent_id,
            user_id=body.user_id,
            client_id=body.client_id,
            dialogue_id=dialogue_id,
            content=full_content,
        ))

        return _ok(ChatRestResponse(task_id=task_id, content=full_content))

    # --- SSE streaming mode ------------------------------------------------
    def _save_conversation(chunks: list[str]) -> None:
        """Persist completed conversation to the store."""
        get_store().save(ConversationRecord(
            agent_id=agent_id, user_id=body.user_id,
            client_id=body.client_id, dialogue_id=dialogue_id,
            content="".join(chunks),
        ))

    async def _sse_stream() -> AsyncIterator[str]:
        sse_chunks: list[str] = []
        async for event in event_gen:
            if event["type"] == "chunk":
                # task_id is present on the first chunk only; emit it
                # so SSE clients can track the daemon task.
                payload: dict[str, Any] = {
                    "content": event["content"],
                    "final": event["final"],
                }
                if event.get("task_id") is not None:
                    payload["task_id"] = event["task_id"]
                yield _sse_format("message", payload)
                sse_chunks.append(event["content"])
                if event["final"]:
                    yield _sse_format("done", {"final": True})
                    _save_conversation(sse_chunks)
                    return
            elif event["type"] == "error":
                yield _sse_format("error", {
                    "message": event["message"],
                    "code": event.get("code", -32000),
                })
                yield _sse_format("done", {"final": True})
                _save_conversation(sse_chunks)
                return
            elif event["type"] == "done":
                yield _sse_format("done", {"final": True})
                _save_conversation(sse_chunks)
                return

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
