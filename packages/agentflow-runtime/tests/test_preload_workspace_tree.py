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
# upload_images：透传 executor 已翻译的确定性路径（消除 LLM 拼路径方差，OP-1）
# ---------------------------------------------------------------------------
# 背景（2026-08-16 uc2-optimization-plan）：preload 的 workspace_tree.upload 此前
# 只返回文件名（files），LLM 只能从文件名拼相对路径 → 001748 轮2 #5 拼 upload/xxx.png
# 失败 + find/pwd 探索。executor.list_dir 返回的 entry["path"] 已是当前执行环境可直接
# 使用的路径（HostExecutor=宿主机绝对路径；e2b=沙箱绝对路径），应透传给 LLM。


def test_upload_images_exposes_entry_path(tmp_path: Path) -> None:
    """upload 目录含图片：upload_images 返回 [{name, path}]，path 透传 executor 已翻译路径。

    HostExecutor 下 path = upload 目录内文件的宿主机绝对路径（LLM 可直接给 outfit_vision）。
    """
    (tmp_path / "upload").mkdir()
    (tmp_path / "upload" / "a.png").write_bytes(b"x")

    tree = _run_preload()
    up = tree["upload"]
    assert "upload_images" in up, f"upload_images 缺失: {list(up.keys())}"
    img = up["upload_images"][0]
    assert img["name"] == "a.png"
    # path 必须是绝对路径（executor 已翻译），且指向该文件本身，不是拼出来的相对路径
    assert img["path"] == str((tmp_path / "upload" / "a.png").resolve()), f"path={img['path']}"
    assert img["path"].endswith("upload/a.png")


def test_upload_images_filters_image_extensions(tmp_path: Path) -> None:
    """只透传图片扩展名（png/jpg/jpeg/webp），非图片文件不进 upload_images。"""
    (tmp_path / "upload").mkdir()
    (tmp_path / "upload" / "a.png").write_bytes(b"x")
    (tmp_path / "upload" / "b.jpg").write_bytes(b"x")
    (tmp_path / "upload" / "notes.txt").write_bytes(b"x")

    tree = _run_preload()
    imgs = tree["upload"]["upload_images"]
    names = [i["name"] for i in imgs]
    assert sorted(names) == ["a.png", "b.jpg"], f"names={names}"


def test_upload_images_absent_when_no_images(tmp_path: Path) -> None:
    """upload 目录无图片：不上传 upload_images 字段（LLM 不会误用）。"""
    (tmp_path / "upload").mkdir()
    (tmp_path / "upload" / "notes.txt").write_bytes(b"x")

    tree = _run_preload()
    up = tree["upload"]
    assert "upload_images" not in up, f"无图片不应有 upload_images: {up}"


def test_upload_images_hint_mentions_path(tmp_path: Path) -> None:
    """_hint 声明 upload_images 的 path 可直接用（禁止拼路径/find 探测）。"""
    (tmp_path / "upload").mkdir()
    (tmp_path / "upload" / "a.png").write_bytes(b"x")

    out = preload.execute({}, sandbox_endpoint=None)
    ctx = json.loads(out)
    hint = ctx.get("_hint", "")
    assert "upload_images" in hint, f"_hint 未声明 upload_images: {hint[:200]}"


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


# ---------------------------------------------------------------------------
# env：非动态环境信息（key 可用性 + 技能挂载）——LLM 直接使用，不再 bash 探测
# ---------------------------------------------------------------------------
# 背景（2026-08-16）：SKILL.md 文生图/视频章节原本写死 `[ -n "$DASHSCOPE_API_KEY" ]`
# 与 `ls ${SKILLS_DIR}/video-gen/...` 探测命令，每次触发一轮额外 LLM tool_call。
# 这些是非动态环境信息（key 是否注入、技能是否挂载），应由 preload 确定性探测
# 一次性返回（M7 识别脚本），LLM 直接基于返回推理。


def test_env_reports_dashscope_key_available(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """密钥已注入时，preload 的非可信探测仍不能看到它。"""
    monkeypatch.setenv("DASHSCOPE_API_KEY", "sk-test-not-a-real-key")
    out = preload.execute({}, sandbox_endpoint=None)
    ctx = json.loads(out)
    assert "env" in ctx, f"env 缺失: {list(ctx.keys())}"
    assert ctx["env"]["dashscope_key_available"] is False


def test_env_reports_dashscope_key_missing(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """DASHSCOPE_API_KEY 未注入 → env.dashscope_key_available=False。"""
    monkeypatch.delenv("DASHSCOPE_API_KEY", raising=False)
    out = preload.execute({}, sandbox_endpoint=None)
    ctx = json.loads(out)
    assert ctx["env"]["dashscope_key_available"] is False


def test_env_reports_skill_mounts(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """forge-image-gen/video-gen 挂载状态按实际目录返回。"""
    pkg = tmp_path / "agent-pkg"
    (pkg / "harness" / "skills" / "forge-image-gen").mkdir(parents=True)
    (pkg / "harness" / "skills" / "outfit-stylist").mkdir(parents=True)
    monkeypatch.setenv("AF_AGENT_PACKAGE_DIR", str(pkg))
    monkeypatch.delenv("AF_WORKDIR", raising=False)

    out = preload.execute({}, sandbox_endpoint=None)
    ctx = json.loads(out)
    skills = ctx["env"]["skills"]
    assert skills["forge_image_gen"] is True, f"skills={skills}"
    assert skills["video_gen"] is False, f"video-gen 未挂载应为 False: {skills}"


def test_env_hint_mentions_env_fields(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """_hint 声明 env 已返回（key/技能挂载直接使用，不再 bash 探测）。"""
    monkeypatch.setenv("DASHSCOPE_API_KEY", "sk-test-not-a-real-key")
    out = preload.execute({}, sandbox_endpoint=None)
    ctx = json.loads(out)
    hint = ctx.get("_hint", "")
    assert "env" in hint, f"_hint 未声明 env: {hint[:200]}"


# ═══════════════════════════════════════════════════════════════════
# 按需加载（load_* 参数，2026-08-17 优化）——LLM 自选要什么，返回只含请求字段
# ═══════════════════════════════════════════════════════════════════


def _exec(tool_input: dict) -> dict:
    """执行 preload 并解析返回 JSON。"""
    return json.loads(preload.execute(tool_input, sandbox_endpoint=None))


def test_load_profile_false_skips_profile_and_is_new_user(
    tmp_path: Path,
) -> None:
    """load_profile=false → 不含 profile_exists/profile_content/is_new_user。"""
    (tmp_path / ".agentfoundry" / "memory" / "outfit-profile").mkdir(parents=True)
    (tmp_path / ".agentfoundry" / "memory" / "outfit-profile" / "profile.md").write_text(
        "# 档案\n- 性别：男", encoding="utf-8"
    )
    ctx = _exec({"load_profile": False})
    assert "profile_exists" not in ctx, f"load_profile=false 不应返回 profile_exists: {list(ctx.keys())}"
    assert "profile_content" not in ctx
    assert "is_new_user" not in ctx, "is_new_user 依赖 profile_exists，应随 load_profile=false 省略"


def test_load_history_false_skips_history(tmp_path: Path) -> None:
    """load_history=false → 不含 history_files。"""
    (tmp_path / ".agentfoundry" / "memory" / "outfit-profile" / "history").mkdir(parents=True)
    (tmp_path / ".agentfoundry" / "memory" / "outfit-profile" / "history" / "20260817.md").write_text("x")
    ctx = _exec({"load_history": False})
    assert "history_files" not in ctx, f"load_history=false 不应返回 history_files: {list(ctx.keys())}"


def test_load_style_guide_false_skips_style_guide(tmp_path: Path) -> None:
    """load_style_guide=false → 不含 style_guide。"""
    ctx = _exec({"load_style_guide": False})
    assert "style_guide" not in ctx


def test_load_workspace_false_skips_tree_and_hint_adapts(tmp_path: Path) -> None:
    """load_workspace=false → 不含 workspace_tree，且 _hint 明示未请求（可再调用补取）。"""
    (tmp_path / "upload").mkdir()
    (tmp_path / "upload" / "a.png").write_bytes(b"x")
    ctx = _exec({"load_workspace": False})
    assert "workspace_tree" not in ctx, f"load_workspace=false 不应返回 workspace_tree: {list(ctx.keys())}"
    hint = ctx.get("_hint", "")
    assert "未请求 workspace_tree" in hint, f"_hint 应提示未请求 workspace_tree: {hint[:200]}"
    assert "可再次调用本工具并传 load_workspace=true" in hint


def test_load_env_false_skips_env_and_hint_adapts(tmp_path: Path) -> None:
    """load_env=false → 不含 env，且 _hint 明示未请求。"""
    ctx = _exec({"load_env": False})
    assert "env" not in ctx, f"load_env=false 不应返回 env: {list(ctx.keys())}"
    hint = ctx.get("_hint", "")
    assert "未请求 env" in hint, f"_hint 应提示未请求 env: {hint[:200]}"


def test_load_weather_false_skips_weather_even_with_location(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """load_weather=false 即使给 location 也不查天气（双条件缺一不查）。"""
    ctx = _exec({"location": "北京", "load_weather": False})
    assert "weather" not in ctx, f"load_weather=false 不应返回 weather: {list(ctx.keys())}"


def test_weather_only_selective_load(tmp_path: Path) -> None:
    """只补查天气：load_weather=true + location，其余 load_* false → 返回只含 weather（不含 profile/tree/env）。"""
    ctx = _exec({
        "location": "北京",
        "load_weather": True,
        "load_profile": False,
        "load_history": False,
        "load_style_guide": False,
        "load_workspace": False,
        "load_env": False,
    })
    assert "weather" in ctx, f"load_weather=true 应返回 weather: {list(ctx.keys())}"
    assert "profile_content" not in ctx, f"按需加载应跳过 profile: {list(ctx.keys())}"
    assert "workspace_tree" not in ctx
    assert "env" not in ctx
    hint = ctx.get("_hint", "")
    assert "weather" in hint, f"_hint 应声明 weather 已返回: {hint[:200]}"
    assert "未请求 workspace_tree" in hint, f"_hint 应提示未请求 workspace_tree: {hint[:200]}"


def test_load_defaults_all_true_backward_compatible(tmp_path: Path) -> None:
    """不传 load_*（默认全 true）= 现状行为：环境相关字段全部返回（向后兼容）。

    style_guide 是条件字段（仅 style-guide.md 文件存在时返回，if sg_out 守护），
    不参与"必返"断言——本测试环境无该文件，缺省不返回属正确尽力而为行为。
    """
    (tmp_path / "upload").mkdir()
    (tmp_path / "upload" / "a.png").write_bytes(b"x")
    ctx = _exec({})
    for key in ("profile_exists", "profile_content", "history_files",
                "workspace_tree", "env", "is_new_user", "_hint"):
        assert key in ctx, f"默认加载应返回 {key}: {list(ctx.keys())}"
