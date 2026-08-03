"""
E2E 测试：agent-external — 真实外部服务调用 + 沙箱全链路（L2 层）。

使用 ``fixtures/agent-external``，包含：
- ``ext-service`` skill：在沙箱内调真实外部 API（如 wttr.in），验证网络连通性

需要设置 ``AF_LLM_API_KEY`` 和 ``AF_LLM_BASE_URL`` 环境变量。

运行：
  # 默认只跑 L0+L1，需显式启用 L2
  E2E_AGENT_LAYERS="0,1,2" uv run pytest \
    frontend/bridge/tests/test_e2e_agent_external.py -v -m e2e
"""

from __future__ import annotations

import json
import os
import re
import signal
import subprocess
import time
import uuid
from pathlib import Path
from typing import Any, Iterator

import pytest
import requests as http_requests

from .e2e_helpers import (
    PROJECT_ROOT,
    EXTERNAL_AGENT_ID,
    AGENT_LAYER_L2,
    _daemon_env,
    _daemon_log_path,
    _test_log_dir,
    await_agent_ready,
    external_agent_daemon,  # noqa: F401
    free_port,
    skip_if_layer_not_in,
    wait_port,
)

# ── 超时配置 ──────────────────────────────────────────────────────────
_E2E_TIMEOUT = 180

# 层选择：默认只跑 L0+L1，L2 需要显式启用
_skip_reason = skip_if_layer_not_in(AGENT_LAYER_L2)
pytestmark = [
    pytest.mark.e2e,
    pytest.mark.skipif(_skip_reason, reason=_skip_reason),
]


# ═══════════════════════════════════════════════════════════════════════
# Fixture: daemon + bridge
# ═══════════════════════════════════════════════════════════════════════


@pytest.fixture(scope="class")
def ext_bridge_daemon(
    request: pytest.FixtureRequest,
    external_agent_daemon: dict[str, Any],  # noqa: F811
) -> Iterator[dict[str, Any]]:
    """启动 agent-external daemon + bridge，返回端点信息。"""
    daemon_info = external_agent_daemon
    daemon_port = daemon_info["daemon_port"]
    llm_mode = daemon_info.get("llm_mode", "REAL")

    print(f"[ext_bridge_daemon] daemon_port={daemon_port} llm_mode={llm_mode}")

    bridge_port = free_port()
    daemon_log_path = daemon_info.get("log_path")
    if daemon_log_path:
        bridge_log = Path(daemon_log_path).parent / "bridge.log"
        bridge_log.parent.mkdir(parents=True, exist_ok=True)
        bridge_log_fp = bridge_log.open("wb")
    else:
        bridge_log = None
        bridge_log_fp = subprocess.DEVNULL

    bridge_dir = PROJECT_ROOT / "frontend" / "bridge"
    env = dict(os.environ)
    env["AF_BR_ENV"] = "test"
    env["AF_BR_DAEMON_HOST"] = "127.0.0.1"
    env["AF_BR_DAEMON_PORT"] = str(daemon_port)
    env["BRIDGE_HTTP_PORT"] = str(bridge_port)
    env["NO_PROXY"] = "127.0.0.1,localhost"
    env["no_proxy"] = "127.0.0.1,localhost"

    bridge_proc = subprocess.Popen(
        [
            "uv", "run", "--directory", str(bridge_dir),
            "python", "-m", "uvicorn", "main:bridge_app",
            "--host", "127.0.0.1",
            "--port", str(bridge_port),
            "--log-level", "warning",
        ],
        cwd=str(bridge_dir),
        env=env,
        stdout=bridge_log_fp,
        stderr=subprocess.STDOUT,
    )

    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        try:
            r = http_requests.get(
                f"http://127.0.0.1:{bridge_port}/api/health", timeout=1
            )
            if r.status_code == 200:
                break
        except Exception:
            pass
        time.sleep(0.3)
    else:
        bridge_proc.terminate()
        bridge_proc.wait()
        pytest.fail("bridge failed to start")

    bridge_url = f"http://127.0.0.1:{bridge_port}"

    try:
        yield {
            "bridge_url": bridge_url,
            "bridge_port": bridge_port,
            "daemon_port": daemon_port,
            "llm_mode": llm_mode,
            "agent_id": EXTERNAL_AGENT_ID,
            "bridge_log_path": str(bridge_log) if bridge_log else None,
        }
    finally:
        bridge_proc.terminate()
        try:
            bridge_proc.wait(timeout=5)
        except Exception:
            bridge_proc.kill()
        if bridge_log_fp is not subprocess.DEVNULL:
            try:
                bridge_log_fp.close()
            except Exception:
                pass


# ═══════════════════════════════════════════════════════════════════════
# 辅助函数
# ═══════════════════════════════════════════════════════════════════════


def _sse_parse(text: str) -> list[dict[str, Any] | str]:
    """解析 OpenAI 兼容 SSE 文本。"""
    events: list[dict[str, Any] | str] = []
    current_lines: list[str] = []
    for line in text.split("\n"):
        if line.startswith("data: "):
            payload = line[6:].strip()
            if payload == "[DONE]":
                events.append(payload)
            else:
                current_lines.append(payload)
        elif line == "":
            if current_lines:
                try:
                    events.append(json.loads("".join(current_lines)))
                except json.JSONDecodeError:
                    pass
                current_lines = []
    return events


# ═══════════════════════════════════════════════════════════════════════
# 测试：外部服务调用全链路
# ═══════════════════════════════════════════════════════════════════════


@pytest.mark.e2e
class TestExternalService:
    """agent-external 在沙箱模式下的外部服务调用全链路测试。"""

    def test_chat_returns_nonempty(
        self, ext_bridge_daemon: dict[str, Any],
    ) -> None:
        """问答场景：发送普通消息，验证 SSE 返回非空内容。"""
        base_url = ext_bridge_daemon["bridge_url"]
        uid = f"e2e-ext-chat-{uuid.uuid4().hex[:6]}"
        cid = f"cli-{uuid.uuid4().hex[:6]}"

        resp = http_requests.post(
            f"{base_url}/api/chat/{EXTERNAL_AGENT_ID}?stream=true",
            json={
                "user_id": uid,
                "client_id": cid,
                "content": "你好",
            },
            stream=True,
            timeout=_E2E_TIMEOUT,
        )
        assert resp.status_code == 200, (
            f"HTTP {resp.status_code}: {resp.text[:500]}"
        )

        text = resp.text
        events = _sse_parse(text)

        assert len(events) >= 2, (
            f"Expected >=2 SSE events, got {len(events)}.\n"
            f"Raw text (first 800 chars):\n{text[:800]}"
        )
        assert events[-1] == "[DONE]", (
            f"Last event should be [DONE], got: {events[-1]!r}"
        )

        all_content = ""
        for event in events[:-1]:
            if not isinstance(event, dict):
                continue
            choices = event.get("choices", [])
            for choice in choices:
                delta = choice.get("delta", {})
                if delta and delta.get("content"):
                    all_content += delta["content"]
                msg = choice.get("message", {})
                if msg and msg.get("content"):
                    all_content += msg["content"]

        assert len(all_content) > 0, "Empty response"
        print(f"[test_chat] content_len={len(all_content)} "
              f"preview={all_content[:80]}...")

    def test_external_service_call(
        self, ext_bridge_daemon: dict[str, Any],
    ) -> None:
        """外部服务调用场景：发送请求，验证沙箱内调真实外部 API 并返回结果。

        验证链路：
          bridge HTTP POST → daemon → agent → coordinator → skill_executor
          → e2b/Docker 沙箱 → ext-service skill → 调真实外部 API
          → 结果回传 → 响应

        注意：需要沙箱网络出站规则允许访问外部 API。
        """
        base_url = ext_bridge_daemon["bridge_url"]
        uid = f"e2e-ext-svc-{uuid.uuid4().hex[:6]}"
        cid = f"cli-{uuid.uuid4().hex[:6]}"

        resp = http_requests.post(
            f"{base_url}/api/chat/{EXTERNAL_AGENT_ID}?stream=true",
            json={
                "user_id": uid,
                "client_id": cid,
                "content": "查询一下今天的天气",
            },
            stream=True,
            timeout=_E2E_TIMEOUT,
        )
        assert resp.status_code == 200, (
            f"HTTP {resp.status_code}: {resp.text[:500]}"
        )

        text = resp.text
        events = _sse_parse(text)

        assert len(events) >= 2, (
            f"Expected >=2 SSE events, got {len(events)}.\n"
            f"Raw text (first 800 chars):\n{text[:800]}"
        )
        assert events[-1] == "[DONE]", (
            f"Last event should be [DONE], got: {events[-1]!r}"
        )

        all_content = ""
        for event in events[:-1]:
            if not isinstance(event, dict):
                continue
            choices = event.get("choices", [])
            for choice in choices:
                delta = choice.get("delta", {})
                if delta and delta.get("content"):
                    all_content += delta["content"]
                msg = choice.get("message", {})
                if msg and msg.get("content"):
                    all_content += msg["content"]

        assert len(all_content) > 0, "Empty response"
        print(f"[test_ext_svc] content_len={len(all_content)} "
              f"preview={all_content[:80]}...")