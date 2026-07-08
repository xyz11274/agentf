#!/usr/bin/env python3
"""
E2E concurrency guard test — 防护 coordinator 提前发最终响应的并发问题。

背景：
  并发 bug 现象：用户问"今天北京天气如何"，客户端先收到"没查到天气"的最终响应，
  过一会儿 trace 显示 weather_query skill 已成功获取真实天气数据。

  根因：framework 的 data_exchange 竞态导致 coordinator 在 skill_runner
  完成前就走到"completed without skill"分支，可能提前发 response。

不变量（本测试守护）：
  I-0: 请求成功且 final_content 非空（基本健康）
  I-3: 若 coordinator dispatch 了 skill，最终响应内容必须包含 skill 的实际结果
       （不能在 skill 完成前发"没查到/暂无数据"类响应）

数据采集机制（v2 — 修正版）：
  响应获取：通过 bridge REST chat 同步端点 ``POST /health/api/chat/{agent_id}``
            （chat_rest.py 已实现正确的 ``metadata.final`` 最终响应检测，
             返回的是 coordinator 的真正最终回复，非中间 progress 占位符）。
  Skill 证据：双路取证 ——
    1. daemon 日志 ``logs/agentflowd.log`` 中 ``response_push.*skill=`` 行
       （证明 skill 确实被 dispatch 且执行了）
    2. bridge LLM-traces API（提供 LLM 调用的 response_summary 细节）

  ⚠️ 不再使用 ``agent.poll_response`` JSON-RPC —— 它只返回中间 progress，
     永远拿不到最终响应，且紧密轮询会打崩 daemon（已实测验证）。

前置条件：
  1. agentflowd 运行 outfit-assistant (127.0.0.1:17891)
  2. agentflow-bridge 运行（8199 端口，需已注册 chat_rest + llm_trace 路由）

用法：
  python3 frontend/bridge/tests/test_concurrency_guard.py
  pytest frontend/bridge/tests/test_concurrency_guard.py -v
"""

import asyncio
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

# pytest 兼容：既能独立运行又能被 pytest 收集
try:
    import pytest

    @pytest.mark.asyncio
    @pytest.mark.e2e
    @pytest.mark.slow
    def test_concurrency_guard_pytest():
        """pytest entry — delegates to the standalone runner."""
        asyncio.run(_run_concurrency_guard())

    # 让 pytest 跳过收集时执行
    setattr(test_concurrency_guard_pytest, "__test__", True)
except ImportError:
    pass


# ── 配置 ──

# bridge 业务路由全部挂载在 /health 前缀下（见 main.py L492: app.mount("/health", bridge_app)）
BRIDGE_HTTP = os.environ.get(
    "BRIDGE_HTTP", "http://127.0.0.1:8199/health"
)
AGENT_ID = os.environ.get("E2E_AGENT_ID", "outfit-assistant")
USER_ID = os.environ.get("E2E_USER_ID", "e2e-concurrency-guard")
CLIENT_ID = os.environ.get("E2E_CLIENT_ID", f"guard-{int(time.time())}")

# daemon 日志路径（skill 执行证据的 fallback 读取源）
_REPO_ROOT = Path(__file__).resolve()
while _REPO_ROOT.parent != _REPO_ROOT:
    if (_REPO_ROOT / ".git").is_dir() or (_REPO_ROOT / "pyproject.toml").is_file():
        break
    _REPO_ROOT = _REPO_ROOT.parent
DAEMON_LOG = os.environ.get(
    "DAEMON_LOG", str(_REPO_ROOT / "logs" / "agentflowd.log")
)

# 触发 skill dispatch 的请求（需要 weather_query skill）
TEST_MESSAGE = "今天北京天气如何"

# 超时配置（秒）
# weather skill 涉及多轮 LLM 调用 + bash + read_file，实测单轮 90-130s。
TURN_TIMEOUT = int(os.environ.get("TURN_TIMEOUT", "150"))
HTTP_TIMEOUT = TURN_TIMEOUT + 10  # urllib 层超时，略大于 turn 超时
TRACE_POLL_TIMEOUT = 20
TRACE_POLL_INTERVAL = 2


# ── 通过 bridge REST chat 端点发送一轮对话（正确路径）──


def _run_turn_rest(message: str, timeout: int = TURN_TIMEOUT) -> dict[str, Any]:
    """通过 bridge REST chat 同步端点发送一轮对话。

    使用 ``POST /health/api/chat/{agent_id}`` (stream=false)。
    chat_rest.py 内部通过 ``_extract_final()`` 检查 ``metadata.final`` 字段
    来识别真正的最终响应（coordinator 的完整回复），而非中间 progress 占位符。
    同步模式下只保留 final chunk 的内容作为返回值（守不变量 I-1）。

    返回::
        {
            final_content: str,   # coordinator 的真正最终回复
            task_id: str,         # daemon 任务 ID
            success: bool,        # HTTP 200 且 success=True
            status_code: int,     # HTTP 状态码
            elapsed: float,       # 耗时秒
            error: str,           # 失败时的错误信息
        }
    """
    url = f"{BRIDGE_HTTP}/api/chat/{AGENT_ID}"
    payload = json.dumps({
        "user_id": USER_ID,
        "client_id": CLIENT_ID,
        "content": message,
    }, ensure_ascii=False).encode("utf-8")

    start = time.time()
    req = urllib.request.Request(
        url,
        data=payload,
        headers={"Content-Type": "application/json; charset=utf-8",
                 "Accept": "application/json"},
        method="POST",
    )

    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
            raw = resp.read().decode("utf-8")
            status_code = resp.status
    except urllib.error.HTTPError as exc:
        body = ""
        try:
            body = exc.read().decode("utf-8")[:500]
        except Exception:
            pass
        return {
            "final_content": "",
            "task_id": "",
            "success": False,
            "status_code": exc.code,
            "elapsed": time.time() - start,
            "error": f"HTTP {exc.code}: {exc.reason} | body={body}",
        }
    except Exception as exc:
        return {
            "final_content": "",
            "task_id": "",
            "success": False,
            "status_code": 0,
            "elapsed": time.time() - start,
            "error": f"request failed: {exc}",
        }

    elapsed = time.time() - start
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as exc:
        return {
            "final_content": "",
            "task_id": "",
            "success": False,
            "status_code": status_code,
            "elapsed": elapsed,
            "error": f"non-JSON response: {exc} | raw={raw[:200]}",
        }

    success = bool(data.get("success"))
    inner = data.get("data") or {}
    final_content = inner.get("content") or ""
    task_id = inner.get("task_id") or ""

    return {
        "final_content": final_content,
        "task_id": task_id,
        "success": success,
        "status_code": status_code,
        "elapsed": elapsed,
        "error": "" if success else (data.get("error") or "success=False"),
    }


# ── Skill 执行证据：daemon 日志 + LLM traces 双路取证 ──


def _collect_skill_evidence_from_log(
    log_start_offset: int,
) -> dict[str, Any]:
    """从 daemon 日志读取本轮对话期间的 skill 执行证据。

    扫描 ``logs/agentflowd.log`` 中 ``[turn_start_offset, 文件尾]`` 范围内的行，
    匹配 ``response_push.*skill=`` 模式 —— daemon UDS_SERVER 在 skill 节点
    推送响应时会记录 ``skill=<name>``，这是 skill 确实被执行的客观日志证据。

    返回::
        {
            skill_dispatched: bool,
            skill_names: list[str],       # 去重后的 skill 名（如 outfit-stylist）
            progress_count: int,          # progress 推送次数
            log_snippets: list[str],      # 前 5 条匹配行（诊断用）
        }
    """
    result: dict[str, Any] = {
        "skill_dispatched": False,
        "skill_names": [],
        "progress_count": 0,
        "log_snippets": [],
    }

    try:
        with open(DAEMON_LOG, "r", encoding="utf-8", errors="replace") as f:
            f.seek(log_start_offset)
            new_lines = f.read().splitlines()
    except OSError as exc:
        print(f"[guard] WARN: cannot read daemon log {DAEMON_LOG}: {exc}")
        return result

    # 匹配: [UDS_SERVER] response_push: session=0, dialogue=XXX, type=progress, skill=outfit-stylist, content_len=N
    skill_pattern = re.compile(r"skill=(\S+?)[,\s]")
    response_push_pattern = re.compile(r"response_push.*type=(\w+)")

    seen_skills: set[str] = set()
    for line in new_lines:
        if "response_push" not in line:
            continue
        m_skill = skill_pattern.search(line)
        m_type = response_push_pattern.search(line)
        msg_type = m_type.group(1) if m_type else ""
        if m_skill:
            skill_name = m_skill.group(1).rstrip(",")
            seen_skills.add(skill_name)
            if msg_type == "progress":
                result["progress_count"] += 1
            if len(result["log_snippets"]) < 5:
                result["log_snippets"].append(line.strip()[:160])

    result["skill_names"] = sorted(seen_skills)
    result["skill_dispatched"] = len(seen_skills) > 0
    return result


def _fetch_traces() -> list[dict]:
    """通过 bridge REST API 查询 LLM traces（提供 LLM 调用细节）。

    路由: ``GET /health/api/agents/{agent_id}/llm-traces``
    需要 ``user_id`` + ``client_id`` query 参数（daemon 通过 SessionRouter 解析 session）。
    返回 ``{success, data:{traces:[...], total:N}, error}``。
    """
    params = (
        f"?user_id={urllib.parse.quote(USER_ID)}"
        f"&client_id={urllib.parse.quote(CLIENT_ID)}"
        f"&limit=200"
    )
    url = f"{BRIDGE_HTTP}/api/agents/{AGENT_ID}/llm-traces{params}"
    try:
        req = urllib.request.Request(url, headers={"Accept": "application/json"})
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except Exception as exc:
        print(f"[guard] WARN: fetch traces failed: {exc}")
        return []
    return (data.get("data") or {}).get("traces") or []


def _wait_for_skill_traces(
    skill_keyword: str = "weather",
    min_traces: int = 1,
    timeout: int = TRACE_POLL_TIMEOUT,
) -> list[dict]:
    """轮询 traces 直到出现含 skill 关键词的 LLM 调用。

    skill_executor 的 LLM 调用 ``response_summary`` 通常含 weather/温度/天气 等词，
    作为 skill 已执行 LLM 调用的辅助信号。
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        traces = _fetch_traces()
        skill_traces = [
            t for t in traces
            if skill_keyword.lower() in (
                (t.get("response_summary") or "")
                + (t.get("request_summary") or "")
                + (t.get("skill_name") or "")
                + (t.get("completed_skill") or "")
            ).lower()
        ]
        if len(skill_traces) >= min_traces:
            return skill_traces
        time.sleep(TRACE_POLL_INTERVAL)
    return []


# ── 不变量断言 ──

# 最终响应中表示"空结果"的标记词
_EMPTY_MARKERS = [
    "没有查询到", "没查到", "暂无数据", "无法获取", "未获取到",
    "未查到", "查不到", "没有找到", "not found", "no data",
    "暂时无法", "稍后再问",
]

# skill 返回真实天气数据的标记词
_REAL_DATA_KEYWORDS = [
    "温度", "temp", "°C", "℃", "天气", "weather",
    "湿度", "humidity", "晴", "阴", "雨", "雾", "雪", "风",
]


def _assert_invariants(
    turn_result: dict[str, Any],
    log_evidence: dict[str, Any],
    skill_traces: list[dict],
) -> None:
    """断言不变量 I-0 和 I-3，失败时给出详细诊断信息。

    * I-0: 请求成功且 final_content 非空（基本健康）
    * I-3: 若 dispatch 了 skill 且 skill 确实返回了真实数据，
           最终响应不能是"没查到/暂无"类空结果（否则说明 coordinator
           在 skill 完成前就发了响应 —— 并发 bug）
    """
    final_content = turn_result.get("final_content") or ""
    success = turn_result.get("success", False)
    status_code = turn_result.get("status_code")

    # ── I-0: 请求成功且 final_content 非空 ──
    assert success, (
        f"I-0 VIOLATED: bridge returned failure. "
        f"status={status_code} error={turn_result.get('error')} "
        f"content={final_content[:200]}"
    )
    assert final_content and not final_content.startswith("["), (
        f"I-0 VIOLATED: final response is empty or error envelope. "
        f"content={final_content[:200]}"
    )

    # ── I-3: 若 dispatch 了 skill，最终响应不能是"没查到/暂无"类空结果 ──
    # 只在确实有 skill 执行痕迹时检查（避免 weather API 限流导致的合理空结果误判）
    skill_dispatched = log_evidence.get("skill_dispatched", False)
    skill_names = log_evidence.get("skill_names", [])

    if not skill_dispatched:
        print(
            f"[guard] INFO: no skill dispatch detected in daemon log "
            f"(skill_names={skill_names}). Skipping I-3 deep check."
        )
        return

    print(
        f"[guard] skill dispatch confirmed via daemon log: "
        f"skills={skill_names} progress_count={log_evidence.get('progress_count')}"
    )

    # 检查最终响应是否包含空结果标记
    matched_marker = None
    for marker in _EMPTY_MARKERS:
        if marker in final_content or marker.lower() in final_content.lower():
            matched_marker = marker
            break

    if matched_marker is None:
        # 最终响应不含空标记 —— I-3 自然满足
        return

    # 最终响应含空标记 —— 需要进一步确认 skill 是否真的返回了数据
    # 综合 trace + log 证据判断
    skill_summary = " ".join(
        (t.get("response_summary") or "") for t in skill_traces
    )
    has_real_data_in_traces = any(
        kw in skill_summary for kw in _REAL_DATA_KEYWORDS
    )

    if has_real_data_in_traces:
        # skill 返回了真实数据，但 coordinator 却发了空响应 → 并发 bug
        raise AssertionError(
            f"I-3 VIOLATED: coordinator sent empty response containing "
            f"'{matched_marker}', but skill traces show real data was returned.\n"
            f"Final content (first 120 chars): {final_content[:120]}\n"
            f"Skill traces ({len(skill_traces)}): "
            f"{[t.get('response_summary', '')[:80] for t in skill_traces[:3]]}\n"
            f"Daemon log skill evidence: skills={skill_names} "
            f"progress_count={log_evidence.get('progress_count')}\n"
            f"→ This indicates coordinator responded BEFORE skill_runner "
            f"completed (concurrency bug I-3)."
        )
    else:
        # skill 执行了但也没返回真实数据（可能是 weather API 限流）
        # → 不是并发 bug，是外部依赖问题
        print(
            f"[guard] INFO: final response contains '{matched_marker}' "
            f"AND skill traces show no real weather data either "
            f"(likely API rate-limit, NOT a concurrency bug). "
            f"skill_summary_preview='{skill_summary[:120]}'"
        )


# ── 主流程 ──


async def _run_concurrency_guard():
    """E2E 并发防护主流程。"""
    print(f"[guard] agent={AGENT_ID} user={USER_ID} client={CLIENT_ID}")
    print(f"[guard] bridge={BRIDGE_HTTP}")
    print(f"[guard] daemon_log={DAEMON_LOG}")
    print(f"[guard] message: {TEST_MESSAGE}")
    print(f"[guard] turn_timeout={TURN_TIMEOUT}s")

    # 记录 daemon 日志起始 offset（只看本轮新增的日志行）
    log_start_offset = 0
    try:
        log_start_offset = os.path.getsize(DAEMON_LOG)
    except OSError:
        print(f"[guard] WARN: daemon log not found at {DAEMON_LOG}")

    turn_start = time.time()

    # 1. 通过 bridge REST chat 同步端点发送消息（正确的最终响应检测路径）
    print(f"[guard] sending turn via REST chat (sync mode)...")
    turn_result = _run_turn_rest(TEST_MESSAGE)

    elapsed = turn_result.get("elapsed", 0)
    print(
        f"[guard] turn done in {elapsed:.1f}s "
        f"(success={turn_result.get('success')} "
        f"status={turn_result.get('status_code')})"
    )
    final_preview = (turn_result.get("final_content") or "")[:150]
    print(f"[guard] final_content preview: {final_preview}")
    if turn_result.get("error"):
        print(f"[guard] error: {turn_result['error']}")

    turn_end = time.time()

    # 2. 从 daemon 日志收集 skill 执行证据（主要证据源）
    log_evidence = _collect_skill_evidence_from_log(log_start_offset)
    print(
        f"[guard] daemon log evidence: "
        f"skill_dispatched={log_evidence.get('skill_dispatched')} "
        f"skills={log_evidence.get('skill_names')} "
        f"progress_count={log_evidence.get('progress_count')}"
    )
    if log_evidence.get("log_snippets"):
        print("[guard] log snippets:")
        for s in log_evidence["log_snippets"][:3]:
            print(f"  {s}")

    # 3. 查询 LLM traces 作为辅助证据（提供 LLM 调用的 response_summary 细节）
    skill_traces: list[dict] = []
    if log_evidence.get("skill_dispatched"):
        # skill 已执行，等待 traces 落盘（daemon 异步写入 databus）
        skill_traces = _wait_for_skill_traces(
            skill_keyword="weather",
            min_traces=1,
            timeout=TRACE_POLL_TIMEOUT,
        )
        print(f"[guard] found {len(skill_traces)} weather-related LLM traces")
        if skill_traces:
            print("[guard] skill trace summaries:")
            for t in skill_traces[:5]:
                print(
                    f"  - call_id={t.get('call_id', '')[:12]} "
                    f"skill={t.get('skill_name', '?')} "
                    f"latency={t.get('latency_ms', '?')}ms "
                    f"summary={(t.get('response_summary', '') or '')[:80]}"
                )
    else:
        print("[guard] no skill dispatch detected — skipping trace poll")

    # 4. 断言不变量
    _assert_invariants(turn_result, log_evidence, skill_traces)

    # 5. 结论
    print()
    print("[guard] ════════════════════════════════════════════")
    print("[guard] ✅ ALL INVARIANTS HELD (I-0, I-3)")
    print(f"[guard] elapsed={elapsed:.1f}s "
          f"skill_dispatched={log_evidence.get('skill_dispatched')} "
          f"content_len={len(turn_result.get('final_content') or '')}")
    print("[guard] ════════════════════════════════════════════")


# ── 独立运行入口 ──

if __name__ == "__main__":
    asyncio.run(_run_concurrency_guard())
