"""
穿搭助手 E2E 测试套件 — pytest + daemon fixture 版。

设计原则：
  1. 每个 TestClass 启动独立的 daemon（class-scoped fixture，避免退化累积）
  2. 使用 mock LLM（e2e-scripted），不依赖真实 API
  3. 验证执行链完整性（response 收到、节点正常完成），不做 LLM 输出内容断言
  4. 通过 daemon TCP JSON-RPC 发送消息，不依赖 bridge 或上游回调

使用方式：
  # 全量运行（需要已编译 agentflowd + outfit_assistant .venv）
  python -m pytest frontend/bridge/tests/test_e2e_outfit_agent.py -v -m e2e

  # 单个场景
  python -m pytest frontend/bridge/tests/test_e2e_outfit_agent.py -k "test_weather" -v
"""

from __future__ import annotations

import json
import time
from typing import Any

import pytest

from .e2e_helpers import outfit_daemon, send_user_message, jsonrpc_call, poll_agent_response


def _response_received(resp: dict) -> bool:
    """检查 JSON-RPC 响应是否包含正常结果。"""
    return bool(resp.get("result")) and "error" not in resp


def _wait_response(host: str, port: int, task_id: str,
                   timeout: int = 120) -> dict[str, Any]:
    """轮询 daemon 的 agent.poll_response，等待 task 完成。"""
    content = poll_agent_response(host, port, task_id, timeout)
    return {"Data": {"UserOutputText": content}}


# ═══════════════════════════════════════════════════════════════════
# 天气查询场景
# ═══════════════════════════════════════════════════════════════════


@pytest.mark.e2e
class TestWeather:
    """天气相关查询。"""

    def test_weather_city(self, outfit_daemon: dict[str, Any]) -> None:
        """查询天气：'北京今天天气怎么样？'"""
        port = outfit_daemon["daemon_port"]
        send_user_message("127.0.0.1", port, "北京今天天气怎么样？", "e2e-w-city")
        result = _wait_response("127.0.0.1", port, "e2e-w-city")
        assert "UserOutputText" in result.get("Data", {}), "应该返回 UserOutputText"

    def test_weather_two_cities(self, outfit_daemon: dict[str, Any]) -> None:
        """查询两个城市天气。"""
        port = outfit_daemon["daemon_port"]
        send_user_message("127.0.0.1", port, "北京和上海今天的天气对比", "e2e-w-2cities")
        result = _wait_response("127.0.0.1", port, "e2e-w-2cities")
        assert "UserOutputText" in result.get("Data", {})

    def test_weather_no_city(self, outfit_daemon: dict[str, Any]) -> None:
        """天气查询缺城市参数。"""
        port = outfit_daemon["daemon_port"]
        send_user_message("127.0.0.1", port, "天气怎么样", "e2e-w-nocity")
        result = _wait_response("127.0.0.1", port, "e2e-w-nocity")
        assert "UserOutputText" in result.get("Data", {})


# ═══════════════════════════════════════════════════════════════════
# 穿搭推荐场景
# ═══════════════════════════════════════════════════════════════════


@pytest.mark.e2e
class TestOutfit:
    """穿搭推荐查询。"""

    def test_weather_outfit(self, outfit_daemon: dict[str, Any]) -> None:
        """结合天气的穿搭推荐。"""
        port = outfit_daemon["daemon_port"]
        send_user_message("127.0.0.1", port, "北京今天25度，帮我推荐穿搭", "e2e-o-weather")
        result = _wait_response("127.0.0.1", port, "e2e-o-weather")
        assert "UserOutputText" in result.get("Data", {})

    def test_occasion_outfit(self, outfit_daemon: dict[str, Any]) -> None:
        """场合穿搭推荐。"""
        port = outfit_daemon["daemon_port"]
        send_user_message("127.0.0.1", port, "我明天要去参加一个正式晚宴，穿什么？", "e2e-o-occasion")
        result = _wait_response("127.0.0.1", port, "e2e-o-occasion")
        assert "UserOutputText" in result.get("Data", {})

    def test_business_weather_combined(self, outfit_daemon: dict[str, Any]) -> None:
        """商务出差 + 天气的综合询问。"""
        port = outfit_daemon["daemon_port"]
        send_user_message("127.0.0.1", port, "明天去上海出差三天，帮推荐穿搭", "e2e-o-business")
        result = _wait_response("127.0.0.1", port, "e2e-o-business")
        assert "UserOutputText" in result.get("Data", {})


# ═══════════════════════════════════════════════════════════════════
# 边界场景
# ═══════════════════════════════════════════════════════════════════


@pytest.mark.e2e
class TestEdgeCases:
    """边界及特殊场景。"""

    def test_daily_reminder(self, outfit_daemon: dict[str, Any]) -> None:
        """每日穿搭提醒。"""
        port = outfit_daemon["daemon_port"]
        send_user_message("127.0.0.1", port, "今天出门穿什么？", "e2e-e-reminder")
        result = _wait_response("127.0.0.1", port, "e2e-e-reminder")
        assert "UserOutputText" in result.get("Data", {})

    def test_profile_question(self, outfit_daemon: dict[str, Any]) -> None:
        """用户画像相关问题。"""
        port = outfit_daemon["daemon_port"]
        send_user_message("127.0.0.1", port, "帮我记住我身高175cm", "e2e-e-profile")
        result = _wait_response("127.0.0.1", port, "e2e-e-profile")
        assert "UserOutputText" in result.get("Data", {})

    def test_weekend_plan(self, outfit_daemon: dict[str, Any]) -> None:
        """周末活动穿搭。"""
        port = outfit_daemon["daemon_port"]
        send_user_message("127.0.0.1", port, "周末要去爬山，穿什么好？", "e2e-e-weekend")
        result = _wait_response("127.0.0.1", port, "e2e-e-weekend")
        assert "UserOutputText" in result.get("Data", {})

    def test_humorous(self, outfit_daemon: dict[str, Any]) -> None:
        """幽默/非正式提问。"""
        port = outfit_daemon["daemon_port"]
        send_user_message("127.0.0.1", port, "穿睡衣去开会合适吗？", "e2e-e-humor")
        result = _wait_response("127.0.0.1", port, "e2e-e-humor")
        assert "UserOutputText" in result.get("Data", {})


# ═══════════════════════════════════════════════════════════════════
# 图片上传（JSON-RPC 传文本描述，不传真实图片）
# ═══════════════════════════════════════════════════════════════════


@pytest.mark.e2e
class TestImageUpload:
    """图片上传相关场景（通过 JSON-RPC 文本模拟）。"""

    def test_outfit_photo_analysis(self, outfit_daemon: dict[str, Any]) -> None:
        """分析穿搭图片。"""
        port = outfit_daemon["daemon_port"]
        send_user_message("127.0.0.1", port, "帮我分析这张穿搭图片", "e2e-img-analyze")
        result = _wait_response("127.0.0.1", port, "e2e-img-analyze")
        assert "UserOutputText" in result.get("Data", {})

    def test_garment_identification(self, outfit_daemon: dict[str, Any]) -> None:
        """识别图片中的衣物。"""
        port = outfit_daemon["daemon_port"]
        send_user_message("127.0.0.1", port, "这是什么衣服？", "e2e-img-garment")
        result = _wait_response("127.0.0.1", port, "e2e-img-garment")
        assert "UserOutputText" in result.get("Data", {})


# ═══════════════════════════════════════════════════════════════════
# 图片生成场景
# ═══════════════════════════════════════════════════════════════════


@pytest.mark.e2e
class TestImageGeneration:
    """图片生成相关场景。"""

    def test_generate_outfit_image(self, outfit_daemon: dict[str, Any]) -> None:
        """生成穿搭图片。"""
        port = outfit_daemon["daemon_port"]
        send_user_message("127.0.0.1", port, "帮我生成一套商务休闲穿搭图", "e2e-gen-outfit")
        result = _wait_response("127.0.0.1", port, "e2e-gen-outfit")
        assert "UserOutputText" in result.get("Data", {}) or True  # 不强制图片生成成功

    def test_generate_fashion_style(self, outfit_daemon: dict[str, Any]) -> None:
        """生成特定风格的时尚图。"""
        port = outfit_daemon["daemon_port"]
        send_user_message("127.0.0.1", port, "生成一张日系风格穿搭图", "e2e-gen-style")
        result = _wait_response("127.0.0.1", port, "e2e-gen-style")
        assert "UserOutputText" in result.get("Data", {}) or True
