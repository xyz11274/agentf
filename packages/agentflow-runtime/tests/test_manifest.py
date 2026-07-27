"""Tests for generate_manifest() — agent-package.manifest.jsonc generation (T-004).

Tests the scanning rules from §3.2:
  ✅ harness/skills/**       — always included if exists
  ✅ harness/tools/**        — included if exists (user overrides)
  ✅ data/**                 — included if exists
  ✅ requirements.txt        — included if exists
  ❌ agent.json, package.json, *.json workflows, nodes/, agent.md, .env, setup.sh
"""

import json
import os
import tempfile
from pathlib import Path

from agentflow_runtime.common.manifest import generate_manifest, load_manifest

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

MANIFEST_NAME = "agent-package.manifest.jsonc"


def _make_package(tmp: Path, files: list[str]) -> None:
    """Create files (relative paths) under ``tmp``, including parent dirs."""
    for rel in files:
        full = tmp / rel
        full.parent.mkdir(parents=True, exist_ok=True)
        full.touch()


def _manifest_path(tmp: Path) -> Path:
    return tmp / MANIFEST_NAME


# ---------------------------------------------------------------------------
# Tests — generate
# ---------------------------------------------------------------------------


def test_manifest_generates_on_first_call():
    """No manifest exists → generates one after scanning."""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        _make_package(tmp, [
            "agent.json",
            "package.json",
            "coordinator_workflow.json",
            "harness/skills/my-skill/SKILL.md",
            "data/guide.json",
            "requirements.txt",
        ])
        manifest = generate_manifest(tmp)
        assert manifest is not None
        assert _manifest_path(tmp).exists()


def test_manifest_includes_skills():
    """harness/skills/** should be in the file list."""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        _make_package(tmp, [
            "agent.json",
            "harness/skills/foo/SKILL.md",
            "harness/skills/foo/scripts/tool.py",
        ])
        manifest = generate_manifest(tmp)
        files = {f["path"] for f in manifest["files"]}
        assert "harness/skills/foo/SKILL.md" in files
        assert "harness/skills/foo/scripts/tool.py" in files


def test_manifest_excludes_daemon_files():
    """agent.json, package.json, workflow *.json, nodes/, agent.md should NOT
    be in the file list."""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        _make_package(tmp, [
            "agent.json",
            "package.json",
            "coordinator_workflow.json",
            "skill_workflow.json",
            "nodes/skill_executor.py",
            "nodes/coordinator_decide.py",
            "agent.md",
            "harness/skills/my-skill/SKILL.md",
        ])
        manifest = generate_manifest(tmp)
        files = {f["path"] for f in manifest["files"]}
        assert "agent.json" not in files
        assert "package.json" not in files
        assert "coordinator_workflow.json" not in files
        assert "skill_workflow.json" not in files
        assert "nodes/skill_executor.py" not in files
        assert "agent.md" not in files


def test_manifest_includes_data():
    """data/** should be included if present."""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        _make_package(tmp, [
            "agent.json",
            "data/guide.json",
            "data/images/logo.png",
            "harness/skills/my-skill/SKILL.md",
        ])
        manifest = generate_manifest(tmp)
        files = {f["path"] for f in manifest["files"]}
        assert "data/guide.json" in files
        assert "data/images/logo.png" in files


def test_manifest_includes_requirements():
    """requirements.txt should be included if present."""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        _make_package(tmp, [
            "agent.json",
            "requirements.txt",
            "harness/skills/my-skill/SKILL.md",
        ])
        manifest = generate_manifest(tmp)
        files = {f["path"] for f in manifest["files"]}
        assert "requirements.txt" in files


def test_manifest_includes_tools_overrides():
    """harness/tools/** should be included if present (user overrides)."""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        _make_package(tmp, [
            "agent.json",
            "harness/tools/bash.py",
            "harness/tools/custom_tool.py",
            "harness/skills/my-skill/SKILL.md",
        ])
        manifest = generate_manifest(tmp)
        files = {f["path"] for f in manifest["files"]}
        assert "harness/tools/bash.py" in files
        assert "harness/tools/custom_tool.py" in files


def test_manifest_with_manifest_already_exists():
    """If manifest already exists, generate_manifest returns it directly without rescanning."""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        _make_package(tmp, [
            "agent.json",
            "data/old.json",
            "harness/skills/my-skill/SKILL.md",
        ])
        # First call generates
        m1 = generate_manifest(tmp)
        # Second call should read existing, not rescan
        m2 = generate_manifest(tmp)
        assert m1 == m2


def test_manifest_generated_at_is_set():
    """generated_at timestamp should be set."""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        _make_package(tmp, [
            "agent.json",
            "harness/skills/my-skill/SKILL.md",
        ])
        manifest = generate_manifest(tmp)
        assert "generated_at" in manifest
        assert manifest["generated_at"]  # not empty


# ---------------------------------------------------------------------------
# Tests — load_manifest
# ---------------------------------------------------------------------------


def test_load_manifest_exists():
    """load_manifest returns parsed manifest when file exists."""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        _make_package(tmp, [
            "agent.json",
            "harness/skills/my-skill/SKILL.md",
        ])
        generate_manifest(tmp)
        loaded = load_manifest(tmp)
        assert loaded is not None
        assert "files" in loaded


def test_load_manifest_not_exists():
    """load_manifest returns None when no manifest file."""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        loaded = load_manifest(tmp)
        assert loaded is None


def test_manifest_invalid_json():
    """load_manifest returns None on parse error."""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        (_manifest_path(tmp)).write_text("{invalid json}")
        loaded = load_manifest(tmp)
        assert loaded is None
