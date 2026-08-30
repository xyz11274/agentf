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

from typing import Generic, List, Literal, Optional, TypeVar, Union

from pydantic import BaseModel, ConfigDict, Field
from typing_extensions import Literal as _Literal  # alias for clarity

SAFE_ID_PATTERN = r"^[A-Za-z0-9._-]{1,120}$"



# ═══════════════════════════════════════════════════════════════════
# Multimodal content parts (Phase A)
# ═══════════════════════════════════════════════════════════════════


# ---------------------------------------------------------------------------
# Multimodal content parts (Phase A: text + image_url inline base64)
# ---------------------------------------------------------------------------


class TextPart(BaseModel):
    """A text fragment in a multimodal content array (OpenAI-compatible)."""

    model_config = ConfigDict(from_attributes=True)

    type: Literal["text"]
    text: str


class ImageURLBody(BaseModel):
    """The ``image_url`` payload inside an :class:`ImagePart`.

    ``url`` accepts either:
    * ``data:image/...;base64,...`` — inline base64 (Phase A primary path)
    * ``file:///abs/path`` — already-landed file URL (Phase B, or bridge output)
    """

    model_config = ConfigDict(from_attributes=True)

    url: str


class ImagePart(BaseModel):
    """An image fragment in a multimodal content array (OpenAI-compatible)."""

    model_config = ConfigDict(from_attributes=True)

    type: Literal["image_url"]
    image_url: ImageURLBody


ContentPart = Union[TextPart, ImagePart]
"""Discriminated union of supported content part types."""


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
    content: Union[str, List[ContentPart]]
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

    user_id: str = Field(..., pattern=SAFE_ID_PATTERN)
    client_id: str = Field(..., pattern=SAFE_ID_PATTERN)
    content: Union[str, List[ContentPart]]


class ArtifactInfo(BaseModel):
    """Structured metadata for a generated artifact in a chat chunk.

    Provides a programmatic alternative to parsing artifact URLs from Markdown text.
    Aligns with OpenAI Assistants / Claude / Gemini content block patterns.
    """

    model_config = ConfigDict(from_attributes=True)

    type: Literal["image", "html", "download"]
    """Semantic type: image (display inline), html (open in browser), download (file)."""

    url: str
    """Public or relative URL to fetch the artifact via Bridge artifact endpoint."""

    name: str
    """Human-readable filename (e.g. look.png, report.pptx)."""

    mime_type: str = "application/octet-stream"
    """MIME type hint for the HTTP response Content-Type header."""

    # 产物输出模式扩展字段（默认空 = 不启用，保持向后兼容）：
    #   placeholder: 文本中产物引用的占位符（如 "{{artifact:1}}"），
    #     artifact_reference=placeholder 时由消费端按此占位符定位并自行替换
    #   data_base64: 产物内容 base64（artifact_payload=base64 时内嵌，无需网络访问 serve URL）
    placeholder: str = ""
    """Placeholder token in the response text ({{artifact:N}}) when reference=placeholder."""

    data_base64: str = ""
    """Base64-encoded artifact content (payload=base64 mode); empty = fetch via url."""


class ChatChunk(BaseModel):
    """Streaming chunk emitted by the bridge during a chat turn.

    ``type`` is fixed by the wire contract and defaults to ``"chunk"``;
    ``final: True`` means this is the last chunk for the current turn.
    ``progress: True`` means this is a progress/status message, not model output.
    The frontend can style it differently (e.g. grey text + spinner) to
    distinguish it from the LLM's final response.

    ``artifacts`` is a structured list of generated files (images, HTML, downloads)
    extracted from the response text, providing programmatic access without
    requiring Markdown/regex parsing.
    """

    model_config = ConfigDict(from_attributes=True)

    type: Literal["chunk", "llm_chunk"] = "chunk"
    chunk_type: str = ""
    """Subtype discriminator: ``"text"`` | ``"progress"`` | ``"llm_chunk"``.
    Always ``"text"`` when empty. Mirrors the SSE path's ``chunk_type`` field
    so the frontend can use a unified handler for both WebSocket and SSE."""
    content: str
    final: bool
    progress: bool = False
    progress_skill: str = ""
    """技能名称（级联折叠展示用）。"""
    progress_round: str = ""
    """技能执行轮次（多轮 LLM 调用）。"""
    progress_status: str = ""
    """进度状态: running | completed | error。"""
    artifacts: list[ArtifactInfo] = Field(default_factory=list)
    """Structured artifact metadata extracted from this chunk's content.
    Empty list when no artifacts are present.  Each entry provides type, URL,
    filename, and MIME type for programmatic consumption."""


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
