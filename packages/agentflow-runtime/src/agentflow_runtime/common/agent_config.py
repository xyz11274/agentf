"""Agent 节点统一配置入口。

所有节点代码从此模块读取配置，不再直接调用 ``os.environ.get()``。
新增配置项只需在此添加一个方法，无需修改各节点文件。
"""

from __future__ import annotations

import os
from typing import Any

_PLACEHOLDER_PREFIXES = ("${", "sk-your-", "your-")


def _is_placeholder(val: str) -> bool:
    if not val:
        return True
    return any(val.startswith(p) for p in _PLACEHOLDER_PREFIXES)


def _get(key: str, default: str = "") -> str:
    """Read env var with placeholder detection, return ``default`` if empty/placeholder."""
    val = os.environ.get(key, "")
    if _is_placeholder(val):
        return default
    return val


def _get_int(key: str, default: int) -> int:
    try:
        return int(_get(key, str(default)))
    except (ValueError, TypeError):
        return default


def _get_float(key: str, default: float) -> float:
    try:
        return float(_get(key, str(default)))
    except (ValueError, TypeError):
        return default


class AgentConfig:
    """Node runtime configuration — single source of truth for all env-var-backed config.

    Usage::

        from agentflow_runtime.common.agent_config import AgentConfig

        iterations = AgentConfig.coordinator_max_iterations()
        tokens = AgentConfig.max_completion_tokens()
    """

    # ── LLM core ─────────────────────────────────────────────────────

    @staticmethod
    def llm_api_key() -> str:
        """LLM API key with fallback chain."""
        v = _get("AF_LLM_API_KEY")
        if not v:
            v = _get("AF_FALLBACK_LLM_API_KEY")
        return v

    @staticmethod
    def llm_base_url() -> str:
        """LLM base URL with fallback chain."""
        v = _get("AF_LLM_BASE_URL")
        if not v:
            v = _get("AF_FALLBACK_LLM_BASE_URL")
        return v

    @staticmethod
    def llm_model(agent_md_model: str = "", node_type: str = "") -> str:
        """LLM model name with fallback chain.

        Priority:
          1. agent.md model 字段（最高，用户配置）
          2. 节点专用 env var（如 ``AF_COORDINATOR_LLM_MODEL``）
          3. 全局 ``AF_LLM_MODEL``
          4. ``AF_FALLBACK_LLM_MODEL``
          5. 硬编码 ``gpt-4o-mini``
        """
        if not _is_placeholder(agent_md_model):
            return agent_md_model

        # 节点专用配置
        if node_type == "coordinator":
            v = _get("AF_COORDINATOR_LLM_MODEL")
            if v:
                return v
        elif node_type == "skill":
            v = _get("AF_SKILL_LLM_MODEL")
            if v:
                return v

        # 全局回退
        v = _get("AF_LLM_MODEL")
        if v:
            return v
        v = _get("AF_FALLBACK_LLM_MODEL")
        if v:
            return v
        return "gpt-4o-mini"

    @staticmethod
    def resolve_llm_config(agent_md_model: str = "", node_type: str = "") -> dict[str, str]:
        """Full LLM config dict (model, base_url, api_key).

        ``node_type`` controls per-node-type model fallback
        (``AF_COORDINATOR_LLM_MODEL`` / ``AF_SKILL_LLM_MODEL``).
        """
        return {
            "model": AgentConfig.llm_model(agent_md_model, node_type),
            "base_url": AgentConfig.llm_base_url(),
            "api_key": AgentConfig.llm_api_key(),
        }

    # ── VL / Vision ──────────────────────────────────────────────────

    @staticmethod
    def vision_model() -> str:
        v = _get("AF_BR_VISION_MODEL")
        if not v:
            v = _get("AF_BR_VISION_FALLBACK_MODEL")
        return v

    @staticmethod
    def vision_api_key() -> str:
        v = _get("AF_BR_VISION_API_KEY")
        if not v:
            v = _get("MINIMAX_API_KEY")
        if not v:
            v = _get("AF_BR_VISION_FALLBACK_API_KEY")
        return v

    @staticmethod
    def vision_base_url() -> str:
        v = _get("AF_BR_VISION_BASE_URL")
        if not v:
            v = _get("AF_BR_VISION_FALLBACK_BASE_URL")
        return v

    @staticmethod
    def resolve_vision_config() -> dict[str, str]:
        """Full Vision config dict (model, base_url, api_key)."""
        return {
            "model": AgentConfig.vision_model(),
            "base_url": AgentConfig.vision_base_url(),
            "api_key": AgentConfig.vision_api_key(),
        }

    # ── LLM request tuning ──────────────────────────────────────────

    @staticmethod
    def llm_timeout() -> int:
        return _get_int("AF_LLM_TIMEOUT", 120)

    @staticmethod
    def llm_connect_timeout() -> int:
        return _get_int("AF_LLM_CONNECT_TIMEOUT", 5)

    @staticmethod
    def llm_max_retries() -> int:
        return _get_int("AF_LLM_MAX_RETRIES", 2)

    @staticmethod
    def llm_retry_base_delay() -> float:
        return _get_float("AF_LLM_RETRY_BASE_DELAY", 1.0)

    # ── LLM generation parameters (coordinator) ─────────────────────
    # Read by coordinator_decide. Fallback to code default when unset.
    # Skill executor reads AF_SKILL_* variants below, with fallback to
    # these coordinator values.

    @staticmethod
    def llm_temperature() -> float:
        """LLM temperature (coordinator). Default 0.7."""
        return _get_float("AF_LLM_TEMPERATURE", 0.7)

    @staticmethod
    def llm_top_p() -> float:
        """LLM top_p (coordinator). Default 1.0."""
        return _get_float("AF_LLM_TOP_P", 1.0)

    @staticmethod
    def llm_frequency_penalty() -> float:
        """LLM frequency_penalty (coordinator). Default 0.0."""
        return _get_float("AF_LLM_FREQUENCY_PENALTY", 0.0)

    @staticmethod
    def llm_presence_penalty() -> float:
        """LLM presence_penalty (coordinator). Default 0.0."""
        return _get_float("AF_LLM_PRESENCE_PENALTY", 0.0)

    @staticmethod
    def llm_top_k() -> int:
        """LLM top_k (coordinator). Default 0 (disabled)."""
        return _get_int("AF_LLM_TOP_K", 0)

    @staticmethod
    def llm_stop() -> list[str]:
        """LLM stop sequences (coordinator). Default empty list."""
        v = _get("AF_LLM_STOP")
        if v:
            return [s.strip() for s in v.split(",") if s.strip()]
        return []

    # ── LLM generation parameters (skill executor) ──────────────────
    # Read by skill_executor. Fallback: AF_SKILL_* → AF_LLM_* → code default.

    @staticmethod
    def skill_llm_temperature() -> float:
        v = _get("AF_SKILL_LLM_TEMPERATURE")
        return _get_float("AF_SKILL_LLM_TEMPERATURE",
                          AgentConfig.llm_temperature())

    @staticmethod
    def skill_llm_top_p() -> float:
        return _get_float("AF_SKILL_LLM_TOP_P",
                          AgentConfig.llm_top_p())

    @staticmethod
    def skill_llm_frequency_penalty() -> float:
        return _get_float("AF_SKILL_LLM_FREQUENCY_PENALTY",
                          AgentConfig.llm_frequency_penalty())

    @staticmethod
    def skill_llm_presence_penalty() -> float:
        return _get_float("AF_SKILL_LLM_PRESENCE_PENALTY",
                          AgentConfig.llm_presence_penalty())

    @staticmethod
    def skill_llm_top_k() -> int:
        return _get_int("AF_SKILL_LLM_TOP_K",
                        AgentConfig.llm_top_k())

    @staticmethod
    def skill_llm_stop() -> list[str]:
        v = _get("AF_SKILL_LLM_STOP")
        if v:
            return [s.strip() for s in v.split(",") if s.strip()]
        return AgentConfig.llm_stop()

    # ── Coordinator ──────────────────────────────────────────────────

    @staticmethod
    def coordinator_max_iterations() -> int:
        return _get_int("AF_MAX_COORDINATOR_ITERATIONS", 10)

    @staticmethod
    def max_completion_tokens() -> int:
        return _get_int("AF_MAX_COMPLETION_TOKENS", 8192)

    @staticmethod
    def model_max_tokens() -> int:
        return _get_int("AF_MODEL_MAX_TOKENS", 128000)

    @staticmethod
    def compaction_threshold() -> float:
        val = _get_float("AF_COMPACTION_THRESHOLD", 0.8)
        return max(0.1, min(1.0, val))

    # ── Context compression (compress_messages) ──────────────────────

    @staticmethod
    def fold_dir() -> str:
        """折叠文件存储目录（压缩机制 L2 微压缩用）。"""
        return _get("AF_FOLD_DIR", "/tmp/agentflow/folded")

    @staticmethod
    def protect_last_rounds() -> int:
        """R2: 保护最近 N 轮完整对话（第一轮 R1 硬编码单独保护，不从此值扣除）。"""
        return _get_int("AF_PROTECT_LAST_ROUNDS", 3)

    @staticmethod
    def hard_compression_enabled() -> bool:
        """是否启用硬压缩（LLM 语义摘要），默认关闭。"""
        return _get("AF_HARD_COMPRESSION_ENABLED", "false").lower() == "true"

    @staticmethod
    def hard_compression_model() -> str:
        """硬压缩使用的模型名，空=使用主模型。"""
        return _get("AF_HARD_COMPRESSION_MODEL", "")

    # ── Skill executor ───────────────────────────────────────────────

    @staticmethod
    def skill_max_iterations() -> int:
        return _get_int("AF_MAX_SKILL_TASK_ITERATIONS",
                        _get_int("AF_SKILL_MAX_ROUNDS", 8))

    @staticmethod
    def skill_max_completion_tokens() -> int:
        return _get_int("AF_SKILL_MAX_COMPLETION_TOKENS", 8192)

    @staticmethod
    def skill_llm_recovery_attempts() -> int:
        return _get_int("AF_SKILL_LLM_RECOVERY_ATTEMPTS", 2)

    @staticmethod
    def node_execution_timeout() -> int:
        return _get_int("AF_NODE_EXECUTION_TIMEOUT",
                        _get_int("AF_NODE_TIMEOUT_SEC", 240))

    @staticmethod
    def node_timeout_sec() -> int:
        """Per-node timeout from framework injection (AF_NODE_TIMEOUT_SEC)."""
        return _get_int("AF_NODE_TIMEOUT_SEC", 180)


# ── Backward-compatible aliases ─────────────────────────────────────
# Existing code imports these functions directly. Keeping the signatures
# unchanged so callers don't break.

def resolve_llm_config(agent_md_model: str = "", node_type: str = "") -> dict[str, str]:
    """Backward-compatible alias for AgentConfig.resolve_llm_config()."""
    return AgentConfig.resolve_llm_config(agent_md_model, node_type)

def resolve_vision_config() -> dict[str, str]:
    """Backward-compatible alias for AgentConfig.resolve_vision_config()."""
    return AgentConfig.resolve_vision_config()

def get_model_max_tokens() -> int:
    """Backward-compatible alias for AgentConfig.model_max_tokens()."""
    return AgentConfig.model_max_tokens()

def get_compaction_threshold() -> float:
    """Backward-compatible alias for AgentConfig.compaction_threshold()."""
    return AgentConfig.compaction_threshold()

def get_env(key: str, default: str = "") -> str:
    """Backward-compatible alias for direct env read (runtime context, not config)."""
    return os.environ.get(key, default)


__all__ = [
    "AgentConfig",
    "resolve_llm_config",
    "resolve_vision_config",
    "get_model_max_tokens",
    "get_compaction_threshold",
    "get_env",
]
