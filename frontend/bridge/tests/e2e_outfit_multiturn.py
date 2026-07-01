#!/usr/bin/env python3
"""
E2E test: outfit-assistant multi-turn conversation via bridge WebSocket.

Tests that the agent remembers user identity across turns:
  Turn 1: "我叫张三"
  Turn 2: "我叫什么？" → expects "张三" in response

Prerequisites:
  1. agentflowd running with outfit_assistant:
```  
./build/agentflowd \
--package "$(pwd)/examples/agent/outfit_assistant" \
--package "$(pwd)/examples/agent/agentflow_assistant"
```                          
  2. agentflow-bridge running:
       agentflow-bridge

Usage:
  python3 frontend/bridge/tests/e2e_outfit_multiturn.py
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

BRIDGE_WS_URL = "ws://127.0.0.1:8199/ws/chat/outfit-assistant"
TIMEOUT = 60
PASS = 0
FAIL = 0
USER_ID = "e2e-test-user"
CLIENT_ID = "e2e-test-client"


def check(condition, msg, detail=""):
    global PASS, FAIL
    if condition:
        PASS += 1
        print(f"  ✅ {msg}")
    else:
        FAIL += 1
        print(f"  ❌ {msg}")
        if detail:
            for line in detail.split("\n"):
                print(f"     {line}")


async def recv_frame(ws, timeout=10):
    """Receive one JSON frame from WebSocket with timeout."""
    raw = await asyncio.wait_for(ws.recv(), timeout=timeout)
    return json.loads(raw)


async def drain_turn(ws, timeout=60):
    """Drain all frames until 'done' frame, return (chunks, session_id=None).

    Note: bridge ``done`` frame does NOT include session_id.
    Multi-turn is handled by staying on the same WebSocket connection
    with the same ``user_id`` + ``client_id``.
    """
    deadline = time.time() + timeout
    chunks = []
    while time.time() < deadline:
        frame = await recv_frame(ws, timeout=30)
        t = frame.get("type")
        if t == "chunk":
            chunks.append(frame.get("content", ""))
            if frame.get("final"):
                continue
        elif t == "done":
            break
        elif t == "error":
            print(f"  [ERROR] {frame.get('message')}")
            break
    return "".join(chunks)


async def test_multiturn_identity():
    """Multi-turn: introduce name → ask name back → expect correct answer."""
    print("\n" + "=" * 60)
    print("场景: 多轮身份记忆")
    print("=" * 60)

    async with websockets.connect(BRIDGE_WS_URL) as ws:
        # ---- Turn 1 ----
        print('\n[Turn 1] 发送: "我叫张三"')
        await ws.send(json.dumps({"user_id": USER_ID, "client_id": CLIENT_ID, "content": "我叫张三"}))
        reply1 = await drain_turn(ws, TIMEOUT)
        print(f"  回复: {(reply1 or '(空)')[:200]}...")

        check(bool(reply1), "Turn 1: 回复不为空")
        check("workflow_failed" not in (reply1 or ""),
              "Turn 1: workflow 未失败",
              (reply1 or "")[:300])
        check(any(kw in (reply1 or "") for kw in ("张三", "穿搭", "你好", "欢迎")),
              "Turn 1: 回复提到'张三'或相关问候",
              (reply1 or "")[:200])

        # ---- Turn 2: same connection, same user_id+client_id ----
        print('\n[Turn 2] 发送: "我叫什么？"')
        await ws.send(json.dumps({"user_id": USER_ID, "client_id": CLIENT_ID, "content": "我叫什么？"}))
        reply2 = await drain_turn(ws, TIMEOUT)
        print(f"  回复: {(reply2 or '(空)')[:300]}...")

        check(bool(reply2), "Turn 2: 回复不为空")
        check("workflow_failed" not in (reply2 or ""),
              "Turn 2: workflow 未失败",
              (reply2 or "")[:300])
        check("张三" in (reply2 or ""),
              "Turn 2: 回复中正确说出'张三'",
              (reply2 or "")[:300])


async def test_multiturn_preference():
    """Multi-turn: state preference → ask for recommendation → follows preference."""
    print("\n" + "=" * 60)
    print("场景: 多轮偏好记忆")
    print("=" * 60)

    async with websockets.connect(BRIDGE_WS_URL) as ws:
        # ---- Turn 1 ----
        msg1 = "我明天要去上海出差，喜欢商务休闲风格"
        print(f'\n[Turn 1] 发送: "{msg1}"')
        await ws.send(json.dumps({"user_id": USER_ID, "client_id": CLIENT_ID, "content": msg1}))
        reply1 = await drain_turn(ws, TIMEOUT)
        print(f"  回复: {(reply1 or '(空)')[:200]}...")

        check(bool(reply1), "Turn 1: 回复不为空")
        check("workflow_failed" not in (reply1 or ""),
              "Turn 1: workflow 未失败")

        # ---- Turn 2 ----
        print('\n[Turn 2] 发送: "帮我推荐穿搭"')
        await ws.send(json.dumps({"user_id": USER_ID, "client_id": CLIENT_ID, "content": "帮我推荐穿搭"}))
        reply2 = await drain_turn(ws, TIMEOUT)
        print(f"  回复: {(reply2 or '(空)')[:300]}...")

        check(bool(reply2), "Turn 2: 回复不为空")
        check("workflow_failed" not in (reply2 or ""),
              "Turn 2: workflow 未失败")
        check(any(kw in (reply2 or "") for kw in ("上海", "出差", "商务", "休闲")),
              "Turn 2: 回复提及上海或商务休闲（记忆了第一轮信息）",
              (reply2 or "")[:300])


async def main():
    print("=" * 60)
    print("OutfitAssistant 多轮对话 E2E 测试")
    print(f"  Bridge: {BRIDGE_WS_URL}")
    print("=" * 60)

    await test_multiturn_identity()
    await test_multiturn_preference()

    print("\n" + "=" * 60)
    print(f"结果: {PASS} passed, {FAIL} failed")
    print("=" * 60)
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
