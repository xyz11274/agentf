"""e2b 沙箱模式产物收集回归测试 — write_file 生成的 HTML 不得被 collect 丢弃。

背景(2026-08-14 真实日志): coordinator v2 在 e2b 沙箱模式下,LLM 用 write_file
写 HTML 报告到沙箱 `/home/user/workspace/./outfit-2026-08-14.html`。文本通道
extract_artifact_paths 对绝对路径做宿主机 isfile 检查 → 宿主机不存在 →
skipped_missing → 产物丢失,前端 ArtifactList 收不到 html/download 入口。
结构化通道(extract_structured_artifacts)已有 ws_prefix 放行,文本通道缺失,
双通道不一致。

不变量: e2b 模式下,沙箱 workspace 前缀(默认 /home/user/workspace)内的
产物路径必须在 collect 阶段放行,由 _ensure_artifacts_in_package 后续从
沙箱拉回(outputs/{session_id}/{name});不能因宿主机磁盘上不存在而丢弃。
"""

from __future__ import annotations

from pathlib import Path

import pytest

from agentflow_runtime import skill_executor
from agentflow_runtime.skill_executor import extract_artifact_paths


def test_e2b_sandbox_html_path_passes_collect(monkeypatch: pytest.MonkeyPatch) -> None:
    """e2b 模式下 /home/user/workspace/... 沙箱路径应被放行(admitted)。

    即使宿主机不存在该文件(文件在沙箱内,由 _ensure_artifacts_in_package
    通过 e2b SDK 拉回),collect 阶段也不得 skipped_missing。
    """
    monkeypatch.setenv("AF_SANDBOX_MODE", "e2b")
    monkeypatch.setenv("AF_SANDBOX_WORKSPACE", "/home/user/workspace")
    monkeypatch.delenv("AF_AGENT_PACKAGE_DIR", raising=False)

    result = extract_artifact_paths(
        '{"path": "/home/user/workspace/./outfit-2026-08-14.html"}'
    )

    assert result == ["/home/user/workspace/./outfit-2026-08-14.html"], (
        f"e2b 沙箱 HTML 路径应放行(由 ensure_in_package 拉回),实际: {result}"
    )


def test_e2b_sandbox_html_path_skipped_missing_without_pass(monkeypatch: pytest.MonkeyPatch) -> None:
    """非 e2b 模式(默认):沙箱绝对路径宿主机不存在仍应被跳过,不误放行。"""
    monkeypatch.delenv("AF_SANDBOX_MODE", raising=False)
    monkeypatch.delenv("AF_SANDBOX_WORKSPACE", raising=False)
    monkeypatch.delenv("AF_AGENT_PACKAGE_DIR", raising=False)

    result = extract_artifact_paths(
        '{"path": "/home/user/workspace/./outfit-2026-08-14.html"}'
    )

    assert result == [], (
        f"非 e2b 模式下沙箱路径不应放行(宿主机不存在),实际: {result}"
    )


def test_local_sandbox_structured_relative_path_passes_collect(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Local Sandbox 的结构化产物应先放行，再由 ensure 拉回宿主机。"""
    pkg = tmp_path / "agent"
    pkg.mkdir()
    monkeypatch.setattr(skill_executor, "AGENT_PACKAGE_DIR", str(pkg))
    monkeypatch.setenv("AF_SANDBOX_MODE", "local")
    monkeypatch.setenv("AF_SANDBOX_ENDPOINT", "http://sandbox:8080")

    result = skill_executor.extract_structured_artifacts(
        '{"files": ["assets/report.html"]}'
    )

    assert result == [str(pkg / "assets" / "report.html")]


def test_ensure_in_package_e2b_html_uses_sdk_channel(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """e2b 模式:沙箱 HTML 应走 e2b SDK 拉回通道,而非 HTTP 通道。

    回归 bug(2026-08-14 实测):macOS 上 Path.resolve() 把 /home/user/workspace/...
    展开为 /System/Volumes/Data/home/user/workspace/...,_ensure_artifacts_in_package
    中 str(p).startswith(ws_prefix) 前缀判断失败 → 误走 _download_from_sandbox
    (HTTP 通道,502)→ skipped_missing → HTML 产物丢失,前端收不到下载入口。
    修复:按 normpath 后的原始沙箱路径判断 ws_prefix,并传给 e2b SDK 拉回。
    """
    pkg = tmp_path / "agent"
    pkg.mkdir()
    monkeypatch.setattr(skill_executor, "AGENT_PACKAGE_DIR", str(pkg))
    monkeypatch.setenv("AF_SANDBOX_MODE", "e2b")
    monkeypatch.setenv("AF_SANDBOX_WORKSPACE", "/home/user/workspace")
    monkeypatch.setenv("AF_SANDBOX_ENDPOINT", "http://sandbox:8080")
    monkeypatch.setenv("AF_USER_ID", "u1")
    monkeypatch.setenv("AF_CLIENT_ID", "c1")

    sandbox_html = "/home/user/workspace/harness/skills/outfit-stylist/outfit-2026-08-14.html"
    sdk_calls: list[tuple[str, str]] = []
    http_calls: list[tuple] = []

    def fake_e2b_download(sandbox_path: str, local_path: str) -> bool:
        sdk_calls.append((sandbox_path, local_path))
        # 模拟 e2b SDK 拉回成功:在宿主机目标路径真实写出文件
        Path(local_path).parent.mkdir(parents=True, exist_ok=True)
        Path(local_path).write_bytes(b"<html>outfit report</html>")
        return True

    def fail_http(*args: object, **kwargs: object) -> None:
        http_calls.append(args)
        raise AssertionError("e2b 模式不应走 HTTP 拉回通道")

    monkeypatch.setattr(skill_executor, "_download_from_sandbox_e2b", fake_e2b_download)
    monkeypatch.setattr(skill_executor, "_download_from_sandbox", fail_http)

    result = skill_executor._ensure_artifacts_in_package([sandbox_html])

    # e2b SDK 通道被选中,且传入的是 normpath 后的原始沙箱路径(非 resolve 展开)
    assert len(sdk_calls) == 1, f"应走 e2b SDK 拉回通道, 实际 sdk_calls={sdk_calls}"
    called_sandbox_path, _ = sdk_calls[0]
    assert called_sandbox_path == sandbox_html, (
        f"e2b 拉回应传原始沙箱路径, 实际: {called_sandbox_path}"
    )
    assert not called_sandbox_path.startswith("/System/Volumes/Data"), (
        f"不应传 resolve 展开的宿主路径, 实际: {called_sandbox_path}"
    )
    assert len(http_calls) == 0, f"HTTP 通道不应被调用, 实际: {http_calls}"
    # HTML 产物进入 relocate 结果(前端据此渲染下载入口)
    assert any(str(p).endswith("outfit-2026-08-14.html") for p in result), (
        f"HTML 应进入产物清单, 实际: {result}"
    )


def test_ensure_in_package_same_name_overwrites_previous_version(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """多轮对话:同名产物(内容更新)应覆盖 outputs/ 下的旧文件,不产生 _1 后缀。

    设计决策(2026-08-14 用户确认):同一 session 内 agent 更新同名 html/产物时
    直接覆盖旧文件(历史消息 URL 内容变为最新版,接受该语义)。回归 bug:
    原逻辑 `if dest.exists() and dest.read_bytes() != p.read_bytes()` → 加
    `_1`/`_2` 后缀,导致 outputs 目录跨轮累积版本文件、文件名失语义。
    """
    pkg = tmp_path / "agent"
    pkg.mkdir()
    monkeypatch.setattr(skill_executor, "AGENT_PACKAGE_DIR", str(pkg))
    monkeypatch.setenv("AF_SANDBOX_MODE", "local")  # 非 e2b:直接走宿主机文件
    monkeypatch.delenv("AF_SANDBOX_WORKSPACE", raising=False)
    monkeypatch.setenv("AF_USER_ID", "u1")
    monkeypatch.setenv("AF_CLIENT_ID", "c1")

    # 第 1 轮:源文件 v1 → 落盘 outputs/{u1_c1}/report.html
    src = tmp_path / "report.html"
    src.write_bytes(b"<html>v1</html>")
    result1 = skill_executor._ensure_artifacts_in_package([str(src)])
    assert len(result1) == 1
    dest = Path(result1[0])
    assert dest.name == "report.html"
    assert dest.read_bytes() == b"<html>v1</html>"

    # 第 2 轮:同路径源文件内容更新为 v2 → 应覆盖 report.html,不产生 report_1.html
    src.write_bytes(b"<html>v2-updated</html>")
    result2 = skill_executor._ensure_artifacts_in_package([str(src)])

    assert len(result2) == 1
    dest2 = Path(result2[0])
    assert dest2.name == "report.html", (
        f"同名产物应覆盖旧文件, 实际生成: {dest2.name}"
    )
    assert dest2.read_bytes() == b"<html>v2-updated</html>", (
        "覆盖后内容应为最新版"
    )
    # 不得残留 _1 后缀版本
    assert not (dest.parent / "report_1.html").exists(), (
        "不应产生 _1 后缀版本文件"
    )
