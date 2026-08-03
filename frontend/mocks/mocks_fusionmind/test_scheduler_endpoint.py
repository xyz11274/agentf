"""
mocks_fusionmind scheduler 端点测试。

验证 sidecar UdsModelProxy coremind-mode 生产路径的转发目标
（/infermind/scheduler/v1/chat/completions）返回**顶层** content 字段格式
（IMInferScheduler 风格），而非 OpenAI choices 包装。
"""
from __future__ import annotations

from typing import Any

import httpx
import pytest
from httpx import ASGITransport

from mocks.mocks_fusionmind.server import app, configure

# ── 配置：echo 模式，快速返回 ────────────────────────────────────


def _make_args() -> Any:
    class _Args:
        mode = "echo"
        response = ""
        token_delay = 0.0
        completion_delay = 0.0
        error_rate = 0.0
        proxy = False
        upstream_url = ""
        api_key = ""
        model = "aisf/deepseek-v4-flash"
        mock_usage = True
        use_result_wrapper = False

    return _Args()


@pytest.fixture(autouse=True)
def _setup_config() -> None:
    configure(_make_args())


@pytest.mark.asyncio
async def test_scheduler_endpoint_returns_top_level_content() -> None:
    """scheduler 端点应返回顶层 content 字段（非 OpenAI choices 包装）。"""
    transport = ASGITransport(app=app)
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        resp = await client.post(
            "/infermind/scheduler/v1/chat/completions",
            json={
                "model": "aisf/deepseek-v4-flash",
                "messages": [
                    {"role": "system", "content": "你是穿搭顾问。"},
                    {"role": "user", "content": "今天穿什么？"},
                ],
                "stream": False,
            },
        )
        assert resp.status_code == 200
        body = resp.json()

        # 顶层 content 字段（对齐 uds_model_proxy.cpp 解析路径）
        assert "content" in body, f"scheduler 响应应含顶层 content，实际 keys={list(body.keys())}"
        assert "你说了: 今天穿什么？" in body["content"]
        # 不应是 OpenAI choices 包装
        assert "choices" not in body


@pytest.mark.asyncio
async def test_openai_endpoint_keeps_openai_format() -> None:
    """既有 OpenAI 端点保持 choices 包装，不受 scheduler 端点影响。"""
    transport = ASGITransport(app=app)
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as client:
        resp = await client.post(
            "/infermind/llm/v1/chat/openai/completions",
            json={
                "messages": [{"role": "user", "content": "你好"}],
                "stream": False,
            },
        )
        assert resp.status_code == 200
        body = resp.json()
        assert "choices" in body
        assert body["choices"][0]["message"]["content"] == "你说了: 你好"
