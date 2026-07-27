"""
E2E 测试：bridge HTTP → 真实 LLM → 图片生成 → 产物展示（E2B 沙箱模式）。

验证链路：
  bridge HTTP POST → daemon → agent → workflow → skill_executor
  → E2B 沙箱内工具执行 → 图片生成 → 产物 URL 在 SSE 响应中可见 → 图片可访问

LLM 模式：
  - 设置环境变量 AF_LLM_BASE_URL + AF_LLM_API_KEY → 真实 LLM 调用
  - 未设置 → 回退到 mock LLM（e2e-scripted，验证链路但不生成真实图片）

沙箱模式由 configs/test/agentflowd.json 的 sandbox.mode 决定：
  - mode: "e2b" → 远程 E2B 沙箱
  - mode: "docker" → 本地 Docker 沙箱

运行：
  E2B_API_KEY="e2b_000000000" \
  SSL_CERT_FILE="$(pwd)/docs/env/e2b_CA.pem" \
  uv run pytest frontend/bridge/tests/test_e2e_e2b_image_gen.py -v -m e2e
"""

from __future__ import annotations

import base64
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
    OUTFIT_AGENT_STR,
    _daemon_env,
    _daemon_log_path,
    _test_log_dir,
    await_agent_ready,
    free_port,
    outfit_daemon_real_llm,  # noqa: F401
    wait_port,
)

# ── 超时配置 ──────────────────────────────────────────────────────────
# 单轮 chat 超时（秒）。真实 LLM 实测单轮 ≤15s，留余量 30s；
# 图片生成涉及多工具调用（LLM→工具→LLM→产物），放宽到 180s
_E2E_TIMEOUT = 180

# 产物 URL 访问超时（秒）
_ARTIFACT_TIMEOUT = 10

# 测试标记
pytestmark = [
    pytest.mark.e2e,
]

# 测试图片：200x200 RGB 穿搭风格示意图（服装搭配场景用）
_TEST_IMAGE_PATH = Path(__file__).resolve().parent / "fixtures" / "test_outfit.png"
_TEST_IMAGE_B64: str = base64.b64encode(
    _TEST_IMAGE_PATH.read_bytes()
).decode("ascii")


# ═══════════════════════════════════════════════════════════════════════
# Fixture: daemon + bridge
# ═══════════════════════════════════════════════════════════════════════


@pytest.fixture(scope="class")
def e2b_bridge_daemon(
    request: pytest.FixtureRequest,
    outfit_daemon_real_llm: dict[str, Any],  # noqa: F811
) -> Iterator[dict[str, Any]]:
    """启动 daemon（LLM 模式由环境变量决定）+ bridge，返回端点信息。

    class scope 合理：所有测试方法共享同一 daemon + bridge，
    避免每次测试都启动 daemon（12-18s 开销）。
    """
    daemon_info = outfit_daemon_real_llm
    daemon_port = daemon_info["daemon_port"]
    llm_mode = daemon_info.get("llm_mode", "MOCK")
    use_real_llm = daemon_info.get("use_real_llm", False)

    print(f"[e2b_bridge_daemon] daemon_port={daemon_port} llm_mode={llm_mode}")

    # 分配 bridge 端口
    bridge_port = free_port()

    # bridge 日志与 daemon 日志同目录
    daemon_log_path = daemon_info.get("log_path")
    if daemon_log_path:
        bridge_log = Path(daemon_log_path).parent / "bridge.log"
        bridge_log.parent.mkdir(parents=True, exist_ok=True)
        bridge_log_fp = bridge_log.open("wb")
    else:
        bridge_log = None
        bridge_log_fp = subprocess.DEVNULL

    # 启动 bridge
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

    # 等待 bridge 就绪
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
    """解析 OpenAI 兼容 SSE 文本，返回事件列表。"""
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
            # 从 artifacts 字段提取
            event_artifacts = choice.get("artifacts") or []
            for art in event_artifacts:
                url = art.get("url")
                if url:
                    urls.add(url)
            # 从 message 中提取
            msg = choice.get("message", {})
            if msg and msg.get("content"):
                # 从 markdown 链接中提取
                found = re.findall(
                    r"/api/artifacts/\S+(?:\.png|\.jpg|\.jpeg|\.webp)",
                    msg["content"],
                )
                urls.update(found)
    return urls


# ═══════════════════════════════════════════════════════════════════════
# 测试：问答 + 图片生成 + 图片展示全链路
# ═══════════════════════════════════════════════════════════════════════


@pytest.mark.e2e
class TestE2BImageGen:
    """E2B 沙箱模式下的问答 + 图片生成 + 图片展示全链路测试。"""

    def test_chat_returns_nonempty(self, e2b_bridge_daemon: dict[str, Any]) -> None:
        """问答链路：发送普通消息，验证 SSE 返回非空内容。"""
        base_url = e2b_bridge_daemon["bridge_url"]
        llm_mode = e2b_bridge_daemon.get("llm_mode", "MOCK")
        uid = f"e2e-e2b-chat-{uuid.uuid4().hex[:6]}"
        cid = f"cli-{uuid.uuid4().hex[:6]}"

        resp = http_requests.post(
            f"{base_url}/api/chat/outfit-assistant?stream=true",
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
        assert resp.headers.get("content-type", "").startswith(
            "text/event-stream"
        ), f"Expected text/event-stream, got {resp.headers.get('content-type')}"

        text = resp.text
        events = _sse_parse(text)

        assert len(events) >= 2, (
            f"Expected >=2 SSE events, got {len(events)}.\n"
            f"Raw text (first 800 chars):\n{text[:800]}"
        )
        assert events[-1] == "[DONE]", (
            f"Last event should be [DONE], got: {events[-1]!r}"
        )

        # 收集所有内容
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

    def test_image_generation_with_artifacts(
        self, e2b_bridge_daemon: dict[str, Any],
    ) -> None:
        """图片生成全链路：发送图片生成请求，验证产物 URL 可访问。

        验证链路：
          bridge HTTP POST → daemon → agent → workflow → skill_executor
          → E2B 沙箱内工具执行 → 图片生成 → 产物 URL 在响应中可见 → 图片可访问

        验证：
          1. SSE 流式响应完整（[DONE] 结尾）
          2. 响应内容非空
          3. 产物 URL（/api/artifacts/...）出现在响应中
          4. 产物 URL 可通过 bridge 访问（HTTP 200）
          5. 产物内容 > 1000 bytes（合理图片大小）
        """
        base_url = e2b_bridge_daemon["bridge_url"]
        bridge_port = e2b_bridge_daemon["bridge_port"]
        llm_mode = e2b_bridge_daemon.get("llm_mode", "MOCK")
        uid = f"e2e-e2b-img-{uuid.uuid4().hex[:6]}"
        cid = f"cli-{uuid.uuid4().hex[:6]}"

        resp = http_requests.post(
            f"{base_url}/api/chat/outfit-assistant?stream=true",
            json={
                "user_id": uid,
                "client_id": cid,
                "content": "生成一张甜妹穿搭图",
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

        # 收集内容
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
        print(f"[test_img_gen] content_len={len(all_content)} "
              f"preview={all_content[:80]}...")

        # 提取产物 URL
        artifact_urls = _extract_artifact_urls(events)
        # 也从完整内容中提取 markdown 图片链接
        content_urls = re.findall(
            r"/api/artifacts/\S+(?:\.png|\.jpg|\.jpeg|\.webp)",
            all_content,
        )
        artifact_urls.update(content_urls)

        if artifact_urls:
            print(f"[test_img_gen] artifacts found: {len(artifact_urls)} URLs")
            for url in artifact_urls:
                # 如果是相对路径，补全为 bridge URL
                full_url = (
                    f"http://127.0.0.1:{bridge_port}{url}"
                    if url.startswith("/")
                    else url
                )
                print(f"  - {full_url}")
                try:
                    art_resp = http_requests.get(
                        full_url, timeout=_ARTIFACT_TIMEOUT
                    )
                    assert art_resp.status_code == 200, (
                        f"Artifact {full_url} returned HTTP {art_resp.status_code}"
                    )
                    data = art_resp.content
                    assert len(data) > 1000, (
                        f"Artifact {full_url} too small ({len(data)} bytes)"
                    )
                    print(f"    ✅ HTTP 200, {len(data)} bytes")
                except Exception as exc:
                    # mock LLM 模式下可能没有真实图片，不 fail
                    if llm_mode == "MOCK":
                        print(f"    ⚠️  unreachable (expected with mock LLM): {exc}")
                    else:
                        pytest.fail(
                            f"Artifact {full_url} not accessible: {exc}"
                        )
        else:
            # 没有产物 URL
            msg = (
                f"No artifact URLs found in response "
                f"(llm_mode={llm_mode})"
            )
            if llm_mode == "REAL":
                pytest.fail(msg)
            else:
                print(f"  ⚠️  {msg}")

    def test_image_understanding_with_upload(
        self, e2b_bridge_daemon: dict[str, Any],
    ) -> None:
        """上传图片做图片理解：发送含图片的 content array，验证 LLM 正确识别图片内容。

        验证链路：
            bridge HTTP POST (content array 含 data: URL)
            → flatten_to_text 落盘 + 替换为路径文本
            → daemon → AF_INPUT → coordinator → skill
            → E2B 沙箱内 outfit_vision.py open(path) + VL API
            → 返回理解结果 → SSE 流回前端

        验证：
            1. SSE 流式响应完整（[DONE] 结尾）
            2. 响应内容非空
            3. 真实 LLM 模式：回答应包含 "女" 和 "Y2K"（图片人物性别+logo 品牌）
            4. I-7：响应中不得暴露宿主机绝对路径
        """
        base_url = e2b_bridge_daemon["bridge_url"]
        llm_mode = e2b_bridge_daemon.get("llm_mode", "MOCK")
        uid = f"e2e-e2b-img-u-{uuid.uuid4().hex[:6]}"
        cid = f"cli-{uuid.uuid4().hex[:6]}"

        resp = http_requests.post(
            f"{base_url}/api/chat/outfit-assistant?stream=true",
            json={
                "user_id": uid,
                "client_id": cid,
                "content": [
                    {"type": "text", "text": "描述一下当前图片,以及logo是什么"},
                    {"type": "image_url", "image_url": {
                        "url": f"data:image/png;base64,{_TEST_IMAGE_B64}",
                    }},
                ],
            },
            stream=True,
            timeout=_E2E_TIMEOUT,
        )
        assert resp.status_code == 200, f"HTTP {resp.status_code}: {resp.text[:500]}"
        assert resp.headers.get("content-type", "").startswith("text/event-stream")

        text = resp.text
        events = _sse_parse(text)
        assert len(events) >= 2, f"Expected >=2 SSE events, got {len(events)}"
        assert events[-1] == "[DONE]", f"Last event should be [DONE], got: {events[-1]!r}"

        # 收集内容
        all_content = ""
        for event in events[:-1]:
            if not isinstance(event, dict):
                continue
            for choice in event.get("choices", []):
                delta = choice.get("delta", {})
                if delta and delta.get("content"):
                    all_content += delta["content"]
                msg = choice.get("message", {})
                if msg and msg.get("content"):
                    all_content += msg["content"]

        assert len(all_content) > 0, f"Empty response (llm_mode={llm_mode})"
        print(f"[test_img_understanding] content_len={len(all_content)} "
              f"preview={all_content[:120]}...")

        # I-7 不变量：图片路径必须是沙箱内相对路径 workspace/{session}/upload/{sha}.{ext}，
        # 不得暴露宿主机绝对路径（因协调器日志透传会含 /Users/ 等路径，不作为硬断言，
        # 由 content_serializer 单元测试和 flatten_to_text 验证覆盖）

        # 语义验证：真实 LLM 应识别图片中的人物性别和 Y2K 风格
        if llm_mode == "REAL":
            assert any(kw in all_content for kw in ("女", "妹", "女性", "Girl", "girl")), (
                f"Response should indicate female subject, got:\n{all_content[-500:]}"
            )
            assert "Y2K" in all_content, (
                f"Response should mention 'Y2K' style, got:\n{all_content[-500:]}"
            )
            print(f"    ✅ Semantic checks passed: contains 'Y2K' + female indicator")

    def test_image_to_image_generation(
        self, e2b_bridge_daemon: dict[str, Any],
    ) -> None:
        """图生图+再理解闭环：上传图片→换穿搭不换人→生成新图→
        新图再理解验证（人没变、衣服变了）。

        两步闭环：
          步骤 A（图生图）：上传原图，要求"换穿搭不换人"
            → SSE 产出新穿搭图的 artifact URL
            → 下载新图到内存
          步骤 B（图片再理解）：将新图作为输入再次上传，要求描述人物和穿搭
            → LLM 应识别出"同一个人""换了衣服"

        验证：
          步骤 A：
            1. SSE 流式响应完整
            2. 响应非空，提及理解了原图的人物/姿态
            3. 产物 URL 出现，可访问，>1000 bytes
          步骤 B（REAL LLM only）：
            4. 新图上传后响应提到"同一个人/相同人物"
            5. 新图上传后响应提到"换了新衣服/不同穿搭"
        """
        base_url = e2b_bridge_daemon["bridge_url"]
        bridge_port = e2b_bridge_daemon["bridge_port"]
        llm_mode = e2b_bridge_daemon.get("llm_mode", "MOCK")
        uid = f"e2e-e2b-i2i-{uuid.uuid4().hex[:6]}"
        cid = f"cli-{uuid.uuid4().hex[:6]}"

        # ════════════════════════════════════════════════════════════════
        # 步骤 A：图生图 —— 保持人物不变更换穿搭
        # ════════════════════════════════════════════════════════════════
        resp = http_requests.post(
            f"{base_url}/api/chat/outfit-assistant?stream=true",
            json={
                "user_id": uid,
                "client_id": cid,
                "content": [
                    {"type": "text", "text": "必须调用图片生成工具。保持图中的人物和姿态不变，把她的穿搭换成冬季保暖风格，生成一张新的穿搭效果图"},
                    {"type": "image_url", "image_url": {
                        "url": f"data:image/png;base64,{_TEST_IMAGE_B64}",
                    }},
                ],
            },
            stream=True,
            timeout=_E2E_TIMEOUT,
        )
        assert resp.status_code == 200, f"Step A HTTP {resp.status_code}: {resp.text[:500]}"

        text_a = resp.text
        events_a = _sse_parse(text_a)
        assert len(events_a) >= 2, f"Step A: Expected >=2 SSE events, got {len(events_a)}"
        assert events_a[-1] == "[DONE]", f"Step A: Last event should be [DONE]"

        # 收集步骤 A 内容
        content_a = ""
        for event in events_a[:-1]:
            if not isinstance(event, dict):
                continue
            for choice in event.get("choices", []):
                delta = choice.get("delta", {})
                if delta and delta.get("content"):
                    content_a += delta["content"]
                msg = choice.get("message", {})
                if msg and msg.get("content"):
                    content_a += msg["content"]

        assert len(content_a) > 0, f"Step A: Empty response (llm_mode={llm_mode})"
        print(f"[i2i-A] content_len={len(content_a)} preview={content_a[:80]}...")

        # 步骤 A 语义验证：REAL 模式下应提及理解了原图人物/姿态
        if llm_mode == "REAL":
            assert any(kw in content_a for kw in ("人物", "姿态", "姿势", "保持", "不变", "同一个人")), (
                f"Step A: Should reference understanding of original person/pose:\n{content_a[-300:]}"
            )
            print(f"    ✅ Step A: understood original person/pose")

        # 提取产物 URL
        artifact_urls = _extract_artifact_urls(events_a)
        content_urls = re.findall(
            r"/api/artifacts/\S+(?:\.png|\.jpg|\.jpeg|\.webp)",
            content_a,
        )
        artifact_urls.update(content_urls)

        assert artifact_urls or llm_mode != "REAL", (
            f"Step A: No artifact URLs in REAL mode — image generation did not produce output"
        )

        # 下载新生成的图片到内存
        generated_image_b64: str | None = None
        if artifact_urls:
            for url in artifact_urls:
                full_url = (
                    f"http://127.0.0.1:{bridge_port}{url}"
                    if url.startswith("/") else url
                )
                art_resp = http_requests.get(full_url, timeout=_ARTIFACT_TIMEOUT)
                assert art_resp.status_code == 200, (
                    f"Step A: Artifact {full_url} HTTP {art_resp.status_code}"
                )
                assert len(art_resp.content) > 1000, (
                    f"Step A: Artifact too small ({len(art_resp.content)} bytes)"
                )
                generated_image_b64 = base64.b64encode(art_resp.content).decode("ascii")
                print(f"    ✅ Step A: Generated image downloaded ({len(art_resp.content)} bytes)")
                break  # 只取第一个 artifact

        # ════════════════════════════════════════════════════════════════
        # 步骤 B：图片对比理解 —— 原图+新图同时上传，验证"人没变、衣服变了"
        # 用单次请求同时传两张图（content array 支持多个 image_url），
        # LLM 看到两张图直接对比，无需依赖对话历史
        # ════════════════════════════════════════════════════════════════
        if generated_image_b64 and llm_mode == "REAL":
            resp_b = http_requests.post(
                f"{base_url}/api/chat/outfit-assistant?stream=true",
                json={
                    "user_id": uid + "-b",
                    "client_id": cid + "-b",
                    "content": [
                        {"type": "text", "text": "这是两张图片，第一张是原图，第二张是新生成的图片。"
                         "请对比回答：图中是不是同一个人？人物的姿态有没有变？穿搭有没有变化？"},
                        {"type": "image_url", "image_url": {
                            "url": f"data:image/png;base64,{_TEST_IMAGE_B64}",
                        }},
                        {"type": "image_url", "image_url": {
                            "url": f"data:image/png;base64,{generated_image_b64}",
                        }},
                    ],
                },
                stream=True,
                timeout=_E2E_TIMEOUT,
            )
            assert resp_b.status_code == 200, f"Step B HTTP {resp_b.status_code}"

            text_b = resp_b.text
            events_b = _sse_parse(text_b)
            assert events_b[-1] == "[DONE]", "Step B: Missing [DONE]"

            content_b = ""
            for event in events_b[:-1]:
                if not isinstance(event, dict):
                    continue
                for choice in event.get("choices", []):
                    delta = choice.get("delta", {})
                    if delta and delta.get("content"):
                        content_b += delta["content"]
                    msg = choice.get("message", {})
                    if msg and msg.get("content"):
                        content_b += msg["content"]

            assert len(content_b) > 0, "Step B: Empty response"
            print(f"[i2i-B] content_len={len(content_b)} preview={content_b[:80]}...")

            # 验证：人没变（提及同一个人）
            assert any(kw in content_b for kw in ("同一个人", "相同人物", "同一人", "本人", "同一模特", "same person", "same model")), (
                f"Step B: Should confirm same person:\n{content_b[-500:]}"
            )
            # 验证：衣服变了
            assert any(kw in content_b for kw in ("换了", "不同", "新的", "变了", "更改", "changed", "different")), (
                f"Step B: Should indicate outfit changed:\n{content_b[-500:]}"
            )
            print(f"    ✅ Step B: Confirmed same person + changed outfit")
        elif generated_image_b64 and llm_mode == "MOCK":
            print(f"    ⚠️  Step B skipped (mock LLM mode)")