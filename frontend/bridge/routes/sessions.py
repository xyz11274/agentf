"""AgentFlow Bridge — session proxy routes.

Pure HTTP-to-JSON-RPC proxy in front of :class:`~client.DaemonClient`.
Sessions are owned by agents on the daemon side, so every endpoint
requires the parent ``agent_id``. There is intentionally no caching,
no in-memory session table and no business logic: the bridge forwards
the call and packages the daemon's payload into the standard
:class:`~models.ApiResponse` envelope.
"""

from __future__ import annotations

import logging
from typing import Any

from fastapi import APIRouter, Depends, HTTPException, Query, status

from client import (
    DaemonClient,
    DaemonConnectionError,
    DaemonError,
    DaemonTimeoutError,
    InvalidParamsError,
    NotFoundError,
)
from dependencies import get_daemon_client

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/api/sessions", tags=["sessions"])


def _ok(data: Any) -> dict[str, Any]:
    """Wrap a daemon payload in the bridge's standard success envelope."""
    return {"success": True, "data": data, "error": None}


def _raise_daemon_error(exc: DaemonError) -> "HTTPException":
    """Translate a :class:`DaemonError` to the corresponding HTTP status.

    Mirrors the mapping in :mod:`bridge.routes.agents` and is locked by
    the same bridge contract.
    """
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
        "daemon proxy error: %s -> HTTP %s", exc, code, exc_info=logger.isEnabledFor(logging.DEBUG)
    )
    return HTTPException(status_code=code, detail=str(exc))


@router.get("", name="list_sessions")
async def list_sessions(
    agent_id: str = Query(..., description="Parent agent id (sessions are owned by an agent)."),
    user_id: str | None = Query(None, description="Optional: filter sessions by user_id."),
    client: DaemonClient = Depends(get_daemon_client),
) -> dict[str, Any]:
    """``GET /api/sessions?agent_id=X[&user_id=Y]`` — list sessions for an agent (``session.list``).

    If ``user_id`` is provided, only sessions matching that user are returned.
    """
    try:
        return _ok(await client.session_list(agent_id, user_id or ""))
    except DaemonError as exc:
        raise _raise_daemon_error(exc) from exc


@router.delete("/{session_id}", name="destroy_session")
async def destroy_session(
    session_id: str,
    agent_id: str = Query(..., description="Parent agent id owning the session."),
    client: DaemonClient = Depends(get_daemon_client),
) -> dict[str, Any]:
    """``DELETE /api/sessions/{id}?agent_id=X`` — destroy a session (``session.destroy``)."""
    try:
        return _ok(await client.session_destroy(agent_id, session_id))
    except DaemonError as exc:
        raise _raise_daemon_error(exc) from exc


@router.delete("/{session_id}/dialogue", name="clear_session_dialogue")
async def clear_session_dialogue(
    session_id: str,
    agent_id: str = Query(..., description="Parent agent id owning the session."),
    dialogue_id: str = Query("", description="Optional dialogue id; empty clears all."),
    client: DaemonClient = Depends(get_daemon_client),
) -> dict[str, Any]:
    """``DELETE /api/sessions/{id}/dialogue?agent_id=X&dialogue_id=Y`` — clear dialogue (``session.clear_dialogue``)."""
    try:
        return _ok(await client.session_clear_dialogue(agent_id, session_id, dialogue_id))
    except DaemonError as exc:
        raise _raise_daemon_error(exc) from exc


__all__ = ["router"]
