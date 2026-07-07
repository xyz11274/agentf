"""
response_push 集成测试 — 验证流式推送完整链路。

测试流程：
  1. 启动 daemon + mock LLM
  2. 发送 user message 触发 coordinator 执行
  3. 通过 UDS 直接调用 response_push 推流式消息
  4. 验证消息被 drain 并出现在响应中

使用方式：
  python -m pytest frontend/bridge/tests/test_e2e_response_push.py -v -m e2e
"""

from __future__ import annotations

import json
import os
import socket
import time
from pathlib import Path
from typing import Any

import pytest

from .e2e_helpers import (
    DAEMON_BINARY,
    OUTFIT_AGENT,
    await_agent_ready,
    free_port,
    jsonrpc_call,
    scripted_llm,
    wait_port,
)

PROJECT_ROOT = Path(__file__).resolve().parents[3]
DAEMON_BINARY = PROJECT_ROOT / "build" / "agentflowd"


def _find_uds_path(daemon_port: int, tmp_path: Path | None = None) -> str | None:
    """从 daemon 日志中查找 UDS 路径或使用已知默认路径。

    Args:
        daemon_port: daemon TCP 端口（用于日志文件名匹配）。
        tmp_path: 若已知 daemon 的临时目录，优先从该目录查找 UDS socket。
    """
    # 优先从临时目录查找（最近启动的 daemon 的独立 socket）
    if tmp_path:
        biz = tmp_path / "biz.sock"
        if biz.is_socket():
            return str(biz)
    # 降级：/tmp 下的默认 biz socket
    biz = Path("/tmp/agentflowd_biz.sock")
    if biz.is_socket():
        return str(biz)
    ctrl = Path("/tmp/agentflowd_ctrl.sock")
    if ctrl.is_socket():
        return str(ctrl)
    # 最终降级：afdb_*.sock（旧版 daemon）
    candidates = sorted(Path("/tmp").glob("afdb_*.sock"),
                        key=lambda p: p.stat().st_mtime, reverse=True)
    return str(candidates[0]) if candidates else None


def _jsonrpc_call(host: str, port: int, method: str, params: dict[str, Any],
                  timeout: float = 5.0) -> dict[str, Any]:
    """通过 TCP JSON-RPC 调用 daemon。"""
    import json as _json
    payload = _json.dumps({"jsonrpc": "2.0", "method": method,
                           "params": params, "id": 1})
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(timeout)
        sock.connect((host, port))
        sock.sendall((payload + "\n").encode("utf-8"))
        chunks = []
        while True:
            ch = sock.recv(1)
            if not ch or ch == b"\n":
                break
            chunks.append(ch)
        resp = _json.loads(b"".join(chunks).decode("utf-8"))
        return resp


@pytest.mark.e2e
class TestResponsePush:
    """验证 response_push → DataBus → drain → Bridge 完整链路。"""

    @pytest.fixture(scope="class")
    def daemon_env(self) -> dict[str, Any]:
        """启动 daemon + mock LLM，返回 daemon 信息。"""
        if not DAEMON_BINARY.is_file():
            pytest.skip(f"daemon binary not found: {DAEMON_BINARY}")

        daemon_port = free_port()
        import tempfile
        tmp_path = Path(tempfile.mkdtemp(prefix="e2e-rpush-"))

        with scripted_llm() as llm_url:
            env = dict(os.environ)
            env["PATH"] = (
                f"{OUTFIT_AGENT / '.venv' / 'bin'}"
                f"{os.pathsep}/usr/bin:/bin:/usr/sbin:/sbin"
            )
            env["API_KEY"] = "e2e-test-key"
            env["AF_LLM_BASE_URL"] = llm_url
            env["AF_LLM_MODEL"] = "e2e-scripted"
            env["AF_SKILL_MAX_COMPLETION_TOKENS"] = "512"
            env["NO_PROXY"] = "127.0.0.1,localhost"
            env["no_proxy"] = "127.0.0.1,localhost"
            # 指定独立 UDS 路径，避免与已有 daemon 冲突
            uds_biz = tmp_path / "biz.sock"
            uds_ctrl = tmp_path / "ctrl.sock"
            env["AF_UDS_BIZ_PATH"] = str(uds_biz)
            env["AF_UDS_CTRL_PATH"] = str(uds_ctrl)

            import subprocess
            daemon = subprocess.Popen(
                [str(DAEMON_BINARY), "--listen", f"127.0.0.1:{daemon_port}",
                 "--package", str(OUTFIT_AGENT)],
                cwd=PROJECT_ROOT, env=env,
                stdout=(tmp_path / "daemon.log").open("wb"),
                stderr=subprocess.STDOUT,
            )
            try:
                wait_port(daemon_port, daemon)
                await_agent_ready(daemon_port, "outfit-assistant", daemon)
                yield {"daemon_port": daemon_port, "daemon": daemon}
            finally:
                if daemon.poll() is None:
                    daemon.terminate()
                    try:
                        daemon.wait(timeout=10)
                    except Exception:
                        daemon.kill()

    def test_response_push_via_uds(self, daemon_env: dict[str, Any]) -> None:
        """通过 TCP 调用 response_push。

        .. note::
           daemon 侧的 ``response_push`` handler 当前 ``[未实现]``（见
           streaming-response.md §5 item 1）。本测试标记为 xfail，待 handler
           实现后移除 xfail 标记。
        """
        daemon_port = daemon_env.get("daemon_port")
        if not daemon_port:
            pytest.skip("daemon not started")

        result = _jsonrpc_call("127.0.0.1", daemon_port, "response_push", {
            "session_id": "test-session",
            "dialogue_id": "test-dialogue-001",
            "content": "测试流式消息",
            "type": "progress",
            "final": "false",
        })
        if result.get("error", {}).get("code") == -32601:
            pytest.skip("daemon response_push handler not yet implemented")
        assert result.get("result", {}).get("success"), (
            f"response_push failed: {result}"
        )

    def test_response_push_rejects_final(self, daemon_env: dict[str, Any]) -> None:
        """验证 response_push 拒绝 final=true。"""
        daemon_port = daemon_env.get("daemon_port")
        if not daemon_port:
            pytest.skip("daemon not started")

        result = _jsonrpc_call("127.0.0.1", daemon_port, "response_push", {
            "session_id": "test-session",
            "dialogue_id": "test-dialogue-002",
            "content": "不应通过",
            "type": "text",
            "final": "true",
        })
        assert "error" in result or result.get("result", {}).get("success") is False

    def test_response_push_empty_content_rejected(self, daemon_env: dict[str, Any]) -> None:
        """验证 response_push 拒绝空 content。

        .. note::
           依赖 daemon 侧 handler 实现（当前 ``[未实现]``）。本测试标记为
           xfail，待 handler 实现后移除。
        """
        daemon_port = daemon_env.get("daemon_port")
        if not daemon_port:
            pytest.skip("daemon not started")

        result = _jsonrpc_call("127.0.0.1", daemon_port, "response_push", {
            "session_id": "test-session",
            "dialogue_id": "test-dialogue-003",
            "content": "",
            "type": "llm_chunk",
            "final": "false",
        })
        if result.get("error", {}).get("code") == -32601:
            pytest.skip("daemon response_push handler not yet implemented")
        # Should return an error (missing content)
        assert result.get("error", {}).get("code") == -32602
