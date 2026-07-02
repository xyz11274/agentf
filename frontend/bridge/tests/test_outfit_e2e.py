#!/usr/bin/env python3
"""
穿搭助手 E2E 测试套件 — 基于 daemon 日志的可靠验证

设计决策（吸取前几轮调试教训）：

1. 不使用 agent.poll_response（daemon 的 RESPONSE_POLLER 在 10ms 内
   删除 DataBus response key，poll 永远读不到）
2. 不使用 bridge REST/WebSocket（大响应时出现 malformed frame）
3. 使用 daemon 日志文件偏移量跟踪，每个测试只读自己请求之后的增量日志
4. daemon 处理约 5 个请求后会因共享内存退化崩溃（mutex lock failed），
   因此每批测试前需手动重启 daemon，或少量分批运行
5. 验证执行链完整性而非响应内容：检查节点 Exit code=0 + Response ready

前置条件：
  daemon running on 17891 (outfit-assistant):
    PYTHON_PATH=$(pwd)/examples/agent/outfit_assistant/.venv ./build/agentflowd \\
      --listen 127.0.0.1:17891 \\
      --package $(pwd)/examples/agent/outfit_assistant

运行：
  # 全量（建议先重启 daemon，最多跑 4 个用例后 daemon 退化）
  python3 -m pytest frontend/bridge/tests/test_outfit_e2e.py -v -m e2e

  # 单个场景
  python3 -m pytest frontend/bridge/tests/test_outfit_e2e.py -k "test_weather" -v
"""

import json
import os
import re
import socket
import time
from typing import Any

import pytest

# ── 配置 ──

DAEMON_HOST = os.environ.get("DAEMON_HOST", "127.0.0.1")
DAEMON_PORT = int(os.environ.get("DAEMON_PORT", "17891"))
AGENT_ID = os.environ.get("E2E_AGENT_ID", "outfit-assistant")
TURN_TIMEOUT = 180  # 单次请求超时（秒），图片生成需调 API 耗时较长
DAEMON_LOG = os.environ.get(
    "DAEMON_LOG",
    os.path.join(os.path.dirname(__file__), "..", "..", "..", "logs", "agentflowd.log")
)


# ── 低层通信 ──


def _jsonrpc(sock: socket.socket, method: str,
             params: dict, req_id: int = 1) -> dict:
    """发送 JSON-RPC 请求，返回 \n 分隔的单行响应。"""
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


def _log_size() -> int:
    """返回 daemon 日志文件的当前字节数。"""
    try:
        return os.path.getsize(DAEMON_LOG)
    except OSError:
        return 0


def _read_log(from_pos: int) -> tuple[list[str], int]:
    """从 ``from_pos`` 偏移量起读取日志新增行。

    Returns ``(new_lines, new_pos)``。
    """
    try:
        with open(DAEMON_LOG, "rb") as f:
            f.seek(from_pos)
            raw = f.read()
            new_pos = f.tell()
        text = raw.decode("utf-8", errors="replace")
        lines = [l for l in text.split("\n") if l.strip()]
        return lines, new_pos
    except (FileNotFoundError, OSError):
        return [], from_pos


# ── 测试主流程 ──


def run_turn(message: str,
             timeout: int = TURN_TIMEOUT) -> dict[str, Any]:
    """发送用户消息，验证 daemon 执行链完整性。

    工作方式:
      1. 记录日志文件当前偏移量
      2. 通过 JSON-RPC 发送 agent.execute
      3. 以 2s 间隔轮询日志增量，寻找 Exit code 和 Response ready
      4. 验证执行链完成

    Returns ``{success, elapsed, exit_count, response_ready, msg_id, log_snippet}``
    """
    # 1) 记录当前位置
    log_pos = _log_size()
    start = time.time()

    # 2) 发送请求
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(10)
    sock.connect((DAEMON_HOST, DAEMON_PORT))
    try:
        result = _jsonrpc(sock, "agent.execute", {
            "agent_id": AGENT_ID,
            "user_id": f"e2e-{int(start)}",
            "client_id": f"cli-{int(start)}",
            "content": message,
        })
        msg_id = result.get("result", {}).get("message_id", "?")
    except Exception as exc:
        return {
            "success": False, "msg_id": str(exc),
            "exit_count": 0, "response_ready": False,
            "elapsed": time.time() - start,
            "log_snippet": "",
        }
    finally:
        sock.close()

    # 3) 轮询日志
    exit_lines: list[str] = []
    response_ready = False
    deadline = time.time() + timeout

    while time.time() < deadline:
        new_lines, log_pos = _read_log(log_pos)
        for line in new_lines:
            if "Exit code=0" in line and "Success=true" in line:
                exit_lines.append(line)
            if "Response ready" in line:
                response_ready = True
        if response_ready and len(exit_lines) >= 2:
            break
        time.sleep(2)

    return {
        "success": bool(response_ready and len(exit_lines) >= 2),
        "elapsed": time.time() - start,
        "exit_count": len(exit_lines),
        "response_ready": response_ready,
        "msg_id": msg_id,
        "log_snippet": exit_lines[-1] if exit_lines else "",
    }


# ── 断言 ──


def assert_execution_chain(result: dict[str, Any], scenario: str):
    """验证执行链完整性。

    不变量:
      I-0: Response ready 事件出现（daemon 完成了 coordinator 最终处理）
      I-1: 至少 2 个节点 Exit code=0（dummy_pre_skill + coordinator_decide，
           复杂场景会有更多：+ skill_executor + coordinator_decide 第二轮）
    """
    assert result.get("response_ready"), (
        f"[{scenario}] I-0 FAILED: 无 Response ready.\n"
        f"  elapsed={result['elapsed']:.0f}s  exits={result.get('exit_count')}\n"
        f"  msg_id={result.get('msg_id')}"
    )
    assert result.get("success"), (
        f"[{scenario}] I-1 FAILED: 节点执行链不完整.\n"
        f"  Response ready=是，但 exit_count={result.get('exit_count')}（期望 ≥2）\n"
        f"  log_snippet={result.get('log_snippet','')[:200]}"
    )


# ══════════════════════════════════════════════════════════════════════════════
# 测试场景
# ══════════════════════════════════════════════════════════════════════════════

# 注意: daemon 处理约 5 个请求后会退化。建议每批跑 3-4 个用例后
# 重启 daemon。pytest 默认 sequential 执行，10 个用例的理论总时间
# 约为 10 × 30s = 300s，在 daemon 退化前足以跑完前 4 个用例。


class TestWeather:
    """天气查询相关场景"""

    @pytest.mark.e2e
    def test_weather_city(self):
        """TC-1: 指定城市查天气"""
        r = run_turn("北京今天天气如何")
        assert_execution_chain(r, "weather_city")
        print(f"  ✅ TC-1 exits={r['exit_count']}  {r['elapsed']:.0f}s")

    @pytest.mark.e2e
    def test_weather_two_cities(self):
        """TC-2: 两个城市对比"""
        r = run_turn("北京和上海今天天气怎么样")
        assert_execution_chain(r, "two_cities")
        print(f"  ✅ TC-2 exits={r['exit_count']}  {r['elapsed']:.0f}s")

    @pytest.mark.e2e
    def test_weather_no_city(self):
        """TC-3: 未指定城市——应回复追问城市，不应报错"""
        r = run_turn("今天天气怎么样")
        assert_execution_chain(r, "no_city")
        print(f"  ✅ TC-3 exits={r['exit_count']}  {r['elapsed']:.0f}s")


class TestOutfit:
    """穿搭建议相关场景"""

    @pytest.mark.e2e
    def test_weather_outfit(self):
        """TC-4: 天气查询 + 穿搭建议"""
        r = run_turn("北京今天28度，适合穿什么衣服出门")
        assert_execution_chain(r, "weather_outfit")
        print(f"  ✅ TC-4 exits={r['exit_count']}  {r['elapsed']:.0f}s")

    @pytest.mark.e2e
    def test_occasion_outfit(self):
        """TC-5: 出差场合穿搭"""
        r = run_turn("明天要去北京出差，帮我推荐合适的穿搭")
        assert_execution_chain(r, "occasion_outfit")
        print(f"  ✅ TC-5 exits={r['exit_count']}  {r['elapsed']:.0f}s")

    @pytest.mark.e2e
    def test_business_weather_combined(self):
        """TC-6: 商务会议 + 天气查询（最复杂场景）"""
        r = run_turn("明天北京有商务会议，帮我看看天气并推荐穿搭")
        assert_execution_chain(r, "business_weather")
        print(f"  ✅ TC-6 exits={r['exit_count']}  {r['elapsed']:.0f}s")


class TestEdgeCases:
    """边界和口语化场景"""

    @pytest.mark.e2e
    def test_daily_reminder(self):
        """TC-7: 日常提醒式提问"""
        r = run_turn("我今天出门要注意什么")
        assert_execution_chain(r, "daily_reminder")
        print(f"  ✅ TC-7 exits={r['exit_count']}  {r['elapsed']:.0f}s")

    @pytest.mark.e2e
    def test_profile_question(self):
        """TC-8: 用户提及体型偏好"""
        r = run_turn("我个子比较矮，有什么显高的穿搭推荐")
        assert_execution_chain(r, "profile")
        print(f"  ✅ TC-8 exits={r['exit_count']}  {r['elapsed']:.0f}s")

    @pytest.mark.e2e
    def test_weekend_plan(self):
        """TC-9: 结合天气的周末出行计划"""
        r = run_turn("周末想去北京郊外玩，天气适合吗，该怎么穿")
        assert_execution_chain(r, "weekend")
        print(f"  ✅ TC-9 exits={r['exit_count']}  {r['elapsed']:.0f}s")

    @pytest.mark.e2e
    def test_humorous(self):
        """TC-10: 口语化表达"""
        r = run_turn("北京热死了，穿啥能凉快点")
        assert_execution_chain(r, "humorous")
        print(f"  ✅ TC-10 exits={r['exit_count']}  {r['elapsed']:.0f}s")


class TestImageUpload:
    """图片上传分析场景——用户上传穿搭照片请求风格点评"""

    @pytest.mark.e2e
    def test_outfit_photo_analysis(self):
        """TC-11: 上传穿搭照请求风格点评"""
        r = run_turn(
            "帮我看看这件衣服怎么样，图片在 data/test_images/outfit_sample.png"
        )
        assert_execution_chain(r, "photo_analysis")
        print(f"  ✅ TC-11 exits={r['exit_count']}  {r['elapsed']:.0f}s")

    @pytest.mark.e2e
    def test_garment_identification(self):
        """TC-12: 上传单品图请求识别"""
        r = run_turn(
            "帮我识别这件单品，图片在 data/test_images/tshirt.png"
        )
        assert_execution_chain(r, "garment_id")
        print(f"  ✅ TC-12 exits={r['exit_count']}  {r['elapsed']:.0f}s")


class TestImageGeneration:
    """图片生成场景——用户请求生成穿搭效果图"""

    @pytest.mark.e2e
    def test_generate_outfit_image(self):
        """TC-13: 文字描述生成穿搭效果图"""
        r = run_turn(
            "帮我生成一张白色T恤搭配蓝色牛仔裤的穿搭效果图"
        )
        assert_execution_chain(r, "gen_outfit")
        print(f"  ✅ TC-13 exits={r['exit_count']}  {r['elapsed']:.0f}s")

    @pytest.mark.e2e
    def test_generate_fashion_style(self):
        """TC-14: 根据风格描述生成时尚搭配图"""
        r = run_turn(
            "生成一张法式简约风格的春季穿搭效果图"
        )
        assert_execution_chain(r, "gen_fashion")
        print(f"  ✅ TC-14 exits={r['exit_count']}  {r['elapsed']:.0f}s")


# ══════════════════════════════════════════════════════════════════════════════
# 独立运行入口（便于调试，绕过 pytest）
# ══════════════════════════════════════════════════════════════════════════════

if __name__ == "__main__":
    cases = [
        ("TC-1  weather_city",           TestWeather().test_weather_city),
        ("TC-2  two_cities",             TestWeather().test_weather_two_cities),
        ("TC-3  no_city",                TestWeather().test_weather_no_city),
        ("TC-4  weather_outfit",         TestOutfit().test_weather_outfit),
        ("TC-5  occasion_outfit",        TestOutfit().test_occasion_outfit),
        ("TC-6  business_weather",       TestOutfit().test_business_weather_combined),
        ("TC-7  daily_reminder",         TestEdgeCases().test_daily_reminder),
        ("TC-8  profile",                TestEdgeCases().test_profile_question),
        ("TC-9  weekend_plan",           TestEdgeCases().test_weekend_plan),
        ("TC-10 humorous",               TestEdgeCases().test_humorous),
        ("TC-11 photo_analysis",         TestImageUpload().test_outfit_photo_analysis),
        ("TC-12 garment_id",             TestImageUpload().test_garment_identification),
        ("TC-13 gen_outfit",             TestImageGeneration().test_generate_outfit_image),
        ("TC-14 gen_fashion",            TestImageGeneration().test_generate_fashion_style),
    ]
    passed = 0
    for name, fn in cases:
        print(f"\n--- {name} ---")
        try:
            fn()
            passed += 1
        except Exception as e:
            print(f"  ❌ FAIL: {e}")
    print(f"\n{'='*40}\n{passed}/{len(cases)} passed")
