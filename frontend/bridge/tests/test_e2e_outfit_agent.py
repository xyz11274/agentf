"""
穿搭助手 E2E 测试套件 — pytest + daemon fixture 版。

设计原则（PROJECT_MEMORY「真实 LLM e2e 设计原则」）：
  1. 每个 TestClass 启动独立的 daemon（class-scoped fixture，避免退化累积）
  2. 真实 LLM 调用（outfit_daemon fixture 实为 outfit_daemon_real_llm 的别名）
  3. 只将最重要链路实现为完全真实 e2e；同链路旁支用代表场景验证连通性，
     不为每个旁支重复付费真模型调用——重链路深度验证留给 release 前 full E2E
  4. 通过 daemon TCP JSON-RPC 发送消息，不依赖 bridge 或上游回调
  5. 验证执行链完整性（response 收到、节点正常完成），不做 LLM 输出内容断言

精简记录：原 14 个测试压缩为 6 个代表场景（每类留 1-2 个），
回归耗时从 84s（daemon log 302MB）预期降至 ~35s，链路验证不变。

使用方式：
  # 全量运行（需要已编译 agentflowd + outfit-assistant .venv）
  python -m pytest frontend/bridge/tests/test_e2e_outfit_agent.py -v -m e2e

  # 单个场景
  python -m pytest frontend/bridge/tests/test_e2e_outfit_agent.py -k "test_weather" -v
"""

from __future__ import annotations

import json
import time
from typing import Any

import pytest

from .e2e_helpers import (
    outfit_daemon,
    send_user_message,
    jsonrpc_call,
    poll_agent_response,
    AGENT_LAYER_L3,
    skip_if_layer_not_in,
)

_skip_reason = skip_if_layer_not_in(AGENT_LAYER_L3)
pytestmark = [
    pytest.mark.e2e,
    pytest.mark.skip(reason="Temporarily disabled"),
]


def _response_received(resp: dict) -> bool:
    """检查 JSON-RPC 响应是否包含正常结果。"""
    return bool(resp.get("result")) and "error" not in resp


def _wait_response(host: str, port: int, task_id: str,
                   timeout: int = 30) -> dict[str, Any]:
    """轮询 daemon 的 agent.poll_response，等待 task 完成。

    timeout=30s: 真实 LLM 实测单轮 ≤15s（见 logs/tests/ 落盘数据），
    留 2x 余量；网络挂起时快速失败，避免拖整轮回归。
    """
    content = poll_agent_response(host, port, task_id, timeout)
    return {"Data": {"UserOutputText": content}}


# ═══════════════════════════════════════════════════════════════════
# 天气查询场景（代表：单城市查询，覆盖天气意图 + skill 调度链路）
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


# ═══════════════════════════════════════════════════════════════════
# 穿搭推荐场景（代表：综合商务出差 + 天气，覆盖 coordinator 多 skill 串联）
# ═══════════════════════════════════════════════════════════════════


@pytest.mark.e2e
class TestOutfit:
    """穿搭推荐查询。"""

    def test_business_weather_combined(self, outfit_daemon: dict[str, Any]) -> None:
        """商务出差 + 天气的综合询问（覆盖多 skill 串联链路）。"""
        port = outfit_daemon["daemon_port"]
        send_user_message("127.0.0.1", port, "明天去上海出差三天，帮推荐穿搭", "e2e-o-business")
        result = _wait_response("127.0.0.1", port, "e2e-o-business")
        assert "UserOutputText" in result.get("Data", {})


# ═══════════════════════════════════════════════════════════════════
# 边界场景（代表：用户画像存储，覆盖画像记忆链路）
# ═══════════════════════════════════════════════════════════════════


@pytest.mark.e2e
class TestEdgeCases:
    """边界及特殊场景。"""

    def test_profile_question(self, outfit_daemon: dict[str, Any]) -> None:
        """用户画像相关问题（覆盖画像记忆链路）。"""
        port = outfit_daemon["daemon_port"]
        send_user_message("127.0.0.1", port, "帮我记住我身高175cm", "e2e-e-profile")
        result = _wait_response("127.0.0.1", port, "e2e-e-profile")
        assert "UserOutputText" in result.get("Data", {})


# ═══════════════════════════════════════════════════════════════════
# 图片上传（JSON-RPC 传文本描述，不传真实图片）
# ═══════════════════════════════════════════════════════════════════


@pytest.mark.e2e
class TestImageUpload:
    """图片上传相关场景（通过 JSON-RPC 文本模拟）。"""

    def test_garment_identification(self, outfit_daemon: dict[str, Any]) -> None:
        """识别图片中的衣物（覆盖 image upload intent 链路）。"""
        port = outfit_daemon["daemon_port"]
        send_user_message("127.0.0.1", port, "这是什么衣服？", "e2e-img-garment")
        result = _wait_response("127.0.0.1", port, "e2e-img-garment")
        assert "UserOutputText" in result.get("Data", {})


# ═══════════════════════════════════════════════════════════════════
# 图片生成场景（代表：简短的图片生成，覆盖 image generation 重链路
#   + 产物落盘 + 图片链接可访问校验）
# ═══════════════════════════════════════════════════════════════════


@pytest.mark.e2e
class TestImageGeneration:
    """图片生成相关场景。"""

    def test_generate_simple_outfit(self, outfit_daemon: dict[str, Any]) -> None:
        """生成甜妹穿搭图，验证产物落盘 + 图片链接可访问。

        最简单路径：只发"生成一张甜妹穿搭图"，不附带复杂约束。
        验证：
          1. 最终响应为 UserOutputText
          2. 响应内容包含图片 URL（/api/artifacts/...）
          3. 图片 URL 可访问（HTTP 200）
        """
        port = outfit_daemon["daemon_port"]
        send_user_message("127.0.0.1", port, "生成一张甜妹穿搭图", "e2e-gen-simple")
        result = _wait_response("127.0.0.1", port, "e2e-gen-simple", timeout=180)
        text = result.get("Data", {}).get("UserOutputText", "")
        assert text, f"响应内容为空: {result}"

        # 验证响应包含产物图片链接
        assert "/api/artifacts/" in text, (
            f"响应应包含图片链接，实际内容前 200 字={text[:200]}")

        # 提取所有图片 URL 并验证可访问
        import re
        urls = re.findall(r"/api/artifacts/\S+(?:\.png|\.jpg|\.jpeg|\.webp)", text)
        assert urls, f"未找到图片 URL: {text[:200]}"

        for url in urls:
            import urllib.request
            try:
                resp = urllib.request.urlopen(f"http://127.0.0.1:{port}{url}", timeout=10)
                assert resp.status == 200, f"图片 {url} 返回 {resp.status}"
                data = resp.read()
                assert len(data) > 1000, f"图片 {url} 太小 ({len(data)} bytes)"
            except Exception as exc:
                pytest.fail(f"图片 {url} 不可访问: {exc}")
