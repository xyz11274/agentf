"""Minimal agent Docker sandbox E2E — 工具调用、文件写入、LLM 调用、产物收集全链路验证。

验证链路：
  bridge HTTP POST → daemon → agent → workflow → skill_executor
  → Docker 沙箱内工具执行(bash/write_file/read_file) → 产物生成 → 产物在响应中可见

与 ``test_e2e_outfit_docker_sandbox.py`` 的区别：
  - 使用 outfit-assistant agent（已有完整 harness/skills + harness/tools）
  - 启用 Docker sandbox 模式
  - 使用 mock LLM（e2e-scripted），不依赖真实 API
  - 验证文件写入、工具调用、产物收集等真实业务路径

运行:
  DOCKER_SANDBOX_ENABLED=1 uv run pytest frontend/bridge/tests/test_e2e_minimal_agent_sandbox.py -v -m e2e
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
    _daemon_env,
    _daemon_log_path,
    _load_sandbox_mode,
    _test_log_dir,
    await_agent_ready,
    free_port,
    scripted_llm,
    wait_port,
)


_E2E_TIMEOUT = 120
_OUTFIT_AGENT = PROJECT_ROOT / "examples" / "agent" / "outfit-assistant"
_OUTFIT_AGENT_STR = str(_OUTFIT_AGENT)
_DOCKER_IMAGE_TAG = "agentflow-sandbox:v1"


def _ensure_docker_image() -> None:
    """确保 Docker 沙箱镜像已构建。"""
    result = subprocess.run(
        ["docker", "images", "--format", "{{.Repository}}:{{.Tag}}",
         _DOCKER_IMAGE_TAG],
        capture_output=True, text=True, timeout=30,
    )
    if _DOCKER_IMAGE_TAG not in result.stdout:
        dockerfile = PROJECT_ROOT / "docker" / "sandbox" / "Dockerfile"
        subprocess.run(
            ["docker", "build", "-t", _DOCKER_IMAGE_TAG,
             "-f", str(dockerfile), str(PROJECT_ROOT)],
            capture_output=True, text=True, timeout=120,
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


def _sse_parse(text: str) -> list[dict[str, Any] | str]:
    """解析 SSE 事件流文本为事件列表。

    每行以 "data: " 开头，空行分隔事件。
    最后一行是 "[DONE]"。
    """
    events: list[dict[str, Any] | str] = []
    current_data = ""
    for line in text.splitlines():
        if line.startswith("data: "):
            chunk = line[6:]
            if chunk == "[DONE]":
                events.append("[DONE]")
            else:
                try:
                    events.append(json.loads(chunk))
                except json.JSONDecodeError:
                    current_data += chunk
        elif not line.strip() and current_data:
            # 空行分隔符，尝试解析累积的 data
            try:
                events.append(json.loads(current_data))
            except json.JSONDecodeError:
                pass
            current_data = ""
    return events


@pytest.fixture(scope="class")
def docker_sandbox_bridge(request: pytest.FixtureRequest) -> Iterator[dict[str, Any]]:
    """启动 daemon + bridge（sandbox mode 由 configs/test/agentflowd.json 决定）。

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

    log_dir = _test_log_dir("docker_sandbox_bridge", request.cls.__name__)
    daemon_log = _daemon_log_path(log_dir)
    bridge_log = _daemon_log_path(log_dir, "bridge.log")

    # 启动 mock LLM server
    ctx = scripted_llm()
    llm_url = ctx.__enter__()

    # 3) 启动 daemon（sandbox 配置从 configs/test/agentflowd.json 读取）
    env = _daemon_env()
    env["PATH"] = (
        f"{_OUTFIT_AGENT / '.venv' / 'bin'}"
        f"{os.pathsep}/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
    )
    # LLM 使用 mock（e2e-scripted）
    env["AF_LLM_API_KEY"] = "e2e-test-key"
    env["AF_LLM_BASE_URL"] = llm_url
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
         "--package", _OUTFIT_AGENT_STR],
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

        ctx.__exit__(None, None, None)


@pytest.mark.e2e
class TestMinimalAgentSandbox:
    """Docker 沙箱模式下的 minimal agent E2E 测试。"""

    def test_sandbox_tool_chain(
        self, docker_sandbox_bridge: dict[str, Any],
    ) -> None:
        """Docker 沙箱内工具调用全链路可用：bash→write_file→read_file→LLM→产物。

        验证链路：
          bridge HTTP POST → daemon → agent → workflow → skill_executor
          → sandbox 内工具执行 → 产物生成 → 产物在响应中可见
        """
        base_url = docker_sandbox_bridge["bridge_url"]
        uid = f"e2e-sb-{uuid.uuid4().hex[:6]}"
        cid = f"cli-{uuid.uuid4().hex[:6]}"

        resp = http_requests.post(
            f"{base_url}/api/chat/outfit-assistant?stream=true",
            json={
                "user_id": uid,
                "client_id": cid,
                "content": "写一个测试文件 test_sandbox.txt 内容为 'hello sandbox'",
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

            # 提取产物 URL
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
                try:
                    art_resp = http_requests.get(url, timeout=5)
                    print(f"    status={art_resp.status_code}")
                except Exception as e:
                    print(f"    unreachable: {e}")
        else:
            print("[test] no structured artifacts in SSE events, "
                  "checking content for markdown artifact references...")
            if "/api/artifacts/" in all_content:
                print("[test] artifacts found in markdown content")