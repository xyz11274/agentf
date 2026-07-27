"""Minimal agent × Bridge REST — 真实 LLM 偶发慢下 daemon→bridge 行为验证.

场景动机
---------
``test_e2e_rest_chat_session.py::test_multi_turn_context_maintained`` 在 outfit-assistant
（复杂 skill 调度路径）下偶发 HTTP 500：bridge 触发 turn_timeout，daemon 端 chunks>0、
connected=True 但从未推 final=true。怀疑真实 LLM 偶发慢，但 outfit-assistant 链路太复杂
（coordinator → skill 多轮 tool_calls → coordinator 再触发），无法 isolate 是 LLM 慢
还是 framework 路由问题。

本文件用 ``fixtures/agent-connectivity``（单 coordinator 节点直连 LLM，不调度任何 skill）
经 bridge REST 发一次 chat，最小化地复现 "daemon 在跑、bridge 等不到 final" 的现场。
agent-connectivity 的 coordinator 只调一次 LLM、无 skill 叠加，耗时 = 单次 LLM 往返，
    - 若此场景下 bridge 也会触发 turn_timeout → 根因是 framework 路由 / timeout 配置
    - 若此场景下 bridge 永远正常返回 → 根因是 outfit-assistant 的 skill 多轮叠加慢，
      不在 framework 层，应回到 outfit-assistant 侧优化

运行:
    python -m pytest frontend/bridge/tests/test_e2e_minimal_agent_bridge.py -v -m e2e

LLM 模式:
    - 设置 AF_LLM_BASE_URL + AF_LLM_API_KEY -> 真实 LLM 调用
    - 未设置 -> 回退到 mock LLM (e2e-scripted，永远不会慢，本文件此时仅验证链路连通)
"""

from __future__ import annotations

import os
import subprocess
import time
import uuid
from pathlib import Path
from typing import Any, Iterator

import pytest
import requests as http_requests

from .e2e_helpers import (
    MINIMAL_AGENT_ID,
    PROJECT_ROOT,
    free_port,
    minimal_daemon_real_llm,
)


@pytest.fixture(scope="class")
def minimal_bridge_daemon(
    request: pytest.FixtureRequest,
    minimal_daemon_real_llm: dict[str, Any],
) -> Iterator[dict[str, Any]]:
    """启动本地 bridge 连接 agent-connectivity daemon（class-scoped）.

    与 ``test_e2e_rest_chat_session.py::bridge_daemon`` 的区别：
      - daemon fixture 用 ``minimal_daemon_real_llm``（agent-connectivity，单 coordinator 节点）
      - agent_id = ``agent-connectivity``，bridge REST 路由到 /api/chat/agent-connectivity

    用于 isolate "真实 LLM 偶发慢下 daemon→bridge 行为"，不引入 outfit-assistant 的
    skill 调度复杂度。
    """
    daemon_info = minimal_daemon_real_llm
    daemon_port = daemon_info["daemon_port"]
    llm_mode = daemon_info.get("llm_mode", "MOCK")

    print(f"[minimal_bridge] daemon_port={daemon_port} llm_mode={llm_mode}")

    port = free_port()

    # bridge 日志与 daemon 日志同目录（logs/tests/<ts>_<cls>_minimal_daemon_real_llm/）
    daemon_log_path = daemon_info.get("log_path")
    if daemon_log_path:
        bridge_log = Path(daemon_log_path).parent / "bridge.log"
        bridge_log.parent.mkdir(parents=True, exist_ok=True)
        bridge_log_fp = bridge_log.open("wb")
    else:
        bridge_log = None
        bridge_log_fp = subprocess.DEVNULL

    bridge_dir = PROJECT_ROOT / "frontend" / "bridge"
    env = os.environ.copy()
    # 确保 Bridge 加载 test 环境配置（configs/test/bridge.yaml）
    env["AF_BR_ENV"] = "test"
    env["AF_BR_DAEMON_HOST"] = "127.0.0.1"
    env["AF_BR_DAEMON_PORT"] = str(daemon_port)
    env["BRIDGE_HTTP_PORT"] = str(port)
    env["NO_PROXY"] = "127.0.0.1,localhost"
    env["no_proxy"] = "127.0.0.1,localhost"

    proc = subprocess.Popen(
        ["uv", "run", "--directory", str(bridge_dir),
         "python", "-m", "uvicorn", "main:bridge_app", "--host", "127.0.0.1",
         "--port", str(port), "--log-level", "warning"],
        cwd=str(bridge_dir),
        env=env,
        stdout=bridge_log_fp,
        stderr=subprocess.STDOUT,
    )

    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        try:
            r = http_requests.get(f"http://127.0.0.1:{port}/api/health", timeout=1)
            if r.status_code == 200:
                break
        except Exception:
            pass
        time.sleep(0.3)
    else:
        proc.terminate()
        proc.wait()
        pytest.fail("bridge failed to start")

    try:
        yield {
            "bridge_url": f"http://127.0.0.1:{port}",
            "bridge_port": port,
            "daemon_port": daemon_port,
            "process": proc,
            "llm_mode": llm_mode,
            "bridge_log_path": str(bridge_log) if bridge_log else None,
        }
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


@pytest.mark.e2e
class TestMinimalAgentBridgeRest:
    """minimal agent × bridge REST — 真实 LLM 偶发慢行为 isolate.

    真实 LLM 调用次数 = 1（单轮单 coordinator，无 skill 叠加）。
    """

    def test_single_chat_returns_200(
        self, minimal_bridge_daemon: dict[str, Any]
    ) -> None:
        """单轮 chat 经 bridge REST，断言返回 200 + 非空 content.

        这是 isolate 测试：minimal agent 的 coordinator 只调一次 LLM、
        无 skill 多轮 tool_calls 叠加。若此场景下偶发 500/timeout → 根因在
        framework 路由或 timeout 配置；若永远 200 → 根因在 outfit-assistant
        skill 叠加慢，不在 framework 层。
        """
        base_url = minimal_bridge_daemon["bridge_url"]
        uid = f"e2e-minbr-{uuid.uuid4().hex[:6]}"
        cid = f"cli-{uuid.uuid4().hex[:6]}"

        # 客户端 timeout 留足余量：bridge turn_timeout=300s（configs/test/bridge.yaml），
        # minimal agent 单轮 LLM 实测远小于此；给 305s 与 outfit-assistant 同标准，
        # 避免客户端先于服务端超时引入假信号。
        resp = http_requests.post(
            f"{base_url}/api/chat/{MINIMAL_AGENT_ID}",
            json={
                "user_id": uid,
                "client_id": cid,
                "content": "你好，请介绍一下你自己",
            },
            timeout=305,
        )
        assert resp.status_code == 200, (
            f"HTTP {resp.status_code}: {resp.text[:500]}"
        )
        data = resp.json()
        assert data.get("success") is True, f"success=False: {data}"
        content = data.get("data", {}).get("content", "")
        assert len(content) > 0, (
            f"empty content (llm_mode={minimal_bridge_daemon.get('llm_mode')})"
        )
        print(f"[minimal_bridge] single chat: {len(content)} chars — "
              f"\"{content[:60]}...\"")
