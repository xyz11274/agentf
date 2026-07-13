#!/usr/bin/env python3
"""
AgentFlow E2E — real LLM + agentflow execution (direct daemon JSON-RPC)

验证完整的 agentflow 执行链路，不含 bridge 层：
  1. daemon 启动 + agent 注册
  2. agent.execute → workflow → LLM call → 返回内容
  3. session.list 返回已创建的会话
  4. 多轮对话保持上下文

LLM 配置优先读取环境变量，缺失时自动从 outfit-assistant/.env 加载。
"""

from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import time
import uuid
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[3]
DAEMON_BINARY = PROJECT_ROOT / "build" / "agentflowd"
OUTFIT_AGENT = str(PROJECT_ROOT / "examples" / "agent" / "outfit-assistant")
OUTFIT_ENV_FILE = PROJECT_ROOT / "examples" / "agent" / "outfit-assistant" / ".env"

PASS = 0
FAIL = 0
ALL_TESTS: list[tuple[str, bool]] = []


def ok(msg: str) -> None:
    global PASS; PASS += 1; ALL_TESTS.append((msg, True))
    print(f"  ✅ {msg}")


def fail(msg: str) -> None:
    global FAIL; FAIL += 1; ALL_TESTS.append((msg, False))
    print(f"  ❌ {msg}")


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0)); return s.getsockname()[1]


def wait_port(port: int, process: subprocess.Popen, timeout: float = 15) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            return False
        try:
            with socket.socket() as s:
                s.settimeout(0.5)
                s.connect(("127.0.0.1", port))
                return True
        except Exception:
            pass
        time.sleep(0.2)
    return False


def jsonrpc(host: str, port: int, method: str, params: dict,
            timeout: float = 10) -> dict:
    """Send JSON-RPC request, read newline-delimited one-line response."""
    with socket.socket() as s:
        s.settimeout(timeout)
        s.connect((host, port))
        req = json.dumps(
            {"jsonrpc": "2.0", "method": method, "params": params, "id": 1})
        s.sendall((req + "\n").encode())
        chunks = []
        while True:
            c = s.recv(1)
            if not c or c == b"\n":
                break
            chunks.append(c)
        return json.loads(b"".join(chunks).decode())


def execute_and_poll(port: int, agent_id: str, task_id: str, user_id: str,
                     client_id: str, content: str,
                     timeout: float = 120) -> dict | None:
    """Execute agent task via agent.execute and poll via agent.poll_response.

    与 e2e_helpers.poll_agent_response 同样的兜底策略：
    agent.poll_response 的消息可能被 ResponsePoller（50ms 周期）异步通知
    消费，消费后 poll 返回 messages=[]、complete=false。因此高频轮询 +
    每 2 秒 fallback 到 workflow.list_instances 检查 workflow 是否已结束。
    """
    r = jsonrpc("127.0.0.1", port, "agent.execute", {
        "agent_id": agent_id, "user_id": user_id, "client_id": client_id,
        "content": content, "request_id": task_id,
    })
    if "error" in r:
        print(f"    ❌ agent.execute error: {r['error']}", flush=True)
        return None
    print(f"    Execute OK, polling task {task_id}...", flush=True)

    deadline = time.monotonic() + timeout
    poll_count = 0
    while time.monotonic() < deadline:
        poll_count += 1
        resp = jsonrpc("127.0.0.1", port, "agent.poll_response", {
            "agent_id": agent_id, "user_id": user_id, "client_id": client_id,
            "timeout_ms": "500",
        }, timeout=5)
        if "error" in resp:
            time.sleep(0.2)
            continue
        result = resp.get("result", {})
        messages = result.get("messages", [])
        contents = [m.get("content", "") for m in messages if m.get("content")]
        if contents:
            return {"Data": {"UserOutputText": contents[-1]}}

        # 每 2 秒尝试一次 workflow.list_instances 兜底
        # （ResponsePoller 可能已消费消息，poll 返回空但 workflow 已完成）
        if poll_count % 20 == 0:
            wf = jsonrpc("127.0.0.1", port, "workflow.list_instances", {},
                        timeout=5)
            wf_result = wf.get("result", {})
            instances = wf_result.get("instances", []) if isinstance(wf_result, dict) else []
            running = any(
                inst.get("status") == "running" for inst in instances
                if isinstance(inst, dict)
            )
            if not running:
                # 无运行中 workflow → 任务已完成（消息被 ResponsePoller 消费）
                return {"Data": {"UserOutputText": ""}}

        time.sleep(0.1)
    return None


def main() -> int:
    print("=" * 60)
    print("AgentFlow E2E — real LLM + agentflow execution")
    print("=" * 60)

    # ── Load LLM config: env vars first, fallback to .env ──
    api_key = os.environ.get("AF_LLM_API_KEY", "")
    base_url = os.environ.get("AF_LLM_BASE_URL", "")
    llm_model = os.environ.get("AF_LLM_MODEL", "")

    if not api_key or not base_url:
        if OUTFIT_ENV_FILE.is_file():
            try:
                from dotenv import load_dotenv
                loaded = load_dotenv(OUTFIT_ENV_FILE, override=False)
                if loaded:
                    api_key = api_key or os.environ.get("AF_LLM_API_KEY", "")
                    base_url = base_url or os.environ.get("AF_LLM_BASE_URL", "")
                    llm_model = llm_model or os.environ.get("AF_LLM_MODEL", "")
                    print(f"   📄 Loaded LLM config from {OUTFIT_ENV_FILE.name}", flush=True)
            except ImportError:
                pass

    if not api_key or not base_url:
        print(f"\n❌ LLM not configured: set AF_LLM_API_KEY + AF_LLM_BASE_URL")
        print(f"   (expected in {OUTFIT_ENV_FILE} or environment)\n", flush=True)
        return 1

    if not DAEMON_BINARY.is_file():
        print(f"❌ Daemon not built: {DAEMON_BINARY}")
        return 1

    print(f"\n✅ Daemon: {DAEMON_BINARY}")
    print(f"✅ LLM: {base_url}  model={llm_model or '(default)'}")
    print()

    # ── Start daemon ──
    port = free_port()
    print(f"🚀 Daemon port {port}...", end=" ", flush=True)

    venv_bin = str(PROJECT_ROOT / 'examples' / 'agent' / 'outfit-assistant' / '.venv' / 'bin')
    daemon_env = {
        "PATH": f"{venv_bin}{os.pathsep}/usr/bin:/bin:/usr/sbin:/sbin",
        "AF_LLM_API_KEY": api_key,
        "AF_LLM_BASE_URL": base_url,
        "AF_LLM_MODEL": llm_model,
        "AF_SKILL_MAX_COMPLETION_TOKENS": "512",
        "NO_PROXY": "127.0.0.1,localhost",
        "no_proxy": "127.0.0.1,localhost",
    }

    daemon = subprocess.Popen(
        [str(DAEMON_BINARY), "--listen", f"127.0.0.1:{port}", "--package", OUTFIT_AGENT],
        cwd=PROJECT_ROOT,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=daemon_env,
    )

    _exit_code = 0
    try:
        if not wait_port(port, daemon):
            print(f"❌ Daemon failed to start on port {port}", flush=True)
            _exit_code = 1
        else:
            time.sleep(1)
            r = jsonrpc("127.0.0.1", port, "agent.list", {})
            agents_data = r.get("result", {}).get("agents", [])
            agents = [a.get("agent_id", "") for a in agents_data]
            if "outfit-assistant" not in agents:
                print(f"❌ Agent not registered. Got: {agents}", flush=True)
                _exit_code = 1
            else:
                print(f"✅ PID={daemon.pid}, agents={agents}")

        if _exit_code == 0:
            # ══════════════════════════════════════════════
            # Test 1: Single-turn
            # ══════════════════════════════════════════════
            print(f"\n─── Test 1: Single-turn agent.execute ───")
            tid1 = f"e2e-{uuid.uuid4().hex[:8]}"
            result = execute_and_poll(port, "outfit-assistant", tid1,
                                      "e2e-u1", "e2e-c1",
                                      "推荐一套适合开会的穿搭", timeout=120)
            if result:
                text = result.get("Data", {}).get("UserOutputText", "")
                if len(text) > 20:
                    ok(f"Single turn: {len(text)} chars — \"{text[:60]}...\"")
                elif text:
                    ok(f"Single turn: {len(text)} chars")
                else:
                    fail(f"Single turn: empty UserOutputText")
            else:
                fail("Single turn: no response (timeout)")

            # ══════════════════════════════════════════════
            # Test 2: Session persistence
            # ══════════════════════════════════════════════
            print(f"\n─── Test 2: Session persistence ───")
            r = jsonrpc("127.0.0.1", port, "session.list",
                        {"agent_id": "outfit-assistant", "user_id": "e2e-u1"})
            sessions = r.get("result", {}).get("sessions", [])
            matching = [s for s in sessions if s.get("user_id") == "e2e-u1"]
            if matching:
                ok(f"Session: {matching[0].get('status','?')} user_id={matching[0].get('user_id','?')}")
            else:
                fail(f"No session for e2e-u1 — {sessions}")

            # ══════════════════════════════════════════════
            # Test 3: Multi-turn (3 turns)
            # ══════════════════════════════════════════════
            print(f"\n─── Test 3: Multi-turn (3 turns) ───")
            uid3 = f"e2e-mt-{uuid.uuid4().hex[:6]}"
            cid3 = f"cli-{uuid.uuid4().hex[:6]}"
            turns_ok = 0
            for i, msg in enumerate([
                "我要去上海出差一周",
                "我更喜欢商务休闲风格",
                "能推荐几套具体的搭配吗？",
            ]):
                tid = f"e2e-{uuid.uuid4().hex[:8]}"
                result = execute_and_poll(port, "outfit-assistant", tid,
                                          uid3, cid3, msg, timeout=120)
                if result:
                    text = result.get("Data", {}).get("UserOutputText", "")
                    turns_ok += 1 if len(text) > 0 else 0
                    print(f"    Turn {i+1}: {len(text)} chars", flush=True)
                else:
                    print(f"    Turn {i+1}: no response", flush=True)
            if turns_ok == 3:
                ok("Multi-turn: 3/3 turns completed with real LLM calls")
            else:
                fail(f"Multi-turn: only {turns_ok}/3")

    finally:
        print(f"\n─── Cleanup ───")
        daemon.terminate()
        try:
            daemon.wait(timeout=10)
        except Exception:
            daemon.kill()
            daemon.wait(timeout=3)
        print(f"   ✅ Stopped")

    print(f"\n{'='*60}")
    print(f"Result: {PASS} pass / {FAIL} fail / {PASS+FAIL} total")
    print(f"{'='*60}")
    return _exit_code if _exit_code else (0 if FAIL == 0 else 1)


if __name__ == "__main__":
    sys.exit(main())
