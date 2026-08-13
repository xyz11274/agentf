"""Tests for generate_manifest() — manifest.jsonc generation (T-004).

Tests the manifest structure after the files-key removal (2026-08-11):
  - manifest carries ``includes`` glob whitelist (harness/skills/**, etc.)
  - manifest carries ``excludes`` glob blacklist (nodes/, .venv/, etc.)
  - ``files`` key is no longer generated — includes glob is the single
    source of truth (I-5); consumers expand it at runtime.
"""

import tempfile
from pathlib import Path

from agentflow_runtime.common.manifest import generate_manifest, load_manifest

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

MANIFEST_NAME = "manifest.jsonc"


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


def test_manifest_has_includes_globs():
    """includes glob whitelist covers skills/tools/data/env/setup/requirements."""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        _make_package(tmp, [
            "agent.json",
            "harness/skills/foo/SKILL.md",
            "harness/skills/foo/scripts/tool.py",
        ])
        manifest = generate_manifest(tmp)
        includes = manifest["includes"]
        assert "harness/skills/**" in includes
        assert "harness/tools/**" in includes
        assert "data/**" in includes
        assert "requirements.txt" in includes
        assert ".env" in includes
        assert "setup.sh" in includes


def test_manifest_no_files_key():
    """files key is removed — includes glob is the single source of truth."""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        _make_package(tmp, [
            "agent.json",
            "harness/skills/my-skill/SKILL.md",
        ])
        manifest = generate_manifest(tmp)
        assert "files" not in manifest


def test_manifest_excludes_daemon_files():
    """excludes glob blacklist covers daemon-only / sandbox-irrelevant dirs."""
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
        excludes = manifest["excludes"]
        # 这些目录/文件由 excludes 或"不在 includes glob 内"双重保证不进沙箱
        assert "nodes" in excludes
        assert ".venv" in excludes
        assert "tests" in excludes
        assert "outputs" in excludes
        # agent.json 等不在 includes glob 内（includes 只含 harness/skills/** 等），
        # 由消费端 glob 展开天然排除——此处验证 includes 不含调度文件
        includes = manifest["includes"]
        assert not any(g.startswith("agent.json") or g.startswith("nodes") for g in includes)


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
        assert "includes" in loaded


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


def test_load_manifest_jsonc_with_comments():
    """load_manifest strips JSONC comments (// ...) before parsing.

    Regression guard: load_manifest previously called a misspelled
    ``_strip_jsonc_comments`` (NameError) instead of ``strip_jsonc_comments``.
    A manifest written by hand with ``//`` comments would crash load_manifest
    even though strip_jsonc_comments exists for exactly this purpose.
    """
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        # Hand-written JSONC with // line comments — mirrors
        # resource/agent-packages/outfit-assistant/manifest.jsonc shape
        (_manifest_path(tmp)).write_text(
            """{
            // includes: upload whitelist globs (relative to agent package root)
            "includes": ["harness/skills/**", ".env", "setup.sh"],
            // excludes: even if matched by includes, drop these
            "excludes": [".venv", "workspace"]
            }"""
        )
        loaded = load_manifest(tmp)
        assert loaded is not None, "load_manifest crashed or returned None on JSONC with comments"
        assert loaded["includes"] == ["harness/skills/**", ".env", "setup.sh"]
        assert loaded["excludes"] == [".venv", "workspace"]


def test_load_manifest_jsonc_with_block_comments():
    """load_manifest strips JSONC block comments (/* ... */) too.

    Covers the strip_jsonc_comments block-comment branch — line-comment-only
    tests would miss a regression where block-comment stripping breaks.
    """
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        (_manifest_path(tmp)).write_text(
            """{
            "includes": ["harness/skills/**"] /* block comment */
            }"""
        )
        loaded = load_manifest(tmp)
        assert loaded is not None
        assert loaded["includes"] == ["harness/skills/**"]
