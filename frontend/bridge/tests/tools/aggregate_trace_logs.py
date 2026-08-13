#!/usr/bin/env python3
"""Aggregate agent execution traces from multi-source logs — 固化版日志抽取分析脚本。

背景: agent 执行日志多源多格式（daemon.log 的 [llm_call]/[flow]/[obs] 混合格式、
skill_stderr 的 [flow] tool_result 段、chunk 需要合并），人工 grep/解析极易
被截断、漏轮次。本脚本把抽取动作固化成可复用工具:

    输入（三选一）:
        python3 aggregate_trace_logs.py --run scene-uc1-20260812_125910
            # 从 logs/tests/<run>/ 自动发现 daemon.log + skill_stderr.*.log
        python3 aggregate_trace_logs.py --daemon <path> [--stderr <path>]
            # 显式指定日志文件
        python3 aggregate_trace_logs.py --latest
            # 最新 logs/tests/ 子目录

    输出:
        stdout 打印:
            1) 每轮 LLM 调用 trajectory（#N >>>/<<< latency finish tool_calls）
            2) 每轮 tool 调用（tool 名 + args 命令内容）
            3) 按轮次聚合的耗时分布（每轮 LLM 耗时合计 + tool 调用数）
            4) 总耗时与分段（USER_INPUT → skill_start → skill_end → workflow-completed）
        --out <path> 落盘完整 JSON（合并 chunk 后的所有事件）

    依赖:
        仅标准库。日志格式见 skill_stderr 的 [llm_call] WARN: [#N] >>>/<<< 段
        与 [flow] tool_result: <tool> => <result> 段、daemon.log 的
        [llm_call] latency=/finish= 段。
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from datetime import datetime
from pathlib import Path
from typing import Any, Optional

# tools(0) → tests(1) → bridge(2) → frontend(3) → 项目根(4)
PROJECT_ROOT = Path(__file__).resolve().parents[4]
LOGS_TESTS = PROJECT_ROOT / "logs/tests"


# ---------------------------------------------------------------------------
# 日志发现
# ---------------------------------------------------------------------------


def _discover_run(run: Optional[str]) -> tuple[Optional[Path], Optional[Path]]:
    """定位 daemon.log 与 skill_stderr.*.log。"""
    if run:
        run_dir = LOGS_TESTS / run
        daemon = run_dir / "daemon.log"
        stderrs = sorted(run_dir.glob("skill_stderr.*.log"))
        stderr = stderrs[0] if stderrs else None
        return (daemon if daemon.is_file() else None,
                stderr if stderr else None)
    # --latest
    if not LOGS_TESTS.is_dir():
        return None, None
    subdirs = sorted(LOGS_TESTS.iterdir(), key=lambda p: p.stat().st_mtime,
                     reverse=True)
    for d in subdirs:
        if not d.is_dir():
            continue
        daemon = d / "daemon.log"
        stderrs = sorted(d.glob("skill_stderr.*.log"))
        if daemon.is_file():
            return (daemon, stderrs[0] if stderrs else None)
    return None, None


# ---------------------------------------------------------------------------
# 事件抽取
# ---------------------------------------------------------------------------


def _extract_llm_calls(text: str) -> list[dict[str, Any]]:
    """抽 [llm_call] 段: [#N] >>>/<<<  lat=Xms finish=Y tool_calls=Z。

    兼容两种格式:
        [llm_call] WARN: [#4] >>> call_id=... node=skill_executor   （请求开始）
        [llm_call] WARN: [#4] <<< ... latency=5505ms finish=tool_calls （响应结束）
        [llm_call] #1 lat=5505ms fin=tool_calls                    （daemon.log 紧凑）
    """
    calls: dict[str, dict[str, Any]] = {}
    for line in text.splitlines():
        if "[llm_call]" not in line:
            continue
        m_num = re.search(r"#(\d+)", line)
        if not m_num:
            continue
        num = m_num.group(1)
        entry = calls.setdefault(num, {"num": num, "req": False, "resp": False,
                                       "latency_ms": None, "finish": None,
                                       "tool_calls": 0})
        m_dir = re.search(r"(>>>|<<<)", line)
        if m_dir:
            entry["req" if m_dir.group(1) == ">>>" else "resp"] = True
        m_lat = re.search(r"lat(?:ency)?=(\d+)ms", line)
        if m_lat:
            entry["latency_ms"] = int(m_lat.group(1))
        m_fin = re.search(r"(?:fin|finish)=([a-z_]+)", line)
        if m_fin:
            entry["finish"] = m_fin.group(1)
        m_tc = re.search(r"tool_calls?=(\d+)", line)
        if m_tc:
            entry["tool_calls"] = int(m_tc.group(1))
    return list(calls.values())


def _extract_tool_calls(text: str) -> list[dict[str, Any]]:
    """抽 tool 调用: [flow] tool_result: <tool> => <result> 与 [obs] JSON tool_call。"""
    tools: list[dict[str, Any]] = []
    for line in text.splitlines():
        if "[flow]" in line and ("tool_result" in line or "tool_call" in line):
            m_tool = re.search(r"tool_(?:result|call)[: ]+([a-z_]+)", line, re.I)
            m_res = re.search(r"=>\s*(.{1,200})", line)
            tools.append({
                "tool": m_tool.group(1) if m_tool else "?",
                "args": (m_res.group(1)[:200] if m_res else ""),
            })
        elif "[obs]" in line and "tool_call" in line:
            m_tool = re.search(r'"tool_name"[:\s]+"([a-z_]+)"', line, re.I) \
                or re.search(r'"tool"[:\s]+"([a-z_]+)"', line, re.I)
            m_cmd = re.search(r'"command"[:\s]+"([^"]{1,200})"', line, re.I)
            tools.append({
                "tool": m_tool.group(1) if m_tool else "?",
                "args": m_cmd.group(1)[:200] if m_cmd else "",
            })
    return tools


def _extract_key_timestamps(text: str) -> dict[str, Optional[str]]:
    """抽关键事件时间戳（USER_INPUT / HANDLE_INTENT / skill 起止 / workflow-completed）。"""
    markers = {
        "user_input": r"USER_INPUT",
        "handle_intent": r"HANDLE_INTENT",
        "build_task": r"BUILD_RUNTIME_TASK",
        "skill_start": r"skill_executor\.(?:start|llm_round_start)",
        "skill_end": r"skill_executor\.(?:end|max_rounds_reached|completed)",
        "workflow_completed": r"workflow-completed|WORKFLOW_COMPLETE",
    }
    result: dict[str, Optional[str]] = {}
    for key, pat in markers.items():
        m = re.search(r"\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\][^\n]*" + pat,
                      text)
        result[key] = m.group(1) if m else None
    return result


def _elapsed_s(t1: Optional[str], t2: Optional[str]) -> Optional[float]:
    if not t1 or not t2:
        return None
    try:
        return (datetime.fromisoformat(t2) - datetime.fromisoformat(t1)).total_seconds()
    except ValueError:
        return None


# ---------------------------------------------------------------------------
# 报告
# ---------------------------------------------------------------------------


def _build_report(daemon: Optional[Path], stderr: Optional[Path]) -> dict[str, Any]:
    report: dict[str, Any] = {"sources": {}}
    all_llm: list[dict[str, Any]] = []
    all_tools: list[dict[str, Any]] = []

    if daemon and daemon.is_file():
        text = daemon.read_text(errors="replace")
        llm = _extract_llm_calls(text)
        tools = _extract_tool_calls(text)
        ts = _extract_key_timestamps(text)
        report["sources"]["daemon"] = {"path": str(daemon), "llm_calls": llm,
                                        "tool_calls": tools, "timestamps": ts}
        all_llm.extend(llm)
        all_tools.extend(tools)

    if stderr and stderr.is_file():
        text = stderr.read_text(errors="replace")
        llm = _extract_llm_calls(text)
        tools = _extract_tool_calls(text)
        report["sources"]["skill_stderr"] = {"path": str(stderr),
                                              "llm_calls": llm,
                                              "tool_calls": tools}
        all_llm.extend(llm)
        all_tools.extend(tools)

    # 合并 LLM 调用（按 #N 去重，跨源合并）
    merged: dict[str, dict[str, Any]] = {}
    for c in all_llm:
        num = c["num"]
        if num not in merged:
            merged[num] = dict(c)
        else:
            for k, v in c.items():
                if v and not merged[num].get(k):
                    merged[num][k] = v
    report["llm_calls"] = sorted(merged.values(),
                                 key=lambda x: int(x["num"]) if x["num"].isdigit() else 0)

    report["tool_calls"] = all_tools

    # 聚合统计
    report["summary"] = {
        "llm_call_count": len(report["llm_calls"]),
        "tool_call_count": len(all_tools),
        "total_llm_latency_ms": sum(c["latency_ms"] or 0 for c in report["llm_calls"]),
        "tool_counter": Counter(t["tool"] for t in all_tools),
        "finish_reason_counter": Counter(c["finish"] or "?" for c in report["llm_calls"]),
    }

    # 分段耗时
    ts = report["sources"].get("daemon", {}).get("timestamps", {})
    segs = {}
    if ts.get("user_input") and ts.get("skill_start"):
        segs["入口→skill_start"] = _elapsed_s(ts["user_input"], ts["skill_start"])
    if ts.get("skill_start") and ts.get("skill_end"):
        segs["skill 循环"] = _elapsed_s(ts["skill_start"], ts["skill_end"])
    if ts.get("skill_end") and ts.get("workflow_completed"):
        segs["skill_end→完成"] = _elapsed_s(ts["skill_end"], ts["workflow_completed"])
    if ts.get("user_input") and ts.get("workflow_completed"):
        segs["总耗时"] = _elapsed_s(ts["user_input"], ts["workflow_completed"])
    report["segments"] = segs

    return report


# ---------------------------------------------------------------------------
# 主入口
# ---------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description="聚合 agent 执行日志，输出每轮 LLM/tool trajectory")
    ap.add_argument("--run", default=None, help="logs/tests/<run>/ 子目录名")
    ap.add_argument("--daemon", default=None, help="daemon.log 路径")
    ap.add_argument("--stderr", default=None, help="skill_stderr.*.log 路径")
    ap.add_argument("--latest", action="store_true", help="用最新 logs/tests/ 子目录")
    ap.add_argument("--out", default=None, help="落盘完整 JSON 的路径（默认不落盘）")
    args = ap.parse_args()

    daemon, stderr = args.daemon, args.stderr
    if not daemon and not stderr:
        if args.run or args.latest:
            daemon, stderr = _discover_run(args.run)
        if not daemon and not stderr:
            ap.error("未找到日志：用 --run <subdir> / --latest / 显式 --daemon/--stderr")
    daemon_p = Path(daemon) if daemon else None
    stderr_p = Path(stderr) if stderr else None

    report = _build_report(daemon_p, stderr_p)

    # 打印报告
    print(f"=== 日志聚合报告 ===")
    for src, info in report["sources"].items():
        print(f"[{src}] {info.get('path', '?')}")
        print(f"   LLM 调用 {len(info.get('llm_calls', []))} 条, "
              f"tool 调用 {len(info.get('tool_calls', []))} 条")
    print()
    print(f"=== 每轮 LLM 调用 ===")
    for c in report["llm_calls"]:
        lat = c.get("latency_ms")
        lat_s = f"{lat}ms" if lat else "-"
        print(f"LLM#{c['num']}: lat={lat_s} fin={c.get('finish') or '?'} "
              f"tc={c.get('tool_calls', 0)}")
    print()
    print(f"=== tool 调用（{report['summary']['tool_call_count']} 次）===")
    for t in report["tool_calls"][:40]:
        args_s = t.get("args", "")
        print(f"  tool={t['tool']} args={args_s[:120]}")
    if len(report["tool_calls"]) > 40:
        print(f"  ... 另有 {len(report['tool_calls']) - 40} 次 tool 调用（--out 落盘看全）")
    print()
    print(f"=== 聚合统计 ===")
    print(f"  LLM 调用: {report['summary']['llm_call_count']} 条, "
          f"合计 LLM 耗时: {report['summary']['total_llm_latency_ms']}ms")
    print(f"  finish_reason 分布: {dict(report['summary']['finish_reason_counter'])}")
    print(f"  tool 分布: {dict(report['summary']['tool_counter'])}")
    print()
    print(f"=== 分段耗时（秒）===")
    for seg, s in report["segments"].items():
        print(f"  {seg}: {s:.1f}s" if s is not None else f"  {seg}: 未知")

    if args.out:
        Path(args.out).write_text(json.dumps(report, ensure_ascii=False, indent=2))
        print(f"\n[out] 完整报告已落盘: {args.out}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
