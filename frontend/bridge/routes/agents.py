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
import re
import shutil
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
from config import settings
from dependencies import get_daemon_client

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/api/agents", tags=["agents"])


class _LoadAgentBody(BaseModel):
    package_path: str | None = None


def _ok(data: Any) -> dict[str, Any]:
    """Wrap a daemon payload in the bridge's standard success envelope."""
    return {"success": True, "data": data, "error": None}


def _raise_daemon_error(exc: DaemonError) -> HTTPException:
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


# 三态模型：② 固化态（agent_packages_path）→ ③ workspace 实例（agent_workspace_path）
# 同步时跳过 ③ 的运行时数据（保留运行现场），资产以 ② 为权威源（I-3S-3）。
# manifest.jsonc 不在此排除清单中——它是 ② 固化态 agent 包的"沙箱上传清单"声明
# （agent-package-structure.md §3.3），属于资产而非运行时数据。随 ②→③ 同步带上后，
# ③ 的 manifest 恒等于 ② 的权威清单：daemon 的 TemplateRegistry 用 ③ 的 manifest
# 构建快照时，只含 ② 声明资产（不含 ③ 独有运行时产物如 dashscope_outfit_skills），
# 且 daemon 全程只感知 ③（不感知 ②/③ 之分，三态模型约束）。
_SYNC_EXCLUDE_NAMES = {
    ".venv", "outputs", "workspace", ".agentfoundry",
    "preferences.json",
}


def _sync_agent_package(pkg_name: str) -> Path:
    """Sync ② 固化态 → ③ workspace 实例，返回 ③ 路径。

    规则（docs/design/features/agent-package/agent-package-three-state.md §5）：
    - ③ 不存在 → 全量拷贝 ② → ③
    - ③ 已存在 → 覆盖式同步：② 的资产文件覆盖 ③ 同名文件；③ 的运行时数据
      （排除清单）保留不动
    - ② 为资产单一真相源（I-5 / I-3S-3）
    - OSError 处理（用户拍板 2026-08-07）：中途失败时 **不清理 ③ 半成品**——
      下次 load 的 `dirs_exist_ok=True` 覆盖式同步会在残缺 ③ 上重拷，② 仍是权威源；
      避免误删 ③ 已有的运行时数据（.venv/outputs 等，恢复代价高）。失败由 500 上报，
      下次 load 以 ② 为准收敛。
    """
    source = settings.agent_packages_path / pkg_name
    target = settings.agent_workspace_path / pkg_name
    if not source.is_dir():
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=(
                f"Agent package '{pkg_name}' not found in "
                f"{settings.agent_packages_path.resolve()}"
            ),
        )
    try:
        target.mkdir(parents=True, exist_ok=True)
        shutil.copytree(
            source,
            target,
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns(*_SYNC_EXCLUDE_NAMES),
        )
    except OSError as exc:
        logger.warning("agent_lifecycle.sync_error pkg=%s error=%s", pkg_name, exc)
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail=f"Failed to sync agent package '{pkg_name}' to workspace: {exc}",
        ) from exc
    # F7.1：记同步规模便于故障定位（② 与 ③ 文件数一致断言 I-3S-3）
    try:
        sync_count = sum(1 for _ in target.rglob("*") if _.is_file())
    except OSError:
        sync_count = -1  # 无法统计时不静默，记 -1 明示异常
    logger.info(
        "agent_lifecycle.sync pkg=%s src=%s dst=%s files=%d",
        pkg_name, source.resolve(), target.resolve(), sync_count,
    )
    return target


@router.post("/{agent_id}/load", name="load_agent")
async def load_agent(
    agent_id: str,
    body: _LoadAgentBody | None = None,
    client: DaemonClient = Depends(get_daemon_client),
) -> dict[str, Any]:
    """``POST /api/agents/{id}/load`` — load an agent (``agent.load``).

    Before loading, syncs the agent package from the 固化态 directory
    (``agent_packages_path``, ②) into the workspace instance directory
    (``agent_workspace_path``, ③), then loads the workspace instance so all
    runtime data (``.venv``/``outputs``/``workspace``/memory) lands in ③ and
    never pollutes ②. Injects default agent configuration into the instance's
    ``.env`` from Bridge ``config.yaml → agent_defaults``.
    Only writes values that are not already set (empty or placeholder).
    """
    pkg_name = (body.package_path if body else None) or agent_id
    from routes.packages import _run_setup_sh, _validate_package_name
    _validate_package_name(pkg_name)
    workspace_path = _sync_agent_package(pkg_name)
    package_path = str(workspace_path)

    # 注入默认配置到 .env（③ workspace 实例内，daemon 从 AGENT_PACKAGE_DIR/.env 读取）
    _inject_agent_env_defaults(pkg_name)

    # setup.sh 在 ③ workspace 实例上执行（三态模型 I-3S-1：.venv/ 等运行时数据只落 ③）
    _run_setup_sh(workspace_path)

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
    "AF_LLM_BASE_URL":      "agent_defaults.llm.base_url",
    "AF_LLM_MODEL":         "agent_defaults.llm.model",
    # LLM 降级
    "AF_FALLBACK_LLM_BASE_URL": "agent_defaults.llm_fallback.base_url",
    "AF_FALLBACK_LLM_MODEL":    "agent_defaults.llm_fallback.model",
    # Coordinator
    "AF_MAX_COORDINATOR_ITERATIONS":  "agent_defaults.coordinator.max_iterations",
    "AF_MAX_COMPLETION_TOKENS":       "agent_defaults.coordinator.max_completion_tokens",
    "AF_COORDINATOR_LLM_MODEL":       "agent_defaults.coordinator.model",
    # Skill
    "AF_MAX_SKILL_TASK_ITERATIONS":     "agent_defaults.skill.max_iterations",
    "AF_SKILL_MAX_COMPLETION_TOKENS":   "agent_defaults.skill.max_completion_tokens",
    "AF_SKILL_LLM_RECOVERY_ATTEMPTS":   "agent_defaults.skill.llm_recovery_attempts",
    "AF_NODE_EXECUTION_TIMEOUT":        "agent_defaults.skill.node_timeout_sec",
    "AF_SKILL_LLM_MODEL":               "agent_defaults.skill.model",
    # Token 预算（YAML 中嵌套在 skill: 下）
    "AF_MODEL_MAX_TOKENS":      "agent_defaults.skill.model_max_tokens",
    "AF_COMPACTION_THRESHOLD":  "agent_defaults.skill.compaction_threshold",
    # HTTP
    "AF_LLM_TIMEOUT":           "agent_defaults.llm_timeout",
    "AF_LLM_CONNECT_TIMEOUT":   "agent_defaults.llm_connect_timeout",
    "AF_LLM_MAX_RETRIES":       "agent_defaults.llm_max_retries",
    "AF_LLM_RETRY_BASE_DELAY":  "agent_defaults.llm_retry_base_delay",
    # LLM 生成参数（Coordinator）
    "AF_LLM_TEMPERATURE":           "agent_defaults.llm.temperature",
    "AF_LLM_TOP_P":                 "agent_defaults.llm.top_p",
    "AF_LLM_FREQUENCY_PENALTY":     "agent_defaults.llm.frequency_penalty",
    "AF_LLM_PRESENCE_PENALTY":      "agent_defaults.llm.presence_penalty",
    "AF_LLM_TOP_K":                 "agent_defaults.llm.top_k",
    "AF_LLM_STOP":                  "agent_defaults.llm.stop",
    # Coordinator 专用
    "AF_COORDINATOR_LLM_TEMPERATURE": "agent_defaults.coordinator.temperature",
    # Skill 专用
    "AF_SKILL_LLM_TEMPERATURE":      "agent_defaults.skill.temperature",
    # UDS 通道切换
    "AF_USE_AISF_MODELS":       "agent_defaults.use_aisf_models",
    # VL
    "AF_BR_VISION_BASE_URL":  "agent_defaults.vision.base_url",
    "AF_BR_VISION_MODEL":     "agent_defaults.vision.model",
    # VL 降级
    "AF_BR_VISION_FALLBACK_BASE_URL": "agent_defaults.vision_fallback.base_url",
    "AF_BR_VISION_FALLBACK_MODEL":    "agent_defaults.vision_fallback.model",
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


# Regex for `${VAR:-default}` or `${VAR}` placeholder syntax
#   Group 1: VAR name (ignored)
#   Group 2: default value (after ":-"), or None if no default
_PLACEHOLDER_RE = re.compile(r"^\$\{([^}:]+)(?::-(.*))?\}$")


def _resolve_placeholder(val: str) -> str:
    """Resolve a ``${VAR:-default}`` placeholder to its default value.

    If the value is not a placeholder, returns it unchanged.
    If it is a placeholder with a default, returns the default.
    If it is a placeholder without a default (``${VAR}``), returns empty string.
    """
    m = _PLACEHOLDER_RE.match(val)
    if not m:
        return val  # Not a placeholder
    # m.group(2) is the default value (after ":-"), or None if no default
    return m.group(2) or ""


def _inject_agent_env_defaults(pkg_name: str) -> None:
    """Merge Bridge ``agent_defaults`` into the agent's ``.env``.

    Only writes values that are currently empty, missing, or hold a
    placeholder (``${...}``).  Never overwrites a user-configured value.

    Credentials are deliberately absent from `_AGENT_ENV_DEFAULTS`; they stay
    in the trusted process environment or an operator-managed ignored file.
    """
    # 三态模型：.env 是运行时数据，注入到 ③ workspace 实例（daemon load ③ 时读取）
    pkg_dir = settings.agent_workspace_path / pkg_name
    env_file = pkg_dir / ".env"
    if not pkg_dir.is_dir():
        logger.warning("agent_env_inject.skip pkg=%s (directory not found)", pkg_name)
        return

    # Restrict an existing operator-managed file before reading or rewriting it.
    # Any chmod failure is fatal: loading from a world-readable credential file
    # would violate the Bridge security boundary.
    if env_file.is_file():
        env_file.chmod(0o600)

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
    # F11：走公开 @property raw_config（单一真相源），不直接访问实例属性 _yaml_source
    raw_cfg = settings.raw_config
    if not raw_cfg:
        return

    # Build injection candidates — resolve `${VAR:-default}` placeholders
    candidates: dict[str, str] = {}
    for env_key, yaml_path in _AGENT_ENV_DEFAULTS.items():
        val = _resolve_yaml_path(raw_cfg, yaml_path)
        if val is not None:
            # YAML 中的 `${VAR:-default}` 是字面量，需解析为实际默认值
            val = _resolve_placeholder(val)
            if val:
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

    # Use boundary markers so the injected block is always identifiable and
    # replaceable regardless of what the user has in the rest of the file.
    _INJECT_START = "# ── BRIDGE_INJECTED_START ──"
    _INJECT_END   = "# ── BRIDGE_INJECTED_END ──"

    #   b) Collect bridge-injected defaults
    defaults_to_write: list[str] = []
    for env_key, val in candidates.items():
        if env_key in user_has_real:
            continue  # user has a real value — skip default, user line below wins
        if not val:
            continue
        defaults_to_write.append(f"{env_key}={val}")
        written_defaults.add(env_key)

    #   c) Read existing file, strip any stale injected block (marker-delimited),
    #      keep all user lines.
    user_lines: list[str] = []
    if env_file.is_file():
        raw_lines = list(env_file.read_text(encoding="utf-8").splitlines())
        in_injected = False
        for line in raw_lines:
            if line.strip() == _INJECT_START:
                in_injected = True
                continue
            if line.strip() == _INJECT_END:
                in_injected = False
                continue
            if not in_injected:
                user_lines.append(line)

    if defaults_to_write:
        # Build the new injected block at the top
        inject_block = [_INJECT_START] + defaults_to_write + [_INJECT_END, ""]
        lines = inject_block + user_lines
    total_written = len(written_defaults) + len(user_has_real)

    if defaults_to_write:
        env_file.touch(mode=0o600, exist_ok=True)
        env_file.chmod(0o600)
        env_file.write_text("\n".join(lines) + "\n", encoding="utf-8")
        logger.info(
            "agent_env_inject.inject pkg=%s keys=%d",
            pkg_name, total_written,
        )


__all__ = ["router"]
