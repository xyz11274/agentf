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
    """Current session ID (UUID)."""
    return get_env("AF_SESSION_ID")


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