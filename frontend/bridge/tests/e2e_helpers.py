"""
E2E 测试共享辅助函数 — daemon 生命周期 + mock LLM + JSON-RPC 通信。

设计原则：
  1. 每个 test class 启动自己的 daemon（≤5 次请求后 daemon 退化，class scope 避免累积）
  2. 使用 mock LLM server（e2e-scripted），不依赖真实 API
  3. 验证执行链完整性（节点 Exit code=0、response 送达），不做 LLM 输出内容断言
  4. fixture 确保 daemon 在 teardown 时被终止
"""

from __future__ import annotations

import json
import os
import signal
import socket
import subprocess
import threading
import time
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Iterator

import pytest

PROJECT_ROOT = Path(__file__).resolve().parents[3]
DAEMON_BINARY = PROJECT_ROOT / "build" / "agentflowd"
OUTFIT_AGENT = PROJECT_ROOT / "examples" / "agent" / "outfit-assistant"
OUTFIT_AGENT_STR = str(OUTFIT_AGENT)  # absolute string path for daemon CLI
LLM_REPLY = "你好，我是服装搭配助手，很高兴为您服务！"


def free_port() -> int:
    """获取随机可用端口。"""
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


# ═══════════════════════════════════════════════════════════════════
# Mock LLM 服务器（返回固定回复 e2e-scripted）
# ═══════════════════════════════════════════════════════════════════


class _ScriptedLLMHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("content-length", "0"))
        self.rfile.read(length)
        payload = json.dumps({
            "id": "chatcmpl-e2e",
            "object": "chat.completion",
            "created": 1,
            "model": "e2e-scripted",
            "choices": [{
                "index": 0,
                "message": {"role": "assistant", "content": LLM_REPLY},
                "finish_reason": "stop",
            }],
            "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15},
        }).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt, *args) -> None:
        pass


@contextmanager
def scripted_llm() -> Iterator[str]:
    """启动 mock LLM HTTP server，yield base URL。"""
    port = free_port()
    server = ThreadingHTTPServer(("127.0.0.1", port), _ScriptedLLMHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{port}/v1"
    finally:
        server.shutdown()
        server.server_close()


# ═══════════════════════════════════════════════════════════════════
# Daemon 生命周期
# ═══════════════════════════════════════════════════════════════════


def wait_port(port: int, process: subprocess.Popen, timeout: float = 15) -> None:
    """等待 TCP 端口就绪。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"daemon exited (rc={process.returncode})")
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(0.5)
                s.connect(("127.0.0.1", port))
                return
        except (ConnectionRefusedError, OSError):
            pass
        time.sleep(0.2)
    raise RuntimeError(f"port {port} not ready within {timeout}s")


def await_agent_ready(port: int, agent_id: str, process: subprocess.Popen,
                      timeout: float = 20) -> None:
    """等待 agent 注册完成（通过 agent.list JSON-RPC）。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"daemon exited (rc={process.returncode})")
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(1)
                s.connect(("127.0.0.1", port))
                req = json.dumps({
                    "jsonrpc": "2.0", "method": "agent.list",
                    "params": {}, "id": 1,
                })
                s.sendall((req + "\n").encode("utf-8"))
                chunks = []
                while True:
                    ch = s.recv(1)
                    if not ch or ch == b"\n":
                        break
                    chunks.append(ch)
                resp = json.loads(b"".join(chunks).decode("utf-8"))
                agents = resp.get("result", {}).get("agents", [])
                if any(a.get("id") == agent_id or a.get("agent_id") == agent_id for a in agents):
                    return
        except (ConnectionRefusedError, OSError, json.JSONDecodeError, KeyError):
            pass
        time.sleep(0.3)
    raise RuntimeError(f"agent {agent_id} not ready within {timeout}s")


@pytest.fixture(scope="class")
def outfit_daemon_real_llm(request: pytest.FixtureRequest,
                           tmp_path_factory: pytest.TempPathFactory) -> Iterator[dict[str, Any]]:
    """启动 outfits_assistant daemon，优先使用真实 LLM（class-scoped）。

    如果宿主机环境变量设置了 ``AF_LLM_BASE_URL`` 和 ``API_KEY``（或 ``AF_LLM_API_KEY``），
    则透传给 daemon，发起真实 LLM 调用；否则回退到 mock LLM。

    验证完整的 agentflow 执行链路：daemon → agent → workflow → LLM call。
    """
    if not DAEMON_BINARY.is_file():
        pytest.skip(f"daemon binary not built: {DAEMON_BINARY}")

    daemon_port = free_port()
    tmp_path = tmp_path_factory.mktemp(f"e2e-real-llm-{request.cls.__name__}")

    # 检查宿主机是否有真实 LLM 配置
    host_api_key = os.environ.get("AF_LLM_API_KEY") or os.environ.get("API_KEY", "")
    host_base_url = os.environ.get("AF_LLM_BASE_URL", "")
    host_model = os.environ.get("AF_LLM_MODEL", "gpt-4o-mini")
    use_real_llm = bool(host_api_key and host_base_url)

    if use_real_llm:
        # 真实 LLM 模式：透传环境变量
        env = dict(os.environ)
        env["PATH"] = (
            f"{OUTFIT_AGENT / '.venv' / 'bin'}"
            f"{os.pathsep}/usr/bin:/bin:/usr/sbin:/sbin"
        )
        env.setdefault("AF_LLM_BASE_URL", host_base_url)
        env.setdefault("AF_LLM_MODEL", host_model)
        env.setdefault("API_KEY", host_api_key)
        env["AF_SKILL_MAX_COMPLETION_TOKENS"] = "512"
        env["NO_PROXY"] = "127.0.0.1,localhost"
        env["no_proxy"] = "127.0.0.1,localhost"

        daemon = subprocess.Popen(
            [str(DAEMON_BINARY), "--listen", f"127.0.0.1:{daemon_port}",
             "--package", OUTFIT_AGENT_STR],
            cwd=PROJECT_ROOT, env=env,
            stdout=(tmp_path / "daemon.log").open("wb"),
            stderr=subprocess.STDOUT,
        )
        llm_mode = "REAL"
    else:
        # 回退到 mock LLM
        ctx = scripted_llm()
        llm_url = ctx.__enter__()
        env = {
            "PATH": (
                f"{OUTFIT_AGENT / '.venv' / 'bin'}"
                f"{os.pathsep}/usr/bin:/bin:/usr/sbin:/sbin"
            ),
            "API_KEY": "e2e-test-key",
            "AF_LLM_BASE_URL": llm_url,
            "AF_LLM_MODEL": "e2e-scripted",
            "AF_SKILL_MAX_COMPLETION_TOKENS": "512",
            "NO_PROXY": "127.0.0.1,localhost",
            "no_proxy": "127.0.0.1,localhost",
        }
        daemon = subprocess.Popen(
            [str(DAEMON_BINARY), "--listen", f"127.0.0.1:{daemon_port}",
             "--package", OUTFIT_AGENT_STR],
            cwd=PROJECT_ROOT, env=env,
            stdout=(tmp_path / "daemon.log").open("wb"),
            stderr=subprocess.STDOUT,
        )
        llm_mode = "MOCK"
        # 注意：ctx.__exit__ 要在 finally 中处理

    try:
        wait_port(daemon_port, daemon)
        await_agent_ready(daemon_port, "outfit-assistant", daemon)
        yield {"daemon_port": daemon_port, "daemon": daemon,
               "llm_mode": llm_mode, "use_real_llm": use_real_llm}
    finally:
        if daemon.poll() is None:
            daemon.send_signal(signal.SIGTERM)
            try:
                daemon.wait(timeout=12)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait(timeout=3)
        if not use_real_llm:
            try:
                ctx.__exit__(None, None, None)
            except Exception:
                pass


# Backward-compatible alias: the original outfit_daemon fixture is now
# outfit_daemon_real_llm.  Tests that import outfit_daemon (outfit_agent,
# outfit_multiturn, outfit_skill) get the same fixture.
outfit_daemon = outfit_daemon_real_llm


# ═══════════════════════════════════════════════════════════════════
# JSON-RPC 通信
# ═══════════════════════════════════════════════════════════════════


# ═══════════════════════════════════════════════════════════════════
# JSON-RPC 通信
# ═══════════════════════════════════════════════════════════════════


def jsonrpc_call(host: str, port: int, method: str,
                 params: dict, req_id: int = 1) -> dict:
    """发送 JSON-RPC 请求到 daemon，返回 \n 分隔的单行响应。"""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(30)
        sock.connect((host, port))
        payload = json.dumps({
            "jsonrpc": "2.0", "method": method,
            "params": params, "id": req_id,
        })
        sock.sendall((payload + "\n").encode("utf-8"))
        chunks = []
        while True:
            ch = sock.recv(1)
            if not ch or ch == b"\n":
                break
            chunks.append(ch)
        return json.loads(b"".join(chunks).decode("utf-8"))


def send_user_message(host: str, port: int, content: str,
                      task_id: str = "e2e-outfit-001") -> dict:
    """通过 daemon 直接发送用户消息（JSON-RPC agent.execute），返回响应。"""
    return jsonrpc_call(host, port, "agent.execute", {
        "agent_id": "outfit-assistant",
        "user_id": "e2e-user",
        "client_id": task_id,
        "content": content,
        "request_id": task_id,
    }, task_id)


def poll_agent_response(host: str, port: int, task_id: str,
                        timeout: int = 120) -> str:
    """轮询 daemon 的 agent.poll_response，等待 task 完成，返回最终消息内容。

    注意事项：
      agent.poll_response 在 daemon 中存在已知限制：
      - 消息可能被 ResponsePoller（50ms 周期）异步通知消费
      - 消费后 poll 返回 messages=[]、complete=false
      - 因此需要高频轮询，在 ResponsePoller 消费前捕获消息
      - 同时 fallback 到检查 workflow 是否已完成（通过 workflow.list_instances）
    """
    deadline = time.monotonic() + timeout
    poll_count = 0
    while time.monotonic() < deadline:
        poll_count += 1

        # 高频 poll（每 100ms），尝试在 ResponsePoller 消费前捕获
        resp = jsonrpc_call(host, port, "agent.poll_response", {
            "agent_id": "outfit-assistant",
            "user_id": "e2e-user",
            "client_id": task_id,
            "timeout_ms": "500",
        }, req_id=2)
        result = resp.get("result", {})
        messages = result.get("messages", [])
        complete = result.get("complete", False)

        contents = [m.get("content", "") for m in messages if m.get("content")]
        if contents:
            return contents[-1]

        # 每 2 秒尝试一次 workflow.list_instances 兜底
        if poll_count % 20 == 0:
            wf = jsonrpc_call(host, port, "workflow.list_instances", {}, req_id=3)
            wf_result = wf.get("result", {})
            instances = wf_result.get("instances", []) if isinstance(wf_result, dict) else []
            running = any(
                inst.get("status") == "running" for inst in instances
                if isinstance(inst, dict)
            )
            if not running and not contents:
                # 无运行中 workflow → 任务已完成
                return ""

        time.sleep(0.1)
    raise TimeoutError(f"task {task_id} not completed within {timeout}s "
                       f"(polls={poll_count}, last_complete={complete})")


@pytest.fixture(scope="class")
def compression_daemon(request: pytest.FixtureRequest,
                       tmp_path_factory: pytest.TempPathFactory) -> Iterator[dict[str, Any]]:
    """同 outfit_daemon_real_llm，但设置极小的 AF_MODEL_MAX_TOKENS（500 tok）以触发上下文压缩。

    用于 E2E 验证：[flow] compaction: 日志行在 multi-turn 场景中正确出现。
    始终使用 mock LLM（不需要真实 API key）。
    """
    if not DAEMON_BINARY.is_file():
        pytest.skip(f"daemon binary not built: {DAEMON_BINARY}")

    daemon_port = free_port()
    tmp_path = tmp_path_factory.mktemp(f"e2e-compress-{request.cls.__name__}")

    with scripted_llm() as llm_url:
        env = {
            "PATH": (
                f"{OUTFIT_AGENT / '.venv' / 'bin'}"
                f"{os.pathsep}/usr/bin:/bin:/usr/sbin:/sbin"
            ),
            "API_KEY": "e2e-test-key",
            "AF_LLM_BASE_URL": llm_url,
            "AF_LLM_MODEL": "e2e-scripted",
            "AF_SKILL_MAX_COMPLETION_TOKENS": "512",
            # 极小窗口强制压缩：threshold = 500 * 0.8 = 400 tok
            # system prompt ≈ 400 tok → 第一轮刚好不触发，
            # 第二轮（含历史对话）> budget → 压缩触发
            "AF_MODEL_MAX_TOKENS": "500",
            "NO_PROXY": "127.0.0.1,localhost",
            "no_proxy": "127.0.0.1,localhost",
        }

        daemon = subprocess.Popen(
            [str(DAEMON_BINARY), "--listen", f"127.0.0.1:{daemon_port}",
             "--package", str(OUTFIT_AGENT)],
            cwd=PROJECT_ROOT, env=env,
            stdout=(tmp_path / "daemon.log").open("wb"),
            stderr=subprocess.STDOUT,
        )
        try:
            wait_port(daemon_port, daemon)
            # daemon 不支持 list_agents JSON-RPC，wait_port 成功即就绪
            yield {"daemon_port": daemon_port, "daemon": daemon,
                   "log_path": str(tmp_path / "daemon.log")}
        finally:
            if daemon.poll() is None:
                daemon.send_signal(signal.SIGTERM)
                try:
                    daemon.wait(timeout=12)
                except subprocess.TimeoutExpired:
                    daemon.kill()
                    daemon.wait(timeout=3)
