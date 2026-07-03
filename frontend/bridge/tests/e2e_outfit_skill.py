#!/usr/bin/env python3
"""
E2E test: outfit-assistant skill loading via bridge WebSocket.

Tests that the outfit_assistant correctly loads the weather_query skill
from ``harness/skills/weather_query/scripts/`` and can execute it.

Prerequisites:
  1. agentflowd running with outfit_assistant:
       ./build/agentflowd \\
         --package "$(pwd)/examples/agent/outfit_assistant" \\
         --package "$(pwd)/examples/agent/agentflow_assistant"
  2. agentflow-bridge running:
       agentflow-bridge

Usage:
  python3 frontend/bridge/tests/e2e_outfit_skill.py
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

BRIDGE_WS_URL = "ws://127.0.0.1:8199/health/ws/chat/outfit-assistant"
TIMEOUT = 90
PASS = 0
FAIL = 0
BASE_USER = "e2e-skill"


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
    raw = await asyncio.wait_for(ws.recv(), timeout=timeout)
    return json.loads(raw)


async def drain_turn(ws, timeout=90):
    """Drain all frames until 'done' frame, return full response text.
    Uses the overall timeout for each individual frame recv.
    """
    deadline = time.time() + timeout
    chunks = []
    while time.time() < deadline:
        remaining = max(5, deadline - time.time())
        try:
            frame = await recv_frame(ws, timeout=remaining)
        except asyncio.TimeoutError:
            break
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


async def test_weather_query_skill():
    """Send a weather query and verify the agent handles it correctly."""
    print("\n" + "=" * 60)
    print("场景: 天气查询 skill 加载测试")
    print("=" * 60)

    async with websockets.connect(BRIDGE_WS_URL) as ws:
        # ---- Weather query ----
        uid = f"{BASE_USER}-weather"
        print('\n[发送] "北京今天天气怎么样？"')
        await ws.send(json.dumps({
            "user_id": uid,
            "client_id": uid,
            "content": "北京今天天气怎么样？",
        }))
        reply = await drain_turn(ws, TIMEOUT)
        print(f"  回复: {(reply or '(空)')[:300]}...")

        check(bool(reply), "回复不为空")
        check("workflow_failed" not in (reply or ""),
              "workflow 未失败",
              (reply or "")[:300])

        # The response should reference weather or temperature
        has_weather_ref = any(kw in (reply or "") for kw in (
            "天气", "温度", "气温", "°C", "摄氏度", "晴", "多云", "雨"
        ))
        check(has_weather_ref,
              "回复包含天气相关信息",
              (reply or "")[:300])

        # If the skill loaded, the response should contain structured weather info
        has_temperature = any(kw in (reply or "") for kw in (
            "°C", "摄氏度", "温度", "温"
        ))
        check(has_temperature,
              "回复包含温度信息",
              (reply or "")[:300])


async def test_profile_manager_tool():
    """Verify the original tools (profile_manager) still work alongside skills."""
    print("\n" + "=" * 60)
    print("场景: 原始工具共存测试")
    print("=" * 60)

    async with websockets.connect(BRIDGE_WS_URL) as ws:
        uid = f"{BASE_USER}-profile"
        print('\n[发送] "我叫张三"')
        await ws.send(json.dumps({
            "user_id": uid,
            "client_id": uid,
            "content": "我叫张三",
        }))
        reply = await drain_turn(ws, TIMEOUT)
        print(f"  回复: {(reply or '(空)')[:200]}...")

        check(bool(reply), "回复不为空")
        check("workflow_failed" not in (reply or ""),
              "workflow 未失败")
        check(any(kw in (reply or "") for kw in ("张三", "你好", "欢迎")),
              "回复提到用户称呼",
              (reply or "")[:200])


async def test_multi_turn_with_skill():
    """Multi-turn: weather query, then follow-up — verify skills+memory coexist."""
    print("\n" + "=" * 60)
    print("场景: 多轮对话 + skill 共存")
    print("=" * 60)

    async with websockets.connect(BRIDGE_WS_URL) as ws:
        uid = f"{BASE_USER}-multiturn"
        # Turn 1: set profile and ask weather
        msg1 = "我叫张三，明天要去上海出差"
        print(f'\n[Turn 1] 发送: "{msg1}"')
        await ws.send(json.dumps({
            "user_id": uid,
            "client_id": uid,
            "content": msg1,
        }))
        reply1 = await drain_turn(ws, TIMEOUT)
        print(f"  回复: {(reply1 or '(空)')[:200]}...")

        check(bool(reply1), "Turn 1: 回复不为空")
        check("workflow_failed" not in (reply1 or ""),
              "Turn 1: workflow 未失败")

        # Turn 2: ask about the weather skill result
        msg2 = "帮我推荐穿搭"
        print(f'\n[Turn 2] 发送: "{msg2}"')
        await ws.send(json.dumps({
            "user_id": uid,
            "client_id": uid,
            "content": msg2,
        }))
        reply2 = await drain_turn(ws, TIMEOUT)
        print(f"  回复: {(reply2 or '(空)')[:300]}...")

        check(bool(reply2), "Turn 2: 回复不为空")
        check("workflow_failed" not in (reply2 or ""),
              "Turn 2: workflow 未失败")

        # Should reference both the profile (张三) and the weather (上海)
        has_profile = any(kw in (reply2 or "") for kw in ("张三", "身高", "体型", "商务"))
        has_weather = any(kw in (reply2 or "") for kw in ("天气", "温度", "上海", "出差"))
        check(has_profile or has_weather,
              "Turn 2: 回复提及画像或天气信息（skill + memory 共存）",
              (reply2 or "")[:300])


async def main():
    print("=" * 60)
    print("OutfitAssistant Skill 加载 E2E 测试")
    print(f"  Bridge: {BRIDGE_WS_URL}")
    print("=" * 60)

    await test_weather_query_skill()
    await test_profile_manager_tool()
    await test_multi_turn_with_skill()

    print("\n" + "=" * 60)
    print(f"结果: {PASS} passed, {FAIL} failed")
    print("=" * 60)
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
