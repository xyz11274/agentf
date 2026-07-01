#!/usr/bin/env python3
"""
E2E test: bridge WebSocket chat → daemon multi-turn conversation.

Prerequisites:
  1. agentflowd running with harness_agent:
       ./build/agentflowd --package "$(pwd)/examples/agent/harness_agent"
  2. agentflow-bridge running:
       agentflow-bridge

Usage:
  python3 frontend/bridge/tests/e2e_test.py
"""

import asyncio
import json
import sys
import time

try:
    import websockets
except ImportError:
    print("Install websockets: pip install websockets")
    sys.exit(1)

BRIDGE_WS_URL = "ws://127.0.0.1:8199/ws/chat/harness-agent"
TIMEOUT = 30


async def recv_frame(ws, timeout=10):
    """Receive one JSON frame from WebSocket with timeout."""
    raw = await asyncio.wait_for(ws.recv(), timeout=timeout)
    return json.loads(raw)


async def drain_turn(ws, timeout=30):
    """Drain all frames until 'done' frame, return (chunks, session_id)."""
    deadline = time.time() + timeout
    chunks = []
    session_id = None
    while time.time() < deadline:
        frame = await recv_frame(ws, timeout=30)
        t = frame.get("type")
        if t == "chunk":
            chunks.append(frame.get("content", ""))
            if frame.get("final"):
                continue  # still expect a 'done' frame
        elif t == "done":
            session_id = frame.get("session_id")
            break
        elif t == "error":
            print(f"  [ERROR] {frame.get('message')}")
            break
    return "".join(chunks), session_id


async def main():
    print("=" * 60)
    print("E2E: bridge WebSocket → daemon multi-turn chat")
    print("=" * 60)

    async with websockets.connect(BRIDGE_WS_URL) as ws:
        # ---- Turn 1 ----
        print("\n[Turn 1] sending: 我叫张三")
        await ws.send(json.dumps({"content": "我叫张三", "session_id": ""}))
        reply1, sid1 = await drain_turn(ws, TIMEOUT)
        print(f"  response: {reply1[:120]}...")
        print(f"  session_id: {sid1}")
        assert reply1, "Turn 1: empty response"
        assert sid1, "Turn 1: no session_id"
        assert "workflow_failed" not in reply1, \
            f"Turn 1: workflow failed, response={reply1[:200]}"
        print("  ✅ Pass")

        # ---- Turn 2: use session_id from turn 1 ----
        print(f"\n[Turn 2] sending: 我叫什么？ (session={sid1})")
        await ws.send(json.dumps({"content": "我叫什么？", "session_id": sid1}))
        reply2, sid2 = await drain_turn(ws, TIMEOUT)
        print(f"  response: {reply2[:300]}...")
        print(f"  session_id: {sid2}")
        assert reply2, "Turn 2: empty response"
        assert sid2 == sid1, f"Turn 2: session_id mismatch: {sid2} != {sid1}"
        assert "workflow_failed" not in reply2, \
            f"Turn 2: workflow failed, response={reply2[:300]}"
        assert "张三" in reply2, \
            f"Turn 2: expected '张三' in response, got: {reply2[:300]}"
        print("  ✅ Pass")

    print("\n" + "=" * 60)
    print("All tests passed!")
    print("=" * 60)


if __name__ == "__main__":
    asyncio.run(main())
