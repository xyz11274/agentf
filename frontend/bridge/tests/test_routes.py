"""Tests for AgentFlow Bridge — agent & session proxy routers (T6).

Each test:
  * mounts the router under a fresh ``FastAPI`` app;
  * overrides ``get_daemon_client`` with a fully-mocked :class:`DaemonClient`;
  * issues an HTTP request via ``httpx.AsyncClient`` and asserts the response.

Covers:
  * happy-path success for every endpoint;
  * the 5-element error-mapping table
    (InvalidParams→400, NotFound→404, Timeout→504, Connection→502, other→500);
  * that ``agent_id`` / ``session_id`` / ``package_path`` are forwarded to
    the daemon client unchanged.
"""

from __future__ import annotations

from collections.abc import AsyncGenerator
from pathlib import Path
from typing import Any
from unittest.mock import AsyncMock

import pytest
from fastapi import FastAPI
from httpx import ASGITransport, AsyncClient

from client import (
    DaemonClient,
    DaemonConnectionError,
    DaemonError,
    DaemonTimeoutError,
    InvalidParamsError,
    NotFoundError,
)
from dependencies import get_daemon_client
from routes import agents as agents_routes
from routes import sessions as sessions_routes

# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


def _build_app(mock_client: AsyncMock) -> FastAPI:
    """Build a fresh FastAPI app with both routers and an overridden client."""
    app = FastAPI()
    app.include_router(agents_routes.router)
    app.include_router(sessions_routes.router)

    async def _override() -> DaemonClient:
        return mock_client  # type: ignore[return-value]

    app.dependency_overrides[get_daemon_client] = _override
    return app


@pytest.fixture
def mock_client() -> AsyncMock:
    """A bare ``AsyncMock`` — only methods the route under test calls are set."""
    return AsyncMock(spec=DaemonClient)


@pytest.fixture
async def client(mock_client: AsyncMock) -> AsyncGenerator[AsyncClient, None]:
    app = _build_app(mock_client)
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://testserver") as ac:
        yield ac


@pytest.fixture
def three_state_dirs(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> tuple[Path, Path]:
    """Point 固化态(②) 与 workspace(③) 到临时目录，并创建源包目录。

    load 路由现在会先同步 ②→③（_sync_agent_package），
    因此 ② 下必须有对应的包目录，否则 404。
    """
    from config import settings
    packages = tmp_path / "agent-packages"
    workspace = tmp_path / "agent-workspace"
    packages.mkdir()
    workspace.mkdir()
    for pkg in ("a1", "custom-agent", "my-agent_123"):
        (packages / pkg).mkdir()
    monkeypatch.setattr(settings, "agent_packages_dir", str(packages))
    monkeypatch.setattr(settings, "agent_workspace_dir", str(workspace))
    return packages, workspace


# ---------------------------------------------------------------------------
# Agent endpoints — success path
# ---------------------------------------------------------------------------


async def test_list_agents_returns_daemon_payload(client: AsyncClient, mock_client: AsyncMock) -> None:
    mock_client.agent_list.return_value = [{"id": "a1"}, {"id": "a2"}]
    resp = await client.get("/api/agents")
    assert resp.status_code == 200
    body = resp.json()
    assert body == {"success": True, "data": [{"id": "a1"}, {"id": "a2"}], "error": None}
    mock_client.agent_list.assert_awaited_once_with()


async def test_get_agent_forwards_id(client: AsyncClient, mock_client: AsyncMock) -> None:
    mock_client.agent_info.return_value = {"id": "a1", "name": "demo", "status": "running"}
    resp = await client.get("/api/agents/a1")
    assert resp.status_code == 200
    assert resp.json() == {
        "success": True,
        "data": {"id": "a1", "name": "demo", "status": "running"},
        "error": None,
    }
    mock_client.agent_info.assert_awaited_once_with("a1")


async def test_load_agent_falls_back_to_agent_id_when_body_empty(
    client: AsyncClient, mock_client: AsyncMock, three_state_dirs: tuple[Path, Path]
) -> None:
    mock_client.agent_load.return_value = {"loaded": True}
    resp = await client.post("/api/agents/a1/load")
    assert resp.status_code == 200
    assert resp.json() == {"success": True, "data": {"loaded": True}, "error": None}
    from config import settings
    expected_path = str(settings.agent_workspace_path / "a1")
    mock_client.agent_load.assert_awaited_once_with(expected_path)


async def test_load_agent_uses_explicit_package_path(
    client: AsyncClient, mock_client: AsyncMock, three_state_dirs: tuple[Path, Path]
) -> None:
    mock_client.agent_load.return_value = {"loaded": True}
    resp = await client.post("/api/agents/a1/load", json={"package_path": "custom-agent"})
    assert resp.status_code == 200
    from config import settings
    expected_path = str(settings.agent_workspace_path / "custom-agent")
    mock_client.agent_load.assert_awaited_once_with(expected_path)


# ── Regression: path traversal guard on load endpoint ──


async def test_load_agent_rejects_path_traversal_in_agent_id(
    client: AsyncClient, mock_client: AsyncMock
) -> None:
    """agent_id with ``..`` → 404 (FastAPI URL normalization catches it first)."""
    mock_client.agent_load.return_value = {"loaded": True}
    resp = await client.post("/api/agents/../etc/load")
    # FastAPI/Starlette normalizes ".." in the URL path at routing layer,
    # producing "/api/etc/load" which doesn't match "/{agent_id}/load".
    assert resp.status_code == 404, (
        f"Expected 404 for '..' in agent_id (FastAPI normalized), got {resp.status_code}"
    )


async def test_load_agent_rejects_absolute_path_in_agent_id(
    client: AsyncClient, mock_client: AsyncMock
) -> None:
    """agent_id with ``/`` → 404 (FastAPI sees nested path, no route match)."""
    mock_client.agent_load.return_value = {"loaded": True}
    resp = await client.post("/api/agents/opt/evil/load")
    # FastAPI matches "/api/agents/{agent_id}/load" only for single-segment agent_id.
    # "opt/evil" is two segments → no route match → 404.
    assert resp.status_code == 404, (
        f"Expected 404 for '/' in agent_id (FastAPI no route), got {resp.status_code}"
    )


async def test_load_agent_rejects_hidden_dir_in_agent_id(
    client: AsyncClient, mock_client: AsyncMock
) -> None:
    """agent_id starting with ``.`` → 403 (hidden dir)."""
    mock_client.agent_load.return_value = {"loaded": True}
    resp = await client.post("/api/agents/.secret/load")
    assert resp.status_code == 403, (
        f"Expected 403 for '.' prefix in agent_id, got {resp.status_code}"
    )
    assert "Invalid package name" in resp.text


async def test_load_agent_rejects_path_traversal_in_body_package_path(
    client: AsyncClient, mock_client: AsyncMock
) -> None:
    """body.package_path with ``..`` → 403."""
    mock_client.agent_load.return_value = {"loaded": True}
    resp = await client.post("/api/agents/a1/load", json={"package_path": "../../etc"})
    assert resp.status_code == 403, (
        f"Expected 403 for '..' in body.package_path, got {resp.status_code}"
    )


async def test_load_agent_rejects_absolute_path_in_body_package_path(
    client: AsyncClient, mock_client: AsyncMock
) -> None:
    """body.package_path with ``/`` → 403."""
    mock_client.agent_load.return_value = {"loaded": True}
    resp = await client.post("/api/agents/a1/load", json={"package_path": "/opt/evil"})
    assert resp.status_code == 403, (
        f"Expected 403 for '/' in body.package_path, got {resp.status_code}"
    )


async def test_load_agent_rejects_dot_in_body_package_path(
    client: AsyncClient, mock_client: AsyncMock
) -> None:
    """body.package_path ``.`` → 403."""
    mock_client.agent_load.return_value = {"loaded": True}
    resp = await client.post("/api/agents/a1/load", json={"package_path": "."})
    assert resp.status_code == 403, (
        f"Expected 403 for '.' in body.package_path, got {resp.status_code}"
    )


async def test_load_agent_still_works_with_valid_name(
    client: AsyncClient, mock_client: AsyncMock, three_state_dirs: tuple[Path, Path]
) -> None:
    """Valid agent_id still works (regression: normal path not broken)."""
    mock_client.agent_load.return_value = {"loaded": True}
    resp = await client.post("/api/agents/my-agent_123/load")
    assert resp.status_code == 200, (
        f"Expected 200 for valid agent_id, got {resp.status_code}"
    )
    from config import settings
    expected_path = str(settings.agent_workspace_path / "my-agent_123")
    mock_client.agent_load.assert_awaited_once_with(expected_path)


# ── R-2：_sync_agent_package OSError 异常路径（用户拍板：留半成品给下次 load 覆盖）──


async def test_load_agent_returns_500_when_sync_copytree_fails(
    client: AsyncClient, mock_client: AsyncMock, three_state_dirs: tuple[Path, Path]
) -> None:
    """copytree 中途 OSError → load 路由返 500，不调用 daemon.load。

    业务规则（agent-package-three-state.md §5 + R-2 拍板）：
    - ②→③ 同步失败时上报 500，不静默吞错。
    - daemon.load 不被调用（避免在残缺 ③ 上 load 崩）。
    - ③ 半成品保留（下次 load 的 dirs_exist_ok=True 覆盖式同步会以 ② 为准收敛）。
    """
    mock_client.agent_load.return_value = {"loaded": True}
    # 用 monkeypatch 替 shutil.copytree 抛 OSError（只影响本轮调用）
    import shutil as _shutil

    import routes.agents as _agents_mod
    original_copytree = _shutil.copytree

    def _raising_copytree(*args, **kwargs):
        raise OSError("simulated sync failure (e.g. disk full)")

    _agents_mod.shutil.copytree = _raising_copytree
    try:
        resp = await client.post("/api/agents/a1/load")
    finally:
        _agents_mod.shutil.copytree = original_copytree

    assert resp.status_code == 500, (
        f"Expected 500 when copytree raises OSError, got {resp.status_code}"
    )
    assert "sync" in resp.text.lower() or "failed" in resp.text.lower()
    # daemon.load 不应被调用（同步失败前置拦截）
    mock_client.agent_load.assert_not_awaited()


def test_sync_agent_package_preserves_partial_on_oserror(
    three_state_dirs: tuple[Path, Path],
) -> None:
    """R-2 不变量：copytree 中途失败时 **不清理 ③ 半成品**——

    下次 load 的 dirs_exist_ok=True 会在残缺 ③ 上重拷，② 仍是权威源。
    避免误删 ③ 已有的运行时数据（.venv/outputs 等，恢复代价高）。
    """
    import shutil as _shutil

    from fastapi import HTTPException

    import routes.agents as _agents_mod
    from routes.agents import _sync_agent_package

    packages, workspace = three_state_dirs
    pkg = "a1"
    # ② 固化态放资产文件
    (packages / pkg / "agent.md").write_text("identity", encoding="utf-8")
    (packages / pkg / "package.json").write_text('{"name":"a1"}', encoding="utf-8")

    # ③ 已有运行时数据（load 前已存在，模拟上一次运行残留）
    runtime_dir = workspace / pkg / ".venv" / "bin"
    runtime_dir.mkdir(parents=True)
    (runtime_dir / "python3").write_text("#!/bin/sh\n")

    # copytree 在拷第一个资产文件后抛 OSError（模拟磁盘满）
    original_copytree = _shutil.copytree
    call_count = {"n": 0}

    def _raising_after_first(*args, **kwargs):
        call_count["n"] += 1
        raise OSError("simulated mid-copy failure")

    _agents_mod.shutil.copytree = _raising_after_first
    try:
        with pytest.raises(HTTPException) as exc_info:
            _sync_agent_package(pkg)
        assert exc_info.value.status_code == 500
    finally:
        _agents_mod.shutil.copytree = original_copytree

    # 断言 1：③ 半成品目录保留（不被清理）
    assert (workspace / pkg).is_dir(), "③ 半成品被清理了——违反 R-2 拍板"

    # 断言 2：③ 的运行时数据保留（不因同步失败误删）
    assert (workspace / pkg / ".venv" / "bin" / "python3").is_file(), (
        "③ 运行时数据被误删——R-2 拍板要求保留"
    )

    # 断言 3：下次 load（copytree 恢复正常）能以 ② 为准收敛
    _agents_mod.shutil.copytree = original_copytree
    result = _sync_agent_package(pkg)
    assert result == (workspace / pkg).resolve()
    # ② 的资产现在在 ③ 中
    assert (workspace / pkg / "agent.md").read_text(encoding="utf-8") == "identity"
    # ③ 的运行时数据仍在（排除清单生效，未被 copytree 覆盖）
    assert (workspace / pkg / ".venv" / "bin" / "python3").is_file()


async def test_unload_agent_forwards_id(client: AsyncClient, mock_client: AsyncMock) -> None:
    mock_client.agent_unload.return_value = {"unloaded": True}
    resp = await client.post("/api/agents/a1/unload")
    assert resp.status_code == 200
    assert resp.json() == {"success": True, "data": {"unloaded": True}, "error": None}
    mock_client.agent_unload.assert_awaited_once_with("a1")


async def test_pause_agent_forwards_id(client: AsyncClient, mock_client: AsyncMock) -> None:
    mock_client.agent_pause.return_value = {"paused": True}
    resp = await client.post("/api/agents/a1/pause")
    assert resp.status_code == 200
    assert resp.json() == {"success": True, "data": {"paused": True}, "error": None}
    mock_client.agent_pause.assert_awaited_once_with("a1")


async def test_resume_agent_forwards_id(client: AsyncClient, mock_client: AsyncMock) -> None:
    mock_client.agent_resume.return_value = {"resumed": True}
    resp = await client.post("/api/agents/a1/resume")
    assert resp.status_code == 200
    assert resp.json() == {"success": True, "data": {"resumed": True}, "error": None}
    mock_client.agent_resume.assert_awaited_once_with("a1")


# ---------------------------------------------------------------------------
# Session endpoints — success path
# ---------------------------------------------------------------------------


async def test_list_sessions_requires_agent_id(client: AsyncClient) -> None:
    resp = await client.get("/api/sessions")
    assert resp.status_code == 422


async def test_list_sessions_forwards_agent_id(
    client: AsyncClient, mock_client: AsyncMock
) -> None:
    mock_client.session_list.return_value = [{"session_id": "s1"}, {"session_id": "s2"}]
    resp = await client.get("/api/sessions", params={"agent_id": "a1"})
    assert resp.status_code == 200
    assert resp.json() == {
        "success": True,
        "data": [{"session_id": "s1"}, {"session_id": "s2"}],
        "error": None,
    }
    mock_client.session_list.assert_awaited_once_with("a1", "")


async def test_destroy_session_forwards_ids(
    client: AsyncClient, mock_client: AsyncMock
) -> None:
    mock_client.session_destroy.return_value = {"destroyed": True}
    resp = await client.delete("/api/sessions/s1", params={"agent_id": "a1"})
    assert resp.status_code == 200
    assert resp.json() == {"success": True, "data": {"destroyed": True}, "error": None}
    mock_client.session_destroy.assert_awaited_once_with("a1", "s1")


async def test_clear_session_dialogue_forwards_params(
    client: AsyncClient, mock_client: AsyncMock
) -> None:
    mock_client.session_clear_dialogue.return_value = {"cleared": True}
    resp = await client.delete(
        "/api/sessions/s1/dialogue",
        params={"agent_id": "a1", "dialogue_id": "d9"},
    )
    assert resp.status_code == 200
    assert resp.json() == {"success": True, "data": {"cleared": True}, "error": None}
    mock_client.session_clear_dialogue.assert_awaited_once_with("a1", "s1", "d9")


async def test_clear_session_dialogue_defaults_empty_dialogue_id(
    client: AsyncClient, mock_client: AsyncMock
) -> None:
    mock_client.session_clear_dialogue.return_value = {"cleared": True}
    resp = await client.delete("/api/sessions/s1/dialogue", params={"agent_id": "a1"})
    assert resp.status_code == 200
    mock_client.session_clear_dialogue.assert_awaited_once_with("a1", "s1", "")


# ---------------------------------------------------------------------------
# Error mapping — one representative endpoint per status code
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("exc", "expected_status"),
    [
        (InvalidParamsError("bad params", code=-32602), 400),
        (NotFoundError("no such agent", code=-2), 404),
        (DaemonTimeoutError("slow", code=-32000), 504),
        (DaemonConnectionError("offline"), 502),
        (DaemonError("boom", code=-1), 500),
    ],
)
async def test_agent_info_error_mapping(
    client: AsyncClient,
    mock_client: AsyncMock,
    exc: DaemonError,
    expected_status: int,
) -> None:
    mock_client.agent_info.side_effect = exc
    resp = await client.get("/api/agents/whatever")
    assert resp.status_code == expected_status
    assert exc.message in resp.json()["detail"]


@pytest.mark.parametrize(
    ("exc", "expected_status"),
    [
        (InvalidParamsError("bad params", code=-32602), 400),
        (NotFoundError("no such session", code=-2), 404),
        (DaemonTimeoutError("slow", code=-32000), 504),
        (DaemonConnectionError("offline"), 502),
        (DaemonError("boom", code=-1), 500),
    ],
)
async def test_session_destroy_error_mapping(
    client: AsyncClient,
    mock_client: AsyncMock,
    exc: DaemonError,
    expected_status: int,
) -> None:
    mock_client.session_destroy.side_effect = exc
    resp = await client.delete("/api/sessions/s1", params={"agent_id": "a1"})
    assert resp.status_code == expected_status
    assert exc.message in resp.json()["detail"]


# ---------------------------------------------------------------------------
# Sanity: routes are wired with the expected paths
# ---------------------------------------------------------------------------


async def test_unknown_agent_id_subpath_returns_404(client: AsyncClient) -> None:
    """Routes only exist for the documented suffixes — extras should 404."""
    resp = await client.post("/api/agents/a1/nonsense")
    assert resp.status_code == 404


async def test_routes_propagate_typed_payload_unmodified(
    client: AsyncClient, mock_client: AsyncMock
) -> None:
    """Daemon payloads must be forwarded verbatim — no field rewriting."""
    payload: dict[str, Any] = {
        "id": "a1",
        "name": "demo",
        "workflow_count": 3,
        "status": "running",
        "extra_field": ["kept", "as", "is"],
    }
    mock_client.agent_info.return_value = payload
    resp = await client.get("/api/agents/a1")
    assert resp.status_code == 200
    assert resp.json()["data"] == payload


# ═══════════════════════════════════════════════════════════════════
# R-1：真实文件系统集成测试（不 mock ②/③ 位置，沉淀上轮 spike）
# 用真实 resource/agent-packages/（②）+ 临时 ③ 验证三态链路：
#   ② → _sync_agent_package → ③ 资产与 ② 一致
#   ② 不被运行时数据污染
#   .env 用户值不被 bridge 注入覆盖
#   重复 load 幂等（资产刷新、运行时数据保留）
# 满足 F13：不 mock 关键集成点（文件系统），走真实 FS 链路。
# ═══════════════════════════════════════════════════════════════════


@pytest.fixture
def real_state_dirs(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> tuple[Path, Path]:
    """真实 ② 固化态（resource/agent-packages/）+ 临时 ③ workspace。

    与 three_state_dirs 的区别：② 用真实 resource/agent-packages/（而非 tmp），
    只把 ③ 指向 tmp 防污染 examples/。这样能发现"② 位置错误"类真实运行时才崩的问题。
    """
    from config import settings

    # ② 走真实固化态（resource/agent-packages/，已在仓库中）
    real_packages = settings.agent_packages_path
    if not real_packages.is_dir():
        pytest.skip(f"real 固化态目录不存在: {real_packages}")

    # ③ 指向临时目录（避免污染 examples/agent/）
    workspace = tmp_path / "real_workspace_test"
    workspace.mkdir()
    monkeypatch.setattr(settings, "agent_workspace_dir", str(workspace))
    return real_packages, workspace


def test_sync_agent_package_real_fs_assets_consistent(
    real_state_dirs: tuple[Path, Path],
) -> None:
    """R-1 不变量 I-3S-3：真实 ② → ③ 同步后，③ 的资产文件与 ② 一致。

    不 mock ②/③ 位置——用真实 resource/agent-packages/ + 临时 ③。
    """
    from routes.agents import _SYNC_EXCLUDE_NAMES, _sync_agent_package

    real_packages, workspace = real_state_dirs
    # 选一个真实存在的包（outfit-assistant 是存量迁移过的）
    pkg = "outfit-assistant"
    if not (real_packages / pkg).is_dir():
        pytest.skip(f"real 固化态包不存在: {pkg}")

    synced = _sync_agent_package(pkg)

    # 断言 1：返回路径 = ③/{pkg}
    assert synced == (workspace / pkg).resolve()

    # 断言 2：③ 的资产文件与 ② 一致（逐文件比对，排除运行时数据）
    excluded = _SYNC_EXCLUDE_NAMES
    for src_file in (real_packages / pkg).rglob("*"):
        if not src_file.is_file():
            continue
        # ② 不应含运行时数据（I-3S-1），但防御性跳过
        if any(part in excluded for part in src_file.relative_to(real_packages / pkg).parts):
            continue
        rel = src_file.relative_to(real_packages / pkg)
        dst_file = workspace / pkg / rel
        assert dst_file.is_file(), f"③ 缺失 ② 的资产文件: {rel}"
        assert src_file.read_bytes() == dst_file.read_bytes(), (
            f"③ 与 ② 资产内容不一致: {rel}"
        )


def test_sync_agent_package_real_fs_no_runtime_in_packages(
    real_state_dirs: tuple[Path, Path],
) -> None:
    """R-1 不变量 I-3S-1：真实 ② 固化态不含任何运行时数据。

    sync 后扫描 ② 确认未被污染（② 是权威源，只应含静态资产）。
    """
    from routes.agents import _SYNC_EXCLUDE_NAMES, _sync_agent_package

    real_packages, workspace = real_state_dirs
    pkg = "outfit-assistant"
    if not (real_packages / pkg).is_dir():
        pytest.skip(f"real 固化态包不存在: {pkg}")

    _sync_agent_package(pkg)

    # 断言：② 不含运行时数据目录/文件
    excluded_names = _SYNC_EXCLUDE_NAMES
    for path in (real_packages / pkg).rglob("*"):
        rel_parts = path.relative_to(real_packages / pkg).parts
        for part in rel_parts:
            assert part not in excluded_names, (
                f"② 固化态被运行时数据污染: {path}（含 {part}）"
            )


def test_sync_agent_package_real_fs_idempotent(
    real_state_dirs: tuple[Path, Path],
) -> None:
    """R-1 不变量 I-3S-4：重复 load 幂等——资产刷新、运行时数据保留。

    两次 sync 后：③ 的资产仍与 ② 一致；③ 模拟的运行时数据保留。
    """
    from routes.agents import _sync_agent_package

    real_packages, workspace = real_state_dirs
    pkg = "outfit-assistant"
    if not (real_packages / pkg).is_dir():
        pytest.skip(f"real 固化态包不存在: {pkg}")

    # 第一次 sync
    _sync_agent_package(pkg)

    # 模拟 ③ 运行时数据（daemon/skill 写入）
    runtime_file = workspace / pkg / ".venv" / "bin" / "python3"
    runtime_file.parent.mkdir(parents=True, exist_ok=True)
    runtime_file.write_text("#!/bin/sh\n# simulated runtime data\n")

    # 第二次 sync（幂等：资产刷新，运行时数据保留）
    _sync_agent_package(pkg)

    # 断言 1：③ 运行时数据保留（不被 sync 误删）
    assert runtime_file.is_file(), "③ 运行时数据被第二次 sync 误删——违反 I-3S-4"
    assert "simulated runtime data" in runtime_file.read_text()

    # 断言 2：③ 资产仍与 ② 一致（取一个资产文件抽样）
    agent_md_src = real_packages / pkg / "agent.md"
    if agent_md_src.is_file():
        agent_md_dst = workspace / pkg / "agent.md"
        assert agent_md_dst.is_file()
        assert agent_md_src.read_bytes() == agent_md_dst.read_bytes()


def test_inject_env_defaults_real_fs_preserves_user_values(
    real_state_dirs: tuple[Path, Path],
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """R-1 + F13：真实 .env 用户值不被 bridge 注入覆盖。

    用真实 ② 的 outfit-assistant/.env（用户上传密钥）→ sync 到 ③ → 注入 →
    断言用户已有值保留、bridge 只补缺失/占位符值。
    """
    from routes.agents import _inject_agent_env_defaults, _sync_agent_package

    real_packages, workspace = real_state_dirs
    pkg = "outfit-assistant"
    src_env = real_packages / pkg / ".env"
    if not src_env.is_file():
        pytest.skip(f"real 固化态 .env 不存在: {src_env}")

    # sync ②→③（.env 随资产同步到 ③）
    _sync_agent_package(pkg)
    dst_env = workspace / pkg / ".env"
    assert dst_env.is_file(), ".env 未从 ② 同步到 ③"

    # 记录 sync 后的用户值（注入前快照）
    user_snapshot: dict[str, str] = {}
    for line in dst_env.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, _, v = line.partition("=")
        user_snapshot[k.strip()] = v.strip()

    # 注入 bridge 默认值（读 config.yaml agent_defaults）
    _inject_agent_env_defaults(pkg)

    # 断言：注入后，用户已有非占位符值仍保留
    PLACEHOLDER_PREFIXES = ("${", "sk-your-", "your-")
    injected_env = dst_env.read_text(encoding="utf-8")
    for k, user_val in user_snapshot.items():
        if not user_val or user_val.startswith(PLACEHOLDER_PREFIXES):
            continue  # 占位符/空值允许被 bridge 补
        # 用户已有真实值 → 注入后仍应能在文件中找到（可能行位置变，但值在）
        assert f"{k}={user_val}" in injected_env, (
            f"用户值 {k}={user_val} 被 bridge 注入覆盖了——违反注入契约"
        )
