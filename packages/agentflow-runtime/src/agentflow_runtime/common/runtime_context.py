"""
Runtime context — semantic accessors for framework-injected AF_* environment variables.

The framework (worker.cpp) injects node inputs as ``AF_{KEY}`` environment variables
before spawning the node process.  This module provides a single source of truth
for reading those variables, so every node script (coordinator_decide, skill_executor,
etc.) reads from the same place instead of duplicating ``os.environ.get("AF_*", "")``.

Usage::

    from agentflow_runtime.common.runtime_context import session_id, dialogue_id, input as af_input

    sid = session_id()
    did = dialogue_id()
    inp = af_input()

Two categories of variables live here:

- **Runtime context** (per-node-execution, changes every call):
  ``AF_SESSION_ID``, ``AF_DIALOGUE_ID``, ``AF_NODE_ID``, ``AF_INPUT``,
  ``AF_COMPLETED_SKILL_NAME``, ``AF_SKILL_SUCCESS``, ``AF_SKILL_RESULT``,
  ``AF_SKILL_ARTIFACTS``, ``AF_EXEC_SKILL_KEY``, ``AF_DATABUS_UDS_FILE``,
  ``AF_SELF_TASK_ID``, ``AF_SKILL_NAME``

- **Config** (stable across executions):
  ``AF_LLM_*``, ``AF_MODEL_MAX_TOKENS``, ``AF_COMPACTION_THRESHOLD``, etc.
  These belong in :class:`agentflow_runtime.common.agent_config.AgentConfig`,
  *not* here.
"""

from __future__ import annotations

import os


# ── Raw accessor (internal use, avoids repeating default) ──

def get_env(key: str, default: str = "") -> str:
    """Read a framework-injected ``AF_*`` environment variable."""
    return os.environ.get(key, default)


# ── Runtime context accessors ──

def session_id() -> str:
    """Current session slot ID (0/1/2 — the agentflow session slot, NOT user identity).

    For artifact routing use ``user_id() + "_" + client_id()`` as the session_id
    segment per ``docs/design/features/sandbox/artifact-storage.md`` §2.2.
    """
    return get_env("AF_SESSION_ID")


def user_id() -> str:
    """Current user ID (injected by framework for artifact routing)."""
    return get_env("AF_USER_ID")


def client_id() -> str:
    """Current client ID (injected by framework for artifact routing)."""
    return get_env("AF_CLIENT_ID")


def artifact_session_id() -> str:
    """Artifact routing key ``{user_id}_{client_id}`` per artifact-storage.md §2.2.

    Returns empty string when both user_id and client_id are unset (e.g. legacy
    paths without these injected). Callers should treat empty as a sentinel that
    artifact routing is not yet provisioned, not as a valid directory segment.
    """
    uid = user_id()
    cid = client_id()
    if not uid and not cid:
        return ""
    return f"{uid}_{cid}"


def dialogue_id() -> str:
    """Current dialogue ID (numeric timestamp)."""
    return get_env("AF_DIALOGUE_ID")


def node_id() -> str:
    """Current node ID (e.g. ``coordinator_decide``, ``skill_executor``)."""
    return get_env("AF_NODE_ID")


def input() -> str:
    """User input payload (JSON string with ``{stream, messages}``)."""
    return get_env("AF_INPUT")


def completed_skill_name() -> str:
    """Name of the skill that just completed (non-empty on re-trigger)."""
    return get_env("AF_COMPLETED_SKILL_NAME")


def skill_success() -> str:
    """``"true"`` / ``"false"`` — whether the completed skill succeeded."""
    return get_env("AF_SKILL_SUCCESS")


def skill_result() -> str:
    """Result text from the completed skill."""
    return get_env("AF_SKILL_RESULT")


def skill_artifacts() -> str:
    """JSON artifact list from the completed skill."""
    return get_env("AF_SKILL_ARTIFACTS")


def exec_skill_key() -> str:
    """DataBus key for the structured skill input payload."""
    return get_env("AF_EXEC_SKILL_KEY")


def databus_uds_file() -> str:
    """Unix Domain Socket path for runtime DataBus access."""
    return get_env("AF_DATABUS_UDS_FILE")


def self_task_id() -> str:
    """Current task ID (currently always ``""`` — known framework gap)."""
    return get_env("AF_SELF_TASK_ID")


def skill_name() -> str:
    """Current skill name (currently always ``""`` — known framework gap)."""
    return get_env("AF_SKILL_NAME")


def node_execution_timeout() -> str:
    """Node-level timeout in seconds (injected by framework)."""
    return get_env("AF_NODE_EXECUTION_TIMEOUT")


# ── Trace context accessors (§6.2 跨进程 span 树) ──
# C++ worker.cpp spawn 子进程前注入 AF_TRACE_ID + AF_PARENT_SPAN_ID，
# Python 侧经此 accessor 读入，由 trace.span_context.SpanContext 建立父链。

def trace_id() -> str:
    """Current trace ID (injected by C++ worker from TraceContext::trace_id()).

    Empty when C++ side has no active trace (e.g. TraceTask not started) —
    SpanContext will then fall back to a local ``py_local_*`` trace_id.
    """
    return get_env("AF_TRACE_ID")


def parent_span_id() -> str:
    """Parent span ID (injected by C++ worker from TraceContext::span_id()).

    The C++ NODE span that spawned this Python subprocess — Python-side spans
    attach as children of this span. Empty when C++ side has no active span.
    """
    return get_env("AF_PARENT_SPAN_ID")