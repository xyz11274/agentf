"""AgentFlow Bridge — configuration.

Configuration sources (low → high priority):
  1. Code defaults (Field(default=...))
  2. ``config.yaml`` (human-readable, all settings documented)
  3. ``.env`` file (legacy, optional)
  4. System environment variables

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

# ── Config file search path ───────────────────────────────────────
_CONFIG_FILE_CANDIDATES = [
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
      2. ``config.yaml``
      3. ``.env`` file
      4. System environment variables
    """

    model_config = SettingsConfigDict(
        env_file=".env",
        env_file_encoding="utf-8",
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
    daemon_host: str = Field(default="127.0.0.1", description="Daemon hostname / IP.")
    daemon_port: int = Field(default=17891, ge=1, le=65535, description="Daemon TCP/WS port.")

    # --- 🌐 bridge server bind -------------------------------------------------
    bridge_listen: str = Field(
        default="0.0.0.0:8199",
        description="Bridge listen address (host:port). Set via BRIDGE_LISTEN env var.",
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
    sidecar_enabled: bool = Field(default=False, description="Enable C++ CoreMind sidecar. Set via SIDECAR_ENABLED.")
    sidecar_listen: str = Field(default="0.0.0.0:23767", description="Sidecar external listen address. Set via SIDECAR_LISTEN.")
    sidecar_aisf_adapter_url: str = Field(default="http://127.0.0.1:18080", description="Sidecar → aisf_adapter URL. Set via SIDECAR_AISF_ADAPTER_URL.")

    # --- 🔧 aisf_adapter（已接入主 Bridge 的 /v1/tasks 路由） ---
    aisf_adapter_agent_id: str = Field(default="outfit-assistant", description="agentflowd 中 agent 标识. Set via AISF_ADAPTER_AGENT_ID.")
    aisf_adapter_mode: str = Field(default="async", description="AISF Adapter mode: async | sync. Set via AISF_ADAPTER_MODE.")
    aisf_adapter_timeout: float = Field(default=360.0, gt=0, description="Per-task timeout (seconds). Set via AISF_ADAPTER_TIMEOUT.")
    aisf_adapter_upstream_scheme: str = Field(default="http", description="Upstream callback scheme: http | https. Set via AISF_ADAPTER_UPSTREAM_SCHEME.")
    aisf_adapter_upstream_callback_port: int = Field(default=23767, description="Upstream SysAgent callback port. Set via AISF_ADAPTER_UPSTREAM_CALLBACK_PORT.")
    aisf_adapter_upstream_callback_path: str = Field(default="/coremind/systemagent/v1/callback", description="Upstream callback path. Set via AISF_ADAPTER_UPSTREAM_CALLBACK_PATH.")
    aisf_adapter_upstream_client_cert_path: str = Field(default="", description="Client cert PEM path for mTLS. Set via AISF_ADAPTER_UPSTREAM_CLIENT_CERT_PATH.")
    aisf_adapter_upstream_client_key_path: str = Field(default="", description="Client key PEM path for mTLS. Set via AISF_ADAPTER_UPSTREAM_CLIENT_KEY_PATH.")
    aisf_adapter_upstream_client_key_password: str = Field(default="", description="Client key PEM password (if encrypted). Set via AISF_ADAPTER_UPSTREAM_CLIENT_KEY_PASSWORD.")
    aisf_adapter_upstream_ca_cert_path: str = Field(default="", description="Custom CA cert path for server verification. Set via AISF_ADAPTER_UPSTREAM_CA_CERT_PATH.")
    aisf_adapter_upstream_verify: bool = Field(default=True, description="Verify server TLS cert. Set via AISF_ADAPTER_UPSTREAM_VERIFY.")

    # --- artifact ------------------------------------------------------------
    artifact_public_base_url: str = Field(
        default="",
        description="Public base URL for artifact links (empty = relative paths).",
    )

    # --- 📂 filesystem paths ----------------------------------------------------
    agent_packages_dir: str = Field(
        default="examples/agent",
        description="Directory containing Agent Package source trees.",
    )
    skill_hub_dir: str = Field(
        default="resource/skill_hub",
        description="Directory containing the Skill Hub central repository.",
    )
    agent_template_dir: str = Field(
        default="templates",
        description="Directory containing agent creation templates (e.g. harness_agent).",
    )

    # --- 🪵 observability / behavior --------------------------------------------
    log_level: str = Field(default="INFO", description="Logging level name.")
    request_timeout_seconds: float = Field(
        default=5.0, gt=0,
        description="Per-request timeout (seconds) for outbound daemon calls.",
    )
    upload_max_body_size_mb: int = Field(
        default=100, ge=1,
        description="Max HTTP request body size in MB for file uploads.",
    )

    # --- 🖼️ multimodal content (Phase A) ---------------------------------------
    content_image_inline_threshold_kb: int = Field(
        default=256, ge=1,
        description="Images with base64 payload ≥ this KB are landed to disk by the bridge.",
    )
    uploads_dir: str = Field(
        default="/tmp/agentflow/uploads",
        description="Directory where bridge lands large inline images before forwarding to daemon.",
    )
    chat_turn_timeout_seconds: float = Field(
        default=900.0, gt=0,
        description="Maximum chat turn duration. Set via CHAT_TURN_TIMEOUT_SECONDS.",
    )
    daemon_status_refresh_interval: float = Field(
        default=10.0, gt=0,
        description="Seconds between background daemon status cache refreshes.",
    )

    # --- 🌍 CORS ---------------------------------------------------------------
    cors_allowed_origins: list[str] = Field(
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


def _is_pydantic_type(key: str) -> bool:
    """Heuristic: skip nested keys that look like Pydantic models."""
    return key in ("model_config", "model_post_init")


@lru_cache(maxsize=1)
def get_settings() -> BridgeSettings:
    """Process-wide cached :class:`BridgeSettings` instance."""
    return BridgeSettings()


# Convenience alias: ``from config import settings``
settings = get_settings()
