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
    """抽 [llm_call] 段: [#N] >>>/<<<  lat=Xms finish=Y tool_calls=Z + token/msgs。

    兼容两种格式:
        [llm_call] WARN: [#4] >>> call_id=... node=skill_executor msgs=6 （请求开始）
        [llm_call] WARN: [#4] <<< ... latency=5505ms finish=tool_calls
            tok=prompt:0/completion:262/total:262   （响应结束）
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
                                       "tool_calls": 0, "call_id": None,
                                       "msgs": None, "prompt_tokens": None,
                                       "completion_tokens": None})
        m_dir = re.search(r"(>>>|<<<)", line)
        if m_dir:
            entry["req" if m_dir.group(1) == ">>>" else "resp"] = True
        m_cid = re.search(r"call_id=([0-9a-f]+)", line)
        if m_cid:
            entry["call_id"] = m_cid.group(1)
        m_msgs = re.search(r"msgs=(\d+)", line)
        if m_msgs:
            entry["msgs"] = int(m_msgs.group(1))
        m_lat = re.search(r"lat(?:ency)?=(\d+)ms", line)
        if m_lat:
            entry["latency_ms"] = int(m_lat.group(1))
        m_fin = re.search(r"(?:fin|finish)=([a-z_]+)", line)
        if m_fin:
            entry["finish"] = m_fin.group(1)
        m_tc = re.search(r"tool_calls?=(\d+)", line)
        if m_tc:
            entry["tool_calls"] = int(m_tc.group(1))
        m_tok = re.search(r"tok=prompt:(\d+)/completion:(\d+)/total:(\d+)", line)
        if m_tok:
            entry["prompt_tokens"] = int(m_tok.group(1))
            entry["completion_tokens"] = int(m_tok.group(2))
    return list(calls.values())


def _extract_ttft(text: str) -> dict[str, float]:
    """抽 skill_stderr 的 TTFT 行,按 call_id 关联。

    [timing] ttft_ms=1371.4 call_id=fbdda1b8e248 node=coordinator_decide
    返回 {call_id: ttft_ms}。
    """
    result: dict[str, float] = {}
    for line in text.splitlines():
        m = re.search(r"\[timing\] ttft_ms=([\d.]+) call_id=([0-9a-f]+)", line)
        if m:
            result[m.group(2)] = float(m.group(1))
    return result


def _extract_tool_calls(text: str) -> list[dict[str, Any]]:
    """抽 tool 调用：兼容 v1（[flow] tool_result）与 v2（[timing] tool_exec / [obs]）格式。

    v1 格式（skill_executor）:
        [flow] tool_result: <tool> => <result>

    v2 格式（coordinator agent loop）:
        [timing] tool_exec name=<tool> duration_ms=<N> [sandbox_mode=e2b]   ← 实际执行（权威）
        [obs] ... "tool_call_names":"['a', 'b']"                             ← LLM 决策工具名
        [obs] ... "tool_calls":"['a', 'b']"                                  ← tool_loop_retry
    """
    tools: list[dict[str, Any]] = []
    # (tool, duration_ms 截断) 去重——v2 同一 exec 打印两次（带/不带 sandbox_mode，
    # duration 差 0.1ms，如 632.5/632.6），int() 截断合并为同一次执行；
    # 不同 duration 的同名工具（如 9 次 bash）各自保留
    seen: set[tuple[str, int]] = set()
    for line in text.splitlines():
        # v2: [timing] tool_exec name=X duration_ms=N args={...}（实际执行，权威来源）
        m_exec = re.search(r"\[timing\] tool_exec name=([a-z_0-9]+) duration_ms=([0-9.]+)", line)
        if m_exec:
            dur = float(m_exec.group(2))
            key = (m_exec.group(1), int(dur))
            if key not in seen:
                seen.add(key)
                # args 字段（F7.1: bash 命令等原始输入；F7.2: base64 图片已截断为元信息）
                m_args = re.search(r"args=(\{.*\}|\[.*\]|\".*\")", line)
                args_raw = m_args.group(1) if m_args else ""
                tools.append({
                    "tool": m_exec.group(1),
                    "args": args_raw[:500],
                    "duration_ms": dur,
                    "source": "exec",
                })
            continue
        # v2: [obs] ... "tool_call_names":"['a', 'b']"（LLM 决策的工具名列表）
        m_names = re.search(r'"tool_call_names"\s*:\s*"(\[[^"]*\])"', line)
        if m_names:
            for n in re.findall(r"'([a-z_0-9]+)'", m_names.group(1)):
                key = ("decision", n)
                if key not in seen:
                    seen.add(key)
                    tools.append({"tool": n, "args": "", "source": "decision"})
            continue
        # v2: [obs] ... "tool_calls":"['bash', 'bash']"（tool_loop_retry 的工具列表）
        m_tcs = re.search(r'"tool_calls"\s*:\s*"(\[[^"]*\])"', line)
        if m_tcs:
            for n in re.findall(r"'([a-z_0-9]+)'", m_tcs.group(1)):
                key = ("decision", n)
                if key not in seen:
                    seen.add(key)
                    tools.append({"tool": n, "args": "", "source": "decision"})
            continue
        # v1: [flow] tool_result: <tool> => <result>
        if "[flow]" in line and ("tool_result" in line or "tool_call" in line):
            m_tool = re.search(r"tool_(?:result|call)[: ]+([a-z_]+)", line, re.I)
            m_res = re.search(r"=>\s*(.{1,200})", line)
            tools.append({
                "tool": m_tool.group(1) if m_tool else "?",
                "args": (m_res.group(1)[:200] if m_res else ""),
                "source": "flow",
            })
        elif "[obs]" in line and "tool_call" in line:
            # 注意: coordinator.final_response 事件含 "has_tool_call" 字段，不是工具调用——
            # 必须匹配到 tool_name/tool 字段才算；匹配不到直接跳过，不产生 "?"。
            m_tool = re.search(r'"tool_name"[:\s]+"([a-z_]+)"', line, re.I) \
                or re.search(r'"tool"[:\s]+"([a-z_]+)"', line, re.I)
            if not m_tool:
                continue
            m_cmd = re.search(r'"command"[:\s]+"([^"]{1,200})"', line, re.I)
            tools.append({
                "tool": m_tool.group(1),
                "args": m_cmd.group(1)[:200] if m_cmd else "",
                "source": "flow",
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
    ttft_by_call: dict[str, float] = {}

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
        ttft_by_call = _extract_ttft(text)
        report["sources"]["skill_stderr"] = {"path": str(stderr),
                                              "llm_calls": llm,
                                              "tool_calls": tools,
                                              "ttft_by_call": ttft_by_call}
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

    # 关联 TTFT(call_id 匹配)并计算 TPS/生成耗时(规范 §3.2)
    for c in merged.values():
        cid = c.get("call_id")
        if cid and cid in ttft_by_call:
            c["ttft_ms"] = ttft_by_call[cid]
            lat = c.get("latency_ms")
            if lat is not None:
                c["generation_ms"] = max(lat - ttft_by_call[cid], 0.0)
        comp = c.get("completion_tokens")
        gen = c.get("generation_ms")
        if comp is not None and gen:
            c["tps"] = round(comp / (gen / 1000.0), 1) if gen > 0 else None
    report["llm_calls"] = sorted(merged.values(),
                                 key=lambda x: int(x["num"]) if x["num"].isdigit() else 0)

    report["tool_calls"] = all_tools

    # 聚合统计(规范 §3.1:总输入/输出 token、平均 TTFT、平均 TPS)
    ttfts = [c["ttft_ms"] for c in report["llm_calls"] if c.get("ttft_ms")]
    comps = [c["completion_tokens"] or 0 for c in report["llm_calls"]]
    gens = [c["generation_ms"] for c in report["llm_calls"] if c.get("generation_ms")]
    tps_vals = [c["tps"] for c in report["llm_calls"] if c.get("tps")]
    # tool 分布只统计"实际执行"(exec/flow)，不统计 LLM 决策(decision)——避免同一工具
    # 被 decision+exec 重复计数；跨源(daemon+stderr)同一 exec 也按 (tool, int(duration))
    # 去重（v2 同一 exec 两次打印 duration 差 0.1ms，截断后合并为一次）
    executed = {}
    for t in all_tools:
        if t.get("source") == "decision":
            continue
        dur = t.get("duration_ms")
        key = (t["tool"], int(dur) if dur is not None else None)
        if key not in executed:
            executed[key] = t["tool"]
    report["summary"] = {
        "llm_call_count": len(report["llm_calls"]),
        "tool_call_count": len(all_tools),
        "total_llm_latency_ms": sum(c["latency_ms"] or 0 for c in report["llm_calls"]),
        "tool_counter": Counter(executed.values()),
        "finish_reason_counter": Counter(c["finish"] or "?" for c in report["llm_calls"]),
        # 性能指标(规范 v1.0 §3.1)
        "total_prompt_tokens": sum(c["prompt_tokens"] or 0 for c in report["llm_calls"]),
        "total_completion_tokens": sum(comps),
        "total_tokens": sum(comps) + sum(c["prompt_tokens"] or 0 for c in report["llm_calls"]),
        "avg_ttft_ms": round(sum(ttfts) / len(ttfts), 1) if ttfts else None,
        "avg_generation_ms": round(sum(gens) / len(gens), 1) if gens else None,
        "avg_tps": round(sum(tps_vals) / len(tps_vals), 1) if tps_vals else None,
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
        ttft_s = f"{c['ttft_ms']:.0f}ms" if c.get("ttft_ms") else "-"
        gen_s = f"{c['generation_ms']:.0f}ms" if c.get("generation_ms") else "-"
        tps_s = f"{c['tps']}" if c.get("tps") else "-"
        comp_s = c.get("completion_tokens") if c.get("completion_tokens") is not None else "-"
        msgs_s = c.get("msgs") if c.get("msgs") is not None else "-"
        print(f"LLM#{c['num']}: lat={lat_s} ttft={ttft_s} gen={gen_s} "
              f"comp={comp_s} tps={tps_s} msgs={msgs_s} "
              f"fin={c.get('finish') or '?'} tc={c.get('tool_calls', 0)}")
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
