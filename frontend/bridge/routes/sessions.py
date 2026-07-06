"""AgentFlow Bridge — session proxy routes.

Pure HTTP-to-JSON-RPC proxy in front of :class:`~client.DaemonClient`.
Sessions are owned by agents on the daemon side, so every endpoint
requires the parent ``agent_id``. There is intentionally no caching,
no in-memory session table and no business logic: the bridge forwards
the call and packages the daemon's payload into the standard
:class:`~models.ApiResponse` envelope.

The router also exposes the **Bridge-side persistent session** API
(`GET /api/sessions/persistent`, ``GET /api/sessions/persistent/{id}/...``)
backed by :class:`~providers.SessionProvider`. Those endpoints are
independent of the daemon and survive restarts. See
``docs/design/features/bridge-session-persistence.md``.
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
from session_dependencies import get_session_provider
from providers import SessionProvider

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
    """``GET /api/sessions?agent_id=X[&user_id=Y]`` — list sessions for an agent (``session.list``)."""
    try:
        result = await client.session_list(agent_id, user_id or "")
        logger.info("session_mgmt.list agent=%s user=%s ok", agent_id, user_id)
        return _ok(result)
    except DaemonError as exc:
        logger.warning("session_mgmt.list agent=%s user=%s error=%s", agent_id, user_id, exc)
        raise _raise_daemon_error(exc) from exc


@router.delete("/{session_id}", name="destroy_session")
async def destroy_session(
    session_id: str,
    agent_id: str = Query(..., description="Parent agent id owning the session."),
    client: DaemonClient = Depends(get_daemon_client),
) -> dict[str, Any]:
    """``DELETE /api/sessions/{id}?agent_id=X`` — destroy a session (``session.destroy``)."""
    try:
        result = await client.session_destroy(agent_id, session_id)
        logger.info("session_mgmt.destroy agent=%s session=%s ok", agent_id, session_id)
        return _ok(result)
    except DaemonError as exc:
        logger.warning("session_mgmt.destroy agent=%s session=%s error=%s", agent_id, session_id, exc)
        raise _raise_daemon_error(exc) from exc


@router.delete("/{session_id}/dialogue", name="clear_session_dialogue")
async def clear_session_dialogue(
    session_id: str,
    agent_id: str = Query(..., description="Parent agent id owning the session."),
    dialogue_id: str = Query("", description="Optional dialogue id; empty clears all."),
    client: DaemonClient = Depends(get_daemon_client),
) -> dict[str, Any]:
    """``DELETE /api/sessions/{id}/dialogue?agent_id=X&dialogue_id=Y`` — clear dialogue."""
    try:
        result = await client.session_clear_dialogue(agent_id, session_id, dialogue_id)
        logger.info("session_mgmt.clear_dialogue agent=%s session=%s dialogue=%s ok", agent_id, session_id, dialogue_id or "(all)")
        return _ok(result)
    except DaemonError as exc:
        logger.warning("session_mgmt.clear_dialogue agent=%s session=%s error=%s", agent_id, session_id, exc)
        raise _raise_daemon_error(exc) from exc


# ---------------------------------------------------------------------------
# Bridge-side persistent sessions (independent of the daemon)
# ---------------------------------------------------------------------------
# These routes read from the configured SessionProvider so the frontend can
# list past conversations across restarts. Distinct path prefix to avoid
# clashing with the daemon-proxy routes above (`/api/sessions/{id}` etc).


@router.get("/persistent", name="list_persistent_sessions")
async def list_persistent_sessions(
    user_id: str = Query("", description="Optional: filter sessions by user_id. Empty = all users."),
    limit: int = Query(20, ge=1, le=200, description="Max sessions to return."),
    offset: int = Query(0, ge=0, description="Pagination offset."),
    provider: SessionProvider = Depends(get_session_provider),  # noqa: B008
) -> dict[str, Any]:
    """``GET /api/sessions/persistent`` — list Bridge-side persistent sessions.

    Returns the most recent ``limit`` sessions newest-first. Each entry
    contains ``id, user_id, client_id, title, created_at, last_active_at,
    status``.
    """
    sessions = await provider.list_sessions(user_id=user_id, limit=limit, offset=offset)
    return _ok({"sessions": sessions, "total": len(sessions)})


@router.get("/persistent/{session_id}", name="get_persistent_session")
async def get_persistent_session(
    session_id: str,
    provider: SessionProvider = Depends(get_session_provider),  # noqa: B008
) -> dict[str, Any]:
    """``GET /api/sessions/persistent/{id}`` — get one persistent session by id."""
    sess = await provider.get_session(session_id)
    if sess is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Persistent session not found: {session_id}",
        )
    return _ok(sess)


@router.get("/persistent/{session_id}/messages", name="list_persistent_messages")
async def list_persistent_messages(
    session_id: str,
    limit: int = Query(50, ge=1, le=500, description="Max messages to return."),
    provider: SessionProvider = Depends(get_session_provider),  # noqa: B008
) -> dict[str, Any]:
    """``GET /api/sessions/persistent/{id}/messages`` — list historical messages.

    Returns messages oldest-first within the returned window. Each entry
    is ``{role, content, dialogue_id, artifacts, created_at}`` and one
    dialogue round yields two messages (user + assistant).
    """
    sess = await provider.get_session(session_id)
    if sess is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Persistent session not found: {session_id}",
        )
    messages = await provider.get_messages(session_id, limit=limit)
    return _ok({"messages": messages, "total": len(messages)})


@router.get("/persistent/{session_id}/traces", name="list_persistent_traces")
async def list_persistent_traces(
    session_id: str,
    dialogue_id: str = Query("", description="Optional: filter traces by dialogue_id."),
    provider: SessionProvider = Depends(get_session_provider),  # noqa: B008
) -> dict[str, Any]:
    """``GET /api/sessions/persistent/{id}/traces[?dialogue_id=Y]`` — list traces."""
    sess = await provider.get_session(session_id)
    if sess is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Persistent session not found: {session_id}",
        )
    traces = await provider.get_traces(session_id, dialogue_id=dialogue_id)
    return _ok({"traces": traces, "total": len(traces)})


@router.delete("/persistent/{session_id}", name="delete_persistent_session")
async def delete_persistent_session(
    session_id: str,
    provider: SessionProvider = Depends(get_session_provider),  # noqa: B008
) -> dict[str, Any]:
    """``DELETE /api/sessions/persistent/{id}`` — physically delete a persistent
    session and all its dialogues/traces from the provider.

    Unlike ``PATCH ?status=deleted`` (soft-delete), this irreversibly removes
    all associated data. Returns 404 if the session does not exist.
    """
    deleted = await provider.delete_session(session_id)
    if not deleted:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Persistent session not found: {session_id}",
        )
    logger.info("persistent_session.deleted id=%s", session_id)
    return _ok({"deleted": True})


@router.patch("/persistent/{session_id}", name="update_persistent_session")
async def update_persistent_session(
    session_id: str,
    new_status: str | None = Query(None, alias="status", description="New status: active|archived."),
    title: str | None = Query(None, description="New title."),
    provider: SessionProvider = Depends(get_session_provider),  # noqa: B008
) -> dict[str, Any]:
    """``PATCH /api/sessions/persistent/{id}?status=Y&title=Z`` — update session metadata."""
    sess = await provider.get_session(session_id)
    if sess is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Persistent session not found: {session_id}",
        )
    if new_status is not None:
        await provider.update_session_status(session_id, new_status)
    if title is not None:
        await provider.update_session_title(session_id, title)
    updated = await provider.get_session(session_id)
    return _ok(updated)


@router.delete("/persistent/{session_id}", name="delete_persistent_session")
async def delete_persistent_session(
    session_id: str,
    provider: SessionProvider = Depends(get_session_provider),  # noqa: B008
) -> dict[str, Any]:
    """``DELETE /api/sessions/persistent/{id}`` — hard-delete a persistent session
    and all its associated traces and dialogue data."""
    sess = await provider.get_session(session_id)
    if sess is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Persistent session not found: {session_id}",
        )
    await provider.delete_session(session_id)
    logger.info("persistent_session.deleted id=%s", session_id)
    return _ok({"deleted": session_id})


__all__ = ["router"]
