"""SandboxObserver 适配层单元测试。

覆盖：
- 模式分派（local → LocalProvider / e2b → E2BProvider）
- LocalProvider 走 daemon RPC（sandbox.list 枚举 / sandbox.destroy 销毁），
  数据源为 daemon 内存活跃沙箱（2026-08-02 重构，移除日志解析/目录扫描）
- SandboxObserver kill/prune 分派：e2b 走 SDK、local/docker 走 daemon RPC（E2）
- exec_command 两条通道：Local 走沙箱内 HTTP server /exec、E2B 走 SDK commands.run
"""

from __future__ import annotations

import asyncio
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from types import SimpleNamespace

import pytest

from services.sandbox_observer import (
    E2BProvider,
    ExecResult,
    ExecUnavailable,
    LocalProvider,
    ProviderError,
    SandboxNotFound,
    SandboxObserver,
)

# ── fixtures ────────────────────────────────────────────────────────


class _ExecServer(BaseHTTPRequestHandler):
    """模拟沙箱内 HTTP server：POST /exec 返回固定 ExecResult。"""

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(length).decode("utf-8")) if length else {}
        assert body.get("tool") == "bash", f"tool 应为 bash: {body}"
        assert body.get("tool_params", {}).get("command"), f"command 缺失: {body}"
        resp = json.dumps(
            {
                "exit_code": 0,
                "stdout": f"out:{body['tool_params']['command']}",
                "stderr": "",
                "truncated": False,
                "duration_ms": 12,
            }
        ).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)

    def log_message(self, *args):
        pass


@pytest.fixture
def exec_server():
    """启动本地 HTTP server 模拟沙箱内 /exec 通道。"""
    server = ThreadingHTTPServer(("127.0.0.1", 0), _ExecServer)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield f"http://127.0.0.1:{port}"
    server.shutdown()


def _make_settings(mode: str = "local", **overrides):
    base = {
        "sandbox_mode": mode,
        "sandbox_e2b_api_url": "http://127.0.0.1:1",
        "sandbox_e2b_api_key": "test-key",
        "sandbox_e2b_ca_cert_path": "",
    }
    base.update(overrides)
    return SimpleNamespace(**base)


def _rpc_list_result(sandboxes: list[dict], server_now_ms: int) -> dict:
    """构造 sandbox.list RPC 响应（对齐 rpc_handler.cpp handle_sandbox_list）。"""
    return {"sandboxes": sandboxes, "total": len(sandboxes), "server_now_ms": server_now_ms}


# ── 模式分派 ────────────────────────────────────────────────────────


def test_observer_dispatches_local():
    obs = SandboxObserver(_make_settings(mode="local"))
    assert isinstance(obs._provider, LocalProvider)
    assert obs.mode == "local"
    # E1/E2 后三模式统一支持 list/kill/prune（local/docker 经 daemon RPC 通道）
    assert obs.capabilities() == {"list": True, "kill": True, "prune": True, "files": True}


def test_observer_dispatches_e2b():
    obs = SandboxObserver(_make_settings(mode="e2b"))
    assert isinstance(obs._provider, E2BProvider)
    assert obs.mode == "e2b"
    assert obs.capabilities() == {"list": True, "kill": True, "prune": True, "files": True}


def test_observer_rejects_unknown_mode():
    with pytest.raises(ProviderError):
        SandboxObserver(_make_settings(mode="quantum"))


# ── SandboxObserver：local/docker 经 daemon RPC（E2）─────────────────


class _FakeDaemonClient:
    """记录 send_request 调用的 fake daemon client（async）。

    支持按方法返回不同结果：``results`` 为 ``{method: result}`` 映射，
    未命中方法回退 ``result``（单一固定响应场景）。
    """

    def __init__(self, result=None, exc=None, results=None):
        self.calls: list[tuple[str, dict]] = []
        self._result = result
        self._exc = exc
        self._results = results or {}

    async def send_request(self, method: str, params: dict | None = None):
        self.calls.append((method, params or {}))
        if self._exc is not None:
            raise self._exc
        if method in self._results:
            return self._results[method]
        return self._result


def test_local_kill_goes_through_daemon_rpc():
    """local 模式 kill 走 daemon sandbox.destroy RPC，返回 KillResult。"""
    client = _FakeDaemonClient(result={"destroyed": True, "already_gone": False})
    obs = SandboxObserver(_make_settings(mode="local"), daemon_client=client)

    result = asyncio.run(obs.kill_sandbox("sb-1"))
    assert client.calls == [("sandbox.destroy", {"sandbox_id": "sb-1"})]
    assert result.sandbox_id == "sb-1"
    assert result.killed is True
    assert result.already_gone is False


def test_local_kill_already_gone_is_idempotent():
    """daemon 返回 already_gone=true（或 404）时按幂等处理，不报错。"""
    client = _FakeDaemonClient(result={"destroyed": False, "already_gone": True})
    obs = SandboxObserver(_make_settings(mode="local"), daemon_client=client)

    result = asyncio.run(obs.kill_sandbox("sb-gone"))
    assert result.killed is False
    assert result.already_gone is True


def test_local_kill_requires_daemon_client():
    """未注入 daemon_client 时 kill 抛 ProviderError（销毁必须经 daemon，I-5）。"""
    obs = SandboxObserver(_make_settings(mode="local"), daemon_client=None)

    with pytest.raises(ProviderError):
        asyncio.run(obs.kill_sandbox("sb-x"))


def test_local_list_uses_sandbox_list_rpc():
    """local 模式 list 走 sandbox.list RPC，返回真实 state/owner/endpoint。"""
    client = _FakeDaemonClient(
        result=_rpc_list_result(
            [
                {
                    "sandbox_id": "sb-1",
                    "state": "RUNNING",
                    "mode": "local",
                    "owner": {"user_id": "u1", "client_id": "c1", "agent_id": "a1"},
                    "endpoint": "http://127.0.0.1:8080",
                    "workdir": "/tmp/dev/sandbox/sb-1",
                    "last_active_at": 1_000_000,
                }
            ],
            server_now_ms=1_000_000 + 3_600_000,  # 闲置 1 小时
        )
    )
    obs = SandboxObserver(_make_settings(mode="local"), daemon_client=client)

    sandboxes = asyncio.run(obs.list_sandboxes())
    assert client.calls == [("sandbox.list", {})]
    assert len(sandboxes) == 1
    sb = sandboxes[0]
    assert sb.sandbox_id == "sb-1"
    assert sb.state == "RUNNING"
    assert sb.source == "daemon_rpc"
    assert sb.owner == {"user_id": "u1", "client_id": "c1", "agent_id": "a1"}
    assert sb.endpoint == "http://127.0.0.1:8080"
    assert sb.age_seconds == 3600  # idle = server_now_ms - last_active_at


def test_local_list_passes_state_filter():
    """state 过滤参数透传给 sandbox.list（大写）。"""
    client = _FakeDaemonClient(result=_rpc_list_result([], server_now_ms=1_000))
    obs = SandboxObserver(_make_settings(mode="local"), daemon_client=client)

    asyncio.run(obs.list_sandboxes(state="running"))
    assert client.calls == [("sandbox.list", {"state": "RUNNING"})]


def test_local_list_daemon_unreachable_raises_provider_error():
    """daemon 不可达时 list 抛 ProviderError（不再有离线兜底空列表）。"""
    client = _FakeDaemonClient(exc=RuntimeError("connection refused"))
    obs = SandboxObserver(_make_settings(mode="local"), daemon_client=client)

    with pytest.raises(ProviderError):
        asyncio.run(obs.list_sandboxes())


def test_local_get_sandbox_not_found():
    """list 中不存在目标沙箱时 get 抛 SandboxNotFound（404）。"""
    client = _FakeDaemonClient(result=_rpc_list_result([], server_now_ms=1_000))
    obs = SandboxObserver(_make_settings(mode="local"), daemon_client=client)

    with pytest.raises(SandboxNotFound):
        asyncio.run(obs.get_sandbox("sb-nope"))


def test_local_prune_uses_daemon_destroy():
    """local 模式 prune：按闲置时长（last_active_at 相对 server_now）判定超龄，
    逐个经 daemon sandbox.destroy 销毁（非 dry_run）。"""
    # sb-old 闲置 2 小时（≥30min 阈值），sb-fresh 闲置 1 分钟
    server_now = 2_000_000_000
    client = _FakeDaemonClient(
        result={"destroyed": True, "already_gone": False},
        results={
            "sandbox.list": _rpc_list_result(
                [
                    {
                        "sandbox_id": "sb-old",
                        "state": "RUNNING",
                        "mode": "local",
                        "owner": {"user_id": "u1", "client_id": "c1", "agent_id": "a1"},
                        "last_active_at": server_now - 2 * 3600 * 1000,
                    },
                    {
                        "sandbox_id": "sb-fresh",
                        "state": "RUNNING",
                        "mode": "local",
                        "owner": {"user_id": "u2", "client_id": "c2", "agent_id": "a2"},
                        "last_active_at": server_now - 60 * 1000,
                    },
                ],
                server_now_ms=server_now,
            )
        },
    )
    obs = SandboxObserver(_make_settings(mode="local"), daemon_client=client)

    result = asyncio.run(obs.prune_stale(older_minutes=30, dry_run=False))
    # 只有 sb-old 超龄（30 分钟阈值），sb-fresh 保留
    assert [m for m, _ in client.calls] == ["sandbox.list", "sandbox.destroy"]
    destroyed = [p for m, p in client.calls if m == "sandbox.destroy"]
    assert [p["sandbox_id"] for p in destroyed] == ["sb-old"]
    assert result.killed == 1
    assert result.failed == 0
    assert [s["sandbox_id"] for s in result.stale] == ["sb-old"]


def test_local_prune_dry_run_does_not_destroy():
    """local 模式 prune dry_run：只返回超龄列表，不发起 destroy。"""
    server_now = 2_000_000_000
    client = _FakeDaemonClient(
        results={
            "sandbox.list": _rpc_list_result(
                [
                    {
                        "sandbox_id": "sb-old",
                        "state": "RUNNING",
                        "mode": "local",
                        "last_active_at": server_now - 2 * 3600 * 1000,
                    }
                ],
                server_now_ms=server_now,
            )
        }
    )
    obs = SandboxObserver(_make_settings(mode="local"), daemon_client=client)

    result = asyncio.run(obs.prune_stale(older_minutes=30, dry_run=True))
    assert [m for m, _ in client.calls] == ["sandbox.list"]  # dry_run 不发起 destroy
    assert result.killed == 0
    assert [s["sandbox_id"] for s in result.stale] == ["sb-old"]


# ── LocalProvider：daemon RPC 数据源（sandbox.list）──────────────


def test_local_provider_lists_via_rpc():
    """LocalProvider 直接经 sandbox.list RPC 枚举（真实 state，非 UNKNOWN）。"""
    client = _FakeDaemonClient(
        result=_rpc_list_result(
            [
                {
                    "sandbox_id": "sb-rpc",
                    "state": "RUNNING",
                    "mode": "local",
                    "owner": {"user_id": "u1", "client_id": "c1", "agent_id": "a1"},
                }
            ],
            server_now_ms=1_000,
        )
    )
    provider = LocalProvider(mode="local", daemon_client=client)

    sandboxes = asyncio.run(provider.list_sandboxes())
    assert len(sandboxes) == 1
    sb = sandboxes[0]
    assert sb.sandbox_id == "sb-rpc"
    assert sb.state == "RUNNING"
    assert sb.source == "daemon_rpc"


def test_local_provider_rpc_error_raises_provider_error():
    """daemon RPC 失败（不可达）时抛 ProviderError，不再返回空列表兜底。"""
    client = _FakeDaemonClient(exc=RuntimeError("daemon down"))
    provider = LocalProvider(mode="local", daemon_client=client)

    with pytest.raises(ProviderError):
        asyncio.run(provider.list_sandboxes())


# ── LocalProvider：exec_command 通道（沙箱内 HTTP server /exec）─────


def test_local_exec_command_via_http(exec_server):
    """exec 的 endpoint 来自 sandbox.list RPC（权威源），再 POST 沙箱内 /exec。"""
    client = _FakeDaemonClient(
        result=_rpc_list_result(
            [
                {
                    "sandbox_id": "sb-http",
                    "state": "RUNNING",
                    "mode": "local",
                    "owner": {"user_id": "u1", "client_id": "c1", "agent_id": "a1"},
                    "endpoint": exec_server,
                }
            ],
            server_now_ms=1_000,
        )
    )
    provider = LocalProvider(mode="local", daemon_client=client)

    result = asyncio.run(provider.exec_command("sb-http", "ls -la", timeout_ms=30000))
    assert isinstance(result, ExecResult)
    assert result.exit_code == 0
    assert result.stdout == "out:ls -la"
    assert result.command == "ls -la"


def test_local_exec_command_no_endpoint():
    """RPC 返回的 endpoint 缺失时抛 ExecUnavailable。"""
    client = _FakeDaemonClient(
        result=_rpc_list_result(
            [{"sandbox_id": "sb-nep", "state": "RUNNING", "mode": "local"}],
            server_now_ms=1_000,
        )
    )
    provider = LocalProvider(mode="local", daemon_client=client)

    with pytest.raises(ExecUnavailable):
        asyncio.run(provider.exec_command("sb-nep", "echo hi"))


def test_local_exec_command_empty_command():
    provider = LocalProvider(mode="local", daemon_client=_FakeDaemonClient())

    with pytest.raises(ProviderError):
        asyncio.run(provider.exec_command("sb-x", "   "))


# ── E2BProvider：exec_command 通道（SDK commands.run）───────────────


class _FakeCommandResult:
    exit_code = 0
    stdout = "e2b stdout"
    stderr = ""


class _FakeSandbox:
    def __init__(self, commands=None):
        self.commands = commands or SimpleNamespace(
            run=lambda cmd, timeout: _FakeCommandResult()
        )


def test_e2b_exec_command_via_sdk(monkeypatch):
    provider = E2BProvider(
        api_url="http://127.0.0.1:1",
        api_key="test-key",
        ca_cert_path="",
    )
    fake_sb = _FakeSandbox()

    def fake_import_sdk():
        return (object(), object(), object())

    def fake_connect(sandbox_cls, sandbox_id):
        assert sandbox_id == "sb-e2b"
        return fake_sb

    monkeypatch.setattr(provider, "_import_sdk", fake_import_sdk)
    monkeypatch.setattr(provider, "_connect_sandbox", fake_connect)

    result = provider.exec_command("sb-e2b", "echo hello", timeout_ms=10000)
    assert isinstance(result, ExecResult)
    assert result.exit_code == 0
    assert result.stdout == "e2b stdout"
    assert result.command == "echo hello"


def test_e2b_exec_command_sdk_error(monkeypatch):
    provider = E2BProvider(api_url="http://127.0.0.1:1", api_key="k", ca_cert_path="")
    fake_sb = _FakeSandbox(
        commands=SimpleNamespace(run=lambda cmd, timeout: (_ for _ in ()).throw(
            RuntimeError("command timed out")
        ))
    )

    def fake_import_sdk():
        return (object(), object(), object())

    monkeypatch.setattr(provider, "_import_sdk", fake_import_sdk)
    monkeypatch.setattr(provider, "_connect_sandbox", lambda cls, sid: fake_sb)

    with pytest.raises(ExecUnavailable):
        provider.exec_command("sb-e2b", "sleep 100")
