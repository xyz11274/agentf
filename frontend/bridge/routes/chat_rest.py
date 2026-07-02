"""AgentFlow Bridge — REST chat endpoint.

Provides a synchronous (request-response) REST alternative to the
WebSocket streaming chat in ``chat.py``. Useful for programmatic
consumers that do not need streaming or persistent connections.

Uses ``user_id`` + ``client_id`` (not ``session_id``) to identify
the caller, matching the daemon's ``SessionRouter`` design.
"""

from __future__ import annotations

import asyncio
import logging
import uuid
from datetime import datetime, timezone
from typing import Any

from fastapi import APIRouter, Depends, HTTPException, status
from pydantic import BaseModel, Field

from client import (
    DaemonClient,
    DaemonConnectionError,
    DaemonError,
    DaemonTimeoutError,
    InvalidParamsError,
    NotFoundError,
)
from dependencies import get_daemon_client

logger = logging.getLogger("agentflow.bridge.routes.chat_rest")

router = APIRouter(prefix="/aisf/healthagent/api/chat", tags=["chat"])

_TURN_TIMEOUT_SECONDS = 120.0


class ChatRestRequest(BaseModel):
    """REST chat request body."""

    model_config = {"from_attributes": True}

    user_id: str = Field(..., description="User identifier.")
    client_id: str = Field(..., description="Client/session identifier.")
    content: str = Field(..., description="User message content.")


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


# --------------------------------------------------------------------------
# Endpoint
# --------------------------------------------------------------------------


@router.post(
    "/{agent_id}",
    status_code=status.HTTP_200_OK,
    summary="Execute a single agent turn and return the complete response (REST).",
)
async def chat_rest(
    agent_id: str,
    body: ChatRestRequest,
    client: DaemonClient = Depends(get_daemon_client),
) -> dict[str, Any]:
    """Execute a single agent turn and return the complete response.

    This is the REST equivalent of the WebSocket streaming endpoint at
    ``/ws/chat/{agent_id}``.

    **Request body** (JSON):

    * ``user_id`` — user identifier (required).
    * ``client_id`` — client/session identifier (required).
    * ``content`` — the user message (required).

    **Response** (``200 OK`` with ``ApiResponse`` envelope):

    .. code-block:: json

        {
          "success": true,
          "data": {
            "task_id": "task_...",
            "content": "..."
          },
          "error": null
        }
    """
    dialogue_id = (
        f"{datetime.now(timezone.utc).strftime('%m%d%H%M%S')}"
        f"-{uuid.uuid4().hex[:12]}"
    )

    # --- Step 1: execute --------------------------------------------------
    try:
        execute_result = await asyncio.wait_for(
            client.agent_execute(
                agent_id,
                session_id="",
                content=body.content,
                user_id=body.user_id,
                client_id=body.client_id,
                dialogue_id=dialogue_id,
            ),
            timeout=_TURN_TIMEOUT_SECONDS,
        )
    except asyncio.TimeoutError:
        raise HTTPException(
            status_code=status.HTTP_504_GATEWAY_TIMEOUT,
            detail=f"agent.execute timed out after {_TURN_TIMEOUT_SECONDS}s",
        )
    except DaemonError as exc:
        raise _raise_daemon_error(exc) from exc

    task_id = _extract_task_id(execute_result)

    if not task_id:
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail="agent.execute did not return a task_id",
        )

    # --- Step 2: wait for response notifications --------------------------
    accumulated_content: list[str] = []
    deadline = asyncio.get_running_loop().time() + _TURN_TIMEOUT_SECONDS

    while True:
        remaining = deadline - asyncio.get_running_loop().time()
        if remaining <= 0:
            logger.warning(
                "chat rest turn timed out after %ss for agent=%s",
                _TURN_TIMEOUT_SECONDS,
                agent_id,
            )
            break

        try:
            notification = await asyncio.wait_for(
                client.wait_for_response(agent_id, timeout=remaining),
                timeout=remaining,
            )
        except (asyncio.TimeoutError, DaemonError):
            logger.warning(
                "chat rest wait_for_response timed out after %.1fs for agent=%s",
                remaining,
                agent_id,
            )
            break

        if not notification:
            continue

        is_final = _extract_final(notification)
        chunk_content = _extract_content(notification)
        if chunk_content:
            accumulated_content.append(chunk_content)

        if is_final:
            break

    full_content = "".join(accumulated_content)

    return _ok(
        ChatRestResponse(
            task_id=task_id,
            content=full_content,
        )
    )


__all__ = ["router"]
