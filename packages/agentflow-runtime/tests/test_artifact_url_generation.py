"""Regression tests for local_artifact_url — verifies no /health hardcoding."""

from __future__ import annotations

from pathlib import Path

import pytest

from agentflow_runtime.common.runtime_context import artifact_routing_id
# 霰弹式收敛(2026-08-14):local_artifact_url / rewrite_local_artifact_links
# 从 coordinator_decide 上移 coordinator_common(单一真相源 F4),测试同步迁移。
from agentflow_runtime.coordinator_common import (
    local_artifact_url,
    rewrite_local_artifact_links,
)


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
    """有 {user_id}_{client_id} + AF_ARTIFACT_STORAGE_URL 时使用新格式.

    E2E-3 契约对齐(2026-08-12):local_artifact_url 的 session_id 段走
    _artifact_session_id() 拼 {user_id}_{client_id},不再读 AF_SESSION_ID(执行槽位).
    与 _upload_artifacts_to_storage 的落盘目录段单一真相来源.
    """
    pkg, path = _setup_artifact(tmp_path)
    # 新契约:会话段走 {user_id}_{client_id},不是 AF_SESSION_ID(执行槽位 0/1/2)
    env_user_id = "user_test"
    env_client_id = "client"
    monkeypatch.setenv("AF_USER_ID", env_user_id)
    monkeypatch.setenv("AF_CLIENT_ID", env_client_id)
    monkeypatch.setenv("AF_ARTIFACT_STORAGE_URL", "http://bridge:8199")
    url = local_artifact_url(str(path), package_dir=str(pkg))
    routing_id = artifact_routing_id(env_user_id, env_client_id)
    # 新格式: /api/artifacts/{agent_name}/{artifact_routing_id}/{relative}
    assert f"/{routing_id}/" in url, (
        f"New format should contain {routing_id} segment, got: {url}"
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


# ═══════════════════════════════════════════════════════════════════
# rewrite_local_artifact_links — 路径错位兜底（LLM 引用 ./assets/x.png，
# 真实产物在 harness/assets/x.png）与死链接去重修正
# ═══════════════════════════════════════════════════════════════════


def _write(pkg: Path, rel: str) -> Path:
    path = pkg / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"image")
    return path


def test_rewrite_resolves_basename_when_package_path_mismatch(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """LLM 引用 ./assets/look.png（相对 skill cwd），真实产物在 harness/assets/look.png
    （相对 AGENT_PACKAGE_DIR）→ basename 兜底解析为可访问 URL（主因修复）。"""
    pkg = tmp_path / "custom-agent"
    image = _write(pkg, "harness/assets/look.png")
    monkeypatch.setattr(
        "agentflow_runtime.coordinator_common.AGENT_PACKAGE_DIR", str(pkg)
    )

    rendered = rewrite_local_artifact_links(
        "搭配完成：![look](./assets/look.png)", [str(image)]
    )

    expected = "/api/artifacts/custom-agent/files/harness/assets/look.png"
    assert expected in rendered, (
        f"路径错位应兜底解析为 {expected}, got: {rendered}"
    )
    assert "(./assets/look.png)" not in rendered, (
        f"死链接应被替换, got: {rendered}"
    )


def test_rewrite_ambiguous_basename_stays_unresolved(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """同 basename 多文件（歧义）→ 不兜底，保持原样（拒绝猜测，防误解析）。"""
    pkg = tmp_path / "custom-agent"
    _write(pkg, "harness/assets/look.png")
    _write(pkg, "outputs/look.png")
    monkeypatch.setattr(
        "agentflow_runtime.coordinator_common.AGENT_PACKAGE_DIR", str(pkg)
    )

    rendered = rewrite_local_artifact_links(
        "![look](./assets/look.png)",
        [str(pkg / "harness/assets/look.png"), str(pkg / "outputs/look.png")],
    )

    assert "./assets/look.png" in rendered, (
        f"歧义时应保持原样, got: {rendered}"
    )


# ═══════════════════════════════════════════════════════════════════
# 失败模式矩阵（failure_mode_matrix.md §一 resolve_url）——
# 参数化收集:大小写 / 查询参数 / 多级错位 / HTML / 绝对路径 / 包外 / 空引用
# ═══════════════════════════════════════════════════════════════════


@pytest.mark.parametrize(
    "content, artifact_rel, expected_fragment",
    [
        # R5: 多级嵌套错位——basename 唯一匹配仍解析
        (
            "![x](./sub/assets/look.png)",
            "harness/sub/assets/look.png",
            "/harness/sub/assets/look.png",
        ),
        # R7: 绝对路径精确命中——不走兜底
        (None, "harness/assets/look.png", "/harness/assets/look.png"),
    ],
)
def test_rewrite_resolves_nested_and_absolute(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    content: str | None,
    artifact_rel: str,
    expected_fragment: str,
) -> None:
    """多级错位(basename 兜底)与绝对路径精确命中均应解析为可访问 URL。"""
    pkg = tmp_path / "custom-agent"
    image = _write(pkg, artifact_rel)
    monkeypatch.setattr(
        "agentflow_runtime.coordinator_common.AGENT_PACKAGE_DIR", str(pkg)
    )
    if content is None:  # R7 用绝对路径引用
        content = f"![x]({image})"

    rendered = rewrite_local_artifact_links(content, [str(image)])

    assert expected_fragment in rendered, (
        f"应解析为含 {expected_fragment} 的 URL, got: {rendered}"
    )


def test_rewrite_html_img_relative_path_resolved_by_basename(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """R6: HTML <img src="assets/look.png"> 相对路径(缺 harness/ 层)同样走
    basename 兜底。"""
    pkg = tmp_path / "custom-agent"
    image = _write(pkg, "harness/assets/look.png")
    monkeypatch.setattr(
        "agentflow_runtime.coordinator_common.AGENT_PACKAGE_DIR", str(pkg)
    )

    rendered = rewrite_local_artifact_links(
        '<section><img src="assets/look.png" alt="look"></section>', [str(image)]
    )

    expected_src = 'src="/api/artifacts/custom-agent/files/harness/assets/look.png"'
    assert expected_src in rendered, (
        f"HTML 相对路径应兜底解析, got: {rendered}"
    )


@pytest.mark.parametrize(
    "reference",
    [
        "LOOK.PNG",        # R3: 大小写不匹配(basename 大小写敏感)
        "look.png?size=1024",  # R4: 查询参数污染 basename
        "missing.png",     # R9: 产物清单外路径
    ],
)
def test_rewrite_refuses_guess_when_basename_unmatchable(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, reference: str
) -> None:
    """R3/R4/R9: basename 无法唯一匹配时拒绝猜测,保持原样(F11.1)。"""
    pkg = tmp_path / "custom-agent"
    _write(pkg, "harness/assets/look.png")
    monkeypatch.setattr(
        "agentflow_runtime.coordinator_common.AGENT_PACKAGE_DIR", str(pkg)
    )

    artifacts = [str(pkg / "harness/assets/look.png")]
    rendered = rewrite_local_artifact_links(
        f"![x](./assets/{reference})", artifacts
    )

    assert f"./assets/{reference}" in rendered, (
        f"basename 不可匹配时保持原样, got: {rendered}"
    )


def test_rewrite_empty_reference_does_not_crash(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """R10: 空引用(如 ![x]())不崩溃,原样保留。"""
    pkg = tmp_path / "custom-agent"
    _write(pkg, "harness/assets/look.png")
    monkeypatch.setattr(
        "agentflow_runtime.coordinator_common.AGENT_PACKAGE_DIR", str(pkg)
    )

    rendered = rewrite_local_artifact_links(
        "![x]()", [str(pkg / "harness/assets/look.png")]
    )

    assert "![x]()" in rendered, f"空引用应原样保留, got: {rendered}"


# ═══════════════════════════════════════════════════════════════════
# A-abs2: 沙箱内绝对路径引用(e2b 模式 LLM 正常输出)——
# /home/user/workspace/... 与宿主机产物 basename 唯一匹配 → 兜底解析
# ═══════════════════════════════════════════════════════════════════

def test_rewrite_sandbox_absolute_path_resolved_by_basename(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A-abs2: LLM 引用沙箱内绝对路径 /home/user/workspace/assets/look.png
    (e2b 下正常输出),产物已在宿主机 harness/assets/look.png →
    basename 唯一匹配 → 兜底解析为可访问 URL。"""
    pkg = tmp_path / "custom-agent"
    image = _write(pkg, "harness/assets/look.png")
    monkeypatch.setattr(
        "agentflow_runtime.coordinator_common.AGENT_PACKAGE_DIR", str(pkg)
    )

    rendered = rewrite_local_artifact_links(
        "![x](/home/user/workspace/assets/look.png)", [str(image)]
    )

    expected = "/api/artifacts/custom-agent/files/harness/assets/look.png"
    assert expected in rendered, (
        f"沙箱绝对路径应 basename 兜底解析为 {expected}, got: {rendered}"
    )
