#!/usr/bin/env python3
"""sessionlog_reader — 读取/校验/重放 sessionlog，构建编排图（节点=trace_id+pid，边=dispatch/return）。

- load(dialogue_id)：读文件，torn 尾行丢弃
- validate()：校验事件序列、LLM/Tool/Skill 起止配对和 session 完整性
- replay()：按文件行序重放事件流（恢复模型看到的一切）
- build_graph()：构建编排图——
    节点 = trace_id+pid（每个 coordinator/skill 子进程一个执行单元）
    派发边（dispatch）：coordinator → skill，按 exec_skill_key 匹配
    回传边（return）：skill → coordinator，按 skill_name=completed_skill + 时间序匹配
"""

import json
import os
import sys
from collections import defaultdict


def _default_dir():
    return os.environ.get("AF_SESSIONLOG_DIR", "")


def load(dialogue_id: str, path: str | None = None) -> list[dict]:
    """读文件，返回事件列表。torn 尾行（无换行/JSON 解析失败）丢弃。"""
    if path is None:
        path = os.path.join(_default_dir() or "trace_output/sessionlogs", f"{dialogue_id}.jsonl")
    events = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.strip():
                continue
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError:
                print(f"[sessionlog.reader] torn line dropped: {line[:80]}...", file=sys.stderr)
    return events


def validate(events: list[dict]) -> list[str]:
    """校验采集正确性（按 trace_id + pid 分组，跨进程不混校验）：
    1. 每个进程内 seq 递增且无重复
    2. llm.request 必须有配对的 llm.response（同进程 + span_id）
    3. source_event_seqs 引用同一进程中存在的 seq
    4. 成功派发的 Skill 必须有 skill.start/skill.end，或有 batch.result 终态兜底
    5. tool.call 必须有对应 tool.result
    6. session.start/session.end 必须成对
    返回问题列表（空=通过）。
    """
    issues = []

    by_process: dict[tuple[str, object], list[dict]] = defaultdict(list)
    for e in events:
        if e.get("type") == "session.header":
            continue
        tid = e.get("trace_id") or f"pid{e.get('pid')}"
        by_process[(tid, e.get("pid"))].append(e)

    for (tid, pid), evs in sorted(by_process.items(), key=lambda item: str(item[0])):
        label = f"trace={tid} pid={pid}"
        seqs = [e.get("seq") for e in evs]
        if seqs != sorted(seqs):
            issues.append(f"{label}: seq 不递增: {seqs}")
        if len(set(seqs)) != len(seqs):
            issues.append(f"{label}: seq 重复: {seqs}")

    for (tid, pid), evs in by_process.items():
        label = f"trace={tid} pid={pid}"
        available_seqs = {e.get("seq") for e in evs}
        req_by_span = {}
        resp_spans = set()
        for e in evs:
            t = e.get("type")
            if t == "llm.request":
                req_by_span[e.get("span_id")] = e
            elif t == "llm.response":
                resp_spans.add(e.get("span_id"))
        for span, req in req_by_span.items():
            if span not in resp_spans:
                issues.append(f"{label}: llm.request 无配对 llm.response span={span} seq={req.get('seq')}")
        for e in evs:
            for source_seq in e.get("source_event_seqs") or []:
                if source_seq not in available_seqs:
                    issues.append(
                        f"{label}: 血缘 seq 不存在: source_seq={source_seq} event_seq={e.get('seq')}"
                    )

        tool_calls = {
            e.get("seq"): e for e in evs if e.get("type") == "tool.call"
        }
        tool_results = [e for e in evs if e.get("type") == "tool.result"]
        for call_seq, call in tool_calls.items():
            paired = [
                result for result in tool_results
                if call_seq in (result.get("source_event_seqs") or [])
            ]
            if not paired:
                issues.append(
                    f"{label}: tool.call 无配对 tool.result seq={call_seq}"
                )
                continue
            call_items = call.get("data", {}).get("tool_calls") or []
            call_ids = {
                item.get("tool_call_id") or item.get("id")
                for item in call_items if isinstance(item, dict)
            } - {None, ""}
            result_ids = {
                item.get("tool_call_id") or item.get("id")
                for result in paired
                for item in (result.get("data", {}).get("tool_results") or [])
                if isinstance(item, dict)
            } - {None, ""}
            if call_ids and not call_ids <= result_ids:
                issues.append(
                    f"{label}: tool.result 缺少 call_id={sorted(call_ids - result_ids)} "
                    f"tool.call_seq={call_seq}"
                )
        for result in tool_results:
            source_seqs = result.get("source_event_seqs") or []
            if not any(source_seq in tool_calls for source_seq in source_seqs):
                issues.append(
                    f"{label}: tool.result 无对应 tool.call seq={result.get('seq')}"
                )

    event_types = {
        event.get("type") for event in events
        if event.get("type") != "session.header"
    }
    if "session.start" in event_types and "session.end" not in event_types:
        issues.append("session.start 无配对 session.end")
    if "session.end" in event_types and "session.start" not in event_types:
        issues.append("session.end 无配对 session.start")

    batch_terminal_keys = {
        result.get("exec_skill_key")
        for event in events if event.get("type") == "batch.result"
        for result in (event.get("data", {}).get("results") or [])
        if isinstance(result, dict) and result.get("exec_skill_key")
    }
    starts = [e for e in events if e.get("type") == "skill.start"]
    ends = [e for e in events if e.get("type") == "skill.end"]
    start_keys = {
        e.get("data", {}).get("exec_skill_key") for e in starts
        if e.get("data", {}).get("exec_skill_key")
    }
    end_keys = {
        e.get("data", {}).get("exec_skill_key") for e in ends
        if e.get("data", {}).get("exec_skill_key")
    }

    for dispatch in (
        e for e in events if e.get("type") == "skill.dispatch"
    ):
        data = dispatch.get("data", {})
        if data.get("status", "dispatched") not in {
            "dispatched", "partially_dispatched",
        }:
            continue
        keys = data.get("exec_skill_keys") or []
        if not keys and data.get("exec_skill_key"):
            keys = [data.get("exec_skill_key")]
        for exec_key in keys:
            if exec_key not in start_keys and exec_key not in batch_terminal_keys:
                issues.append(
                    f"skill.dispatch 无 Skill 启动或 Batch 终态 exec_skill_key={exec_key}"
                )

    for start in starts:
        data = start.get("data", {})
        exec_key = data.get("exec_skill_key", "")
        has_terminal = (
            exec_key in end_keys or exec_key in batch_terminal_keys
            if exec_key else any(
                _execution_id(end) == _execution_id(start) for end in ends
            )
        )
        if not has_terminal:
            issues.append(
                f"skill.start 无终态 skill.end/batch.result "
                f"skill={data.get('skill_name', '')} exec_skill_key={exec_key}"
            )

    for end in ends:
        exec_key = end.get("data", {}).get("exec_skill_key", "")
        has_start = (
            exec_key in start_keys
            if exec_key else any(
                _execution_id(start) == _execution_id(end) for start in starts
            )
        )
        if not has_start:
            issues.append(
                f"skill.end 无配对 skill.start exec_skill_key={exec_key}"
            )

    return issues


def replay(events: list[dict]):
    """按文件行序重放：恢复每次 llm.request 的 messages 序列（模型看到的一切）。"""
    for e in events:
        t = e.get("type")
        if t == "session.header":
            continue
        if t == "llm.request":
            msgs = e.get("data", {}).get("messages") or []
            last = msgs[-1] if msgs else {}
            print(f"[replay] trace={str(e.get('trace_id'))[:20]} seq={e.get('seq')} {t} node={e.get('node_id')} "
                  f"round={e.get('round')} msgs={len(msgs)} last={last.get('role')}:{str(last.get('content',''))[:40]!r}")
        elif t == "llm.response":
            resp = e.get("data", {}).get("response") or {}
            meta = e.get("data", {}).get("meta") or {}
            choice = (resp.get("choices") or [{}])[0]
            print(f"[replay] trace={str(e.get('trace_id'))[:20]} seq={e.get('seq')} {t} finish={choice.get('finish_reason')} "
                  f"node={meta.get('node_id')} task_type={meta.get('task_type')} "
                  f"tokens={meta.get('prompt_tokens')}/{meta.get('completion_tokens')}")
        elif t in (
            "tool.call", "tool.result", "skill.start", "skill.end",
            "skill.dispatch", "batch.result", "messages.snapshot",
        ):
            d = e.get("data", {})
            extra = ""
            if t == "tool.call":
                extra = f" tools={d.get('tool_names')}"
            elif t == "tool.result":
                extra = f" tools={d.get('tool_names')} dur_ms={d.get('duration_ms')}"
            elif t in ("skill.start", "skill.end"):
                extra = f" skill={d.get('skill_name')} exec_key={str(d.get('exec_skill_key',''))[:30]}"
            elif t == "skill.dispatch":
                extra = f" skills={d.get('skill_names')} exec_key={str(d.get('exec_skill_key',''))[:30]}"
            elif t == "batch.result":
                extra = (
                    f" batch_id={d.get('batch_id','')!r}"
                    f" status={d.get('status','')}"
                    f" results={len(d.get('results') or [])}"
                )
            elif t == "messages.snapshot":
                extra = f" messages={d.get('message_count', 0)}"
            print(f"[replay] trace={str(e.get('trace_id'))[:20]} seq={e.get('seq')} {t} node={e.get('node_id')}"
                  f" parent={str(e.get('parent_span_id',''))[:20]}{extra}")
        elif t in ("session.start", "coordinator.activation", "session.end"):
            d = e.get("data", {})
            extra = ""
            if t == "coordinator.activation":
                extra = (
                    f" trigger={d.get('trigger_type')}"
                    f" completed_skill={d.get('completed_skill','')!r}"
                    f" batch_id={d.get('batch_id','')!r}"
                )
            print(f"[replay] trace={str(e.get('trace_id'))[:20]} seq={e.get('seq')} {t} node={e.get('node_id')}{extra}")


def _execution_id(event: dict) -> str:
    """返回端到端 trace 内的进程级执行单元标识。"""
    tid = event.get("trace_id") or f"pid{event.get('pid')}"
    pid = event.get("pid")
    return f"{tid}:pid{pid}" if pid is not None else tid


def build_graph(events: list[dict]):
    """构建编排图：节点=trace_id+pid，边=派发边(dispatch) / 回传边(return)。

    节点属性：node_id、事件数、起始/结束 ts、包含的 skill_name（skill 进程）
    派发边：skill.dispatch（coordinator，带 exec_skill_key）→ skill.start（skill，带 exec_skill_key）
            —— 按 exec_skill_key 匹配
    回传边：skill.end → coordinator.activation(trigger_type=skill_completion)
            —— 并行批次按 batch_id 精确匹配；旧单 Skill 轨迹按名称+时序兜底
    """
    evs = [e for e in events if e.get("type") != "session.header"]
    if not evs:
        return

    # ── 节点：同一个端到端 trace 中，每个进程仍是独立执行单元 ──
    nodes: dict[str, dict] = {}
    for e in evs:
        tid = e.get("trace_id") or f"pid{e.get('pid')}"
        execution_id = _execution_id(e)
        n = nodes.setdefault(execution_id, {
            "trace_id": tid, "node_id": e.get("node_id"), "events": 0,
            "start_ts": e.get("ts"), "end_ts": e.get("ts"), "skill_name": "",
        })
        n["events"] += 1
        if e.get("ts") is not None:
            n["start_ts"] = min(n["start_ts"] or e["ts"], e["ts"])
            n["end_ts"] = max(n["end_ts"] or e["ts"], e["ts"])
        if e.get("type") in ("skill.start", "skill.end"):
            n["skill_name"] = e.get("data", {}).get("skill_name", "")

    # ── 边 ──
    edges = []  # {type, from, to, skill, exec_key, ts}
    starts_by_key = {
        e.get("data", {}).get("exec_skill_key"): e for e in evs
        if e.get("type") == "skill.start"
        and e.get("data", {}).get("exec_skill_key")
    }
    ends_by_key = {
        e.get("data", {}).get("exec_skill_key"): e for e in evs
        if e.get("type") == "skill.end"
        and e.get("data", {}).get("exec_skill_key")
    }
    batch_terminals = {}
    for batch_event in (e for e in evs if e.get("type") == "batch.result"):
        for result in batch_event.get("data", {}).get("results") or []:
            if not isinstance(result, dict):
                continue
            exec_key = result.get("exec_skill_key", "")
            if exec_key:
                batch_terminals[exec_key] = (batch_event, result)

    synthetic_execution_ids = {}
    for exec_key, (batch_event, result) in batch_terminals.items():
        if exec_key in starts_by_key:
            continue
        execution_id = f"framework:{exec_key}"
        synthetic_execution_ids[exec_key] = execution_id
        skill_name = (
            result.get("business_skill_name")
            or result.get("framework_skill_name")
            or exec_key
        )
        nodes[execution_id] = {
            "trace_id": execution_id,
            "node_id": result.get("framework_skill_name") or "framework",
            "events": 1,
            "start_ts": batch_event.get("ts"),
            "end_ts": batch_event.get("ts"),
            "skill_name": skill_name,
        }

    # ── 派发边：skill.dispatch（coordinator）→ skill.start（skill）──
    # 强键：exec_skill_key 精确匹配（exec_skill_keys 数组支持一次派发多个 skill）
    # 兜底：无 exec_key 时 skill_names == skill_name + 时序
    dispatches = [e for e in evs if e.get("type") == "skill.dispatch"]
    starts = [e for e in evs if e.get("type") == "skill.start"]
    for d in dispatches:
        d_keys = d.get("data", {}).get("exec_skill_keys") or []
        if not d_keys and d.get("data", {}).get("exec_skill_key"):
            d_keys = [d.get("data", {}).get("exec_skill_key")]
        d_names = d.get("data", {}).get("skill_names") or []
        key_to_name = {
            key: d_names[index]
            for index, key in enumerate(d_keys)
            if index < len(d_names)
        }
        d_ts = d.get("ts") or 0
        matched = []
        for s in starts:
            sk = s.get("data", {}).get("exec_skill_key", "")
            s_name = s.get("data", {}).get("skill_name", "")
            s_ts = s.get("ts") or 0
            if s_ts <= d_ts:
                continue  # start 必须在 dispatch 之后
            if d_keys and sk in d_keys:
                matched.append((s, 0))          # exec_key 精确匹配，优先级 0
            elif not d_keys and d_names and s_name in d_names:
                matched.append((s, 1))          # 名称+时序兜底，优先级 1
        if matched:
            # 一个 dispatch 可连多个并行 skill：exec_key 精确匹配的取全部，
            # 名称兜底只取最近一个（避免跨轮串边）
            matched.sort(key=lambda x: (x[1], x[0].get("ts") or 0))
            if matched[0][1] == 0:  # 有 exec_key 精确匹配 → 连所有精确匹配的 start
                picked = [m for m in matched if m[1] == 0]
            else:
                picked = [matched[0]]
            for s, _prio in picked:
                exec_key = s.get("data", {}).get("exec_skill_key", "")
                edges.append({
                    "type": "dispatch", "from": _execution_id(d), "to": _execution_id(s),
                    "skill": key_to_name.get(exec_key)
                             or s.get("data", {}).get("skill_name", "")
                             or (d_names or [""])[0],
                    "exec_key": exec_key
                                or (d_keys[0] if d_keys else ""), "ts": d_ts,
                })
        matched_keys = {
            s.get("data", {}).get("exec_skill_key", "")
            for s, _priority in matched
        }
        for exec_key in d_keys:
            if exec_key in matched_keys or exec_key not in synthetic_execution_ids:
                continue
            _batch_event, result = batch_terminals[exec_key]
            skill_name = (
                key_to_name.get(exec_key)
                or result.get("business_skill_name")
                or result.get("framework_skill_name")
                or exec_key
            )
            edges.append({
                "type": "dispatch",
                "from": _execution_id(d),
                "to": synthetic_execution_ids[exec_key],
                "skill": skill_name,
                "exec_key": exec_key,
                "ts": d_ts,
            })

    ends = [e for e in evs if e.get("type") == "skill.end"]
    activations = [
        e for e in evs
        if e.get("data", {}).get("trigger_type") == "skill_completion"
        and e.get("type") in {"coordinator.activation", "session.start"}
    ]
    for en in ends:
        end_data = en.get("data", {})
        skill = end_data.get("skill_name", "")
        ek = end_data.get("exec_skill_key", "")
        end_batch_id = end_data.get("batch_id", "")
        candidates = []
        for activation in activations:
            activation_data = activation.get("data", {})
            activation_batch_id = activation_data.get("batch_id", "")
            completed_skill = activation_data.get("completed_skill", "")
            same_batch = (
                end_batch_id
                and activation_batch_id
                and end_batch_id == activation_batch_id
            )
            legacy_name_match = (
                not activation_batch_id
                and skill
                and skill == completed_skill
            )
            if (
                (same_batch or legacy_name_match)
                and (en.get("ts") or 0) < (activation.get("ts") or 0)
            ):
                candidates.append(activation)
        if candidates:
            candidates.sort(key=lambda x: x.get("ts") or 0)
            activation = candidates[0]
            edges.append({
                "type": "return",
                "from": _execution_id(en),
                "to": _execution_id(activation),
                "skill": skill,
                "exec_key": ek,
                "ts": en.get("ts"),
            })

    # Python skill.end 缺失时，以框架 batch.result 作为权威终态回传。
    for exec_key, (batch_event, result) in batch_terminals.items():
        if exec_key in ends_by_key:
            continue
        start = starts_by_key.get(exec_key)
        from_execution = (
            _execution_id(start) if start is not None
            else synthetic_execution_ids.get(exec_key)
        )
        if not from_execution:
            continue
        skill_name = (
            result.get("business_skill_name")
            or result.get("framework_skill_name")
            or exec_key
        )
        edges.append({
            "type": "return",
            "from": from_execution,
            "to": _execution_id(batch_event),
            "skill": skill_name,
            "exec_key": exec_key,
            "ts": batch_event.get("ts"),
        })

    print(f"\n=== 编排图（{len(nodes)} 节点，{len(edges)} 边）===")
    for tid, n in sorted(nodes.items(), key=lambda x: x[1].get("start_ts") or 0):
        role = f"skill={n['skill_name']}" if n["skill_name"] else f"node={n['node_id']}"
        print(f"  节点 {str(tid)[:24]:24s} {role:28s} events={n['events']} "
              f"ts={n['start_ts']}~{n['end_ts']}")

    print("")
    for edge in sorted(edges, key=lambda x: x.get("ts") or 0):
        arrow = "─▶" if edge["type"] == "dispatch" else "◀─"
        print(f"  {edge['type']:8s} {str(edge['from'])[:20]} {arrow} {str(edge['to'])[:20]}  skill={edge['skill']}")


if __name__ == "__main__":
    did = sys.argv[1] if len(sys.argv) > 1 else ""
    if not did:
        print("usage: sessionlog_reader.py <dialogue_id> [path]")
        sys.exit(1)
    path = sys.argv[2] if len(sys.argv) > 2 else None
    evs = load(did, path)
    print(f"=== {did}: {len(evs)} 事件（含 header）===")
    issues = validate(evs)
    if issues:
        print("问题:")
        for i in issues:
            print(f"  - {i}")
    else:
        print("校验通过")
    print("\n=== 重放（按行序）===")
    replay(evs)
    build_graph(evs)
