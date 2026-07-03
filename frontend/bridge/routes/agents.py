"""AgentFlow Bridge — agent proxy routes.

This router is a thin HTTP-to-JSON-RPC proxy in front of the
:class:`~client.DaemonClient`. It does no caching, no business logic
and no daemon round-trip is avoided: every endpoint forwards the call
to the daemon via the client and packages the result into an
:class:`~models.ApiResponse`.

The route paths, methods and target RPC methods are fixed by the
frontend ``agentStore`` contract; renaming or restructuring them here
will break the Vue UI.
"""

from __future__ import annotations

import logging
from typing import Any

from fastapi import APIRouter, Depends, HTTPException, status
from pydantic import BaseModel

from client import (
    DaemonClient,
    DaemonConnectionError,
    DaemonError,
    DaemonTimeoutError,
    InvalidParamsError,
    NotFoundError,
)
from dependencies import get_daemon_client
from config import settings

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/api/agents", tags=["agents"])


class _LoadAgentBody(BaseModel):
    package_path: str | None = None


def _ok(data: Any) -> dict[str, Any]:
    """Wrap a daemon payload in the bridge's standard success envelope."""
    return {"success": True, "data": data, "error": None}


def _raise_daemon_error(exc: DaemonError) -> "HTTPException":
    """Translate a :class:`DaemonError` to the corresponding HTTP status.

    Mapping (locked by the bridge contract):

    ==========================  =====
    Daemon error class          HTTP
    ==========================  =====
    ``InvalidParamsError``      400
    ``NotFoundError``           404
    ``DaemonTimeoutError``      504
    ``DaemonConnectionError``   502
    Other ``DaemonError``       500
    ==========================  =====
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


@router.get("", name="list_agents")
async def list_agents(
    client: DaemonClient = Depends(get_daemon_client),
) -> dict[str, Any]:
    """``GET /api/agents`` — list all loaded agents (``agent.list``)."""
    try:
        result = await client.agent_list()
        logger.info("agent_lifecycle.list ok count=%d", len(result) if isinstance(result, list) else 0)
        return _ok(result)
    except DaemonError as exc:
        logger.warning("agent_lifecycle.list error=%s", exc)
        raise _raise_daemon_error(exc) from exc


@router.get("/{agent_id}", name="get_agent")
async def get_agent(
    agent_id: str,
    client: DaemonClient = Depends(get_daemon_client),
) -> dict[str, Any]:
    """``GET /api/agents/{id}`` — fetch a single agent (``agent.info``)."""
    try:
        return _ok(await client.agent_info(agent_id))
    except DaemonError as exc:
        logger.warning("agent_lifecycle.info agent=%s error=%s", agent_id, exc)
        raise _raise_daemon_error(exc) from exc


@router.post("/{agent_id}/load", name="load_agent")
async def load_agent(
    agent_id: str,
    body: _LoadAgentBody | None = None,
    client: DaemonClient = Depends(get_daemon_client),
) -> dict[str, Any]:
    """``POST /api/agents/{id}/load`` — load an agent (``agent.load``).
    """
    pkg_name = (body.package_path if body else None) or agent_id.replace("-", "_")
    package_path = str(settings.agent_packages_path / pkg_name)
    try:
        result = await client.agent_load(package_path)
        logger.info("agent_lifecycle.load agent=%s pkg=%s ok", agent_id, pkg_name)
        return _ok(result)
    except DaemonError as exc:
        logger.warning("agent_lifecycle.load agent=%s pkg=%s error=%s", agent_id, pkg_name, exc)
        raise _raise_daemon_error(exc) from exc


@router.post("/{agent_id}/unload", name="unload_agent")
async def unload_agent(
    agent_id: str,
    client: DaemonClient = Depends(get_daemon_client),
) -> dict[str, Any]:
    """``POST /api/agents/{id}/unload`` — unload an agent (``agent.unload``)."""
    try:
        result = await client.agent_unload(agent_id)
        logger.info("agent_lifecycle.unload agent=%s ok", agent_id)
        return _ok(result)
    except DaemonError as exc:
        logger.warning("agent_lifecycle.unload agent=%s error=%s", agent_id, exc)
        raise _raise_daemon_error(exc) from exc


@router.post("/{agent_id}/pause", name="pause_agent")
async def pause_agent(
    agent_id: str,
    client: DaemonClient = Depends(get_daemon_client),
) -> dict[str, Any]:
    """``POST /api/agents/{id}/pause`` — pause a running agent (``agent.pause``)."""
    try:
        result = await client.agent_pause(agent_id)
        logger.info("agent_lifecycle.pause agent=%s ok", agent_id)
        return _ok(result)
    except DaemonError as exc:
        logger.warning("agent_lifecycle.pause agent=%s error=%s", agent_id, exc)
        raise _raise_daemon_error(exc) from exc


@router.post("/{agent_id}/resume", name="resume_agent")
async def resume_agent(
    agent_id: str,
    client: DaemonClient = Depends(get_daemon_client),
) -> dict[str, Any]:
    """``POST /api/agents/{id}/resume`` — resume a paused agent (``agent.resume``)."""
    try:
        result = await client.agent_resume(agent_id)
        logger.info("agent_lifecycle.resume agent=%s ok", agent_id)
        return _ok(result)
    except DaemonError as exc:
        logger.warning("agent_lifecycle.resume agent=%s error=%s", agent_id, exc)
        raise _raise_daemon_error(exc) from exc


__all__ = ["router"]
