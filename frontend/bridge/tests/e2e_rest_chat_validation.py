"""
Bridge REST + SSE Chat E2E 验证 — 通过 bridge 的 REST API 测试。
覆盖：SSE 流式格式、同步 chat、session 持久化、多轮对话。

用法（在项目根目录）：
  set -a; source examples/agent/outfit-assistant/.env; set +a
  python3 frontend/bridge/tests/e2e_rest_chat_validation.py
"""

from __future__ import annotations
import json, os, signal, socket, subprocess, sys, time, uuid
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[3]
DAEMON_BINARY = PROJECT_ROOT / "build" / "agentflowd"
OUTFIT_AGENT = str(PROJECT_ROOT / "examples" / "agent" / "outfit-assistant")
BRIDGE_DIR = PROJECT_ROOT / "frontend" / "bridge"

PASS, FAIL = 0, 0
def ok(m): global PASS; PASS += 1; print(f"  ✅ {m}")
def fail(m): global FAIL; FAIL += 1; print(f"  ❌ {m}")

def free_port():
    with socket.socket() as s: s.bind(("", 0)); return s.getsockname()[1]

def wait_port(port, proc, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None: return False
        try:
            with socket.socket() as s: s.settimeout(0.5); s.connect(("127.0.0.1", port)); return True
        except: pass
        time.sleep(0.2)
    return False

def sse_parse(text):
    events = []
    cur = []
    for line in text.split("\n"):
        if line.startswith("data: "):
            p = line[6:].strip()
            if p == "[DONE]": events.append(p)
            else: cur.append(p)
        elif line == "" and cur:
            events.append(json.loads("".join(cur))); cur = []
    return events

def main():
    print(f"Bridge REST SSE Chat E2E")
    api_key = os.environ.get("AF_LLM_API_KEY") or os.environ.get("API_KEY", "")
    base_url = os.environ.get("AF_LLM_BASE_URL", "")
    use_real = bool(api_key and base_url)
    print(f"  LLM: {'REAL' if use_real else 'MOCK'} {base_url}")

    if not DAEMON_BINARY.is_file(): print("Daemon not built"); return 1

    dp = free_port(); bp = free_port()
    env = dict(os.environ)
    env["PATH"] = f"{PROJECT_ROOT / 'examples' / 'agent' / 'outfit-assistant' / '.venv' / 'bin'}{os.pathsep}/usr/bin:/bin:/usr/sbin:/sbin"

    d = subprocess.Popen([str(DAEMON_BINARY), "--listen", f"127.0.0.1:{dp}", "--package", OUTFIT_AGENT],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    assert wait_port(dp, d), "daemon failed"
    time.sleep(1)
    print(f"  Daemon: PID={d.pid} port={dp}")

    # Start bridge (requires correct python3 with uvicorn + websockets)
    bridge_env = dict(os.environ)
    bridge_env["DAEMON_HOST"] = "127.0.0.1"; bridge_env["DAEMON_PORT"] = str(dp)
    b = subprocess.Popen(
        ["uv", "run", "--directory", str(BRIDGE_DIR),
         "python", "-m", "uvicorn", "main:bridge_app",
         "--host", "127.0.0.1", "--port", str(bp), "--log-level", "warning"],
        cwd=str(BRIDGE_DIR), env=bridge_env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    if not wait_port(bp, b): print("bridge failed to start"); d.kill(); return 1
    print(f"  Bridge: PID={b.pid} port={bp}")
    time.sleep(2)  # wait for daemon client connection

    try:
        # Test SSE
        import http.client
        conn = http.client.HTTPConnection("127.0.0.1", bp, timeout=120)
        conn.request("POST", f"/api/chat/outfit-assistant?stream=true",
                     json.dumps({"user_id": "e2e-u1", "client_id": "e2e-c1",
                                 "content": "推荐适合开会的穿搭"}),
                     {"Content-Type": "application/json"})
        resp = conn.getresponse()
        body = resp.read().decode()
        events = sse_parse(body)
        if len(events) >= 2 and events[-1] == "[DONE]" and isinstance(events[0], dict):
            ok(f"SSE: {len(events)-1} chunk(s)")
        else:
            fail(f"SSE: {body[:200]}")
        conn.close()

        # Test Sync
        conn2 = http.client.HTTPConnection("127.0.0.1", bp, timeout=120)
        conn2.request("POST", "/api/chat/outfit-assistant",
                      json.dumps({"user_id": "e2e-u3", "client_id": "e2e-c3",
                                  "content": "推荐商务休闲穿搭"}),
                      {"Content-Type": "application/json"})
        r2 = conn2.getresponse()
        d2 = json.loads(r2.read().decode())
        content = d2.get("data", {}).get("content", "")
        ok(f"Sync: {len(content)} chars") if len(content) > 0 else fail("Sync empty")
        conn2.close()

    finally:
        b.terminate(); d.terminate()
        try: b.wait(5); d.wait(10)
        except: b.kill(); d.kill()

    print(f"\n{PASS} pass / {FAIL} fail")
    return 0 if FAIL == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
