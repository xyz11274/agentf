"""AgentFlow Bridge — configuration.

Configuration sources (low → high priority):
  1. Code defaults (Field(default=...))
  2. ``configs/{env}/bridge.yaml`` (per-environment YAML, env=``AF_BR_ENV``)
  3. ``config.yaml`` (fallback CWD / frontend path)
  4. System environment variables

``AF_BR_ENV`` controls which per-environment config is loaded.
  - Not set / ``dev`` → ``configs/dev/bridge.yaml``
  - ``prod`` → ``configs/prod/bridge.yaml``
  - Any other value → ``configs/{value}/bridge.yaml``

Importing ``settings`` gives the process-wide singleton.
"""

from __future__ import annotations

import os
import re
from functools import lru_cache
from pathlib import Path
from typing import Any

import yaml
from pydantic import Field
from pydantic_settings import BaseSettings, SettingsConfigDict

# Project root: parent of the ``frontend/bridge/`` package directory.
_PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent

# ── Environment mode ──────────────────────────────────────────────
# Controls which configs/{env}/bridge.yaml is loaded.
_AGENTFLOW_ENV = os.environ.get("AF_BR_ENV", "dev")

# ── Config file search path ───────────────────────────────────────
# First match wins.  Environment-specific configs at the front so they
# take precedence over generic fallback paths.
_CONFIG_FILE_CANDIDATES = [
    _PROJECT_ROOT / "configs" / _AGENTFLOW_ENV / "bridge.yaml",
    Path("config.yaml"),
    Path("frontend/bridge/config.yaml"),
    _PROJECT_ROOT / "frontend" / "bridge" / "config.yaml",
    Path("/opt/agentflow/config.yaml"),
]

# ── Environment variable expansion in YAML ────────────────────────
# Supports ${VAR} and ${VAR:-default} syntax (same as docker-compose / helm).
_ENV_VAR_RE = re.compile(r"\$\{([^}:]+)(?::-(.*?))?\}")


def _expand_str(value: str) -> str:
    """Expand ``${VAR}`` and ``${VAR:-default}`` in a YAML string value."""

    def _repl(m: re.Match) -> str:
        name, default = m.group(1), m.group(2)
        val = os.environ.get(name)
        if val is not None:
            return val
        if default is not None:
            return default
        return ""

    return _ENV_VAR_RE.sub(_repl, value)


def _expand_value(obj: Any) -> Any:
    """Recursively expand env vars in all values from parsed YAML."""
    if isinstance(obj, str):
        return _expand_str(obj)
    if isinstance(obj, dict):
        return {k: _expand_value(v) for k, v in obj.items()}
    if isinstance(obj, list):
        return [_expand_value(v) for v in obj]
    return obj


def _load_yaml_config() -> dict[str, Any]:
    """Load config.yaml with ``${VAR}`` / ``${VAR:-default}`` expansion."""
    for candidate in _CONFIG_FILE_CANDIDATES:
        if candidate.exists():
            with open(candidate) as f:
                raw = yaml.safe_load(f) or {}
                return _expand_value(raw)
    return {}


# ── Helper to traverse nested YAML ────────────────────────────────
def _yget(cfg: dict[str, Any], *keys: str, default: Any = None) -> Any:
    current = cfg
    for k in keys:
        if not isinstance(current, dict):
            return default
        current = current.get(k)
        if current is None:
            return default
    return current


class BridgeSettings(BaseSettings):
    """Bridge runtime configuration.

    Load order (low → high priority):
      1. Code defaults below
      2. ``configs/{env}/bridge.yaml`` / ``config.yaml`` (YAML)
      3. System environment variables
    """

    model_config = SettingsConfigDict(
        env_prefix="",
        case_sensitive=False,
        extra="ignore",
    )

    def __init__(self, **kwargs):
        # Step 1: 先让 Pydantic 从 env 加载（优先级高）
        super().__init__(**kwargs)

        # Step 2: 再从 YAML 加载（优先级低，不覆盖 env 已有的值）
        yaml_cfg = _load_yaml_config()
        if yaml_cfg:
            self._apply_yaml_defaults(yaml_cfg)
            self._yaml_source = yaml_cfg
        else:
            self._yaml_source = {}

    def _apply_yaml_defaults(self, cfg: dict[str, Any]) -> None:
        """Apply YAML values only for fields not already overridden by env."""
        flat = self._yaml_to_flat(cfg)
        for key, value in flat.items():
            if key not in self.__class__.model_fields:
                continue
            field = self.__class__.model_fields[key]
            current = getattr(self, key)
            # 只当当前值等于代码默认值时，才用 YAML 的值覆盖
            if current == field.default:
                setattr(self, key, value)

    # ── YAML → flat dict ──────────────────────────────────────────
    @staticmethod
    def _yaml_to_flat(cfg: dict[str, Any], prefix: str = "") -> dict[str, Any]:
        """Convert nested YAML dict to flat keys (daemon.host → daemon_host)."""
        flat: dict[str, Any] = {}
        for key, value in cfg.items():
            flat_key = f"{prefix}{key}" if not prefix else f"{prefix}_{key}"
            if isinstance(value, dict) and not _is_pydantic_type(key):
                flat.update(BridgeSettings._yaml_to_flat(value, flat_key))
            elif not isinstance(value, dict):
                flat[flat_key] = value
        return flat

    # ── Sidecar config (bridge doesn't run sidecar, but exposes shared config) ──
    @property
    def sidecar_enabled(self) -> bool:
        return bool(_yget(self._yaml_source, "sidecar", "enabled"))

    # ── AISF Adapter config（已接入主 Bridge，仅保留运行时参数） ──
    # aisf_adapter_agent_id 走标准 Pydantic 字段加载（见上方 Field 定义）

    # ── Raw YAML for other components to consume ───────────────────
    @property
    def raw_config(self) -> dict[str, Any]:
        return self._yaml_source

    # --- 🔌 daemon connectivity ------------------------------------------------
    daemon_host: str = Field(validation_alias="AF_BR_DAEMON_HOST", default="127.0.0.1", description="Daemon hostname / IP.")
    daemon_port: int = Field(validation_alias="AF_BR_DAEMON_PORT", default=17891, ge=1, le=65535, description="Daemon TCP/WS port.")

    # --- 🌐 bridge server bind -------------------------------------------------
    bridge_listen: str = Field(validation_alias="AF_BR_LISTEN", 
        default="0.0.0.0:8199",
        description="Bridge listen address (host:port). Set via AF_BR_LISTEN.",
    )

    @property
    def bridge_host(self) -> str:
        """Derive host from bridge_listen."""
        return self.bridge_listen.split(":")[0]

    @property
    def bridge_port(self) -> int:
        """Derive port from bridge_listen."""
        parts = self.bridge_listen.split(":")
        return int(parts[1]) if len(parts) > 1 else 8199

    # --- sidecar (production entry point) ------------------------------------
    sidecar_enabled: bool = Field(validation_alias="AF_BR_SIDECAR_ENABLED", default=False, description="Enable C++ CoreMind sidecar. Set via AF_BR_SIDECAR_ENABLED.")
    sidecar_listen: str = Field(validation_alias="AF_BR_SIDECAR_LISTEN", default="0.0.0.0:23767", description="Sidecar external listen address. Set via AF_BR_SIDECAR_LISTEN.")
    sidecar_aisf_adapter_url: str = Field(validation_alias="AF_BR_SIDECAR_AISF_ADAPTER_URL", default="http://127.0.0.1:18080", description="Sidecar → aisf_adapter URL. Set via AF_BR_SIDECAR_AISF_ADAPTER_URL.")

    # --- 🔧 aisf_adapter（已接入主 Bridge 的 /v1/tasks 路由） ---
    aisf_adapter_agent_id: str = Field(validation_alias="AF_BR_AISF_ADAPTER_AGENT_ID", default="outfit-assistant", description="agentflowd 中 agent 标识. Set via AF_BR_AISF_ADAPTER_AGENT_ID.")
    aisf_adapter_mode: str = Field(validation_alias="AF_BR_AISF_ADAPTER_MODE", default="async", description="AISF Adapter mode: async | sync. Set via AF_BR_AISF_ADAPTER_MODE.")
    aisf_adapter_timeout: float = Field(validation_alias="AF_BR_AISF_ADAPTER_TIMEOUT", default=360.0, gt=0, description="Per-task timeout (seconds). Set via AF_BR_AISF_ADAPTER_TIMEOUT.")
    aisf_adapter_upstream_scheme: str = Field(validation_alias="AF_BR_AISF_ADAPTER_UPSTREAM_SCHEME", default="http", description="Upstream callback scheme: http | https. Set via AF_BR_AISF_ADAPTER_UPSTREAM_SCHEME.")
    aisf_adapter_upstream_callback_port: int = Field(validation_alias="AF_BR_AISF_ADAPTER_UPSTREAM_CALLBACK_PORT", default=23767, description="Upstream SysAgent callback port. Set via AF_BR_AISF_ADAPTER_UPSTREAM_CALLBACK_PORT.")
    aisf_adapter_upstream_callback_path: str = Field(validation_alias="AF_BR_AISF_ADAPTER_UPSTREAM_CALLBACK_PATH", default="/coremind/systemagent/v1/callback", description="Upstream callback path. Set via AF_BR_AISF_ADAPTER_UPSTREAM_CALLBACK_PATH.")
    aisf_adapter_upstream_client_cert_path: str = Field(validation_alias="AF_BR_AISF_ADAPTER_UPSTREAM_CLIENT_CERT_PATH", default="", description="Client cert PEM path for mTLS. Set via AF_BR_AISF_ADAPTER_UPSTREAM_CLIENT_CERT_PATH.")
    aisf_adapter_upstream_client_key_path: str = Field(validation_alias="AF_BR_AISF_ADAPTER_UPSTREAM_CLIENT_KEY_PATH", default="", description="Client key PEM path for mTLS. Set via AF_BR_AISF_ADAPTER_UPSTREAM_CLIENT_KEY_PATH.")
    aisf_adapter_upstream_client_key_password: str = Field(validation_alias="AF_BR_AISF_ADAPTER_UPSTREAM_CLIENT_KEY_PASSWORD", default="", description="Client key PEM password (if encrypted). Set via AF_BR_AISF_ADAPTER_UPSTREAM_CLIENT_KEY_PASSWORD.")
    aisf_adapter_upstream_ca_cert_path: str = Field(validation_alias="AF_BR_AISF_ADAPTER_UPSTREAM_CA_CERT_PATH", default="", description="Custom CA cert path for server verification. Set via AF_BR_AISF_ADAPTER_UPSTREAM_CA_CERT_PATH.")
    aisf_adapter_upstream_verify: bool = Field(validation_alias="AF_BR_AISF_ADAPTER_UPSTREAM_VERIFY", default=True, description="Verify server TLS cert. Set via AF_BR_AISF_ADAPTER_UPSTREAM_VERIFY.")

    # --- agent_defaults — LLM 配置降级链路 (from config.yaml) ----------
    agent_defaults_llm_api_key: str = Field(validation_alias="AF_BR_AGENT_DEFAULTS_LLM_API_KEY", default="", description="Default LLM API key. From config.yaml agent_defaults.llm.api_key.")
    agent_defaults_llm_base_url: str = Field(validation_alias="AF_BR_AGENT_DEFAULTS_LLM_BASE_URL", default="", description="Default LLM base URL. From config.yaml agent_defaults.llm.base_url.")
    agent_defaults_llm_model: str = Field(validation_alias="AF_BR_AGENT_DEFAULTS_LLM_MODEL", default="gpt-4o", description="Default LLM model. From config.yaml agent_defaults.llm.model.")
    agent_defaults_llm_fallback_api_key: str = Field(validation_alias="AF_BR_AGENT_DEFAULTS_LLM_FALLBACK_API_KEY", default="", description="Fallback LLM API key. From config.yaml agent_defaults.llm_fallback.api_key.")
    agent_defaults_llm_fallback_base_url: str = Field(validation_alias="AF_BR_AGENT_DEFAULTS_LLM_FALLBACK_BASE_URL", default="", description="Fallback LLM base URL. From config.yaml agent_defaults.llm_fallback.base_url.")
    agent_defaults_llm_fallback_model: str = Field(validation_alias="AF_BR_AGENT_DEFAULTS_LLM_FALLBACK_MODEL", default="", description="Fallback LLM model. From config.yaml agent_defaults.llm_fallback.model.")

    # --- artifact ------------------------------------------------------------
    artifact_public_base_url: str = Field(validation_alias="AF_BR_ARTIFACT_PUBLIC_BASE_URL", 
        default="",
        description="Public base URL for artifact links (empty = relative paths).",
    )
    # 产物输出模式（两维正交开关，详见 docs/design/features/artifact-output-pipeline.md §3.6）：
    #   artifact_reference: link（默认，Markdown 链接）| placeholder（{{artifact:N}} 占位符，由消费端自行替换）
    #   artifact_payload:    url（默认，HTTP serve 地址）| base64（内容以 base64 内嵌，无需网络访问）
    artifact_reference: str = Field(validation_alias="AF_BR_ARTIFACT_REFERENCE",
        default="link",
        description="Artifact reference form in response text: link | placeholder.",
    )
    artifact_payload: str = Field(validation_alias="AF_BR_ARTIFACT_PAYLOAD",
        default="url",
        description="Artifact content transport: url | base64 (base64 embeds content inline).",
    )
    artifacts_storage_dir: str = Field(validation_alias="AF_BR_ARTIFACTS_STORAGE_DIR",
        default="",
        description="Artifact storage directory (local filesystem). Empty = agent_packages_path.parent / artifacts_storage.",
    )

    # --- 📂 filesystem paths ----------------------------------------------------
    agent_packages_dir: str = Field(validation_alias="AF_BR_AGENT_PACKAGES_DIR", 
        default="examples/agent",
        description="Directory containing Agent Package source trees.",
    )
    skill_hub_dir: str = Field(validation_alias="AF_BR_SKILL_HUB_DIR", 
        default="resource/skill_hub",
        description="Directory containing the Skill Hub central repository.",
    )
    agent_template_dir: str = Field(validation_alias="AF_BR_AGENT_TEMPLATE_DIR",
        default="frontend/bridge/templates/agent_template",
        description="Directory containing agent creation templates (e.g. harness_agent).",
    )

    # --- 🪵 observability / behavior --------------------------------------------
    log_level: str = Field(validation_alias="AF_BR_LOG_LEVEL", default="INFO", description="Logging level name.")
    request_timeout_seconds: float = Field(validation_alias="AF_BR_REQUEST_TIMEOUT_SECONDS", 
        default=5.0, gt=0,
        description="Per-request timeout (seconds) for outbound daemon calls.",
    )
    upload_max_body_size_mb: int = Field(
        default=100, ge=1,
        description="Max HTTP request body size in MB for file uploads.",
    )

    # --- 🖼️ multimodal content (Phase A) ---------------------------------------
    uploads_dir: str = Field(validation_alias="AF_BR_UPLOADS_DIR", 
        default="/tmp/agentflow/uploads",
        description="Directory where bridge lands large inline images before forwarding to daemon.",
    )
    chat_turn_timeout_seconds: float = Field(validation_alias="AF_BR_CHAT_TURN_TIMEOUT_SECONDS", 
        default=900.0, gt=0,
        description="Maximum chat turn duration. Set via AF_BR_.",
    )
    daemon_status_refresh_interval: float = Field(validation_alias="AF_BR_DAEMON_STATUS_REFRESH_INTERVAL", 
        default=10.0, gt=0,
        description="Seconds between background daemon status cache refreshes.",
    )

    # --- 🏖️ sandbox management (local dev observability) ----------------------
    # 对齐 daemon 配置 configs/{env}/agentflowd.json 的 sandbox.* 与
    # scripts/e2b/sandbox_list.py 的默认值（见 docs/design/features/sandbox-management.md）。
    sandbox_mode: str = Field(
        validation_alias="AF_BR_SANDBOX_MODE",
        default="local",
        description="Sandbox mode: e2b | local | docker. Mirrors daemon sandbox.mode.",
    )
    # 注：无 sandbox_workdir_root / sandbox_daemon_log_path——local/docker 模式
    # 已切换为 daemon RPC 数据源（sandbox.list），不再目录扫描/日志解析
    # （2026-08-02 重构）
    sandbox_e2b_api_url: str = Field(
        validation_alias="AF_BR_SANDBOX_E2B_API_URL",
        default="http://218.245.96.148:30030",
        description="e2b API base URL (mirrors daemon sandbox.e2b_api_url).",
    )
    sandbox_e2b_api_key: str = Field(
        validation_alias="AF_BR_SANDBOX_E2B_API_KEY",
        default="e2b_000000000",
        description="e2b API key (kept in memory, never exposed to responses).",
    )
    sandbox_e2b_ca_cert_path: str = Field(
        validation_alias="AF_BR_SANDBOX_E2B_CA_CERT_PATH",
        default="resource/certs/e2b_CA.pem",
        description="e2b CA cert path for TLS verification.",
    )
    # 注：无 sandbox_daemon_log_path——local/docker 模式已切换为 daemon RPC
    # 数据源（sandbox.list），不再解析 daemon 日志（2026-08-02 重构）

    # --- 💾 session persistence (Bridge sessions) ------------------------------
    session_provider: str = Field(validation_alias="AF_BR_SESSION_PROVIDER", 
        default="local",
        description=(
            "Session persistence backend: 'local' (SQLite file) or 'kmm' "
            "(aisf platform KMM service, Phase 3). Set via AF_BR_."
        ),
    )
    session_db_path: str = Field(validation_alias="AF_BR_SESSION_DB_PATH", 
        default="/tmp/agentflow/sessions.db",
        description=(
            "SQLite database path used by LocalSessionProvider. "
            "Set via AF_BR_."
        ),
    )
    session_context_limit_rounds: int = Field(
        default=5, ge=1,
        description=(
            "Default number of past dialogue rounds injected into the next "
            "chat turn as history context. Set via AF_BR_."
        ),
    )
    session_sync_interval: float = Field(validation_alias="AF_BR_SESSION_SYNC_INTERVAL", 
        default=30.0, gt=0,
        description=(
            "Seconds between background agentflow→Bridge session status "
            "syncs. Set via AF_BR_. 0 disables the poller "
            "(sync runs only on lifespan start)."
        ),
    )

    # --- 🌍 CORS ---------------------------------------------------------------
    cors_allowed_origins: list[str] = Field(validation_alias="AF_BR_CORS_ALLOWED_ORIGINS", 
        default_factory=lambda: [
            "http://localhost:15173",
            "http://127.0.0.1:15173",
        ],
        description="Origins allowed by CORS middleware.",
    )

    # --- derived helpers -------------------------------------------------------
    @property
    def daemon_ws_url(self) -> str:
        return f"ws://{self.daemon_host}:{self.daemon_port}"

    @property
    def agent_packages_path(self) -> Path:
        p = Path(self.agent_packages_dir)
        if p.is_absolute():
            return p.resolve()
        return (_PROJECT_ROOT / p).resolve()

    @property
    def skill_hub_path(self) -> Path:
        p = Path(self.skill_hub_dir)
        if p.is_absolute():
            return p.resolve()
        return (_PROJECT_ROOT / p).resolve()

    @property
    def agent_template_path(self) -> Path:
        p = Path(self.agent_template_dir)
        if p.is_absolute():
            return p.resolve()
        return (_PROJECT_ROOT / p).resolve()

    @property
    def uploads_path(self) -> Path:
        p = Path(self.uploads_dir)
        return p.resolve()

    @property
    def session_db_path_resolved(self) -> Path:
        """Absolute path to the SQLite session database.

        Resolves relative ``session_db_path`` against the project root,
        matching the pattern of ``agent_packages_path`` and ``skill_hub_path``.
        """
        p = Path(self.session_db_path)
        if p.is_absolute():
            return p.resolve()
        return (_PROJECT_ROOT / p).resolve()


def _is_pydantic_type(key: str) -> bool:
    """Heuristic: skip nested keys that look like Pydantic models."""
    return key in ("model_config", "model_post_init")


@lru_cache(maxsize=1)
def get_settings() -> BridgeSettings:
    """Process-wide cached :class:`BridgeSettings` instance."""
    return BridgeSettings()


# Convenience alias: ``from config import settings``
settings = get_settings()
