"""AgentFlow Bridge — LLM Trace query routes (daemon RPC proxy).

These endpoints implement the Phase-2 design of ADR-0004: the Bridge queries
LLM execution traces through the daemon's ``databus.get`` / ``databus.list_keys``
JSON-RPC methods, avoiding any direct DataBus UDS dependency.

This is the **only** data path — there is no fallback to UDS direct access.

See Also
--------
* L4 module design: ``frontend/bridge/docs/design/architecture/modules/llm-trace.md``
* ADR-0004: ``frontend/bridge/docs/design/adrs/0004-llm-trace-data-bus-bridge.md``
* Enhancement 041: ``docs/enhancements/041-feature-daemon-databus-rpc-proxy-260625-wip.md``
"""

from __future__ import annotations

import asyncio
import logging
from typing import Any, Optional

from fastapi import APIRouter, Depends, HTTPException, Query, Request, status

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

router = APIRouter(prefix="/aisf/healthagent/api/agents", tags=["llm-traces"])


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------


def _build_trace_summary(
    call_id: str,
    meta: Optional[dict],
) -> dict[str, Any]:
    """Build the summary dict for a single trace entry."""
    if meta is None:
        meta = {}
    return {
        "call_id": call_id,
        "model": meta.get("model", ""),
        "latency_ms": meta.get("latency_ms", 0),
        "request_summary": meta.get("request_summary", ""),
        "response_summary": meta.get("response_summary", ""),
        "timestamp": meta.get("timestamp", ""),
        "token_usage": {
            "prompt_tokens": meta.get("prompt_tokens", 0),
            "completion_tokens": meta.get("completion_tokens", 0),
            "total_tokens": meta.get("total_tokens", 0),
        },
    }


def _raise_daemon_error(exc: DaemonError, agent_id: str, user_id: str) -> "HTTPException":
    """Map a daemon RPC error to the correct HTTP status."""
    if isinstance(exc, InvalidParamsError):
        raise HTTPException(status_code=400, detail={
            "success": False, "data": None,
            "error": {"code": -32602, "message": str(exc)},
        })
    if isinstance(exc, NotFoundError):
        raise HTTPException(status_code=404, detail={
            "success": False, "data": None,
            "error": {"code": -32001, "message": str(exc)},
        })
    if isinstance(exc, DaemonTimeoutError):
        raise HTTPException(status_code=504, detail={
            "success": False, "data": None,
            "error": {"code": -32003, "message": "daemon timeout: " + str(exc)},
        })
    # DaemonConnectionError or generic DaemonError
    raise HTTPException(status_code=502, detail={
        "success": False, "data": None,
        "error": {"code": -32003, "message": str(exc)},
    })


async def _resolve_trace_session(
    daemon: "DaemonClient",
    agent_id: str,
    user_id: str,
    client_id: str,
) -> dict[str, str]:
    """Resolve session identity for trace queries.

    Tries (user_id + client_id) first; if the daemon cannot map them to a
    session (e.g. session expired/cleaned but trace data still in DataBus),
    falls back to session_id="0" (default session where traces are stored
    when no explicit session is created).

    Returns params dict with either user_id+client_id or session_id set.
    """
    base = {"agent_id": agent_id, "user_id": user_id, "client_id": client_id}
    try:
        # Probe: try a trivial list_keys to see if route resolves
        await daemon.send_request("databus.list_keys", {**base, "prefix": "llm/"})
        return base
    except (InvalidParamsError, NotFoundError):
        # Session not found via route key — fall back to default session 0
        return {"agent_id": agent_id, "session_id": "0"}


# ---------------------------------------------------------------------------
# Endpoints
# ---------------------------------------------------------------------------


@router.get("/{agent_id}/llm-traces")
async def list_llm_traces(
    agent_id: str,
    user_id: str = Query(..., description="User ID (required)."),
    client_id: str = Query(..., description="Client ID (required)."),
    dialogue_id: Optional[str] = Query(None, description="Dialogue ID (optional)."),
    limit: int = Query(50, ge=1, le=200, description="Max traces to return."),
    offset: int = Query(0, ge=0, description="Pagination offset."),
    daemon: DaemonClient = Depends(get_daemon_client),
) -> dict[str, Any]:
    """Return a paginated list of LLM trace summaries for a session.

    The endpoint queries the daemon's ``databus.list_keys`` to enumerate
    all ``meta`` keys under ``llm/traces/``, then fetches each ``meta``
    sub-key via ``databus.get``.

    User identity is resolved via ``(user_id, client_id)`` — the daemon
    internally maps these to a session_id via its SessionRouter.

    The ``agent_id`` path parameter is passed through to the daemon so it
    can route to the correct ``MMDataBus`` instance.
    """
    _ = dialogue_id  # reserved

    # Resolve session identity (with fallback to default session 0)
    sess_params = await _resolve_trace_session(daemon, agent_id, user_id, client_id)

    # Step 1: list all keys under the traces namespace.
    prefix = "llm/traces/"
    try:
        list_result = await daemon.send_request("databus.list_keys", {
            **sess_params,
            "prefix": prefix,
        })
    except DaemonError as exc:
        _raise_daemon_error(exc, agent_id, user_id)

    raw_keys: list[str] = list_result.get("keys", [])

    # Step 2: filter to only "/meta" keys, extract call_id.
    meta_suffix = "/meta"
    call_ids: list[str] = []
    for k in raw_keys:
        if k.endswith(meta_suffix):
            # Key format:  llm/traces/{call_id}/meta
            call_id = k[len(prefix):-len(meta_suffix)]
            call_ids.append(call_id)

    # Step 3: paginate.
    total = len(call_ids)
    page = call_ids[offset: offset + limit]

    # Step 4: fetch meta for each call_id.
    traces: list[dict[str, Any]] = []
    for call_id in page:
        meta_key = f"{prefix}{call_id}/meta"
        try:
            get_result = await daemon.send_request("databus.get", {
                **sess_params,
                "key": meta_key,
            })
        except DaemonError:
            meta = None
        else:
            meta = get_result.get("value") if get_result.get("exists") else None
        traces.append(_build_trace_summary(call_id, meta))

    return {
        "success": True,
        "data": {"traces": traces, "total": total},
        "error": None,
    }


@router.get("/{agent_id}/llm-traces/{call_id}")
async def get_llm_trace_detail(
    agent_id: str,
    call_id: str,
    user_id: str = Query(..., description="User ID (required)."),
    client_id: str = Query(..., description="Client ID (required)."),
    daemon: DaemonClient = Depends(get_daemon_client),
) -> dict[str, Any]:
    """Return the full request, response and meta for a single LLM call.

    Reads three sub-keys in parallel via ``asyncio.gather``:
    ``llm/traces/{call_id}/request``, ``llm/traces/{call_id}/response``,
    ``llm/traces/{call_id}/meta``.
    """
    prefix = "llm/traces/"

    # Resolve session identity (with fallback to default session 0)
    sess_params = await _resolve_trace_session(daemon, agent_id, user_id, client_id)

    async def _safe_get(key: str) -> Optional[Any]:
        """Return the value or None on any error/miss."""
        try:
            result = await daemon.send_request("databus.get", {
                **sess_params,
                "key": key,
            })
            if result.get("exists"):
                return result.get("value")
        except DaemonError:
            pass
        return None

    req_task = _safe_get(f"{prefix}{call_id}/request")
    resp_task = _safe_get(f"{prefix}{call_id}/response")
    meta_task = _safe_get(f"{prefix}{call_id}/meta")

    request_data, response_data, meta_data = await asyncio.gather(
        req_task, resp_task, meta_task,
    )

    # 404 if none of the sub-keys exist (unknown call_id).
    if request_data is None and response_data is None and meta_data is None:
        raise HTTPException(
            status_code=404,
            detail={
                "success": False,
                "data": None,
                "error": {
                    "code": -32001,
                    "message": f"LLM trace '{call_id}' not found for user '{user_id}'",
                },
            },
        )

    return {
        "success": True,
        "data": {
            "call_id": call_id,
            "request": request_data,
            "response": response_data,
            "meta": meta_data,
        },
        "error": None,
    }
