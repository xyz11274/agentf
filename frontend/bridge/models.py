"""
AgentFlow shared data models (Python side).

This module defines Pydantic v2 BaseModel classes that mirror the TypeScript
interfaces in ``frontend/web/src/types/agentflow.ts`` exactly. Any change to the TS
file MUST be reflected here to keep the bridge <-> frontend contract in sync.

Design principles:
  - Field names are snake_case to match the wire format (daemon / bridge JSON).
  - Optional fields use ``Optional[...]`` with a default of ``None``.
  - No runtime logic; this module is type/schema definitions only.
"""

from __future__ import annotations

from typing import Generic, List, Optional, TypeVar, Union

from pydantic import BaseModel, ConfigDict, Field
from typing_extensions import Literal

# ---------------------------------------------------------------------------
# Agent
# ---------------------------------------------------------------------------


class AgentInfo(BaseModel):
    """Information about a single agent instance known to the daemon."""

    model_config = ConfigDict(from_attributes=True)

    id: str
    name: str
    workflow_count: int
    status: Literal["loaded", "running", "paused", "unloaded"]


# ---------------------------------------------------------------------------
# Session
# ---------------------------------------------------------------------------


class SessionInfo(BaseModel):
    """Information about an active runtime session."""

    model_config = ConfigDict(from_attributes=True)

    session_id: str
    user_id: str
    client_id: str
    age_ms: int
    status: str


# ---------------------------------------------------------------------------
# Package
# ---------------------------------------------------------------------------


class PackageInfo(BaseModel):
    """A packaged agent collection on disk.

    Files are read directly by the bridge (no daemon round-trip).
    """

    model_config = ConfigDict(from_attributes=True)

    name: str
    path: str
    file_count: int
    last_modified: str


# ---------------------------------------------------------------------------
# File tree
# ---------------------------------------------------------------------------


class FileNode(BaseModel):
    """A node in the file tree displayed in the file-explorer UI.

    - ``type`` distinguishes files from directories.
    - ``children`` is present for directories and absent for files.
    - ``content`` is only populated when the file has been opened/read.
    """

    model_config = ConfigDict(from_attributes=True)

    name: str
    path: str
    type: Literal["file", "dir"]
    children: Optional[List["FileNode"]] = None
    content: Optional[str] = None


# Resolve the self-referencing forward reference introduced by ``children``.
FileNode.model_rebuild()


# ---------------------------------------------------------------------------
# Workflow
# ---------------------------------------------------------------------------


class WorkflowNode(BaseModel):
    """A single node in a workflow DAG."""

    model_config = ConfigDict(from_attributes=True)

    id: str
    name: str
    type: str
    execution_mode: Optional[str] = None
    description: Optional[str] = None
    risk_level: Optional[Literal["green", "yellow", "red"]] = None


class WorkflowEdge(BaseModel):
    """A directed edge in a workflow DAG.

    ``from_`` / ``to`` reference ``WorkflowNode.id``.

    Note: ``from`` is a Python keyword, so the attribute is ``from_`` and the
    wire-format alias is ``from``. Call ``model_dump(by_alias=True)`` (or use
    ``model_dump_json(by_alias=True)``) to serialize to the JSON key the
    daemon / frontend expect.
    """

    model_config = ConfigDict(
        from_attributes=True,
        protected_namespaces=(),
    )

    from_: str = Field(alias="from")
    to: str
    control_type: Optional[Literal["none", "if", "switch_case"]] = None
    condition: Optional[str] = None

    def model_dump(self, **kwargs):  # type: ignore[override]
        """Force ``by_alias=True`` so ``from_`` always serializes as ``from``."""
        kwargs.setdefault("by_alias", True)
        return super().model_dump(**kwargs)

    def model_dump_json(self, **kwargs):  # type: ignore[override]
        """Force ``by_alias=True`` so ``from_`` always serializes as ``from``."""
        kwargs.setdefault("by_alias", True)
        return super().model_dump_json(**kwargs)


class WorkflowDef(BaseModel):
    """A complete workflow definition: nodes + edges.

    The overridden ``model_dump`` / ``model_dump_json`` force ``by_alias=True``
    so nested ``WorkflowEdge`` instances also serialize ``from_`` as ``from``.
    Pydantic v2 does not propagate the flag to nested models automatically.
    """

    model_config = ConfigDict(from_attributes=True)

    id: str
    name: str
    nodes: List[WorkflowNode]
    edges: List[WorkflowEdge]

    def model_dump(self, **kwargs):  # type: ignore[override]
        kwargs.setdefault("by_alias", True)
        return super().model_dump(**kwargs)

    def model_dump_json(self, **kwargs):  # type: ignore[override]
        kwargs.setdefault("by_alias", True)
        return super().model_dump_json(**kwargs)


# ---------------------------------------------------------------------------
# Daemon
# ---------------------------------------------------------------------------


class DaemonStatus(BaseModel):
    """Runtime status of the AgentFlow daemon, as reported by the bridge."""

    model_config = ConfigDict(from_attributes=True)

    status: str
    version: str
    uptime: int
    active_agents: int
    running: bool


# ---------------------------------------------------------------------------
# Chat
# ---------------------------------------------------------------------------


class ChatMessage(BaseModel):
    """A single chat message exchanged with the agent."""

    model_config = ConfigDict(from_attributes=True)

    id: str
    role: Literal["user", "assistant"]
    content: str
    timestamp: int


# ---------------------------------------------------------------------------
# Generic API wrapper
# ---------------------------------------------------------------------------

T = TypeVar("T")


class ApiResponse(BaseModel, Generic[T]):
    """Standard wrapper used by the bridge REST API.

    - ``data`` is the typed payload on success.
    - ``error`` is populated when ``success`` is ``False``.
    """

    model_config = ConfigDict(from_attributes=True)

    success: bool
    data: T
    error: Optional[str] = None


# ---------------------------------------------------------------------------
# WebSocket chat (JSON-RPC over WS between bridge and daemon)
# ---------------------------------------------------------------------------


class ChatRequest(BaseModel):
    """Client -> bridge chat request.

    Uses ``user_id`` + ``client_id`` to identify the caller.
    ``session_id`` is a daemon-internal concept and is NOT exposed to clients.
    """

    model_config = ConfigDict(from_attributes=True)

    user_id: str
    client_id: str
    content: str


class ChatChunk(BaseModel):
    """Streaming chunk emitted by the bridge during a chat turn.

    ``type`` is fixed by the wire contract and defaults to ``"chunk"``;
    ``final: True`` means this is the last chunk for the current turn.
    """

    model_config = ConfigDict(from_attributes=True)

    type: Literal["chunk"] = "chunk"
    content: str
    final: bool


class ChatDone(BaseModel):
    """Terminal ``done`` message closing a chat turn.

    No ``session_id`` is returned — it is a daemon-internal concept.
    """

    model_config = ConfigDict(from_attributes=True)

    type: Literal["done"] = "done"
    final: Literal[True] = True


# Discriminated union for WebSocket chat frames. Consumers can use this
# instead of hand-rolling ``Union[...]`` at the call site.
ChatFrame = Union[ChatChunk, ChatDone]
