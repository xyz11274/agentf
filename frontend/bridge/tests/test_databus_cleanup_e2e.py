"""DataBus Bus-B 对话级清理黑盒 E2E 测试

启动 agentflowd → 通过 WebSocket 发送 chat 请求 → 等待对话完成 →
验证 daemon 日志中包含 Bus-B 清理证据。

关联修复：docs/design/DataBus总线B修复方案.md §3.3.1
  store_with_ref 新增反向映射 "data_ref_logical:{hash}" → logical_key，
  cleanup_dialogue 通过映射连带删除 logical key 份，根治漏删。

依赖：
  - build/agentflowd（已编译）
  - redis-server（可选，有则做完整 cleanup 验证，无则跳过）
"""

from __future__ import annotations

import asyncio
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import time
from pathlib import Path
from typing import Any

import pytest
import websockets


PROJECT_ROOT = Path(__file__).resolve().parents[3]
DAEMON_BINARY = PROJECT_ROOT / "build" / "agentflowd"
TEST_AGENT = PROJECT_ROOT / "examples" / "agent" / "outfit-test-agent"
HAS_REDIS = shutil.which("redis-server") is not None


def _free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


async def _request(
    port: int, method: str, params: dict[str, Any]
) -> dict[str, Any]:
    async with websockets.connect(
        f"ws://127.0.0.1:{port}", open_timeout=3, close_timeout=1
    ) as ws:
        await ws.send(
            json.dumps({"jsonrpc": "2.0", "method": method, "params": params, "id": 1})
        )
        return json.loads(await asyncio.wait_for(ws.recv(), timeout=5))


def _start_daemon(
    port: int, log_path: Path, extra_env: dict[str, str] | None = None
) -> tuple[subprocess.Popen[bytes], Any]:
    log_file = log_path.open("wb")
    cmd = [
        str(DAEMON_BINARY),
        "--listen",
        f"127.0.0.1:{port}",
        "--package",
        str(TEST_AGENT),
        "--log-level",
        "debug",
    ]
    env = {**os.environ, "PYTHONUNBUFFERED": "1"}
    if extra_env:
        env.update(extra_env)
    process = subprocess.Popen(
        cmd,
        env=env,
        stdout=log_file,
        stderr=subprocess.STDOUT,
    )
    return process, log_file


def _wait_ready(process: subprocess.Popen[bytes], port: int, timeout: int = 12) -> None:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(
                f"daemon exited during startup with code {process.returncode}"
            )
        try:
            response = asyncio.run(_request(port, "agent.list", {}))
            if "result" in response:
                return
        except Exception as exc:
            last_error = exc
        time.sleep(0.2)
    raise AssertionError(f"daemon did not become ready: {last_error}")


def _stop_daemon(process: subprocess.Popen[bytes], log_file: Any) -> int:
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    if log_file and not log_file.closed:
        log_file.close()
    return process.returncode


def _read_log(log_path: Path) -> str:
    return log_path.read_text(encoding="utf-8", errors="replace")


# ============================================================================
# 测试用例
# ============================================================================


@pytest.fixture(scope="module")
def daemon_port() -> int:
    return _free_port()


@pytest.fixture(scope="module")
def log_path(tmp_path_factory: pytest.TempPathFactory) -> Path:
    return tmp_path_factory.mktemp("daemon_logs") / "daemon.log"


@pytest.mark.skipif(
    not DAEMON_BINARY.exists(),
    reason="agentflowd binary not found (build required)",
)
def test_daemon_starts_and_agent_loaded(daemon_port: int, log_path: Path) -> None:
    """基础启动验证：daemon 启动后 agent.list 返回非空列表。"""
    process, log_file = _start_daemon(daemon_port, log_path)
    try:
        _wait_ready(process, daemon_port)
        resp = asyncio.run(_request(daemon_port, "agent.list", {}))
        assert "result" in resp, f"agent.list failed: {resp}"
        result = resp["result"]
        # agent.list returns {"agents": [...]} — extract the list
        agents = result.get("agents", []) if isinstance(result, dict) else result
        assert isinstance(agents, list), f"expected list, got {type(agents)}: {agents}"
        assert len(agents) > 0, f"no agents loaded: {agents}"
        agent_ids = [a.get("agent_id", a.get("name", str(a))) for a in agents]
        print(f"Loaded agents: {agent_ids}")
    finally:
        rc = _stop_daemon(process, log_file)
        log = _read_log(log_path)
        # 检查 did not crash
        assert "SIGSEGV" not in log
        assert "panic" not in log
        assert "FATAL" not in log


@pytest.mark.skipif(
    not DAEMON_BINARY.exists(),
    reason="agentflowd binary not found (build required)",
)
def test_chat_request_completes_with_bus_b_cleanup_log(
    daemon_port: int, log_path: Path
) -> None:
    """发送 chat 请求 → 等待完成 → 验证日志含 Bus-B 清理条目。

    这条验证的核心不变量：
      I-1）对话完成后 cleanup_dialogue_data 被触发 → 打印 CLEANUP_DIALOGUE 日志
      I-2）清理路径追加了 Bus-B 清理 → 打印 "Cleaned N DataRef entries (Bus-B)" 日志

    注意：无 Redis 时 cleanup_dialogue 返回 -1 并打印 warning，
          有 Redis 时才会打印完整的 found=N, cleared=N。
    """
    extra_env = {}
    if HAS_REDIS:
        extra_env["AF_DATAREF_REDIS_URL"] = "redis://127.0.0.1:6379"

    process, log_file = _start_daemon(daemon_port, log_path, extra_env)
    try:
        _wait_ready(process, daemon_port, timeout=15)

        # 发送 chat 请求
        agent_id = "outfit-assistant"
        payload = {
            "agent_id": agent_id,
            "user_id": "e2e-test-user",
            "client_id": "e2e-test-client",
            "content": "测试消息",
            "stream": False,
            "message_id": f"msg_{int(time.time())}",
        }
        resp = asyncio.run(
            _request(daemon_port, "agent.execute", payload)
        )
        # 不管 LLM 是否可用，请求应被接受（code != -2 表示 agent 找到了）
        assert isinstance(resp, dict)
        err_code = resp.get("error", {}).get("code") if "error" in resp else None
        if err_code is not None and err_code == -2:
            pytest.fail(f"Agent not found: {resp}")
        if err_code is not None and err_code == -32603:
            # 内部执行错误（如 LLM 无 key）也可以接受——cleanup 在错误路径同样触发
            pass

        # 等待 daemon 处理 + 日志刷盘
        # LLM 无 key 时会因 connect_timeout=5s 快速失败，触发 cleanup
        timeout_at = time.monotonic() + 25
        last_log_len = 0
        while time.monotonic() < timeout_at:
            time.sleep(2.0)
            log = _read_log(log_path)
            if len(log) > last_log_len:
                last_log_len = len(log)
            if "CLEANUP_DIALOGUE" in log:
                break
        else:
            # 超时——读最新日志用于调试
            log = _read_log(log_path)
        print(f"\n--- daemon.log (port={daemon_port}) ---")
        print(log[-3000:])
        print("--- end daemon.log ---")

        # === I-1：CLEANUP_DIALOGUE 必须被触发 ===
        cleanup_entries = re.findall(
            r"\[CLEANUP_DIALOGUE\] Cleaning up dialogue_id=([^\s]+)", log
        )
        assert len(cleanup_entries) > 0, (
            f"不变量 I-1 违反：未发现 CLEANUP_DIALOGUE 日志。"
            f"对话完成后 cleanup_dialogue_data 未被触发。"
            f"\n最近日志:\n{log[-1500:]}"
        )
        dialogue_id = cleanup_entries[0]

        # === I-2：Bus-B 清理必须被触发 ===
        bus_b_entries = re.findall(
            r"\[CLEANUP_DIALOGUE\] Cleaned (\d+) DataRef entries \(Bus-B\) for "
            f"dialogue_id={re.escape(dialogue_id)}",
            log,
        )
        assert len(bus_b_entries) > 0, (
            f"不变量 I-2 违反：未发现 Bus-B 清理日志。"
            f"cleanup_dialogue_data 被触发了但未调用 dataref_bridge::cleanup_dialogue。"
            f"可能原因：dataref_bridge.h 未在 builtin_dispatcher.cpp 中 include。"
            f"\nCLEANUP_DIALOGUE 日志:\n{[l for l in log.split(chr(10)) if 'CLEANUP_DIALOGUE' in l]}"
            f"\n最近日志:\n{log[-1500:]}"
        )

        if HAS_REDIS:
            # 有 Redis 时：verify cleanup 实际找到了 DataRef 并清理
            for match in bus_b_entries:
                cleaned_count = int(match)
                # 至少清理了 1 条 DataRef（消息体的 store_payload 会创建 DataRef）
                assert cleaned_count > 0, (
                    f"不变量 I-1 违反（有 Redis）：cleanup 找到了 0 条 DataRef，"
                    f"但消息处理过程中应当 store_payload 创建了至少 1 条。"
                    f"dialogue_id={dialogue_id}"
                )
    finally:
        _stop_daemon(process, log_file)
