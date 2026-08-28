"""Security and identity validation for Skill IDs used as filesystem paths."""

from __future__ import annotations

import json
import os
from types import SimpleNamespace

import pytest

from agentflow_runtime import coordinator_decide, skill_executor
from agentflow_runtime.skills_loader import load_skill_md
from agentflow_runtime.tool_registry import _LocalToolRegistry, _load_dir


class _ManifestBus:
    def __init__(self, manifest):
        self.manifest = manifest

    def is_connected(self):
        return True

    def get_global(self, _key):
        return json.dumps(self.manifest)


@pytest.mark.parametrize(
    ("manifest", "expected_error"),
    [
        (
            {"skill_name": "../outside", "prompt": "run", "load_skills": []},
            "manifest_invalid_skill_name",
        ),
        (
            {
                "skill_name": "safe-skill",
                "prompt": "run",
                "load_skills": ["../outside"],
            },
            "manifest_invalid_load_skill_name",
        ),
    ],
)
def test_dispatch_manifest_rejects_unsafe_skill_ids(manifest, expected_error):
    _manifest, error = skill_executor._read_dispatch_manifest(
        _ManifestBus(manifest), "dispatch:test:1"
    )
    assert error == expected_error


def test_run_skills_rejects_unregistered_load_skill():
    error = coordinator_decide._validate_run_skills_args(
        {
            "skills": [{
                "name": "outfit-stylist",
                "prompt": "recommend an outfit",
                "load_skills": ["unregistered-skill"],
            }],
        },
        ["outfit-stylist"],
    )
    assert "load_skills[0]" in error
    assert "unregistered-skill" in error


def test_skill_loader_rejects_traversal_absolute_and_escaping_symlink(
    tmp_path, monkeypatch,
):
    skills_base = tmp_path / "skills"
    valid_dir = skills_base / "valid-skill"
    valid_dir.mkdir(parents=True)
    (valid_dir / "SKILL.md").write_text("valid content", encoding="utf-8")

    outside_dir = tmp_path / "outside"
    outside_dir.mkdir()
    (outside_dir / "SKILL.md").write_text("secret content", encoding="utf-8")
    os.symlink(outside_dir, skills_base / "linked-skill")
    file_link_dir = skills_base / "file-link-skill"
    file_link_dir.mkdir()
    os.symlink(outside_dir / "SKILL.md", file_link_dir / "SKILL.md")

    monkeypatch.setattr(
        skill_executor, "_get_skills_base", lambda: str(skills_base)
    )
    monkeypatch.delenv("AF_SANDBOX_MODE", raising=False)

    assert load_skill_md("valid-skill") == "valid content"
    assert load_skill_md("../outside") is None
    assert load_skill_md(str(outside_dir)) is None
    assert load_skill_md("linked-skill") is None
    assert load_skill_md("file-link-skill") is None


def test_tool_registry_does_not_load_scripts_through_escaping_symlink(
    tmp_path, monkeypatch,
):
    skills_base = tmp_path / "skills"
    skills_base.mkdir()
    outside_dir = tmp_path / "outside-skill"
    scripts_dir = outside_dir / "scripts"
    scripts_dir.mkdir(parents=True)
    (scripts_dir / "unsafe.py").write_text(
        "TOOL_NAME='unsafe'\nTOOL_DESCRIPTION=''\nTOOL_PARAMS={}\n"
        "def execute(args, sandbox_endpoint=None): return 'unsafe'\n",
        encoding="utf-8",
    )
    os.symlink(outside_dir, skills_base / "linked-skill")
    safe_skill_dir = skills_base / "safe-skill"
    safe_skill_dir.mkdir()
    os.symlink(scripts_dir, safe_skill_dir / "scripts")

    missing_dir = tmp_path / "missing"
    loaded_dirs = []
    monkeypatch.setattr(
        skill_executor, "_get_skills_base", lambda: str(skills_base)
    )
    monkeypatch.setattr(
        skill_executor, "_get_builtin_tools_dir", lambda: str(missing_dir)
    )
    monkeypatch.setattr(
        skill_executor, "_get_agent_tools_dir", lambda: str(missing_dir)
    )
    monkeypatch.setattr(
        skill_executor,
        "_load_dir",
        lambda _registry, directory, **_kwargs: loaded_dirs.append(directory),
    )
    monkeypatch.setattr(skill_executor, "MCP_AVAILABLE", False)

    _LocalToolRegistry("linked-skill")
    _LocalToolRegistry("safe-skill")

    assert loaded_dirs == []


def test_tool_loader_rejects_symlinked_python_file(tmp_path):
    tools_dir = tmp_path / "tools"
    tools_dir.mkdir()
    outside_tool = tmp_path / "outside.py"
    outside_tool.write_text(
        "TOOL_NAME='outside'\nTOOL_DESCRIPTION=''\nTOOL_PARAMS={}\n"
        "def execute(args, sandbox_endpoint=None): return 'outside'\n",
        encoding="utf-8",
    )
    os.symlink(outside_tool, tools_dir / "outside.py")
    registry = SimpleNamespace(_modules={}, _tools=[])

    _load_dir(registry, str(tools_dir))

    assert registry._tools == []
    assert registry._modules == {}
