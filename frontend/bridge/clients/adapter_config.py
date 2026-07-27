"""Generic config reader (clients use this to load config.yaml)."""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any

import yaml

_DEFAULT_CONFIG_PATHS = [
    "config.yaml",
    "sidecar/config.yaml",
    "/opt/agentflow/config.yaml",
]


def load_config(config_path: str = "") -> dict[str, Any]:
    """Load configuration from YAML file.

    Args:
        config_path: Path to config.yaml. If empty, search default locations.

    Returns:
        Dict with all config values. Missing keys fall back to empty string.
    """
    # ① Find config file
    paths = (
        [config_path]
        if config_path
        else [str(p) for p in map(Path, _DEFAULT_CONFIG_PATHS) if Path(p).exists()]
    )

    if not paths:
        return {}

    resolved = None
    for p in paths:
        if Path(p).exists():
            resolved = p
            break

    if not resolved:
        return {}

    with open(resolved) as f:
        return yaml.safe_load(f) or {}


def get(cfg: dict[str, Any], *keys: str, default: Any = "") -> Any:
    """Safely traverse nested dict. Returns default on missing key."""
    current = cfg
    for k in keys:
        if not isinstance(current, dict):
            return default
        current = current.get(k)
        if current is None:
            return default
    return current


def get_env_or_config(
    cfg: dict[str, Any],
    env_var: str,
    *config_keys: str,
    default: Any = "",
) -> str:
    """Get value from env var first, then config file, then default."""
    val = os.environ.get(env_var)
    if val:
        return val
    val = get(cfg, *config_keys, default=default)
    return str(val) if val else str(default)
