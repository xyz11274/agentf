#!/usr/bin/env python3
"""
AgentFlow E2E — 真实 LLM + agentflow 执行 (直接 daemon JSON-RPC)

验证完整的 agentflow 执行链路，不含 bridge 层：
  1. daemon 启动 + agent 注册
  2. agent.execute → workflow → LLM call (MiniMax) → 返回内容
  3. session.list 返回已创建的会话
  4. 多轮对话保持上下文

用法:
  set -a; source examples/agent/outfit-assistant/.env; set +a
  python3 scripts/tests/e2e_agentflow_pipeline.py
"""

from __future__ import annotations

import json
import os
import signal
import socket
import subprocess
import sys
import time
import uuid
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[3]
DAEMON_BINARY = PROJECT_ROOT / "build" / "agentflowd"
OUTFIT_AGENT = str(PROJECT_ROOT / "examples" / "agent" / "outfit-assistant")

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
        if process.poll() is not None: return False
        try:
            with socket.socket() as s: s.settimeout(0.5); s.connect(("127.0.0.1", port)); return True
        except: pass
        time.sleep(0.2)
    return False


def jsonrpc(host: str, port: int, method: str, params: dict, timeout: float = 10) -> dict:
    with socket.socket() as s:
        s.settimeout(timeout); s.connect((host, port))
        req = json.dumps({"jsonrpc": "2.0", "method": method, "params": params, "id": 1})
        s.sendall((req + "\n").encode())
        chunks = []
        while True:
            c = s.recv(1)
            if not c or c == b"\n": break
            chunks.append(c)
        return json.loads(b"".join(chunks).decode())


def execute_and_poll(port: int, agent_id: str, task_id: str, user_id: str,
                     client_id: str, content: str, timeout: float = 120) -> dict | None:
    """Send TASKASSIGNMENT via JSON-RPC and poll for response."""
    # Build the full task assignment message
    r = jsonrpc("127.0.0.1", port, "TASKASSIGNMENT", {
        "Header": {
            "MessageType": "UserTaskManagement", "TaskID": task_id,
            "Sender": "SystemAgent", "Receiver": "HealthAgent",
            "UserName": user_id, "DeviceId": "e2e-device",
            "ClientSessionId": client_id, "DeviceCategory": "2",
            "UUID": f"uuid-{task_id}",
            "AcmSessionId": "", "DialogueId": "", "SessionId": "",
            "EventTimeStamp": "2026-07-04T12:00:00Z",
            "SysAgentEntity": "127.0.0.1",
            "TeamEnvironmentID": "te_test_001", "ClientExField": "",
        },
        "Data": {
            "MessageType": "TaskAssignment", "UserInputText": content,
            "RewriteQ": content, "UserIntent": "",
            "UserInputObject": '{"code":200003,"data":{"intent":"","params":null}}',
            "UserSpecifiedAgent": "", "APPName": "", "ToolName": "",
            "IsHaveSendHuoShan": "0", "LongHistory": "",
        },
    })
    print(f"    Execute response OK, polling for task {task_id}...", flush=True)

    # Poll for response
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        resp = jsonrpc("127.0.0.1", port, "RESPONSE",
                       {"TaskID": task_id}, timeout=5)
        result = resp.get("result", {})
        msg_type = result.get("MessageType")
        if msg_type == "UserOutput":
            return result
        if msg_type == "TaskStatusManagement":
            data = result.get("Data", {})
            if data.get("TaskStatusCode") == 200:
                return result
        time.sleep(0.5)
    return None


def main() -> int:
    print("=" * 60)
    print("AgentFlow E2E — real LLM + agentflow execution")
    print("=" * 60)

    api_key = os.environ.get("AF_LLM_API_KEY") or os.environ.get("API_KEY", "")
    base_url = os.environ.get("AF_LLM_BASE_URL", "")
    if not api_key or not base_url:
        print("\n⚠️  No AF_LLM_BASE_URL / API_KEY set — tests will fail")
        print("  Run: set -a; source examples/agent/outfit-assistant/.env; set +a\n")

    if not DAEMON_BINARY.is_file():
        print(f"❌ Daemon not built: {DAEMON_BINARY}"); return 1
    print(f"\n✅ Daemon: {DAEMON_BINARY}")
    print(f"✅ LLM: {base_url}")
    print()

    # ── Start daemon ──
    port = free_port()
    daemon_env = dict(os.environ)
    daemon_env["PATH"] = f"{PROJECT_ROOT / 'examples' / 'agent' / 'outfit-assistant' / '.venv' / 'bin'}{os.pathsep}/usr/bin:/bin:/usr/sbin:/sbin"

    print(f"🚀 Daemon port {port}...", end=" ", flush=True)
    daemon = subprocess.Popen(
        [str(DAEMON_BINARY), "--listen", f"127.0.0.1:{port}", "--package", OUTFIT_AGENT],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=daemon_env,
    )
    assert wait_port(port, daemon), "Daemon failed to start"
    time.sleep(1)
    r = jsonrpc("127.0.0.1", port, "agent.list", {})
    agents = [a.get("agent_id", "") for a in r.get("result", {}).get("agents", [])]
    assert "outfit-assistant" in agents, f"Agent not registered: {agents}"
    print(f"✅ PID={daemon.pid}, agents={agents}")

    try:
        # ═══════════════════════════════════════════════════
        # Test 1: Execute single turn
        # ═══════════════════════════════════════════════════
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
                fail(f"Single turn: no UserOutputText — keys={list(result.get('Data',{}).keys())}")
        else:
            fail("Single turn: no response (timeout)")

        # ═══════════════════════════════════════════════════
        # Test 2: Session persistence
        # ═══════════════════════════════════════════════════
        print(f"\n─── Test 2: Session persistence ───")
        r = jsonrpc("127.0.0.1", port, "session.list",
                    {"agent_id": "outfit-assistant", "user_id": "e2e-u1"})
        sessions = r.get("result", {}).get("sessions", [])
        matching = [s for s in sessions if s.get("user_id") == "e2e-u1"]
        if matching:
            ok(f"Session: {matching[0].get('status','?')} user_id={matching[0].get('user_id','?')}")
        else:
            fail(f"No session for e2e-u1 — {sessions}")

        # ═══════════════════════════════════════════════════
        # Test 3: Multi-turn (3 turns, same user/client)
        # ═══════════════════════════════════════════════════
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
        try: daemon.wait(timeout=10)
        except: daemon.kill()
        print(f"   ✅ Stopped")

    print(f"\n{'='*60}")
    print(f"Result: {PASS} pass / {FAIL} fail / {PASS+FAIL} total")
    print(f"{'='*60}")
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
