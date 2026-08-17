"""I-6 边界回归测试 — 用户会话工作目录下的生成产物不得被误判为上传输入。

背景(2026-08-15 真实日志, 前端会话 dialogue=0815145902): e2b 沙箱模式下,
LLM 把生成的图片写到用户会话工作目录 workspace/{user_id}_{client_id}/assets/
(与用户上传的穿搭照同目录就近存放)。宿主机映射路径为
{agent_package}/workspace/{user_id}_{client_id}/assets/outfit-1.png:
  - 含 /workspace/ 子串,但不含 /upload/
  - 前面两个精确分支(has_workspace+has_upload / ws_prefix 前缀)均不命中
  - 兜底 any(marker in lowered for marker in _USER_UPLOAD_INPUT_MARKERS)
    把裸 /workspace/ 当 marker 命中 → 误判为上传输入 → skipped_upload
  - 产物未 admitted → rewrite_links 无从重写 → 响应保留相对路径 assets/outfit-1.png
    → 前端图片无法显示(对照: 22:20 测试运行产物在 agent 包根 assets/ 下,正常显示)

不变量: I-6 边界只应拦截真正的上传输入目录(/workspace/.../upload/、/uploads/、
legacy /workspace/upload/);agent 包内 workspace/{session}/assets/ 下的生成产物
必须 admitted,由 _ensure_artifacts_in_package 后续落盘 outputs/{session_id}/。
"""

from __future__ import annotations

from pathlib import Path

import pytest

from agentflow_runtime.skill_executor import extract_artifact_paths

_FAKE_PNG = b"\x89PNG\r\n\x1a\n" + b"0" * 2048


def test_session_workspace_assets_abs_path_admitted(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    """宿主机 workspace/{session}/assets/ 下真实存在的绝对路径产物应 admitted。

    LLM 在用户会话目录生成图片(与上传照同目录),宿主机映射路径含
    /workspace/{user}_{client}/assets/,是生成产物而非上传输入,必须放行。
    """
    monkeypatch.setenv("AF_SANDBOX_MODE", "e2b")
    monkeypatch.setenv("AF_SANDBOX_WORKSPACE", "/home/user/workspace")
    monkeypatch.delenv("AF_AGENT_PACKAGE_DIR", raising=False)

    pkg = tmp_path / "agent"
    product = pkg / "workspace" / "sim-user_client-abc" / "assets" / "outfit-1.png"
    product.parent.mkdir(parents=True, exist_ok=True)
    product.write_bytes(_FAKE_PNG)

    result = extract_artifact_paths(f'{{"path": "{product}"}}')

    assert str(product) in result, (
        f"用户会话目录下的生成产物应 admitted(非上传输入),实际: {result}"
    )


def test_session_workspace_assets_rel_path_admitted(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    """相对路径 assets/outfit-1.png(以 AGENT_PACKAGE_DIR 为基准)解析后
    落在 workspace/{session}/assets/ 下,同样不得被误判为上传输入。
    """
    pkg = tmp_path / "agent"
    monkeypatch.setattr("agentflow_runtime.skill_executor.AGENT_PACKAGE_DIR", str(pkg))
    monkeypatch.setenv("AF_SANDBOX_MODE", "e2b")
    monkeypatch.setenv("AF_SANDBOX_WORKSPACE", "/home/user/workspace")

    product = pkg / "workspace" / "sim-user_client-abc" / "assets" / "outfit-1.png"
    product.parent.mkdir(parents=True, exist_ok=True)
    product.write_bytes(_FAKE_PNG)

    result = extract_artifact_paths(
        '{"path": "workspace/sim-user_client-abc/assets/outfit-1.png"}'
    )

    assert str(product) in result, (
        f"相对路径解析后落在会话目录也应 admitted,实际: {result}"
    )


def test_session_workspace_upload_input_still_skipped(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    """对照: workspace/{session}/upload/ 下的上传输入必须仍被 I-6 拦截。"""
    monkeypatch.setenv("AF_SANDBOX_MODE", "e2b")
    monkeypatch.setenv("AF_SANDBOX_WORKSPACE", "/home/user/workspace")
    monkeypatch.delenv("AF_AGENT_PACKAGE_DIR", raising=False)

    pkg = tmp_path / "agent"
    upload = pkg / "workspace" / "sim-user_client-abc" / "upload" / "photo.png"
    upload.parent.mkdir(parents=True, exist_ok=True)
    upload.write_bytes(_FAKE_PNG)

    result = extract_artifact_paths(f'{{"path": "{upload}"}}')

    assert upload not in result, (
        f"workspace/{session}/upload/ 上传输入应被 I-6 拦截,实际: {result}"
    )


def test_legacy_workspace_upload_skipped(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    """对照: legacy /workspace/upload/ 上传输入仍被 I-6 拦截。"""
    monkeypatch.setenv("AF_SANDBOX_MODE", "local")
    monkeypatch.delenv("AF_SANDBOX_WORKSPACE", raising=False)
    monkeypatch.delenv("AF_AGENT_PACKAGE_DIR", raising=False)

    pkg = tmp_path / "agent"
    upload = pkg / "workspace" / "upload" / "photo.png"
    upload.parent.mkdir(parents=True, exist_ok=True)
    upload.write_bytes(_FAKE_PNG)

    result = extract_artifact_paths(f'{{"path": "{upload}"}}')

    assert upload not in result, (
        f"legacy /workspace/upload/ 上传输入应被 I-6 拦截,实际: {result}"
    )


def test_uploads_dir_skipped(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    """对照: /uploads/ 上传输入仍被 I-6 拦截(兜底 marker 保留)。"""
    monkeypatch.setenv("AF_SANDBOX_MODE", "local")
    monkeypatch.delenv("AF_SANDBOX_WORKSPACE", raising=False)
    monkeypatch.delenv("AF_AGENT_PACKAGE_DIR", raising=False)

    pkg = tmp_path / "agent"
    upload = pkg / "uploads" / "photo.png"
    upload.parent.mkdir(parents=True, exist_ok=True)
    upload.write_bytes(_FAKE_PNG)

    result = extract_artifact_paths(f'{{"path": "{upload}"}}')

    assert upload not in result, (
        f"/uploads/ 上传输入应被 I-6 拦截,实际: {result}"
    )
