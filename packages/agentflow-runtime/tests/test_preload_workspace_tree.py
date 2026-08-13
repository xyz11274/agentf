"""preload_user_context.workspace_tree 行为测试 — 锁定 P1/P3 review 修复。

修复背景（/code-review 提交 f8ad68a6 后）：
- P1: executor.list_dir 对"目录不存在"和"目录存在但空"都返回 []（无 stat 能力），
  原实现 `if not entries: {"exists": False}` 会把空目录误判为不存在 → 修复为
  `{"exists": False, "empty_or_missing": True}`（表达"未列出内容"，不断言不存在）
- P3: files[:20]/dirs[:10] 截断无 truncated 标记（F7 preview 铁律）→ 修复为
  补 `files_truncated`/`total_files`/`dirs_truncated`/`total_dirs`

本测试锁定这三态行为，防回归。
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

import pytest

# preload 工具在 agent 包的 harness/tools/ 下，经 sys.path 引入模块
# 路径：tests(0) → agentflow-runtime(1) → packages(2) → 项目根(3)
_PROJECT_ROOT = Path(__file__).resolve().parents[3]
_PRELOAD_DIR = _PROJECT_ROOT / "resource" / "agent-packages" / "outfit-assistant" / "harness" / "tools"
_RUNTIME_SRC = Path(__file__).resolve().parents[1] / "src"
for _p in (str(_PRELOAD_DIR), str(_RUNTIME_SRC)):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from agentflow_runtime.executors.host import HostExecutor  # noqa: E402

import preload_user_context as preload  # noqa: E402


@pytest.fixture(autouse=True)
def _workdir_env(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """把 AF_WORKDIR 指向临时目录，隔离工作区。"""
    monkeypatch.setenv("AF_WORKDIR", str(tmp_path))


def _run_preload() -> dict:
    """执行 preload 并返回 workspace_tree。"""
    out = preload.execute({}, sandbox_endpoint=None)
    ctx = json.loads(out)
    assert "workspace_tree" in ctx, f"workspace_tree 缺失: {list(ctx.keys())}"
    return ctx["workspace_tree"]


def test_workspace_tree_normal_dir(tmp_path: Path) -> None:
    """正常目录：exists=True + files/dirs 列出。"""
    (tmp_path / "upload").mkdir()
    (tmp_path / "upload" / "a.png").write_bytes(b"x")
    (tmp_path / "upload" / "sub").mkdir()

    tree = _run_preload()
    up = tree["upload"]
    assert up["exists"] is True
    assert up["files"] == ["a.png"]
    assert up["dirs"] == ["sub"]
    assert "files_truncated" not in up  # 未超 20 不标记
    assert "empty_or_missing" not in up


def test_workspace_tree_empty_dir_is_not_exists_false(tmp_path: Path) -> None:
    """空目录：exists=False + empty_or_missing=True（P1 修复——不得误判为不存在）。"""
    (tmp_path / "outputs").mkdir()  # 存在但空

    tree = _run_preload()
    out = tree["outputs"]
    assert out["exists"] is False
    assert out["empty_or_missing"] is True  # 关键断言：表达"未列出内容"而非"目录不存在"


def test_workspace_tree_missing_dir_also_empty_or_missing(tmp_path: Path) -> None:
    """不存在的目录：同样 empty_or_missing=True（executor 无 stat，无法区分，语义诚实）。"""
    tree = _run_preload()  # 临时目录下 .agentfoundry 不存在
    af = tree[".agentfoundry"]
    assert af["exists"] is False
    assert af["empty_or_missing"] is True


def test_workspace_tree_truncation_marks_total(tmp_path: Path) -> None:
    """文件超 20：files_truncated=True + total_files=N（P3 修复——F7 preview 标记）。"""
    (tmp_path / "upload").mkdir()
    for i in range(25):
        (tmp_path / "upload" / f"img{i:02d}.png").write_bytes(b"x")

    tree = _run_preload()
    up = tree["upload"]
    assert up["exists"] is True
    assert up["files_truncated"] is True
    assert up["total_files"] == 25
    assert len(up["files"]) == 20  # 截断到 20


def test_workspace_tree_hint_mentions_workspace_tree() -> None:
    """_hint 声明 workspace_tree 已返回（防探查约束生效）。"""
    out = preload.execute({}, sandbox_endpoint=None)
    ctx = json.loads(out)
    assert "workspace_tree" in ctx.get("_hint", ""), "_hint 未声明 workspace_tree"


# ---------------------------------------------------------------------------
# 工作根解析（AF_AGENT_PACKAGE_DIR + AF_USER_ID/AF_CLIENT_ID → 用户数据目录）
# ---------------------------------------------------------------------------


def _run_preload_with_workdir() -> tuple[dict, str]:
    """在 AF_AGENT_PACKAGE_DIR + AF_USER_ID/AF_CLIENT_ID 下执行，返回 (tree, workdir)。"""
    out = preload.execute({}, sandbox_endpoint=None)
    ctx = json.loads(out)
    return ctx["workspace_tree"], ctx["workspace_tree"]["workdir"]


def test_workdir_uses_agent_package_dir_and_user_client(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """工作根 = {AF_AGENT_PACKAGE_DIR}/workspace/{user_id}_{client_id}（用户数据目录层）。"""
    # 模拟 daemon 注入：agent_package_dir + user/client（rpc_handler → task → worker env）
    pkg = tmp_path / "agent-pkg"
    (pkg / "workspace" / "u123_c456" / "upload").mkdir(parents=True)
    (pkg / "workspace" / "u123_c456" / "upload" / "look.png").write_bytes(b"x")
    monkeypatch.setenv("AF_AGENT_PACKAGE_DIR", str(pkg))
    monkeypatch.setenv("AF_USER_ID", "u123")
    monkeypatch.setenv("AF_CLIENT_ID", "c456")
    monkeypatch.delenv("AF_WORKDIR", raising=False)

    tree, workdir = _run_preload_with_workdir()
    # 工作根必须指向用户数据目录 workspace/{uid}_{cid}，不是包根/沙箱根
    assert workdir == str(pkg / "workspace" / "u123_c456"), f"workdir={workdir}"
    up = tree["upload"]
    assert up["exists"] is True, f"upload 应列出（workdir 指向用户层），实际={up}"
    assert up["files"] == ["look.png"]


def test_workdir_falls_back_when_user_client_missing(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """user_id/client_id 缺失时退回 AF_WORKDIR（兼容独立调试）。"""
    (tmp_path / "outputs").mkdir()
    (tmp_path / "outputs" / "outfit-1.png").write_bytes(b"x")  # 非空，exists=True
    monkeypatch.setenv("AF_WORKDIR", str(tmp_path))
    monkeypatch.delenv("AF_USER_ID", raising=False)
    monkeypatch.delenv("AF_CLIENT_ID", raising=False)

    tree, workdir = _run_preload_with_workdir()
    assert workdir == str(tmp_path)
    assert tree["outputs"]["exists"] is True
    assert tree["outputs"]["files"] == ["outfit-1.png"]
