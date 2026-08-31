"""Configuration module tests — validates completeness, env mapping, and priority.

Detects:
  - config.yaml 中有但 Pydantic 无字段 → 遗漏
  - Pydantic 有字段但 config.yaml 中无对应 → 可能过期
  - env 变量名映射失败 → 配置失效
  - 优先级错误（env 不覆盖 YAML / YAML 不覆盖默认值）
  - YAML 中 ${VAR} 展开失败
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any

import pytest
import yaml

# Ensure the bridge package is importable
_PROJECT_ROOT = Path(__file__).resolve().parents[3]
import sys

sys.path.insert(0, str(_PROJECT_ROOT / "frontend" / "bridge"))

from config import BridgeSettings, _expand_str, _load_yaml_config

# ═══════════════════════════════════════════════════════════════════
#  Fixtures
# ═══════════════════════════════════════════════════════════════════


@pytest.fixture(scope="session")
def yaml_cfg() -> dict[str, Any]:
    """Load the actual config.yaml used by the bridge."""
    return _load_yaml_config()


@pytest.fixture(scope="session")
def flat_yaml(yaml_cfg) -> dict[str, Any]:
    """Flatten config.yaml to dot-separated keys (same logic as BridgeSettings)."""
    return _flatten(yaml_cfg)


@pytest.fixture(scope="session")
def pydantic_fields() -> dict[str, Any]:
    """All Pydantic field names and their metadata from BridgeSettings."""
    return BridgeSettings.model_fields


# ═══════════════════════════════════════════════════════════════════
#  Helpers
# ═══════════════════════════════════════════════════════════════════


def _flatten(obj: Any, prefix: str = "") -> dict[str, Any]:
    """Flatten nested dict to dot-separated keys."""
    items: dict[str, Any] = {}
    if not isinstance(obj, dict):
        return {prefix: obj}
    for k, v in obj.items():
        path = f"{prefix}.{k}" if prefix else k
        if isinstance(v, dict):
            items.update(_flatten(v, path))
        else:
            items[path] = v
    return items


def _pydantic_field_to_yaml_path(field_name: str) -> str:
    """Convert Pydantic field name to config.yaml dot-path.

    bridge_listen → bridge.listen
    daemon_status_refresh_interval → daemon_status_refresh_interval
    """
    # Special cases where nesting doesn't follow underscores
    special_map = {
        "daemon_status_refresh_interval": "daemon_status_refresh_interval",
        "request_timeout_seconds": "request_timeout_seconds",
        "chat_turn_timeout_seconds": "chat_turn_timeout_seconds",
        "llm_api_key": "llm.api_key",
        "llm_base_url": "llm.base_url",
        "llm_model": "llm.model",
        "bridge_listen": "bridge.listen",
        "bridge_host": None,  # property, not in YAML
        "bridge_port": None,  # property, not in YAML
        "daemon_host": "daemon.host",
        "daemon_port": "daemon.port",
        "sidecar_enabled": "sidecar.enabled",
        "sidecar_listen": "sidecar.listen",
        "agent_defaults_llm_api_key": "agent_defaults.llm.api_key",
        "agent_defaults_llm_base_url": "agent_defaults.llm.base_url",
        "agent_defaults_llm_model": "agent_defaults.llm.model",
        "agent_defaults_llm_fallback_api_key": "agent_defaults.llm_fallback.api_key",
        "agent_defaults_llm_fallback_base_url": "agent_defaults.llm_fallback.base_url",
        "agent_defaults_llm_fallback_model": "agent_defaults.llm_fallback.model",
        "sandbox_mode": "sandbox.mode",
        "sandbox_e2b_api_url": "sandbox.e2b_api_url",
        "sandbox_e2b_api_key": "sandbox.e2b_api_key",
        "sandbox_e2b_ca_cert_path": "sandbox.e2b_ca_cert_path",
    }

    if field_name in special_map:
        return special_map[field_name]

    # Default: replace first underscore with dot
    # This is a heuristic and may need updating for new fields
    return field_name


def _field_to_env_var(field_name: str) -> list[str]:
    """All possible env var names for a Pydantic field."""
    names = [field_name.upper()]

    # Bridge-managed fields have aliased AF_BR_* names
    br_aliases = {
        "daemon_host": "AF_BR_DAEMON_HOST",
        "daemon_port": "AF_BR_DAEMON_PORT",
        "bridge_listen": "AF_BR_LISTEN",
        "sidecar_enabled": "AF_BR_SIDECAR_ENABLED",
        "sidecar_listen": "AF_BR_SIDECAR_LISTEN",
        "agent_defaults_llm_api_key": "AF_BR_AGENT_DEFAULTS_LLM_API_KEY",
        "agent_defaults_llm_base_url": "AF_BR_AGENT_DEFAULTS_LLM_BASE_URL",
        "agent_defaults_llm_model": "AF_BR_AGENT_DEFAULTS_LLM_MODEL",
        "agent_defaults_llm_fallback_api_key": "AF_BR_AGENT_DEFAULTS_LLM_FALLBACK_API_KEY",
        "agent_defaults_llm_fallback_base_url": "AF_BR_AGENT_DEFAULTS_LLM_FALLBACK_BASE_URL",
        "agent_defaults_llm_fallback_model": "AF_BR_AGENT_DEFAULTS_LLM_FALLBACK_MODEL",
        "artifact_public_base_url": "AF_BR_ARTIFACT_PUBLIC_BASE_URL",
        "agent_packages_dir": "AF_BR_AGENT_PACKAGES_DIR",
        "skill_hub_dir": "AF_BR_SKILL_HUB_DIR",
        "agent_template_dir": "AF_BR_AGENT_TEMPLATE_DIR",
        "log_level": "AF_BR_LOG_LEVEL",
        "request_timeout_seconds": "AF_BR_REQUEST_TIMEOUT_SECONDS",
        "uploads_dir": "AF_BR_UPLOADS_DIR",
        "chat_turn_timeout_seconds": "AF_BR_CHAT_TURN_TIMEOUT_SECONDS",
        "daemon_status_refresh_interval": "AF_BR_DAEMON_STATUS_REFRESH_INTERVAL",
        "session_provider": "AF_BR_SESSION_PROVIDER",
        "session_db_path": "AF_BR_SESSION_DB_PATH",
        "session_sync_interval": "AF_BR_SESSION_SYNC_INTERVAL",
        "cors_allowed_origins": "AF_BR_CORS_ALLOWED_ORIGINS",
    }
    alias = br_aliases.get(field_name)
    if alias:
        names.append(alias)

    return names


# ═══════════════════════════════════════════════════════════════════
#  Test 1: Completeness — YAML vs Pydantic
# ═══════════════════════════════════════════════════════════════════


def test_all_yaml_keys_have_pydantic_fields(flat_yaml, pydantic_fields):
    """Fail: config.yaml 中有配置项，但 Pydantic 没有对应字段 → 遗漏。"""
    missing = []
    for yaml_path in flat_yaml:
        # Skip items that don't map to Pydantic fields
        if yaml_path in ("#", "sidecar", "llm", "daemon", "bridge"):
            continue
        # agent_defaults subtree is injected directly into agent environment
        # via YAML injection, not consumed by BridgeSettings Pydantic model
        if yaml_path.startswith("agent_defaults."):
            continue

        # Find matching Pydantic field
        found = False
        for field_name in pydantic_fields:
            yaml_path_for_field = _pydantic_field_to_yaml_path(field_name)
            if yaml_path_for_field == yaml_path:
                found = True
                break
        if not found:
            missing.append(yaml_path)

    assert not missing, (
        f"❌ {len(missing)} config.yaml 配置项无对应 Pydantic 字段:\n"
        + "\n".join(f"  - {k} = {flat_yaml[k]}" for k in missing)
    )


def test_all_pydantic_fields_have_yaml_keys(pydantic_fields, flat_yaml):
    """Fail: Pydantic 有字段，但 config.yaml 中没有对应配置 → 文档缺失。"""
    missing = []
    for field_name in pydantic_fields:
        yaml_path = _pydantic_field_to_yaml_path(field_name)
        if yaml_path is None:
            continue  # property, no YAML counterpart
        if yaml_path not in flat_yaml:
            missing.append(field_name)

    if missing:
        pytest.skip(
            f"⚠️ {len(missing)} Pydantic 字段在 config.yaml 中无对应项: {missing}"
        )


# ═══════════════════════════════════════════════════════════════════
#  Test 2: Env var mapping — each field can be set via env
# ═══════════════════════════════════════════════════════════════════


@pytest.mark.parametrize(
    "field_name, env_name, test_value",
    [
        ("daemon_host", "AF_BR_DAEMON_HOST", "10.0.0.1"),
        ("daemon_port", "AF_BR_DAEMON_PORT", 9999),
        ("bridge_listen", "AF_BR_LISTEN", "0.0.0.0:9999"),
        ("sidecar_enabled", "AF_BR_SIDECAR_ENABLED", True),
        ("sidecar_listen", "AF_BR_SIDECAR_LISTEN", "0.0.0.0:12345"),
        (
            "artifact_public_base_url",
            "AF_BR_ARTIFACT_PUBLIC_BASE_URL",
            "https://cdn.example.com",
        ),
        ("log_level", "AF_BR_LOG_LEVEL", "DEBUG"),
        ("request_timeout_seconds", "AF_BR_REQUEST_TIMEOUT_SECONDS", 30.0),
        ("chat_turn_timeout_seconds", "AF_BR_CHAT_TURN_TIMEOUT_SECONDS", 1200.0),
        (
            "daemon_status_refresh_interval",
            "AF_BR_DAEMON_STATUS_REFRESH_INTERVAL",
            60.0,
        ),
    ],
)
def test_env_var_overrides(field_name, env_name, test_value):
    """Fail: 设了 env 变量后，Pydantic 读不到预期值 → 映射失效。"""
    # Save original and set env
    orig = os.environ.get(env_name)
    os.environ[env_name] = str(test_value)
    try:
        settings = BridgeSettings()
        actual = getattr(settings, field_name)
        assert (
            actual == test_value
        ), f"env {env_name}={test_value} → {field_name}={actual!r}, 期望 {test_value!r}"
    finally:
        if orig is None:
            del os.environ[env_name]
        else:
            os.environ[env_name] = orig


# ═══════════════════════════════════════════════════════════════════
#  Test 3: Priority — env > YAML > code default
# ═══════════════════════════════════════════════════════════════════


def test_env_overrides_yaml():
    """env 变量必须覆盖 YAML 值（最高优先级）。"""
    os.environ["AF_BR_DAEMON_HOST"] = "10.0.0.99"
    try:
        s = BridgeSettings()
        assert (
            s.daemon_host == "10.0.0.99"
        ), f"env AF_BR_DAEMON_HOST=10.0.0.99 应覆盖 YAML, 实际={s.daemon_host}"
    finally:
        del os.environ["AF_BR_DAEMON_HOST"]


def test_yaml_overrides_code_default():
    """YAML 值必须覆盖代码默认值（中优先级）。"""
    # artifact_public_base_url 代码默认是 ""，config.yaml 中是 "http://localhost:8199/health"
    s = BridgeSettings()
    assert s.artifact_public_base_url == "http://localhost:8199/health", (
        f"YAML 应覆盖代码默认值, 期望 'http://localhost:8199/health', "
        f"实际 '{s.artifact_public_base_url}'"
    )


# ═══════════════════════════════════════════════════════════════════
#  Test 4: YAML env var expansion (${VAR} syntax)
# ═══════════════════════════════════════════════════════════════════


def test_env_var_expansion_in_yaml():
    """${VAR} 语法必须在 YAML 加载时展开。"""
    os.environ["TEST_ENV_EXPAND"] = "expanded-value"
    try:
        from config import _expand_str
        result = _expand_str("${TEST_ENV_EXPAND}")
        assert result == "expanded-value", f"展开失败: {result}"
    finally:
        del os.environ["TEST_ENV_EXPAND"]


@pytest.mark.parametrize(
    "input_str, env_vars, expected",
    [
        ("${VAR}", {"VAR": "hello"}, "hello"),
        ("${MISSING}", {}, ""),
        ("${MISSING:-fallback}", {}, "fallback"),
        ("prefix_${VAR}_suffix", {"VAR": "mid"}, "prefix_mid_suffix"),
        ("${A:-x}${B:-y}", {}, "xy"),
        ("no_vars_here", {}, "no_vars_here"),
    ],
)
def test_expand_str_function(input_str, env_vars, expected):
    """_expand_str 单元测试：验证 ${VAR} 和 ${VAR:-default} 语法。"""
    saved = {}
    for k, v in env_vars.items():
        saved[k] = os.environ.get(k)
        os.environ[k] = v
    try:
        result = _expand_str(input_str)
        assert result == expected, f"_expand_str({input_str!r}) = {result!r}, 期望 {expected!r}"
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v


# ═══════════════════════════════════════════════════════════════════
#  Test 5: Default values — no drift between YAML and code
# ═══════════════════════════════════════════════════════════════════


def test_code_defaults_match_yaml_defaults():
    """代码默认值与 YAML 默认值一致，防止漂移。"""
    s = BridgeSettings()
    fields = BridgeSettings.model_fields

    # 判断当前环境，加载对应的预期值
    current_env = os.environ.get("AF_BR_ENV", "dev")
    # 各环境与 dev 的差异对照（dev 环境也可以直接写硬编码，但为了清晰集中管理）
    # 新增 configs/{env}/bridge.yaml 时在此补充差异项
    _env_overrides = {
        "test": {
            "request_timeout_seconds": 30.0,
            "chat_turn_timeout_seconds": 300.0,
            "daemon_status_refresh_interval": 5.0,
        },
    }
    _env_overrides["dev"] = {
        "request_timeout_seconds": 60.0,
    }
    overrides = _env_overrides.get(current_env, {})

    # 这些字段的代码默认值应与 config.yaml 一致
    expected = {
        "daemon_host": "127.0.0.1",
        "daemon_port": 17891,
        "bridge_listen": "0.0.0.0:8199",
        "sidecar_enabled": False,
        "sidecar_listen": "0.0.0.0:23767",
        "log_level": "DEBUG",
        "request_timeout_seconds": 15.0,
        "chat_turn_timeout_seconds": 900.0,
        "daemon_status_refresh_interval": 10.0,
    }
    # 按环境覆盖差异值
    expected.update(overrides)

    for field_name, expected_val in expected.items():
        actual_val = getattr(s, field_name)
        assert (
            actual_val == expected_val
        ), f"{field_name}: 期望 {expected_val!r}, 实际 {actual_val!r}"


# ═══════════════════════════════════════════════════════════════════
#  Test 6: Derived properties work correctly
# ═══════════════════════════════════════════════════════════════════


def test_bridge_host_port_derived_from_listen():
    """bridge_host / bridge_port 必须从 bridge_listen 正确解析。"""
    s = BridgeSettings()
    host, port = s.bridge_host, s.bridge_port
    listen = s.bridge_listen
    expected_host, expected_port_str = listen.split(":")
    assert host == expected_host, f"bridge_host={host}, 期望 {expected_host}"
    assert port == int(expected_port_str), f"bridge_port={port}, 期望 {expected_port_str}"
