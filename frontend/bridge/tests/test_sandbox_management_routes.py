"""Sandbox 管理 REST 路由测试。

用独立 FastAPI app 挂载 sandbox router，注入 fake SandboxObserver，
验证端点响应信封、错误映射（404/400/502）与 exec 契约。
"""

from __future__ import annotations

from fastapi import FastAPI
from fastapi.testclient import TestClient

from routes.sandbox import router as sandbox_router


class _FakeObserver:
    """最小 fake SandboxObserver（路由层只依赖统一接口）。"""

    def __init__(self, mode="local", sandboxes=None):
        self._mode = mode
        self._sandboxes = sandboxes or []

    @property
    def mode(self):
        return self._mode

    def capabilities(self):
        # E1/E2 后三模式统一支持 kill/prune（local/docker 经 daemon RPC 通道）
        return {"list": True, "kill": True, "prune": True, "files": True}

    async def list_sandboxes(self, state=None, mode=None):
        return self._sandboxes

    async def get_sandbox(self, sandbox_id):
        for sb in self._sandboxes:
            if sb.sandbox_id == sandbox_id:
                return sb
        from services.sandbox_observer import SandboxNotFound
        raise SandboxNotFound(f"Sandbox {sandbox_id} not found")

    async def kill_sandbox(self, sandbox_id):
        from services.sandbox_observer import KillResult
        return KillResult(sandbox_id=sandbox_id, killed=True, already_gone=False)

    async def prune_stale(self, older_minutes, dry_run=True):
        from services.sandbox_observer import PruneResult
        return PruneResult(dry_run=dry_run, older_minutes=older_minutes, stale=[], killed=0, failed=0)

    async def exec_command(self, sandbox_id, command, timeout_ms=30000, cwd=None):
        from services.sandbox_observer import ExecResult
        return ExecResult(
            sandbox_id=sandbox_id, command=command, exit_code=0,
            stdout=f"ok:{command}", stderr="", truncated=False, duration_ms=5,
        )


def _make_client(observer=None):
    app = FastAPI()
    app.state.sandbox_observer = observer or _FakeObserver(mode="local")
    app.include_router(sandbox_router)
    return TestClient(app)


def _sb(sandbox_id="sb-1", mode="local"):
    from services.sandbox_observer import SandboxInfo
    return SandboxInfo(
        sandbox_id=sandbox_id,
        mode=mode,
        state="RUNNING",
        owner={"user_id": "u1", "client_id": "c1", "agent_id": "a1"},
        endpoint="http://127.0.0.1:8080",
        created_at="2026-08-02T10:00:00+00:00",
        age_seconds=60,
        source="daemon_log",
    )


# ── overview ────────────────────────────────────────────────────────


def test_overview_local_mode():
    client = _make_client(_FakeObserver(mode="local", sandboxes=[_sb()]))
    resp = client.get("/api/sandbox/overview")
    assert resp.status_code == 200, f"{resp.status_code} {resp.text}"
    data = resp.json()
    assert data["success"] is True
    assert data["data"]["mode"] == "local"
    assert data["data"]["capabilities"] == {"list": True, "kill": True, "prune": True, "files": True}
    assert data["data"]["counts"]["total"] == 1


def test_overview_e2b_mode():
    client = _make_client(_FakeObserver(mode="e2b"))
    resp = client.get("/api/sandbox/overview")
    assert resp.status_code == 200
    data = resp.json()["data"]
    assert data["mode"] == "e2b"
    assert data["capabilities"]["kill"] is True


# ── list / detail ───────────────────────────────────────────────────


def test_list_sandboxes():
    client = _make_client(_FakeObserver(mode="local", sandboxes=[_sb("sb-a"), _sb("sb-b")]))
    resp = client.get("/api/sandbox/list")
    assert resp.status_code == 200
    data = resp.json()["data"]
    assert data["total"] == 2
    assert [s["sandbox_id"] for s in data["sandboxes"]] == ["sb-a", "sb-b"]


def test_detail_found():
    client = _make_client(_FakeObserver(mode="local", sandboxes=[_sb("sb-1")]))
    resp = client.get("/api/sandbox/sb-1")
    assert resp.status_code == 200
    assert resp.json()["data"]["sandbox_id"] == "sb-1"


def test_detail_not_found():
    client = _make_client(_FakeObserver(mode="local", sandboxes=[_sb("sb-1")]))
    resp = client.get("/api/sandbox/sb-missing")
    assert resp.status_code == 404, f"{resp.status_code} {resp.text}"
    assert "not found" in resp.json()["detail"].lower()


# ── kill / prune ────────────────────────────────────────────────────


def test_kill_ok():
    client = _make_client(_FakeObserver(mode="e2b"))
    resp = client.post("/api/sandbox/sb-1/kill")
    assert resp.status_code == 200
    data = resp.json()["data"]
    assert data["killed"] is True


def test_prune_dry_run_default():
    client = _make_client(_FakeObserver(mode="e2b"))
    resp = client.post("/api/sandbox/prune?older_minutes=30")
    assert resp.status_code == 200
    data = resp.json()["data"]
    assert data["dry_run"] is True
    assert data["older_minutes"] == 30


def test_prune_invalid_minutes():
    client = _make_client(_FakeObserver(mode="e2b"))
    resp = client.post("/api/sandbox/prune?older_minutes=0")
    assert resp.status_code == 400


# ── exec（webshell）─────────────────────────────────────────────────


def test_exec_ok():
    client = _make_client(_FakeObserver(mode="e2b"))
    resp = client.post("/api/sandbox/sb-1/exec", json={"command": "ls -la"})
    assert resp.status_code == 200, f"{resp.status_code} {resp.text}"
    data = resp.json()["data"]
    assert data["exit_code"] == 0
    assert data["stdout"] == "ok:ls -la"
    assert data["command"] == "ls -la"


def test_exec_cwd_passthrough():
    """cwd 字段从请求体透传到 observer.exec_command——锁住 I-3 元数据一致性。

    不变量：cwd 路径要原样透传，不得静默改写或丢弃。
    """
    captured: dict = {}

    class _CapturingObserver(_FakeObserver):
        async def exec_command(self, sandbox_id, command, timeout_ms=30000, cwd=None):
            captured["cwd"] = cwd
            from services.sandbox_observer import ExecResult
            return ExecResult(
                sandbox_id=sandbox_id, command=command, exit_code=0,
                stdout=f"ok:{command}", stderr="", truncated=False, duration_ms=5,
            )

    client = _make_client(_CapturingObserver(mode="e2b"))
    resp = client.post(
        "/api/sandbox/sb-1/exec",
        json={"command": "pwd", "cwd": "/workspace/logs"},
    )
    assert resp.status_code == 200, f"{resp.status_code} {resp.text}"
    assert captured["cwd"] == "/workspace/logs"


def test_exec_cwd_omitted():
    """cwd 缺省时 observer 收到 None——保持 SDK 默认 cwd 行为。"""
    captured: dict = {}

    class _CapturingObserver(_FakeObserver):
        async def exec_command(self, sandbox_id, command, timeout_ms=30000, cwd=None):
            captured["cwd"] = cwd
            from services.sandbox_observer import ExecResult
            return ExecResult(
                sandbox_id=sandbox_id, command=command, exit_code=0,
                stdout=f"ok:{command}", stderr="", truncated=False, duration_ms=5,
            )

    client = _make_client(_CapturingObserver(mode="e2b"))
    resp = client.post("/api/sandbox/sb-1/exec", json={"command": "pwd"})
    assert resp.status_code == 200
    assert captured["cwd"] is None


def test_exec_empty_command():
    client = _make_client(_FakeObserver(mode="e2b"))
    resp = client.post("/api/sandbox/sb-1/exec", json={"command": "   "})
    assert resp.status_code == 400


def test_exec_unavailable_mapping():
    """ExecUnavailable → 502（对齐 L4 契约 §3）。"""
    from services.sandbox_observer import ExecUnavailable

    class _UnavailableObserver(_FakeObserver):
        async def exec_command(self, sandbox_id, command, timeout_ms=30000, cwd=None):
            raise ExecUnavailable("no endpoint for sandbox sb-x")

    client = _make_client(_UnavailableObserver(mode="local"))
    resp = client.post("/api/sandbox/sb-x/exec", json={"command": "echo hi"})
    assert resp.status_code == 502
    assert "no endpoint" in resp.json()["detail"]


def test_unsupported_action_mapping():
    """UnsupportedAction → 400（local 模式 kill）。"""
    from services.sandbox_observer import UnsupportedAction

    class _ReadOnlyObserver(_FakeObserver):
        async def kill_sandbox(self, sandbox_id):
            raise UnsupportedAction("local mode is read-only")

    client = _make_client(_ReadOnlyObserver(mode="local"))
    resp = client.post("/api/sandbox/sb-1/kill")
    assert resp.status_code == 400
    assert "read-only" in resp.json()["detail"]
