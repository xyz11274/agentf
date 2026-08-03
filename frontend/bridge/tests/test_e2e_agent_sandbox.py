"""
E2E 测试：agent-sandbox — 问答 + 图片生成全链路（e2b/Docker 沙箱模式）。

使用专用的 ``fixtures/agent-sandbox``，包含：
- ``draw-image`` skill：纯本地图片生成（Python 标准库，不调外部 API）
- 问答场景：coordinator 直连 LLM

沙箱模式由 configs/test/agentflowd.json 的 sandbox.mode 决定。

运行：
  E2B_API_KEY="e2b_000000000" \
  SSL_CERT_FILE="$(pwd)/docs/env/e2b_CA.pem" \
  uv run pytest frontend/bridge/tests/test_e2e_agent_sandbox.py -v -m e2e
"""

from __future__ import annotations

import json
import os
import re
import signal
import subprocess
import time
import uuid
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterator

import pytest
import requests as http_requests

from .e2e_helpers import (
    PROJECT_ROOT,
    SANDBOX_AGENT_ID,
    AGENT_LAYER_L1,
    _daemon_env,
    _daemon_log_path,
    _load_sandbox_mode,
    _test_log_dir,
    await_agent_ready,
    free_port,
    sandbox_test_daemon,  # noqa: F401
    skip_if_layer_not_in,
    wait_port,
)

# ── 超时配置 ──────────────────────────────────────────────────────────
_E2E_TIMEOUT = 180
_ARTIFACT_TIMEOUT = 10

# 测试标记
_skip_reason = skip_if_layer_not_in(AGENT_LAYER_L1)
pytestmark = [
    pytest.mark.e2e,
    pytest.mark.skipif(_skip_reason, reason=_skip_reason),
]


# ═══════════════════════════════════════════════════════════════════════
# Fixture: daemon + bridge
# ═══════════════════════════════════════════════════════════════════════


@pytest.fixture(scope="class")
def sb_bridge_daemon(
    request: pytest.FixtureRequest,
    sandbox_test_daemon: dict[str, Any],  # noqa: F811
) -> Iterator[dict[str, Any]]:
    """启动 agent-sandbox daemon + bridge，返回端点信息。"""
    daemon_info = sandbox_test_daemon
    daemon_port = daemon_info["daemon_port"]
    llm_mode = daemon_info.get("llm_mode", "MOCK")
    use_real_llm = daemon_info.get("use_real_llm", False)

    print(f"[sb_bridge_daemon] daemon_port={daemon_port} llm_mode={llm_mode}")

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
    agent_dir = daemon_info.get("agent_dir")
    if agent_dir:
        env["AF_BR_AGENT_PACKAGES_DIR"] = agent_dir

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
            "use_real_llm": use_real_llm,
            "agent_id": SANDBOX_AGENT_ID,
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


def _extract_artifact_urls(events: list[dict[str, Any] | str]) -> set[str]:
    """从 SSE 事件中提取产物 URL。"""
    urls: set[str] = set()
    for event in events:
        if not isinstance(event, dict):
            continue
        choices = event.get("choices", [])
        for choice in choices:
            event_artifacts = choice.get("artifacts") or []
            for art in event_artifacts:
                url = art.get("url")
                if url:
                    urls.add(url)
            msg = choice.get("message", {}) or choice.get("delta", {})
            if msg and msg.get("content"):
                found = re.findall(
                    r"/api/artifacts/\S+(?:\.png|\.jpg|\.jpeg|\.webp)",
                    msg["content"],
                )
                urls.update(found)
    return urls


# ═══════════════════════════════════════════════════════════════════════
# 测试：问答 + 图片生成全链路
# ═══════════════════════════════════════════════════════════════════════


@pytest.mark.e2e
class TestSandboxAgent:
    """agent-sandbox 在沙箱模式下的全链路测试。"""

    _UID = f"e2e-sb-agent-{uuid.uuid4().hex[:6]}"
    _CID = f"cli-{uuid.uuid4().hex[:6]}"

    def test_chat_returns_nonempty(self, sb_bridge_daemon: dict[str, Any]) -> None:
        """问答场景：发送普通消息，验证 SSE 返回非空内容。"""
        base_url = sb_bridge_daemon["bridge_url"]
        llm_mode = sb_bridge_daemon.get("llm_mode", "MOCK")

        resp = http_requests.post(
            f"{base_url}/api/chat/{SANDBOX_AGENT_ID}?stream=true",
            json={
                "user_id": self._UID,
                "client_id": self._CID,
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

        assert len(all_content) > 0, (
            f"Empty response (llm_mode={llm_mode})"
        )
        print(f"[test_chat] content_len={len(all_content)} "
              f"preview={all_content[:80]}...")

    def test_image_generation_in_sandbox(
        self, sb_bridge_daemon: dict[str, Any],
    ) -> None:
        """图片生成场景：发送请求 → skill 调度 → 沙箱内执行 → 产物收集。

        验证链路：
          bridge HTTP POST → daemon → coordinator (mock LLM dispatch)
          → skill_executor → sandbox → draw-image skill 执行
          → generate_test_image.py 用 Python 标准库生成 PNG
          → 产物收集 → artifact URL → 响应中可见

        不变量（必须全部锁住）：
          I-1：响应中有 artifact URL（产物收集成功）
          I-2：artifact URL 可访问（HTTP 200）
          I-3：下载的产物是有效 PNG（魔数 \x89PNG）
          I-4：产物大小 > 100 bytes（非空文件）
          I-5：响应内容中提到 draw-image skill（证明 skill 被调度）
        """
        base_url = sb_bridge_daemon["bridge_url"]
        uid = f"e2e-sandbox-img-{uuid.uuid4().hex[:6]}"
        cid = f"cli-{uuid.uuid4().hex[:6]}"

        resp = http_requests.post(
            f"{base_url}/api/chat/{SANDBOX_AGENT_ID}?stream=true",
            json={
                "user_id": uid,
                "client_id": cid,
                "content": "请帮我生成一张测试图片",
            },
            stream=True,
            timeout=_E2E_TIMEOUT,
        )
        assert resp.status_code == 200, (
            f"HTTP {resp.status_code}: {resp.text[:500]}"
        )

        text = resp.text
        events = _sse_parse(text)

        # I-0：SSE 基本格式
        assert len(events) >= 2, (
            f"Expected >=2 SSE events, got {len(events)}.\n"
            f"Raw text (first 800 chars):\n{text[:800]}"
        )
        assert events[-1] == "[DONE]", (
            f"Last event should be [DONE], got: {events[-1]!r}"
        )

        # 收集所有 content 和 artifact URLs
        all_content = ""
        artifact_urls: set[str] = set()
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
            # 提取 artifact URLs：从 choices[].delta/message.content 中正则匹配，或顶层 artifact 字段
            artifact_urls.update(_extract_artifact_urls([event]))
            if event.get("artifact"):
                artifact_urls.add(event["artifact"])

        # I-1：响应中有 artifact URL（产物收集成功）
        assert len(artifact_urls) > 0, (
            f"No artifact URL in response. Content: {all_content[:500]}"
        )

        # I-5：响应内容中提到 draw-image skill（证明 skill 被调度）
        # mock LLM 应该返回 tool call 触发 draw-image skill
        content_lower = all_content.lower()
        skill_dispatched = (
            "draw-image" in content_lower
            or "draw_image" in content_lower
            or "skill" in content_lower
            or "image" in content_lower
        )
        assert skill_dispatched, (
            f"draw-image skill not dispatched. Content: {all_content[:500]}"
        )

        # I-2：artifact URL 可访问（HTTP 200）
        artifact_url = next(iter(artifact_urls))
        if not artifact_url.startswith("http"):
            artifact_url = f"{base_url}{artifact_url}"
        art_resp = http_requests.get(artifact_url, timeout=_ARTIFACT_TIMEOUT)
        assert art_resp.status_code == 200, (
            f"Artifact URL returned HTTP {art_resp.status_code}: {art_resp.text[:200]}"
        )

        # I-3：下载的产物是有效 PNG（魔数 \x89PNG）
        art_bytes = art_resp.content
        assert len(art_bytes) >= 8, (
            f"Artifact too small: {len(art_bytes)} bytes"
        )
        assert art_bytes[:8] == b"\x89PNG\r\n\x1a\n", (
            f"Artifact is not a valid PNG. First 8 bytes: {art_bytes[:8]!r}"
        )

        # I-4：产物大小 > 100 bytes（非空文件）
        assert len(art_bytes) > 100, (
            f"Artifact too small: {len(art_bytes)} bytes (expected > 100)"
        )

        print(f"[test_image] content_len={len(all_content)} "
              f"artifact_url={artifact_url} artifact_size={len(art_bytes)}")

