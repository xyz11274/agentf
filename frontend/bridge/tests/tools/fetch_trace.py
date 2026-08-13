#!/usr/bin/env python3
"""Fetch LLM execution traces from the bridge — 固化版模型调用 trajectory 拉取脚本。

用法（三选一）:
    python3 fetch_trace.py --dialogue 0812032940-ce2baaf2ce48
    python3 fetch_trace.py --user uc1-user-xxx --client client_xxx
    python3 fetch_trace.py --latest          # 最近一次评测（读 runs/ 最新 raw-results.json）

输出:
    stdout 打印每轮 LLM 调用的紧凑 trajectory（latency/finish_reason/token/tool_calls）
    --out <path> 落盘完整 JSON（列表 + 每个 call 的 request/response/meta）

依赖:
    仅标准库（urllib）——与 e2e_perf_baseline.py 一致，无第三方依赖。
    bridge 默认 http://localhost:8199（configs/dev/bridge.yaml listen）。

数据链路（ADR-0004）:
    bridge GET /api/agents/{agent_id}/llm-traces
        → daemon databus.list_keys/databus.get（查 DataBus llm/traces/ 命名空间）
        → 按 dialogue_id 过滤 → 返回 trace 摘要列表（含 call_id/latency/finish/token）
    bridge GET /api/agents/{agent_id}/llm-traces/{call_id}
        → daemon databus.get × 3 → 完整 request/response/meta

注意事项:
    - DataBus 是内存态存储——daemon 重启后 trace 丢失，须在评测后、daemon 关闭前拉取
    - 仅覆盖 LLM 调用（_TraceRecorder 记录）；tool 命令内容不在 trace 里，
      需配合 skill_stderr 日志抽取（见 trace-log-aggregation 项目技能）
"""

from __future__ import annotations

import argparse
import json
import sys
import urllib.request
from pathlib import Path
from typing import Any, Optional

BRIDGE_BASE = "http://localhost:8199"
AGENT_ID = "outfit-assistant"
# tools(0) → tests(1) → bridge(2) → frontend(3) → 项目根(4)
PROJECT_ROOT = Path(__file__).resolve().parents[4]


def _http_get(url: str, timeout: int = 30) -> dict[str, Any]:
    """GET 并解析 JSON 响应（标准库 urllib）。"""
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def list_traces(base: str, agent_id: str, user_id: str, client_id: str,
                dialogue_id: Optional[str] = None,
                limit: int = 200) -> list[dict[str, Any]]:
    """拉 trace 摘要列表（可按 dialogue_id 过滤）。"""
    url = (f"{base}/api/agents/{agent_id}/llm-traces"
           f"?user_id={user_id}&client_id={client_id}"
           f"&limit={limit}")
    if dialogue_id:
        url += f"&dialogue_id={dialogue_id}"
    result = _http_get(url)
    if not result.get("success"):
        raise RuntimeError(f"trace list 失败: {result.get('error')}")
    return result["data"]["traces"]


def get_trace_detail(base: str, agent_id: str, user_id: str, client_id: str,
                     call_id: str) -> dict[str, Any]:
    """拉单个 call 的完整 request/response/meta。"""
    url = (f"{base}/api/agents/{agent_id}/llm-traces/{call_id}"
           f"?user_id={user_id}&client_id={client_id}")
    result = _http_get(url)
    if not result.get("success"):
        raise RuntimeError(f"trace detail {call_id} 失败: {result.get('error')}")
    return result["data"]


def _compact_trajectory(trace: dict[str, Any]) -> str:
    """把一条 trace 压成紧凑单行（用于 stdout trajectory 打印）。"""
    meta = trace.get("meta") or {}
    request = trace.get("request") or {}
    response = trace.get("response") or {}
    latency = meta.get("latency_ms", trace.get("latency_ms", 0))
    fin = (response.get("finish_reason")
           or meta.get("finish_reason", "")
           or trace.get("finish_reason", ""))
    # tool_calls 数量：从 request 的 tools 或 response 的 tool_calls 推断
    tool_calls = response.get("tool_calls") or []
    tc_count = len(tool_calls) if isinstance(tool_calls, list) else 0
    usage = meta.get("token_usage", {}) or {}
    req_summary = meta.get("request_summary", "") or ""
    resp_summary = meta.get("response_summary", "") or ""
    parts = [
        f"call={trace.get('call_id', '?')[:12]}",
        f"lat={latency}ms",
        f"fin={fin or '?'}",
        f"tc={tc_count}",
        f"tokens={usage.get('total_tokens', '?')}",
    ]
    if req_summary:
        parts.append(f"req={req_summary[:60]}")
    if resp_summary:
        parts.append(f"resp={resp_summary[:60]}")
    return " | ".join(parts)


def _latest_run_params() -> tuple[str, str, str]:
    """从 .agent-improver/runs/ 最新 raw-results.json 读 user/client（供 --latest）。

    raw-results.json 本身不写 user_id/client_id（scene 脚本用动态
    `uc1-user-{ts}` 值），故兜底从 log_path 指向的 daemon.log
    BUILD_RUNTIME_TASK 段 grep `Set user_id=` / `Set client_id=`；
    日志已被清理时给出明确报错提示显式传 --user/--client。
    """
    runs_dir = PROJECT_ROOT / ".agents/.coding_gumdam/workspace/.agent-improver/runs"
    raw_files = sorted(runs_dir.glob("scene-uc*/raw-results.json"),
                       key=lambda p: p.stat().st_mtime, reverse=True)
    if not raw_files:
        raise SystemExit(f"未找到 raw-results.json（{runs_dir}）")
    r = json.loads(raw_files[0].read_text())
    user = r.get("user_id") or r.get("user") or ""
    client = r.get("client_id") or r.get("client") or ""
    if not (user and client):
        # 从 log_path 的 daemon.log BUILD_RUNTIME_TASK 段兜底抽
        log_path = r.get("log_path", "")
        if log_path and Path(log_path).is_file():
            log_text = Path(log_path).read_text(errors="replace")
            import re
            m_u = re.search(r"Set user_id=(\S+)", log_text)
            m_c = re.search(r"Set client_id=(\S+)", log_text)
            if m_u and m_c:
                user, client = m_u.group(1), m_c.group(1)
        if not (user and client):
            raise SystemExit(
                "raw-results.json 与 log_path 均无法解析 user_id/client_id"
                f"（{raw_files[0]}）。scene 脚本用动态值且未写入 raw-results，"
                "且 logs/tests 可能已被清理。请显式传 --user + --client。"
            )
    return user, client, raw_files[0]


def main() -> int:
    ap = argparse.ArgumentParser(description="从 bridge 拉取 LLM trace（模型调用 trajectory）")
    ap.add_argument("--base", default=BRIDGE_BASE, help=f"bridge base url（默认 {BRIDGE_BASE}）")
    ap.add_argument("--agent", default=AGENT_ID, help=f"agent_id（默认 {AGENT_ID}）")
    ap.add_argument("--user", default=None, help="user_id")
    ap.add_argument("--client", default=None, help="client_id")
    ap.add_argument("--dialogue", default=None, help="dialogue_id（过滤）")
    ap.add_argument("--latest", action="store_true", help="用最新评测 raw-results.json 的 user/client")
    ap.add_argument("--out", default=None, help="落盘完整 JSON 的路径（默认不落盘）")
    args = ap.parse_args()

    user, client = args.user, args.client
    if args.latest:
        user, client, src = _latest_run_params()
        print(f"[latest] 从 {src} 读 user={user} client={client}")
    if not user or not client:
        ap.error("需要 --user + --client（或用 --latest 自动读最新评测参数）")

    # 拉列表
    traces = list_traces(args.base, args.agent, user, client, args.dialogue)
    print(f"[trace] 共 {len(traces)} 条 LLM 调用（agent={args.agent} user={user} client={client[:12]}… dialogue={args.dialogue or 'all'}）")
    print()

    # 拉每个 call 的完整详情
    details: list[dict[str, Any]] = []
    for t in traces:
        try:
            d = get_trace_detail(args.base, args.agent, user, client, t["call_id"])
            details.append(d)
            print(_compact_trajectory(d))
        except Exception as exc:  # 单条失败不中断
            print(f"call={t.get('call_id', '?')[:12]} <拉取失败: {exc}>")
            details.append({"call_id": t.get("call_id"), "error": str(exc)})

    # 落盘完整 JSON
    if args.out:
        out_path = Path(args.out)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "agent_id": args.agent, "user_id": user, "client_id": client,
            "dialogue_id": args.dialogue, "traces": details,
        }
        out_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2))
        print(f"\n[out] 完整 trace 已落盘: {out_path}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
