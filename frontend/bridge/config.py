"""AgentFlow Bridge — configuration.

All runtime knobs are exposed as environment variables (with sensible
defaults) and consumed via :class:`BridgeSettings`, a Pydantic Settings
class. Importing ``settings`` gives the process-wide singleton; tests can
construct fresh instances via ``BridgeSettings(_env_file=...)`` if needed.

Required env vars: none (everything has a default).

Configuration groups
--------------------
- ``DAEMON_*``:       daemon connectivity  (host, port)
- ``BRIDGE_*``:       bridge bind settings (host, port)
- ``*_DIR``:          filesystem paths     (agent packages, skill hub)
- ``LOG_LEVEL``:      logging
- ``*_TIMEOUT_*``:    timeouts & intervals
- ``CORS_*``:         CORS origins

See ``.env.example`` for a complete template with inline documentation.
See ``docs/design/architecture/modules/config-system.md`` for full spec.
"""

from __future__ import annotations

from functools import lru_cache
from pathlib import Path

from pydantic import Field
from pydantic_settings import BaseSettings, SettingsConfigDict

# Project root: parent of the ``frontend/bridge/`` package directory.
# Relative paths in ``*_dir`` fields are resolved against this anchor
# so that the bridge behaves the same regardless of the caller's cwd.
_PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent


class BridgeSettings(BaseSettings):
    """Bridge runtime configuration.

    Configuration is loaded from (low to high priority):
    1. Code defaults (the ``default=`` values below)
    2. ``.env`` file  (see ``.env.example`` for template)
    3. System environment variables

    Attributes
    ----------
    # --- daemon connectivity ---
    daemon_host:
        Hostname or IP of the AgentFlow daemon. Default ``localhost``.
    daemon_port:
        TCP / WS port of the daemon. Default ``17891``.

    # --- bridge server bind ---
    bridge_host:
        Bind host for the FastAPI/uvicorn server. Default ``0.0.0.0``.
    bridge_port:
        Bind port for the FastAPI/uvicorn server. Default ``8199``.

    # --- filesystem paths ---
    agent_packages_dir:
        Directory under which Agent Package source trees live. May be
        absolute or relative to the project root. Default
        ``examples/agent`` (matches the AgentFlow repository layout).
    skill_hub_dir:
        Directory under which the Skill Hub central repository lives.
        May be absolute or relative to the project root. Default
        ``resource/skill_hub``.

    # --- observability / behavior ---
    log_level:
        Standard logging level name. Default ``INFO``.
    request_timeout_seconds:
        Per-request timeout (seconds) for outbound daemon calls.
        Default ``5.0``.
    daemon_status_refresh_interval:
        Seconds between background refreshes of the daemon status
        cache. Default ``10.0``.

    # --- CORS ---
    cors_allowed_origins:
        Origins permitted by CORS middleware. Defaults cover the
        local Vite dev server on both ``localhost`` and ``127.0.0.1``
        loopback aliases.
    """

    model_config = SettingsConfigDict(
        env_file=".env",
        env_file_encoding="utf-8",
        env_prefix="",
        case_sensitive=False,
        extra="ignore",
    )

    # --- 🔌 daemon connectivity ------------------------------------------------
    daemon_host: str = Field(default="localhost", description="Daemon hostname / IP.")
    daemon_port: int = Field(default=17891, ge=1, le=65535, description="Daemon TCP/WS port.")

    # --- 🌐 bridge server bind -------------------------------------------------
    bridge_host: str = Field(default="0.0.0.0", description="Bridge bind host.")
    bridge_port: int = Field(default=8199, ge=1, le=65535, description="Bridge bind port.")

    # --- 📂 filesystem paths ----------------------------------------------------
    agent_packages_dir: str = Field(
        default="examples/agent",
        description="Directory containing Agent Package source trees.",
    )
    skill_hub_dir: str = Field(
        default="resource/skill_hub",
        description="Directory containing the Skill Hub central repository.",
    )

    # --- 🪵 observability / behavior --------------------------------------------
    log_level: str = Field(default="INFO", description="Logging level name.")
    request_timeout_seconds: float = Field(
        default=5.0,
        gt=0,
        description="Per-request timeout (seconds) for outbound daemon calls.",
    )
    daemon_status_refresh_interval: float = Field(
        default=10.0,
        gt=0,
        description=(
            "Seconds between background refreshes of the daemon status "
            "cache consumed by /api/daemon/status and /api/daemon/uptime."
        ),
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
        """WebSocket URL for the daemon (RFC 6455 upgrade on the same port).

        Derived from: ``ws://{daemon_host}:{daemon_port}``
        """
        return f"ws://{self.daemon_host}:{self.daemon_port}"

    @property
    def agent_packages_path(self) -> Path:
        """Resolved absolute path to the agent packages directory.

        Relative paths are anchored at the project root (parent of
        ``frontend/bridge/``), NOT at the caller's cwd. This keeps bridge
        behavior stable across ``cd frontend/bridge && uvicorn main:app`` and
        ``uvicorn frontend.bridge.main:app`` invocations.
        """
        p = Path(self.agent_packages_dir)
        if p.is_absolute():
            return p.resolve()
        return (_PROJECT_ROOT / p).resolve()

    @property
    def skill_hub_path(self) -> Path:
        """Resolved absolute path to the Skill Hub central repository.

        Resolution rules follow ``agent_packages_path``.
        """
        p = Path(self.skill_hub_dir)
        if p.is_absolute():
            return p.resolve()
        return (_PROJECT_ROOT / p).resolve()


@lru_cache(maxsize=1)
def get_settings() -> BridgeSettings:
    """Process-wide cached :class:`BridgeSettings` instance."""
    return BridgeSettings()


# Convenience alias: ``from config import settings``
settings = get_settings()