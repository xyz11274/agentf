"""
Regression tests for built-in tool path resolution.

These tests guard against the CRITICAL issue where harness/tools/*.py were
moved to the agentflow-runtime package but still used ``__file__``-relative
paths that no longer pointed to the correct agent package directory.

The fix: tools read ``AF_AGENT_PACKAGE_DIR`` environment variable (injected by
``_LocalToolRegistry`` before loading), falling back to ``__file__``-relative
paths for direct-import scenarios.

design: see ``packages/agentflow-runtime/src/agentflow_runtime/harness/tools/``
"""

from __future__ import annotations

import importlib.util as _importlib_util
import os
import sys
import tempfile
from pathlib import Path

import pytest


# ── Helpers ────────────────────────────────────────────────────────────────

_HERE = Path(__file__).resolve().parent
_TOOLS_SRC = _HERE.parent / "src" / "agentflow_runtime" / "harness" / "tools"


def _import_tool_module(name: str) -> object:
    """Dynamically import a tool module from the harness/tools source dir."""
    py_file = _TOOLS_SRC / f"{name}.py"
    assert py_file.is_file(), f"Tool source not found: {py_file}"
    spec = _importlib_util.spec_from_file_location(f"_test_tool_{name}", str(py_file))
    assert spec is not None and spec.loader is not None
    mod = _importlib_util.module_from_spec(spec)
    # Clean env before loading so the module uses its fallback path
    os.environ.pop("AF_AGENT_PACKAGE_DIR", None)
    spec.loader.exec_module(mod)
    return mod


# ═══════════════════════════════════════════════════════════════════════════
# Tests: bash.py — _get_agent_package_dir()
# ═══════════════════════════════════════════════════════════════════════════

class TestBashAgentPackageDir:
    """bash.py 的 _get_agent_package_dir() 必须正确返回 agent 包根目录。"""

    def test_fallback_when_no_env(self):
        """未设置 AF_AGENT_PACKAGE_DIR 时，fallback 到 __file__ 的 parents[2]。
        此时路径应为 runtime 包根 (src/)，不是 agent 包根，但不会崩溃。"""
        mod = _import_tool_module("bash")
        # 没有 env var → 使用 _DEFAULT_AGENT_DIR（parents[2]）
        result = mod._get_agent_package_dir()
        assert isinstance(result, str)
        assert os.path.isabs(result)
        # 应指向 runtime 包 src/ 目录
        assert "agentflow_runtime" in result

    def test_respects_env_var(self):
        """设置 AF_AGENT_PACKAGE_DIR 后，_get_agent_package_dir() 返回该值。"""
        mod = _import_tool_module("bash")
        test_dir = "/tmp/test-agent-pkg"
        os.environ["AF_AGENT_PACKAGE_DIR"] = test_dir
        try:
            result = mod._get_agent_package_dir()
            assert result == test_dir
        finally:
            os.environ.pop("AF_AGENT_PACKAGE_DIR", None)

    def test_bash_execute_cwd_is_agent_package_dir(self, tmp_path, monkeypatch):
        """bash 工具的 cwd 必须等于 AF_AGENT_PACKAGE_DIR。"""
        mod = _import_tool_module("bash")
        test_dir = str(tmp_path)
        monkeypatch.setenv("AF_ALLOW_UNSAFE_LOCAL_SECRET_TOOLS", "1")
        os.environ["AF_AGENT_PACKAGE_DIR"] = test_dir
        try:
            result = mod.execute({"command": "pwd"})
            import json
            data = json.loads(result)
            assert data.get("tool") == "bash"
            assert data.get("exit_code") == 0
            assert data["stdout"].strip() == test_dir
        finally:
            os.environ.pop("AF_AGENT_PACKAGE_DIR", None)


# ═══════════════════════════════════════════════════════════════════════════
# Tests: load_skill.py — _get_skills_dir()
# ═══════════════════════════════════════════════════════════════════════════

class TestLoadSkillSkillsDir:
    """load_skill.py 的 _get_skills_dir() 必须正确找到 skill 目录。"""

    def test_fallback_when_no_env(self):
        """未设置 AF_AGENT_PACKAGE_DIR 时，fallback 到 __file__ 相对路径。"""
        mod = _import_tool_module("load_skill")
        result = mod._get_skills_dir()
        assert isinstance(result, str)
        assert os.path.isabs(result)
        # 应指向 runtime 包内的 harness/skills/
        assert result.endswith("harness/skills")

    def test_respects_env_var(self):
        """设置 AF_AGENT_PACKAGE_DIR 后，_get_skills_dir() 返回 agent 包的 skills 目录。"""
        mod = _import_tool_module("load_skill")
        test_dir = "/tmp/test-agent-pkg"
        os.environ["AF_AGENT_PACKAGE_DIR"] = test_dir
        try:
            result = mod._get_skills_dir()
            assert result == os.path.join(test_dir, "harness", "skills")
        finally:
            os.environ.pop("AF_AGENT_PACKAGE_DIR", None)

    def test_load_skill_from_agent_package(self, tmp_path):
        """load_skill 能从 AF_AGENT_PACKAGE_DIR 指定的 agent 包中找到 SKILL.md。"""
        # 构造一个假的 agent 包结构
        skills_dir = tmp_path / "harness" / "skills" / "test-skill"
        skills_dir.mkdir(parents=True)
        skill_md = skills_dir / "SKILL.md"
        skill_md.write_text("name: test-skill\ndescription: A test skill")

        mod = _import_tool_module("load_skill")
        os.environ["AF_AGENT_PACKAGE_DIR"] = str(tmp_path)
        try:
            result = mod.execute({"skill_name": "test-skill"})
            import json
            data = json.loads(result)
            assert data["skill_name"] == "test-skill"
            assert "name: test-skill" in data["content"]
        finally:
            os.environ.pop("AF_AGENT_PACKAGE_DIR", None)

    def test_load_skill_not_found(self, tmp_path):
        """load_skill 在 skill 不存在时返回错误。"""
        mod = _import_tool_module("load_skill")
        os.environ["AF_AGENT_PACKAGE_DIR"] = str(tmp_path)
        try:
            result = mod.execute({"skill_name": "nonexistent"})
            import json
            data = json.loads(result)
            assert "error" in data
            assert "not found" in data["error"]
        finally:
            os.environ.pop("AF_AGENT_PACKAGE_DIR", None)


# ═══════════════════════════════════════════════════════════════════════════
# Tests: list_skills.py — _get_skills_dir()
# ═══════════════════════════════════════════════════════════════════════════

class TestListSkillsSkillsDir:
    """list_skills.py 的 _get_skills_dir() 必须正确找到 skill 目录。"""

    def test_fallback_when_no_env(self):
        """未设置 AF_AGENT_PACKAGE_DIR 时，fallback 到 __file__ 相对路径。"""
        mod = _import_tool_module("list_skills")
        result = mod._get_skills_dir()
        assert isinstance(result, str)
        assert os.path.isabs(result)
        assert result.endswith("harness/skills")

    def test_respects_env_var(self):
        """设置 AF_AGENT_PACKAGE_DIR 后，_get_skills_dir() 返回 agent 包的 skills 目录。"""
        mod = _import_tool_module("list_skills")
        test_dir = "/tmp/test-agent-pkg"
        os.environ["AF_AGENT_PACKAGE_DIR"] = test_dir
        try:
            result = mod._get_skills_dir()
            assert result == os.path.join(test_dir, "harness", "skills")
        finally:
            os.environ.pop("AF_AGENT_PACKAGE_DIR", None)

    def test_list_skills_from_agent_package(self, tmp_path):
        """list_skills 能列出 AF_AGENT_PACKAGE_DIR 指定 agent 包中的 skill。"""
        # 构造两个假 skill
        for name in ("alpha", "beta"):
            (tmp_path / "harness" / "skills" / name / "SKILL.md").parent.mkdir(parents=True, exist_ok=True)
            (tmp_path / "harness" / "skills" / name / "SKILL.md").write_text(f"name: {name}")

        mod = _import_tool_module("list_skills")
        os.environ["AF_AGENT_PACKAGE_DIR"] = str(tmp_path)
        try:
            result = mod.execute({})
            import json
            data = json.loads(result)
            names = {s["name"] for s in data["skills"]}
            assert "alpha" in names
            assert "beta" in names
        finally:
            os.environ.pop("AF_AGENT_PACKAGE_DIR", None)

    def test_list_skills_empty_dir(self, tmp_path):
        """list_skills 在无 skill 的 agent 包中返回空列表。"""
        (tmp_path / "harness" / "skills").mkdir(parents=True)

        mod = _import_tool_module("list_skills")
        os.environ["AF_AGENT_PACKAGE_DIR"] = str(tmp_path)
        try:
            result = mod.execute({})
            import json
            data = json.loads(result)
            assert data["skills"] == []
        finally:
            os.environ.pop("AF_AGENT_PACKAGE_DIR", None)


# ═══════════════════════════════════════════════════════════════════════════
# Tests: skill_executor.py — _LocalToolRegistry injects env var
# ═══════════════════════════════════════════════════════════════════════════

class TestLocalToolRegistryInjectsEnv:
    """_LocalToolRegistry 在加载工具前必须设置 AF_AGENT_PACKAGE_DIR。"""

    def test_env_set_before_tool_load(self, monkeypatch):
        """验证 _LocalToolRegistry.__init__ 设置了环境变量。"""
        # 模拟 AGENT_PACKAGE_DIR
        from agentflow_runtime.skill_executor import _LocalToolRegistry, AGENT_PACKAGE_DIR

        # 清空 env var，确认 registry 初始化后会被设置
        monkeypatch.delenv("AF_AGENT_PACKAGE_DIR", raising=False)
        registry = _LocalToolRegistry()
        assert os.environ.get("AF_AGENT_PACKAGE_DIR") == AGENT_PACKAGE_DIR