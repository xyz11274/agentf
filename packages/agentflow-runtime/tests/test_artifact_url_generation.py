"""Regression tests for local_artifact_url — verifies no /health hardcoding."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

from agentflow_runtime.coordinator_decide import local_artifact_url


# ═══════════════════════════════════════════════════════════════════
# local_artifact_url — 产物 URL 生成
# ═══════════════════════════════════════════════════════════════════


def _setup_artifact(tmp_path: Path, rel: str = "outputs/look.png") -> Path:
    """Create a dummy artifact file under a fake agent package dir."""
    pkg = tmp_path / "test-agent"
    pkg.mkdir()
    path = pkg / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"fake-image-content")
    return pkg, path


def test_url_starts_with_api_artifacts(tmp_path: Path) -> None:
    """URL 应以 /api/artifacts/ 开头，不应包含 /health。"""
    pkg, path = _setup_artifact(tmp_path)
    url = local_artifact_url(str(path), package_dir=str(pkg))
    assert url.startswith("/api/artifacts/"), (
        f"URL should start with /api/artifacts/, got: {url}"
    )
    assert "/health" not in url, (
        f"URL should NOT contain /health (gateway adds it), got: {url}"
    )


def test_url_uses_legacy_format_when_no_session_id(tmp_path: Path) -> None:
    """无 AF_SESSION_ID 时使用 legacy /files/ 格式。"""
    pkg, path = _setup_artifact(tmp_path)
    url = local_artifact_url(str(path), package_dir=str(pkg))
    assert "/files/" in url, (
        f"Legacy format should contain /files/, got: {url}"
    )


def test_url_includes_agent_name(tmp_path: Path) -> None:
    """URL 中应包含 agent 名称（包目录名）。"""
    pkg, path = _setup_artifact(tmp_path)
    url = local_artifact_url(str(path), package_dir=str(pkg))
    assert "test-agent" in url, (
        f"URL should contain agent name 'test-agent', got: {url}"
    )


def test_url_with_session_id_uses_new_format(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """有 AF_SESSION_ID + AF_ARTIFACT_STORAGE_URL 时使用新格式。"""
    pkg, path = _setup_artifact(tmp_path)
    monkeypatch.setenv("AF_SESSION_ID", "user_test_client")
    monkeypatch.setenv("AF_ARTIFACT_STORAGE_URL", "http://bridge:8199")
    url = local_artifact_url(str(path), package_dir=str(pkg))
    # 新格式: /api/artifacts/{agent_name}/{session_id}/{relative}
    assert "/user_test_client/" in url, (
        f"New format should contain session_id, got: {url}"
    )
    assert "/health" not in url, (
        f"URL should still NOT contain /health with session_id, got: {url}"
    )


def test_url_external_path_uses_basename(tmp_path: Path) -> None:
    """包外的路径降级为 basename。"""
    pkg, path = _setup_artifact(tmp_path)
    # 路径在包外
    outside = tmp_path / "outside.png"
    outside.write_bytes(b"outside-content")
    url = local_artifact_url(str(outside), package_dir=str(pkg))
    assert "outside.png" in url, (
        f"URL should contain basename 'outside.png', got: {url}"
    )


def test_url_preserves_relative_subdirectory(tmp_path: Path) -> None:
    """包内子目录的路径保持相对关系。"""
    pkg, path = _setup_artifact(tmp_path, rel="assets/sub/dir/look.png")
    url = local_artifact_url(str(path), package_dir=str(pkg))
    assert "assets/sub/dir/look.png" in url, (
        f"URL should preserve relative subdirectory, got: {url}"
    )
    assert "/health" not in url, (
        f"URL should NOT contain /health, got: {url}"
    )
