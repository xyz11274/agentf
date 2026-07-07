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
import os
from pathlib import Path
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

    Before loading, injects default agent configuration into the agent's
    ``.env`` file from Bridge ``config.yaml → agent_defaults``.
    Only writes values that are not already set (empty or placeholder).
    """
    pkg_name = (body.package_path if body else None) or agent_id.replace("-", "_")
    package_path = str(settings.agent_packages_path / pkg_name)

    # 注入默认配置到 .env（不覆盖用户已有值）
    _inject_agent_env_defaults(pkg_name)

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


# ═══════════════════════════════════════════════════════════════════
# Agent .env 默认配置注入
# ═══════════════════════════════════════════════════════════════════

# 从 config.yaml → agent_defaults 映射到 .env 变量名和默认值
_AGENT_ENV_DEFAULTS: dict[str, str] = {
    # LLM 主模型
    "AF_LLM_API_KEY":       "agent_defaults.llm.api_key",
    "AF_LLM_BASE_URL":      "agent_defaults.llm.base_url",
    "AF_LLM_MODEL":         "agent_defaults.llm.model",
    # LLM 降级
    "AF_FALLBACK_LLM_API_KEY":  "agent_defaults.llm_fallback.api_key",
    "AF_FALLBACK_LLM_BASE_URL": "agent_defaults.llm_fallback.base_url",
    "AF_FALLBACK_LLM_MODEL":    "agent_defaults.llm_fallback.model",
    # Coordinator
    "AF_MAX_COORDINATOR_ITERATIONS":  "agent_defaults.coordinator.max_iterations",
    "AF_MAX_COMPLETION_TOKENS":       "agent_defaults.coordinator.max_completion_tokens",
    # Skill
    "AF_MAX_SKILL_TASK_ITERATIONS":     "agent_defaults.skill.max_iterations",
    "AF_SKILL_MAX_COMPLETION_TOKENS":   "agent_defaults.skill.max_completion_tokens",
    "AF_SKILL_LLM_RECOVERY_ATTEMPTS":   "agent_defaults.skill.llm_recovery_attempts",
    "AF_NODE_EXECUTION_TIMEOUT":        "agent_defaults.skill.node_timeout_sec",
    # Token 预算
    "AF_MODEL_MAX_TOKENS":      "agent_defaults.model_max_tokens",
    "AF_COMPACTION_THRESHOLD":  "agent_defaults.compaction_threshold",
    # HTTP
    "AF_LLM_TIMEOUT":           "agent_defaults.llm_timeout",
    "AF_LLM_CONNECT_TIMEOUT":   "agent_defaults.llm_connect_timeout",
    "AF_LLM_MAX_RETRIES":       "agent_defaults.llm_max_retries",
    "AF_LLM_RETRY_BASE_DELAY":  "agent_defaults.llm_retry_base_delay",
    # VL
    "VISION_BASE_URL":  "agent_defaults.vision.base_url",
    "VISION_MODEL":     "agent_defaults.vision.model",
    "VISION_API_KEY":   "agent_defaults.vision.api_key",
    # VL 降级
    "VISION_FALLBACK_BASE_URL": "agent_defaults.vision_fallback.base_url",
    "VISION_FALLBACK_MODEL":    "agent_defaults.vision_fallback.model",
    "VISION_FALLBACK_API_KEY":  "agent_defaults.vision_fallback.api_key",
}


def _resolve_yaml_path(cfg: dict[str, Any], dotted: str) -> str | None:
    """Resolve a dotted YAML path like ``agent_defaults.llm.model``."""
    current: Any = cfg
    for part in dotted.split("."):
        if not isinstance(current, dict):
            return None
        current = current.get(part)
    if current is None:
        return None
    # Normalise: bool/int → str
    if isinstance(current, (bool, int, float)):
        return str(current)
    return str(current)


def _inject_agent_env_defaults(pkg_name: str) -> None:
    """Merge Bridge ``agent_defaults`` into the agent's ``.env``.

    Only writes values that are currently empty, missing, or hold a
    placeholder (``${...}``).  Never overwrites a user-configured value.
    """
    pkg_dir = settings.agent_packages_path / pkg_name
    env_file = pkg_dir / ".env"
    if not pkg_dir.is_dir():
        logger.warning("agent_env_inject.skip pkg=%s (directory not found)", pkg_name)
        return

    # Read current .env
    current_env: dict[str, str] = {}
    if env_file.is_file():
        for line in env_file.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" in line:
                k, _, v = line.partition("=")
                current_env[k.strip()] = v.strip()

    # Resolve agent_defaults from config.yaml
    raw_cfg = getattr(settings, "_yaml_source", {})
    if not raw_cfg:
        return

    # Build injection candidates
    candidates: dict[str, str] = {}
    for env_key, yaml_path in _AGENT_ENV_DEFAULTS.items():
        val = _resolve_yaml_path(raw_cfg, yaml_path)
        if val is not None:
            candidates[env_key] = val

    if not candidates:
        return

    # Merge: Bridge defaults go first, user values go after so they naturally win.
    # dotenv.load_dotenv() reads top-to-bottom — last occurrence of the same key wins.
    _PLACEHOLDER_PREFIXES = ("${", "sk-your-", "your-")
    top_lines: list[str] = []
    bottom_lines: list[str] = []
    written_defaults: set[str] = set()

    #   a) Collect bridge-default keys that the user already has a real value for.
    #      These will NOT be written in the defaults section.
    user_has_real: set[str] = set()
    if env_file.is_file():
        for line in env_file.read_text(encoding="utf-8").splitlines():
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            if "=" in stripped:
                k = stripped.split("=", 1)[0].strip()
                if k in candidates:
                    v = current_env.get(k, "")
                    if v and not v.startswith(_PLACEHOLDER_PREFIXES):
                        user_has_real.add(k)

    #   b) Write bridge-injected defaults at the TOP (for keys user hasn't set)
    defaults_to_write: list[str] = []
    for env_key, val in candidates.items():
        if env_key in user_has_real:
            continue  # user has a real value — skip default, user line below wins
        if not val:
            continue
        defaults_to_write.append(f"{env_key}={val}")
        written_defaults.add(env_key)

    if defaults_to_write:
        top_lines.append("# ── Bridge-injected defaults (user values below override) ──")
        top_lines.extend(defaults_to_write)
        top_lines.append("")

    #   c) Preserve all original user lines (always wins over bridge defaults)
    if env_file.is_file():
        bottom_lines = list(env_file.read_text(encoding="utf-8").splitlines())

    # Strip trailing empty lines from bottom for clean join
    while bottom_lines and bottom_lines[-1] == "":
        bottom_lines.pop()

    lines = top_lines + bottom_lines
    total_written = len(written_defaults) + len(user_has_real)

    if defaults_to_write:
        env_file.write_text("\n".join(lines) + "\n", encoding="utf-8")
        logger.info(
            "agent_env_inject.inject pkg=%s keys=%d",
            pkg_name, total_written,
        )


__all__ = ["router"]
