"""AgentFlow Bridge — Conversation history API.

Provides persistent storage for completed conversations so the frontend can
list and retrieve past conversations keyed by ``(user_id, client_id)``.

This decouples the frontend's "conversation list" from the daemon's ephemeral
dialogue lifecycle.
"""

from __future__ import annotations

import logging
from typing import Any

from fastapi import APIRouter, HTTPException, Query, status
from pydantic import BaseModel, Field

from .conversations import get_store

logger = logging.getLogger("agentflow.bridge.routes.conversations")

router = APIRouter(prefix="/api/conversations", tags=["conversations"])


class ConversationResponse(BaseModel):
    """A single conversation record."""

    agent_id: str
    user_id: str
    client_id: str
    dialogue_id: str
    content: str
    created_at: str


class ConversationListResponse(BaseModel):
    """List of conversations."""

    conversations: list[ConversationResponse]


@router.get(
    "/{agent_id}",
    response_model=ConversationListResponse,
    summary="List conversations for a user+client",
)
async def list_conversations(
    agent_id: str,
    user_id: str = Query(..., description="User identifier."),
    client_id: str = Query(..., description="Client identifier."),
    limit: int = Query(20, ge=1, le=100, description="Max conversations to return."),
) -> dict[str, Any]:
    """List past conversations for a ``(agent_id, user_id, client_id)`` tuple.

    Returns the most recent ``limit`` conversations, newest first.
    Each entry contains the full response content.
    """
    store = get_store()
    records = store.list_by_user(agent_id, user_id, client_id, limit=limit)
    return {
        "conversations": [
            ConversationResponse(
                agent_id=r.agent_id,
                user_id=r.user_id,
                client_id=r.client_id,
                dialogue_id=r.dialogue_id,
                content=r.content,
                created_at=r.created_at,
            )
            for r in records
        ],
    }


@router.get(
    "/{agent_id}/{dialogue_id}",
    response_model=ConversationResponse,
    summary="Get a specific conversation by dialogue_id",
)
async def get_conversation(
    agent_id: str,
    dialogue_id: str,
) -> dict[str, Any]:
    """Get a single conversation by its dialogue_id."""
    store = get_store()
    record = store.get(dialogue_id)
    if record is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Conversation not found: {dialogue_id}",
        )
    return {
        "agent_id": record.agent_id,
        "user_id": record.user_id,
        "client_id": record.client_id,
        "dialogue_id": record.dialogue_id,
        "content": record.content,
        "created_at": record.created_at,
    }
