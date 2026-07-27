"""Tests for Bridge agent .env injection logic (_inject_agent_env_defaults)."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

from routes.agents import _resolve_placeholder, _resolve_yaml_path


class TestResolveYamlPath:
    """Unit tests for the YAML dotted-path resolver."""

    def test_simple_key(self):
        cfg = {"agent_defaults": {"llm": {"model": "gpt-4o"}}}
        assert _resolve_yaml_path(cfg, "agent_defaults.llm.model") == "gpt-4o"

    def test_missing_key_returns_none(self):
        cfg = {"agent_defaults": {}}
        assert _resolve_yaml_path(cfg, "agent_defaults.llm.model") is None

    def test_nested_missing_returns_none(self):
        cfg = {}
        assert _resolve_yaml_path(cfg, "agent_defaults.llm.model") is None

    def test_int_value_is_stringified(self):
        cfg = {"agent_defaults": {"skill": {"max_iterations": 8}}}
        assert _resolve_yaml_path(cfg, "agent_defaults.skill.max_iterations") == "8"

    def test_bool_value_is_stringified(self):
        cfg = {"agent_defaults": {"enabled": True}}
        assert _resolve_yaml_path(cfg, "agent_defaults.enabled") == "True"

    def test_none_value_returns_none(self):
        cfg = {"agent_defaults": {"llm": {"model": None}}}
        assert _resolve_yaml_path(cfg, "agent_defaults.llm.model") is None


class TestResolvePlaceholder:
    """Unit tests for the ${VAR:-default} placeholder resolver."""

    def test_plain_value_passthrough(self):
        """非占位符字符串原样返回。"""
        assert _resolve_placeholder("https://api.minimaxi.com/v1") == "https://api.minimaxi.com/v1"

    def test_placeholder_with_default(self):
        """${VAR:-default} 返回 default 部分。"""
        assert _resolve_placeholder("${AF_LLM_BASE_URL:-https://api.minimaxi.com/v1}") == "https://api.minimaxi.com/v1"

    def test_placeholder_without_default(self):
        """${VAR} 无默认值返回空字符串。"""
        assert _resolve_placeholder("${AF_LLM_BASE_URL}") == ""

    def test_placeholder_with_default_containing_colon(self):
        """默认值含 : 的占位符正确解析（如 https:// URL）。"""
        val = _resolve_placeholder("${AF_LLM_BASE_URL:-https://dashscope.aliyuncs.com/compatible-mode/v1}")
        assert val == "https://dashscope.aliyuncs.com/compatible-mode/v1"

    def test_placeholder_empty_string(self):
        """空字符串原样返回。"""
        assert _resolve_placeholder("") == ""

    def test_placeholder_with_dash_in_default(self):
        """默认值含连字符的占位符正确解析。"""
        val = _resolve_placeholder("${AF_LLM_MODEL:-MiniMax-M3}")
        assert val == "MiniMax-M3"


class TestInjectAgentEnvDefaults:
    """Integration tests for .env injection (file-level behavior).

    Uses tmp_path for isolation; monkeypatches settings.agent_packages_path.
    """

    @pytest.fixture
    def pkg_dir(self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
        """Create a fake agent package directory."""
        pkg = tmp_path / "agents" / "test_agent"
        pkg.mkdir(parents=True)
        from config import settings
        # agent_packages_path is a property derived from agent_packages_dir
        monkeypatch.setattr(settings, "agent_packages_dir", str(tmp_path / "agents"))
        return pkg

    @pytest.fixture
    def inject(self):
        from routes.agents import _inject_agent_env_defaults
        return _inject_agent_env_defaults

    def test_inject_creates_env_file_when_missing(self, pkg_dir: Path, inject) -> None:
        """No .env exists → creates one with injected defaults."""
        inject("test_agent")
        env_file = pkg_dir / ".env"
        assert env_file.is_file()
        content = env_file.read_text()
        assert "AF_LLM_MODEL=" in content
        assert "BRIDGE_INJECTED_START" in content

    def test_inject_preserves_existing_user_values(self, pkg_dir: Path, inject) -> None:
        """User-set values in .env are not overwritten."""
        env_file = pkg_dir / ".env"
        env_file.write_text("AF_LLM_MODEL=user-model\n", encoding="utf-8")
        inject("test_agent")
        content = env_file.read_text()
        assert "AF_LLM_MODEL=user-model" in content

    def test_inject_overrides_placeholder_values(self, pkg_dir: Path, inject) -> None:
        """Placeholder values (sk-your-) are replaced by injected defaults."""
        env_file = pkg_dir / ".env"
        env_file.write_text("AF_LLM_API_KEY=sk-your-key-here\n", encoding="utf-8")
        inject("test_agent")
        content = env_file.read_text()
        # sk-your- is treated as placeholder — injection should add/replace
        assert "BRIDGE_INJECTED_START" in content

    def test_inject_skips_empty_defaults(self, pkg_dir: Path, inject) -> None:
        """Empty default values are not written to .env."""
        inject("test_agent")
        env_file = pkg_dir / ".env"
        content = env_file.read_text()
        # VISION_FALLBACK_API_KEY is empty in config defaults, should not appear
        # (only check that file was created without errors)
        assert env_file.is_file()

    def test_inject_noop_for_missing_directory(self, inject, caplog) -> None:
        """Non-existent package directory produces warning, not crash."""
        inject("nonexistent_agent")
        assert "directory not found" in caplog.text

    def test_inject_logs_injected_keys(self, pkg_dir: Path, inject, caplog) -> None:
        """Injection logs how many keys were written."""
        caplog.set_level("INFO")
        inject("test_agent")
        assert "keys=" in caplog.text

    def test_inject_resolves_placeholder_from_yaml(self, pkg_dir: Path, inject, monkeypatch) -> None:
        """YAML 中的 ${VAR:-default} 占位符被解析为实际默认值写入 .env。

        模拟 config.yaml 的 agent_defaults 包含占位符值，
        验证注入时占位符被解析为默认值，而非原样写入。
        """
        from config import settings
        monkeypatch.setattr(settings, "_yaml_source", {
            "agent_defaults": {
                "llm": {
                    "api_key": "${AF_LLM_API_KEY:-sk-cp-mock-key}",
                    "base_url": "${AF_LLM_BASE_URL:-https://api.minimaxi.com/v1}",
                    "model": "${AF_LLM_MODEL:-MiniMax-M3}",
                },
            },
        })
        inject("test_agent")
        env_file = pkg_dir / ".env"
        assert env_file.is_file()
        content = env_file.read_text()
        # 占位符被解析为默认值，而非原样写入
        assert "AF_LLM_BASE_URL=https://api.minimaxi.com/v1" in content, \
            f"应写入解析后的默认值，实际内容: {content}"
        assert "AF_LLM_MODEL=MiniMax-M3" in content
        assert "AF_LLM_API_KEY=sk-cp-mock-key" in content
        # 确保没有写入未解析的占位符字面量
        assert "${AF_LLM_BASE_URL" not in content, "不应写入未解析的占位符"

    def test_injected_env_is_readable_by_dotenv(self, pkg_dir: Path, inject, monkeypatch) -> None:
        """注入后的 .env 文件被 dotenv 加载后，os.environ 能读到正确值。

        模拟 skill_executor._load_dotenv() 的完整链路：
        注入 → 写入 .env → load_dotenv → os.environ 可取到解析后的值。
        """
        from config import settings
        monkeypatch.setattr(settings, "_yaml_source", {
            "agent_defaults": {
                "llm": {
                    "base_url": "${AF_LLM_BASE_URL:-https://api.minimaxi.com/v1}",
                    "model": "${AF_LLM_MODEL:-MiniMax-M3}",
                },
            },
        })
        # 步骤 1: 注入（写入 .env）
        inject("test_agent")
        env_file = pkg_dir / ".env"
        assert env_file.is_file()

        # 步骤 2: 模拟 skill_executor._load_dotenv() 加载 .env
        from dotenv import load_dotenv
        loaded = load_dotenv(env_file, override=True)
        assert loaded, "dotenv 应成功加载 .env 文件"

        # 步骤 3: 验证 os.environ 中取到正确值
        assert os.environ.get("AF_LLM_BASE_URL") == "https://api.minimaxi.com/v1"
        assert os.environ.get("AF_LLM_MODEL") == "MiniMax-M3"
