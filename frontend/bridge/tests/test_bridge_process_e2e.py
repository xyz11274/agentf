"""Deterministic real-process E2E tests for Bridge -> daemon -> Agent."""

from __future__ import annotations

import asyncio
import json
import os
import re
import signal
import socket
import subprocess
import sys
import threading
import time
import urllib.request
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Iterator

import httpx
import pytest
import websockets

PROJECT_ROOT = Path(__file__).resolve().parents[3]
DAEMON_BINARY = PROJECT_ROOT / "build" / "agentflowd"
OUTFIT_AGENT = PROJECT_ROOT / "examples" / "agent" / "outfit-assistant"
CONFIRMATION = "E2E_BRIDGE_CONFIRMATION_7F3A"
LARGE_REQUEST_MARKER = "E2E_LARGE_RESPONSE_REQUEST"
LARGE_RESPONSE = "LARGE_BEGIN|" + ("0123456789abcdef" * 12_000) + "|LARGE_END"
CONTEXT_SECRET = "CONTEXT_SECRET_A91C"
TRUNCATED_TOOL_MARKER = "E2E_TRUNCATED_TOOL_RECOVERY"
INVALID_TOOL_MARKER = "E2E_INVALID_TOOL_EXHAUSTION"
ARTIFACT_FAILURE_MARKER = "E2E_ARTIFACT_THEN_LLM_FAILURE"
TRUNCATED_TOOL_TARGET = Path("/tmp/agentflow_e2e_truncated_tool_must_not_run.txt")
ARTIFACT_RELATIVE_PATH = Path("outfit-assistant/outputs/e2e-artifact/look.png")
ARTIFACT_FILE = PROJECT_ROOT / "examples" / "agent" / ARTIFACT_RELATIVE_PATH
ARTIFACT_BYTES = (
    b"\x89PNG\r\n\x1a\n"
    b"\x00\x00\x00\rIHDR"
    b"agentflow-e2e-artifact"
)


def _free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


class _ScriptedLLMHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_POST(self) -> None:  # noqa: N802 - stdlib callback name
        length = int(self.headers.get("content-length", "0"))
        raw = self.rfile.read(length)
        request = json.loads(raw or b"{}")
        self.server.requests.append(request)  # type: ignore[attr-defined]
        messages = request.get("messages", [])
        serialized_messages = json.dumps(messages, ensure_ascii=False)
        system_content = str(messages[0].get("content", "")) if messages else ""
        is_skill_executor = system_content.startswith("## Skill:")
        latest_user = ""
        for message in reversed(messages):
            if message.get("role") == "user":
                latest_user = str(message.get("content", ""))
                break

        message: dict[str, Any] = {"role": "assistant", "content": CONFIRMATION}
        finish_reason = "stop"
        response_error: str | None = None

        if (
            not is_skill_executor
            and any(marker in latest_user for marker in (
                TRUNCATED_TOOL_MARKER,
                INVALID_TOOL_MARKER,
                ARTIFACT_FAILURE_MARKER,
            ))
        ):
            marker = next(marker for marker in (
                TRUNCATED_TOOL_MARKER,
                INVALID_TOOL_MARKER,
                ARTIFACT_FAILURE_MARKER,
            ) if marker in latest_user)
            message = {
                "role": "assistant",
                "content": None,
                "tool_calls": [{
                    "id": f"dispatch-{marker}",
                    "type": "function",
                    "function": {
                        "name": "run_skills",
                        "arguments": json.dumps({
                            "skills": [{
                                "name": "outfit-stylist",
                                "prompt": marker,
                            }],
                            "reason": "deterministic e2e recovery test",
                        }),
                    },
                }],
            }
            finish_reason = "tool_calls"
        elif not is_skill_executor and latest_user.startswith("Skill '"):
            if TRUNCATED_TOOL_MARKER in serialized_messages:
                message["content"] = "TRUNCATED_RECOVERY_FINAL_OK"
            elif INVALID_TOOL_MARKER in serialized_messages:
                message["content"] = "INVALID_RECOVERY_FINAL_OK"
            elif ARTIFACT_FAILURE_MARKER in serialized_messages:
                message["content"] = "ARTIFACT_FAILURE_FINAL_OK"
        elif is_skill_executor and TRUNCATED_TOOL_MARKER in serialized_messages:
            count = self.server.scenario_counts.get(TRUNCATED_TOOL_MARKER, 0)  # type: ignore[attr-defined]
            self.server.scenario_counts[TRUNCATED_TOOL_MARKER] = count + 1  # type: ignore[attr-defined]
            if count == 0:
                message = {
                    "role": "assistant",
                    "content": None,
                    "tool_calls": [{
                        "id": "truncated-write",
                        "type": "function",
                        "function": {
                            "name": "write_file",
                            "arguments": json.dumps({
                                "file_path": str(TRUNCATED_TOOL_TARGET),
                                "content": "MUST_NOT_BE_WRITTEN",
                            }),
                        },
                    }],
                }
                finish_reason = "length"
            else:
                message["content"] = "TRUNCATED_SKILL_RECOVERED"
        elif is_skill_executor and INVALID_TOOL_MARKER in serialized_messages:
            count = self.server.scenario_counts.get(INVALID_TOOL_MARKER, 0)  # type: ignore[attr-defined]
            self.server.scenario_counts[INVALID_TOOL_MARKER] = count + 1  # type: ignore[attr-defined]
            message = {
                "role": "assistant",
                "content": None,
                "tool_calls": [{
                    "id": f"invalid-write-{count}",
                    "type": "function",
                    "function": {
                        "name": "write_file",
                        "arguments": '{"file_path":"/tmp/never","content":',
                    },
                }],
            }
            finish_reason = "tool_calls"
        elif is_skill_executor and ARTIFACT_FAILURE_MARKER in serialized_messages:
            count = self.server.scenario_counts.get(ARTIFACT_FAILURE_MARKER, 0)  # type: ignore[attr-defined]
            self.server.scenario_counts[ARTIFACT_FAILURE_MARKER] = count + 1  # type: ignore[attr-defined]
            if count == 0:
                encoded = ARTIFACT_BYTES.hex()
                command = (
                    "python3 -c \"import pathlib; "
                    "p=pathlib.Path('outputs/e2e-artifact/look.png'); "
                    "p.parent.mkdir(parents=True, exist_ok=True); "
                    f"p.write_bytes(bytes.fromhex('{encoded}')); "
                    "print(p.resolve())\""
                )
                message = {
                    "role": "assistant",
                    "content": None,
                    "tool_calls": [{
                        "id": "generate-artifact",
                        "type": "function",
                        "function": {
                            "name": "bash",
                            "arguments": json.dumps({"command": command}),
                        },
                    }],
                }
                finish_reason = "tool_calls"
            else:
                response_error = "forced post-artifact LLM failure"
        elif LARGE_REQUEST_MARKER in latest_user:
            content = LARGE_RESPONSE
        elif "E2E_CONTEXT_QUERY" in latest_user:
            content = CONTEXT_SECRET if CONTEXT_SECRET in serialized_messages else "NO_CONTEXT_SECRET"
        else:
            token_match = re.search(r"TURN_TOKEN_[A-Z0-9_]+", latest_user)
            content = token_match.group(0) if token_match else CONFIRMATION
        if message.get("content") == CONFIRMATION and 'content' in locals():
            message["content"] = content
        if "TURN_TOKEN_SLOW" in latest_user:
            time.sleep(0.5)

        if response_error:
            payload = json.dumps({"error": response_error}).encode()
        else:
            payload = json.dumps({
                "id": "chatcmpl-e2e",
                "object": "chat.completion",
                "created": 1,
                "model": request.get("model", "e2e-scripted"),
                "choices": [{
                    "index": 0,
                    "message": message,
                    "finish_reason": finish_reason,
                }],
                "usage": {
                    "prompt_tokens": 10,
                    "completion_tokens": 3,
                    "total_tokens": 13,
                },
            }).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, format: str, *args: Any) -> None:
        return


@contextmanager
def _scripted_llm() -> Iterator[tuple[str, list[dict[str, Any]]]]:
    port = _free_port()
    server = ThreadingHTTPServer(("127.0.0.1", port), _ScriptedLLMHandler)
    server.requests = []  # type: ignore[attr-defined]
    server.scenario_counts = {}  # type: ignore[attr-defined]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{port}/v1", server.requests  # type: ignore[attr-defined]
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)


async def _daemon_request(port: int, method: str, params: dict[str, Any]) -> dict[str, Any]:
    async with websockets.connect(
        f"ws://127.0.0.1:{port}", open_timeout=1, close_timeout=1
    ) as ws:
        await ws.send(json.dumps({
            "jsonrpc": "2.0",
            "method": method,
            "params": params,
            "id": 1,
        }))
        return json.loads(await asyncio.wait_for(ws.recv(), timeout=3))


def _wait_daemon(process: subprocess.Popen[bytes], port: int) -> None:
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(f"daemon exited during startup: {process.returncode}")
        try:
            response = asyncio.run(_daemon_request(port, "agent.list", {}))
            result = response.get("result", {})
            agents = result.get("agents", []) if isinstance(result, dict) else result
            if any(
                (a.get("agent_id") or a.get("id")) == "outfit-assistant"
                for a in agents
            ):
                return
        except Exception:
            pass
        time.sleep(0.1)
    raise AssertionError("outfit-assistant daemon did not become ready")


def _wait_bridge(process: subprocess.Popen[bytes], base_url: str) -> None:
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(f"bridge exited during startup: {process.returncode}")
        try:
            with urllib.request.urlopen(
                f"{base_url}/health/api/health", timeout=1
            ) as response:
                if response.status == 200:
                    return
        except Exception:
            pass
        time.sleep(0.1)
    raise AssertionError("bridge did not become ready")


def _stop_process(process: subprocess.Popen[bytes], log_file: Any) -> None:
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=12)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)
    log_file.close()


@pytest.fixture(scope="module")
def live_bridge(tmp_path_factory: pytest.TempPathFactory):
    if not DAEMON_BINARY.is_file():
        pytest.skip(f"daemon binary not built: {DAEMON_BINARY}")

    tmp_path = tmp_path_factory.mktemp("bridge-process-e2e")
    daemon_port = _free_port()
    bridge_port = _free_port()
    daemon_log_path = tmp_path / "daemon.log"
    bridge_log_path = tmp_path / "bridge.log"

    with _scripted_llm() as (llm_url, llm_requests):
        env = {
            **os.environ,
            "PATH": (
                f"{OUTFIT_AGENT / '.venv' / 'bin'}"
                f"{os.pathsep}{os.environ.get('PATH', '')}"
            ),
            "API_KEY": "e2e-test-key",
            "AF_LLM_BASE_URL": llm_url,
            "AF_LLM_MODEL": "e2e-scripted",
            "AF_SKILL_MAX_COMPLETION_TOKENS": "512",
            "DAEMON_HOST": "127.0.0.1",
            "DAEMON_PORT": str(daemon_port),
            "BRIDGE_LISTEN": f"127.0.0.1:{bridge_port}",
            "REQUEST_TIMEOUT_SECONDS": "5",
            "PYTHONPATH": str(PROJECT_ROOT / "frontend" / "bridge"),
            "NO_PROXY": "127.0.0.1,localhost",
            "no_proxy": "127.0.0.1,localhost",
        }
        daemon_log = daemon_log_path.open("wb")
        daemon = subprocess.Popen(
            [
                str(DAEMON_BINARY),
                "--listen",
                f"127.0.0.1:{daemon_port}",
                "--package",
                str(OUTFIT_AGENT),
            ],
            cwd=PROJECT_ROOT,
            env=env,
            stdout=daemon_log,
            stderr=subprocess.STDOUT,
        )
        bridge_log = bridge_log_path.open("wb")
        bridge: subprocess.Popen[bytes] | None = None
        try:
            _wait_daemon(daemon, daemon_port)
            bridge = subprocess.Popen(
                [
                    sys.executable,
                    "-m",
                    "uvicorn",
                    "main:app",
                    "--app-dir",
                    str(PROJECT_ROOT / "frontend" / "bridge"),
                    "--host",
                    "127.0.0.1",
                    "--port",
                    str(bridge_port),
                    "--log-level",
                    "warning",
                ],
                cwd=PROJECT_ROOT,
                env=env,
                stdout=bridge_log,
                stderr=subprocess.STDOUT,
            )
            root_url = f"http://127.0.0.1:{bridge_port}"
            _wait_bridge(bridge, root_url)
            yield {
                "http": f"{root_url}/health",
                "ws": f"ws://127.0.0.1:{bridge_port}",
                "llm_requests": llm_requests,
                "daemon_log": daemon_log_path,
                "bridge_log": bridge_log_path,
            }
        finally:
            if bridge is not None:
                _stop_process(bridge, bridge_log)
            else:
                bridge_log.close()
            _stop_process(daemon, daemon_log)


@pytest.mark.e2e
@pytest.mark.slow
def test_rest_main_chain_returns_result(live_bridge: dict[str, Any]) -> None:
    """TC-brg-001: REST 经真实 Bridge 和 daemon 返回最终结果。

    业务规则:
    - Bridge 接受有效任务后必须把 daemon 最终结果交付给调用方。

    错误模型:
    - notification 未消费、路由断裂或 daemon 完成但 REST 永久等待。

    失败意味着:
    - “发送消息不返回”的主链路回归仍未被拦截。
    """
    with httpx.Client(timeout=30) as client:
        response = client.post(
            f"{live_bridge['http']}/api/chat/outfit-assistant",
            json={
                "user_id": "e2e-rest-user",
                "client_id": "e2e-rest-client",
                "content": "请回复本次请求中的确认码。",
            },
        )

    assert response.status_code == 200, response.text
    body = response.json()
    assert body["success"] is True
    assert body["error"] is None
    assert body["data"]["task_id"]
    assert body["data"]["content"] == CONFIRMATION
    assert live_bridge["llm_requests"], "scripted LLM did not receive a request"


async def _ws_turn(
    ws_url: str,
    *,
    content: str = "请返回一个简短确认。",
    user_id: str = "e2e-ws-user",
    client_id: str = "e2e-ws-client",
) -> list[dict[str, Any]]:
    frames: list[dict[str, Any]] = []
    async with websockets.connect(f"{ws_url}/health/ws/chat/outfit-assistant") as ws:
        await ws.send(json.dumps({
            "user_id": user_id,
            "client_id": client_id,
            "content": content,
        }))
        while True:
            frame = json.loads(await asyncio.wait_for(ws.recv(), timeout=30))
            frames.append(frame)
            if frame.get("type") == "done":
                return frames


@pytest.mark.e2e
@pytest.mark.slow
def test_rest_and_ws_public_contracts(live_bridge: dict[str, Any]) -> None:
    """TC-brg-003: WS 经真实进程按 chunk -> done 契约完整收口。

    业务规则:
    - WS 最终文本与 REST 可消费语义一致，且不暴露内部 session_id。

    错误模型:
    - WS 终态丢失、帧顺序漂移或旧 session_id 契约回潮。

    失败意味着:
    - 前端无法判断本轮结束，或外部调用方依赖了错误 Schema。
    """
    frames = asyncio.run(_ws_turn(live_bridge["ws"]))
    chunks = [frame for frame in frames if frame.get("type") == "chunk"]

    assert chunks
    assert "".join(frame.get("content", "") for frame in chunks) == CONFIRMATION
    assert sum(frame.get("final") is True for frame in chunks) == 1
    assert frames[-1] == {"type": "done", "final": True}
    assert all("session_id" not in frame for frame in frames)


@pytest.mark.e2e
@pytest.mark.slow
def test_ws_large_response_is_complete(live_bridge: dict[str, Any]) -> None:
    """TC-brg-002: WS 大响应跨真实进程传输后字节完整且唯一收口。

    业务规则:
    - 大响应不能被截断、重复、错误拼接或误判为 malformed frame。

    错误模型:
    - daemon WebSocket 帧截断；Bridge 拼接不完整；终态重复或丢失。

    失败意味着:
    - 普通短消息正常时，大型 HTML/长文本响应仍可能无法交付。
    """
    frames = asyncio.run(_ws_turn(
        live_bridge["ws"],
        content=f"请完整返回长内容：{LARGE_REQUEST_MARKER}",
        user_id="e2e-large-user",
        client_id="e2e-large-client",
    ))
    chunks = [frame for frame in frames if frame.get("type") == "chunk"]
    actual = "".join(frame.get("content", "") for frame in chunks)

    assert actual == LARGE_RESPONSE
    assert len(actual.encode()) == len(LARGE_RESPONSE.encode())
    assert actual.startswith("LARGE_BEGIN|") and actual.endswith("|LARGE_END")
    assert sum(frame.get("final") is True for frame in chunks) == 1
    assert frames[-1] == {"type": "done", "final": True}


def _rest_turn(
    base_url: str,
    *,
    user_id: str,
    client_id: str,
    content: str,
) -> str:
    with httpx.Client(timeout=30) as client:
        response = client.post(
            f"{base_url}/api/chat/outfit-assistant",
            json={"user_id": user_id, "client_id": client_id, "content": content},
        )
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["success"] is True and body["error"] is None
    return str(body["data"]["content"])


@pytest.mark.e2e
@pytest.mark.slow
def test_same_identity_preserves_context(live_bridge: dict[str, Any]) -> None:
    """TC-brg-005: 相同 user_id + client_id 的多轮历史连续。

    业务规则:
    - 客户端无需传 session_id，同一复合身份仍应进入同一会话。

    错误模型:
    - 每轮错误创建新 session，或上下文更新未进入下一轮模型消息。

    失败意味着:
    - Agent 无法记住用户在上一轮提供的信息。
    """
    base_url = live_bridge["http"]
    _rest_turn(
        base_url,
        user_id="e2e-context-user",
        client_id="e2e-context-client",
        content=f"请记住我的代号：{CONTEXT_SECRET}",
    )
    answer = _rest_turn(
        base_url,
        user_id="e2e-context-user",
        client_id="e2e-context-client",
        content="我的代号是什么？E2E_CONTEXT_QUERY",
    )

    assert answer == CONTEXT_SECRET


@pytest.mark.e2e
@pytest.mark.slow
def test_different_identities_are_isolated(live_bridge: dict[str, Any]) -> None:
    """TC-brg-006: 不同 client_id 不能读取另一会话的历史。

    业务规则:
    - 会话历史必须按 user_id + client_id 隔离。

    错误模型:
    - 只按 agent 或 user 路由，导致不同客户端共享 message list。

    失败意味着:
    - 用户隐私和 Agent 响应会发生跨会话泄漏。
    """
    base_url = live_bridge["http"]
    _rest_turn(
        base_url,
        user_id="e2e-isolation-user",
        client_id="client-a",
        content=f"请记住我的代号：{CONTEXT_SECRET}",
    )
    answer_b = _rest_turn(
        base_url,
        user_id="e2e-isolation-user",
        client_id="client-b",
        content="我的代号是什么？E2E_CONTEXT_QUERY",
    )

    assert answer_b == "NO_CONTEXT_SECRET"
    assert CONTEXT_SECRET not in answer_b


@pytest.mark.e2e
@pytest.mark.slow
def test_repeated_turns_do_not_degrade_daemon(live_bridge: dict[str, Any]) -> None:
    """TC-brg-016: 连续多轮返回各自结果且 daemon 不发生累计退化。

    业务规则:
    - 连续执行不能导致消息丢失、跨轮串线或共享状态损坏。

    错误模型:
    - DataBus/共享内存资源累计泄漏；旧 notification 被下一轮消费。

    失败意味着:
    - 单次冒烟通过，但服务运行若干轮后仍会失效。
    """
    base_url = live_bridge["http"]
    for index in range(6):
        token = f"TURN_TOKEN_REPEAT_{index}"
        answer = _rest_turn(
            base_url,
            user_id="e2e-repeat-user",
            client_id="e2e-repeat-client",
            content=f"请只回复确认码 {token}",
        )
        assert answer == token, f"turn {index} received wrong content: {answer!r}"

    with httpx.Client(timeout=3) as client:
        health = client.get(f"{base_url}/api/health")
    assert health.status_code == 200


@pytest.mark.e2e
@pytest.mark.slow
def test_concurrent_turns_do_not_cross_route(live_bridge: dict[str, Any]) -> None:
    """TC-brg-017: 并发快慢请求不能跨客户端串线。

    业务规则:
    - 每个客户端只能收到自己的最终内容，慢请求不能污染快请求。

    错误模型:
    - 共享 notification 队列按 agent 而非 task 路由，导致并发响应互换。

    失败意味着:
    - 并发外部调用可能拿到另一个用户的结果。
    """
    async def run_pair() -> tuple[str, str]:
        async with httpx.AsyncClient(timeout=30) as client:
            async def one(client_id: str, token: str) -> str:
                response = await client.post(
                    f"{live_bridge['http']}/api/chat/outfit-assistant",
                    json={
                        "user_id": "e2e-concurrent-user",
                        "client_id": client_id,
                        "content": f"请只回复确认码 {token}",
                    },
                )
                assert response.status_code == 200, response.text
                return str(response.json()["data"]["content"])

            slow, fast = await asyncio.gather(
                one("slow-client", "TURN_TOKEN_SLOW_A"),
                one("fast-client", "TURN_TOKEN_FAST_B"),
            )
            return slow, fast

    slow, fast = asyncio.run(run_pair())
    assert slow == "TURN_TOKEN_SLOW_A"
    assert fast == "TURN_TOKEN_FAST_B"


@pytest.mark.e2e
@pytest.mark.slow
def test_truncated_tool_call_recovers_without_execution(
    live_bridge: dict[str, Any],
) -> None:
    """TC-brg-008: finish=length 的工具调用不执行并可恢复完成。

    业务规则:
    - 被截断的工具参数永远不能进入工具执行器，恢复后整轮仍有结果。

    错误模型:
    - 忽略 finish reason 执行残缺调用；解析失败直接中断 turn。

    失败意味着:
    - 模型截断可能产生错误文件副作用，或让用户永远收不到响应。
    """
    TRUNCATED_TOOL_TARGET.unlink(missing_ok=True)
    answer = _rest_turn(
        live_bridge["http"],
        user_id="e2e-tool-user",
        client_id="truncated-client",
        content=f"请执行工具恢复场景 {TRUNCATED_TOOL_MARKER}",
    )

    assert answer == "TRUNCATED_RECOVERY_FINAL_OK"
    assert not TRUNCATED_TOOL_TARGET.exists()


@pytest.mark.e2e
@pytest.mark.slow
def test_invalid_tool_json_exhaustion_returns_terminal_result(
    live_bridge: dict[str, Any],
) -> None:
    """TC-brg-009: 非法工具 JSON 重试耗尽后仍返回唯一终态。

    业务规则:
    - 非法参数不得执行；达到恢复上限后应把失败结果交还 coordinator。

    错误模型:
    - 将坏 JSON 强制转为空对象执行；无限重试；worker 异常退出无响应。

    失败意味着:
    - 工具解析故障会再次表现为“发送消息不返回”。
    """
    answer = _rest_turn(
        live_bridge["http"],
        user_id="e2e-tool-user",
        client_id="invalid-client",
        content=f"请执行非法参数场景 {INVALID_TOOL_MARKER}",
    )

    assert answer == "INVALID_RECOVERY_FINAL_OK"


@pytest.mark.e2e
@pytest.mark.slow
def test_artifact_survives_later_model_failure(live_bridge: dict[str, Any]) -> None:
    """TC-brg-010: 图片生成成功后模型失败仍通过聊天结果交付。

    业务规则:
    - 已生成 Artifact 是独立成果，后续 LLM 失败不能覆盖或丢弃它。

    错误模型:
    - 错误分支只返回文本错误，遗漏 skill 已累积的 artifacts_json。

    失败意味着:
    - 服务端确实有图片，但前端和外部调用方永远拿不到。
    """
    ARTIFACT_FILE.unlink(missing_ok=True)
    try:
        answer = _rest_turn(
            live_bridge["http"],
            user_id="e2e-artifact-user",
            client_id="artifact-failure-client",
            content=f"请生成图片后模拟整理失败 {ARTIFACT_FAILURE_MARKER}",
        )

        expected_path = f"/api/artifacts/outfit-assistant/files/{ARTIFACT_RELATIVE_PATH}"
        assert "ARTIFACT_FAILURE_FINAL_OK" in answer
        assert expected_path in answer
        assert str(ARTIFACT_FILE) not in answer

        with httpx.Client(timeout=5) as client:
            response = client.get(f"{live_bridge['http']}{expected_path}")
        assert response.status_code == 200
        assert response.content == ARTIFACT_BYTES
        assert response.headers["content-type"] == "image/png"
    finally:
        ARTIFACT_FILE.unlink(missing_ok=True)
        try:
            ARTIFACT_FILE.parent.rmdir()
        except OSError:
            pass


@pytest.mark.e2e
@pytest.mark.slow
def test_session_messages_preserves_assistant_replies(live_bridge: dict[str, Any]) -> None:
    """TC-brg-026: coordinator final_response path persists assistant messages.

    业务规则:
    - 每次 coordinator 决策（含直接回复用户）必须把 assistant 自身的回复写入
      session_messages，下轮 LLM 调用时能从历史中找回完整的 user+assistant 交替记录。

    错误模型:
    - coordinator 在 final_response 分支忘记 append assistant 消息到 messages 再落盘；
    - session_messages 只含连续 user 消息，LLM 看不懂对话上下文。

    失败意味着:
    - 多轮对话退化：LLM 每次都被当作"初次对话"。
    """
    base_url = live_bridge["http"]
    user_id = "e2e-session-msgs-user"
    client_id = "e2e-session-msgs-client"

    # Turn 1 — coordinator saves history (user + assistant)
    _rest_turn(
        base_url,
        user_id=user_id,
        client_id=client_id,
        content="你好，请确认收到。",
    )
    initial_request_count = len(live_bridge["llm_requests"])

    # Turn 2 — coordinator restores history, LLM should see Turn 1's assistant
    _rest_turn(
        base_url,
        user_id=user_id,
        client_id=client_id,
        content="继续对话。",
    )

    # Last LLM request should contain Turn 1's assistant message in history
    new_requests = live_bridge["llm_requests"][initial_request_count:]
    assert new_requests, "Turn 2 did not trigger any LLM call"
    turn2_messages = new_requests[-1].get("messages", [])
    roles = [m.get("role") for m in turn2_messages]

    assistant_count = roles.count("assistant")
    assert assistant_count >= 1, (
        f"Expected ≥1 assistant message in Turn 2 LLM request, "
        f"got roles={roles}. Coordinator may not be persisting its own replies "
        f"to session_messages in the final_response path."
    )
