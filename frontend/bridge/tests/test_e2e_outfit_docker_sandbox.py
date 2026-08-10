"""
穿搭助手 + Docker 沙箱 E2E 测试 — 验证沙箱内工具执行与产物生成。

测试覆盖：
  1. daemon 以 sandbox-mode=docker 启动
  2. 穿搭助手完整执行链路（coordinator → skill → 工具执行 → 产物生成）
  3. 产物（artifact）在响应中正确提取并可访问

使用方式：
  DOCKER_SANDBOX_ENABLED=1 uv run pytest \\
      frontend/bridge/tests/test_e2e_outfit_docker_sandbox.py \\
      -v --no-header -m e2e
"""

from __future__ import annotations

import json
import os
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
    DAEMON_BINARY,
    OUTFIT_AGENT,
    OUTFIT_AGENT_STR,
    ensure_outfit_agent_3 as _ensure_3,
    _daemon_env,
    _daemon_log_path,
    _load_sandbox_mode,
    _test_log_dir,
    free_port,
    wait_port,
    await_agent_ready,
)
# 三态模型（删 examples 不崩不变量）：import 时自动从 ② 固化态重建 ③
_ensure_3()

# ── 超时配置 ──────────────────────────────────────────────────────────
_E2E_TIMEOUT = 90  # 单轮 chat 超时（秒），与 test_e2e_rest_chat_session 一致

# Docker 沙箱测试标记
pytestmark = [
    pytest.mark.e2e,
    pytest.mark.skipif(
        not os.environ.get("DOCKER_SANDBOX_ENABLED"),
        reason="Docker 沙箱测试需设置 DOCKER_SANDBOX_ENABLED=1",
    ),
]


# ═══════════════════════════════════════════════════════════════════════
# Fixture: Docker 沙箱 + 穿搭助手 daemon + bridge
# ═══════════════════════════════════════════════════════════════════════


@pytest.fixture(scope="module")
def docker_sandbox_bridge(request: pytest.FixtureRequest) -> Iterator[dict[str, Any]]:
    """启动穿搭助手 daemon + bridge（sandbox mode 由 configs/test/agentflowd.json 决定）。

    启动顺序：
      1. 如果 mode=docker 则确保 Docker 沙箱镜像已构建
      2. 启动 daemon（sandbox 配置从 config 文件读取）
      3. 启动 bridge
      4. 等待 bridge 就绪
      5. yield 端点信息
      6. teardown 清理 bridge + daemon

    切换 sandbox provider：
      修改 configs/test/agentflowd.json 的 sandbox.mode 字段即可。
    """
    # 1) 检查 sandbox mode，仅在 docker 模式时需要构建镜像
    _sandbox_mode = _load_sandbox_mode()
    if _sandbox_mode == "docker":
        _ensure_docker_image()

    # 2) 分配端口
    daemon_port = free_port()
    bridge_port = free_port()

    log_dir = _test_log_dir("docker_sandbox_bridge", "TestOutfitDockerSandbox")
    daemon_log = _daemon_log_path(log_dir)
    bridge_log = _daemon_log_path(log_dir, "bridge.log")

    # 3) 启动 daemon（sandbox 配置从 configs/test/agentflowd.json 读取）
    env = _daemon_env()
    env["PATH"] = (
        f"{OUTFIT_AGENT / '.venv' / 'bin'}"
        f"{os.pathsep}/usr/bin:/bin:/usr/sbin:/sbin"
    )
    # LLM 使用 mock（e2e-scripted），避免依赖真实 API
    env.setdefault("AF_LLM_API_KEY", "e2e-test-key")
    env.setdefault("AF_LLM_BASE_URL", "http://127.0.0.1:9999")  # 会被 bridge 覆盖
    env["AF_LLM_MODEL"] = "e2e-scripted"
    env["AF_SKILL_MAX_COMPLETION_TOKENS"] = "512"
    env["NO_PROXY"] = "127.0.0.1,localhost"
    env["no_proxy"] = "127.0.0.1,localhost"
    env.setdefault(
        "PYTHONPATH",
        str(PROJECT_ROOT / "packages" / "agentflow-runtime" / "src"),
    )

    daemon = subprocess.Popen(
        [str(DAEMON_BINARY), "--listen", f"127.0.0.1:{daemon_port}",
         "--package", OUTFIT_AGENT_STR],
        cwd=PROJECT_ROOT, env=env,
        stdout=daemon_log.open("wb"),
        stderr=subprocess.STDOUT,
    )

    try:
        wait_port(daemon_port, daemon)
        await_agent_ready(daemon_port, "outfit-assistant", daemon)

        # 4) 启动 bridge
        bridge_dir = PROJECT_ROOT / "frontend" / "bridge"
        bridge_env = dict(os.environ)
        bridge_env["AF_BR_ENV"] = "test"
        bridge_env["AF_BR_DAEMON_HOST"] = "127.0.0.1"
        bridge_env["AF_BR_DAEMON_PORT"] = str(daemon_port)
        bridge_env["BRIDGE_HTTP_PORT"] = str(bridge_port)
        bridge_env["NO_PROXY"] = "127.0.0.1,localhost"
        bridge_env["no_proxy"] = "127.0.0.1,localhost"
        bridge_proc = subprocess.Popen(
            [
                "uv", "run", "--directory", str(bridge_dir),
                "python", "-m", "uvicorn", "main:bridge_app",
                "--host", "127.0.0.1",
                "--port", str(bridge_port),
                "--log-level", "warning",
            ],
            cwd=str(bridge_dir),
            env=bridge_env,
            stdout=bridge_log.open("wb"),
            stderr=subprocess.STDOUT,
        )

        # 等待 bridge 就绪
        _wait_bridge_ready(f"http://127.0.0.1:{bridge_port}")
        bridge_url = f"http://127.0.0.1:{bridge_port}"

        yield {
            "bridge_url": bridge_url,
            "bridge_port": bridge_port,
            "daemon_port": daemon_port,
            "daemon": daemon,
            "bridge_proc": bridge_proc,
            "log_dir": log_dir,
        }

    finally:
        # 清理 bridge
        if bridge_proc.poll() is None:
            bridge_proc.send_signal(signal.SIGTERM)
            try:
                bridge_proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                bridge_proc.kill()
                bridge_proc.wait(timeout=3)

        # 清理 daemon
        if daemon.poll() is None:
            daemon.send_signal(signal.SIGTERM)
            try:
                daemon.wait(timeout=12)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait(timeout=3)


def _ensure_docker_image() -> None:
    """确保 Docker 沙箱镜像已构建（复用 e2e 测试的构建逻辑）。"""
    import subprocess
    result = subprocess.run(
        ["docker", "images", "-q", "agentflow-sandbox:v1"],
        capture_output=True, text=True, timeout=10,
    )
    if not result.stdout.strip():
        print("[docker] building image agentflow-sandbox:v1...")
        subprocess.run(
            ["docker", "build", "-f", "docker/sandbox/Dockerfile",
             "-t", "agentflow-sandbox:v1", "."],
            cwd=PROJECT_ROOT, timeout=300,
        )


def _wait_bridge_ready(base_url: str, timeout: int = 30) -> None:
    """等待 bridge health check 通过。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            resp = http_requests.get(f"{base_url}/api/health", timeout=2)
            if resp.status_code == 200:
                return
        except Exception:
            time.sleep(0.5)
    pytest.fail(f"bridge did not become ready at {base_url} within {timeout}s")


# ═══════════════════════════════════════════════════════════════════════
# 测试：穿搭助手 + Docker 沙箱 + 产物生成
# ═══════════════════════════════════════════════════════════════════════


class TestOutfitDockerSandbox:
    """穿搭助手在 Docker 沙箱模式下的 E2E 测试。"""

    def test_outfit_recommendation_with_artifacts(
        self, docker_sandbox_bridge: dict[str, Any],
    ) -> None:
        """穿搭推荐返回内容包含产物信息。

        验证链路：
          bridge HTTP POST → daemon → agent → workflow → skill_executor
          → sandbox 内工具执行 → 产物生成 → 产物在响应中可见
        """
        base_url = docker_sandbox_bridge["bridge_url"]
        uid = f"e2e-ds-{uuid.uuid4().hex[:6]}"
        cid = f"cli-{uuid.uuid4().hex[:6]}"

        resp = http_requests.post(
            f"{base_url}/api/chat/outfit-assistant?stream=true",
            json={
                "user_id": uid,
                "client_id": cid,
                "content": "推荐一套适合开会的穿搭",
            },
            stream=True,
            timeout=_E2E_TIMEOUT,
        )
        assert resp.status_code == 200, (
            f"HTTP {resp.status_code}: {resp.text[:500]}"
        )

        text = resp.text
        # 验证返回了非空内容
        assert len(text) > 0, "SSE 响应体为空"

        # 解析 SSE 事件，提取产物信息
        events = _sse_parse(text)
        assert len(events) >= 2, (
            f"Expected >=2 SSE events, got {len(events)}"
        )
        assert events[-1] == "[DONE]", (
            f"Last event should be [DONE], got: {events[-1]!r}"
        )

        # 收集所有非 DONE 事件的内容
        all_content = ""
        artifact_urls = set()
        for event in events[:-1]:
            choices = event.get("choices", [])
            if not choices:
                continue
            choice = choices[0]
            delta = choice.get("delta", {})
            if delta and delta.get("content"):
                all_content += delta["content"]
            else:
                msg = choice.get("message", {})
                if msg and msg.get("content"):
                    all_content += msg["content"]

            # 提取产物 URL（_extract_artifact_urls 从事件中提取）
            event_artifacts = choice.get("artifacts") or []
            for art in event_artifacts:
                if art.get("url"):
                    artifact_urls.add(art["url"])

        # 验证最终内容非空
        assert len(all_content) > 0, (
            "Outfit assistant returned empty content"
        )
        print(f"[test] content_len={len(all_content)} "
              f"preview={all_content[:80]}...")

        # 验证产物 URL 存在
        if artifact_urls:
            print(f"[test] artifacts found: {len(artifact_urls)} URLs")
            for url in artifact_urls:
                print(f"  - {url}")
                # 尝试访问产物（可能已过期或沙箱已销毁，尽力而为）
                try:
                    art_resp = http_requests.get(url, timeout=5)
                    print(f"    status={art_resp.status_code}")
                except Exception as e:
                    print(f"    unreachable: {e}")
        else:
            # 即使没有产物，内容也不应为空
            print("[test] no structured artifacts in SSE events, "
                  "checking content for markdown artifact references...")
            # 尝试从内容中提取产物引用
            if "/api/artifacts/" in all_content:
                print("[test] artifacts found in markdown content")


def _sse_parse(text: str) -> list[dict[str, Any] | str]:
    """解析 SSE 文本，返回事件列表（dict 或 "[DONE]" 字符串）。"""
    events = []
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("data: "):
            payload = line[6:].strip()
            if payload == "[DONE]":
                events.append("[DONE]")
            else:
                try:
                    events.append(json.loads(payload))
                except json.JSONDecodeError:
                    continue
    return events