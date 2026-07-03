"""Trusted internal endpoint used by the C++ CoreMind SideCar."""

from __future__ import annotations

import asyncio
import os
import secrets
from typing import Any, Literal

from fastapi import APIRouter, Depends, Header, HTTPException, Request
from pydantic import BaseModel, Field, model_validator

from client import DaemonClient, DaemonError
from dependencies import get_daemon_client

router = APIRouter(prefix="/internal/coremind/v1", tags=["coremind-internal"])


class ServiceInstance(BaseModel):
    host: str = Field(min_length=1)
    port: int = Field(ge=1, le=65535)


class ServiceSnapshot(BaseModel):
    status: Literal["resolved", "unavailable"]
    instances: list[ServiceInstance] = Field(default_factory=list)
    error_code: str | None = None

    @model_validator(mode="after")
    def validate_state(self) -> ServiceSnapshot:
        if self.status == "resolved" and not self.instances:
            raise ValueError("resolved service requires at least one instance")
        if self.status == "unavailable" and not self.error_code:
            raise ValueError("unavailable service requires error_code")
        return self


class PlatformContext(BaseModel):
    services: dict[str, ServiceSnapshot] = Field(default_factory=dict)


class CoreMindEnvelope(BaseModel):
    schema_version: Literal["1.0"]
    request_id: str = Field(min_length=1)
    source: Literal["coremind-sidecar"]
    original_request: dict[str, Any]
    platform_context: PlatformContext


def _authenticate(request: Request, authorization: str | None) -> None:
    allowed_hosts = {
        host.strip()
        for host in os.environ.get("AISF_INTERNAL_ALLOWED_HOSTS", "127.0.0.1,::1").split(",")
        if host.strip()
    }
    client_host = request.client.host if request.client else ""
    if client_host not in allowed_hosts:
        raise HTTPException(status_code=403, detail="UNTRUSTED_INTERNAL_SOURCE")
    expected = os.environ.get("AISF_INTERNAL_TOKEN", "")
    supplied = authorization.removeprefix("Bearer ") if authorization else ""
    if not expected or not supplied or not secrets.compare_digest(supplied, expected):
        raise HTTPException(status_code=401, detail="UNTRUSTED_INTERNAL_REQUEST")


def _extract_accessctrl(envelope: CoreMindEnvelope) -> tuple[str, str, str]:
    header = envelope.original_request.get("Header", {})
    data = envelope.original_request.get("Data", {})
    if not isinstance(header, dict) or not isinstance(data, dict):
        raise HTTPException(status_code=400, detail="INVALID_PLATFORM_CONTEXT")
    content = str(data.get("UserInputText", "")).strip()
    if not content:
        raise HTTPException(status_code=400, detail="missing Data.UserInputText")
    user_id = str(header.get("UserName") or "coremind-user")
    client_id = str(header.get("DeviceId") or header.get("ClientSessionId") or "coremind")
    return content, user_id, client_id


@router.post("/callback")
async def callback(
    request: Request,
    envelope: CoreMindEnvelope,
    authorization: str | None = Header(default=None),
    daemon: DaemonClient = Depends(get_daemon_client),  # noqa: B008
) -> dict[str, Any]:
    """Authenticate a SideCar envelope and execute one AgentFlow turn."""
    _authenticate(request, authorization)
    content, user_id, client_id = _extract_accessctrl(envelope)
    agent_id = os.environ.get("COREMIND_AGENT_ID", "outfit-assistant")
    try:
        result = await daemon.agent_execute(
            agent_id,
            session_id="",
            content=content,
            user_id=user_id,
            client_id=client_id,
            dialogue_id=envelope.request_id,
            platform_context=envelope.platform_context.model_dump(mode="json", exclude_none=True),
            request_id=envelope.request_id,
        )
        task_id = result.get("task_id") or result.get("message_id")
        chunks: list[str] = []
        while True:
            response = await daemon.wait_for_response(
                agent_id, dialogue_id=envelope.request_id, timeout=360.0
            )
            if isinstance(response, dict) and response.get("content"):
                chunks.append(str(response["content"]))
            if isinstance(response, dict) and response.get("final") is True:
                break
    except (DaemonError, asyncio.TimeoutError) as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc
    return {
        "success": True,
        "data": {"task_id": task_id, "content": "".join(chunks)},
        "error": None,
    }
