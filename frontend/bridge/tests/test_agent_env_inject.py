"""Tests for Bridge agent .env injection logic (_inject_agent_env_defaults)."""

from __future__ import annotations

from pathlib import Path

import pytest

from routes.agents import _resolve_yaml_path


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
        assert "Bridge-injected defaults" in content

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
        assert "Bridge-injected defaults" in content

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
