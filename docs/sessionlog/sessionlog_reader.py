#!/usr/bin/env python3
"""sessionlog_reader — 读取/校验/重放轨迹日志，构建编排图（节点=trace_id+pid，边=dispatch/return）。

- load(dialogue_id)：读文件，torn 尾行丢弃
- validate()：校验事件序列、LLM/Tool/Skill 起止配对和 dialogue 完整性
- replay()：按文件行序重放事件流（恢复模型看到的一切）
- build_graph()：构建编排图——
    节点 = trace_id+pid（每个 coordinator/skill 子进程一个执行单元）
    派发边（dispatch）：coordinator → skill，按 exec_skill_key 匹配
    回传边（return）：skill → coordinator，按 skill_name=completed_skill + 时间序匹配
"""

import hashlib
import json
import os
import sys
from collections import defaultdict
from pathlib import Path


_HEADER_TYPES = {"dialogue.header", "session.header"}
_DIALOGUE_START_TYPES = {"dialogue.start", "session.start"}
_DIALOGUE_END_TYPES = {"dialogue.end", "session.end"}


def _default_dir():
    return os.environ.get("AF_SESSIONLOG_DIR", "")


def _safe_path_component(value: str, fallback: str) -> str:
    """与采集 Hook 使用相同的安全文件名规则。"""
    original = str(value or "")
    if not original:
        return fallback
    component = "".join(
        char if char.isalnum() or char in "-_." else "_"
        for char in original
    ).strip(".")
    changed = component != original or len(component) > 48
    component = component[:48] or fallback
    if changed:
        digest = hashlib.sha256(original.encode("utf-8")).hexdigest()[:12]
        component = f"{component}-{digest}"
    return component


def _find_dialogue_file(root: str, dialogue_id: str) -> str:
    """定位新层级文件，并兼容历史平铺与旧投影目录。"""
    root_path = Path(root)
    filename = f"{_safe_path_component(dialogue_id, 'unknown-dialogue')}.jsonl"
    direct = root_path / filename
    if direct.is_file():
        return str(direct)
    for pattern in (
        f"by-user/*/*/{filename}",
        f"by-user/*/sessions/*/{filename}",
        f"by-user/*/conversations/*/{filename}",
    ):
        match = next((path for path in sorted(root_path.glob(pattern)) if path.is_file()), None)
        if match is not None:
            return str(match)
    return str(direct)


def load(dialogue_id: str, path: str | None = None) -> list[dict]:
    """读文件；仅丢弃无换行且无法解析的最后一条 torn 记录。"""
    if path is None:
        path = _find_dialogue_file(
            _default_dir() or "trace_output/sessionlogs", dialogue_id,
        )
    events = []
    raw_lines = Path(path).read_bytes().splitlines(keepends=True)
    for line_number, raw_line in enumerate(raw_lines, 1):
        if not raw_line.strip():
            continue
        has_line_ending = raw_line.endswith((b"\n", b"\r"))
        try:
            line = raw_line.decode("utf-8")
            record = json.loads(line)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            is_torn_tail = line_number == len(raw_lines) and not has_line_ending
            if is_torn_tail:
                preview = raw_line[:80].decode("utf-8", errors="replace")
                print(
                    f"[sessionlog.reader] torn tail dropped at line "
                    f"{line_number}: {preview}...",
                    file=sys.stderr,
                )
                continue
            raise ValueError(
                f"sessionlog corruption at line {line_number}: {exc}"
            ) from exc
        if not isinstance(record, dict):
            raise ValueError(
                f"sessionlog record at line {line_number} must be an object"
            )
        events.append(record)
    return events


def validate(events: list[dict], *, require_header: bool = False) -> list[str]:
    """校验采集正确性（按 trace_id + pid 分组，跨进程不混校验）：
    1. 每个进程内 seq 递增且无重复
    2. llm.request 必须有配对的 llm.response（同进程 + span_id）
    3. source_event_seqs 引用同一进程中存在的 seq
    4. 成功派发的 Skill 必须有 skill.start/skill.end，或有 batch.result 终态兜底
    5. tool.call 必须有对应 tool.result
    6. dialogue.start/dialogue.end 必须成对
    返回问题列表（空=通过）。
    """
    issues = []
    headers = [
        (index, event) for index, event in enumerate(events)
        if isinstance(event, dict) and event.get("type") in _HEADER_TYPES
    ]
    if require_header and len(headers) != 1:
        issues.append(f"dialogue.header 数量应为 1，实际 {len(headers)}")
    if headers:
        if headers[0][0] != 0:
            issues.append("dialogue.header 必须是文件第一条记录")
        if len(headers) > 1:
            issues.append(f"dialogue.header 重复: {len(headers)}")
        header_dialogue_id = headers[0][1].get("dialogue_id")
        for event in events:
            if (
                isinstance(event, dict)
                and event.get("type") not in _HEADER_TYPES
                and header_dialogue_id
                and event.get("dialogue_id") != header_dialogue_id
            ):
                issues.append(
                    "dialogue_id 与 header 不一致: "
                    f"event={event.get('event_id') or event.get('seq')} "
                    f"value={event.get('dialogue_id')!r}"
                )

    seen_event_ids: set[str] = set()
    seen_idempotency_keys: set[str] = set()
    process_events = []
    for index, event in enumerate(events):
        if not isinstance(event, dict):
            issues.append(f"第 {index + 1} 条记录不是对象")
            continue
        if event.get("type") in _HEADER_TYPES:
            continue
        if not isinstance(event.get("type"), str) or not event.get("type"):
            issues.append(f"第 {index + 1} 条记录缺少 type")
        if not isinstance(event.get("data"), dict):
            issues.append(f"第 {index + 1} 条事件 data 不是对象")
        event_id = event.get("event_id")
        if event_id:
            if event_id in seen_event_ids:
                issues.append(f"event_id 重复: {event_id}")
            seen_event_ids.add(event_id)
        idempotency_key = event.get("idempotency_key")
        if idempotency_key:
            if idempotency_key in seen_idempotency_keys:
                issues.append(f"idempotency_key 重复: {idempotency_key}")
            seen_idempotency_keys.add(idempotency_key)
        seq = event.get("seq")
        if not isinstance(seq, int) or isinstance(seq, bool):
            issues.append(f"第 {index + 1} 条事件 seq 非整数: {seq!r}")
            continue
        process_events.append(event)


    by_process: dict[tuple[str, object], list[dict]] = defaultdict(list)
    for e in process_events:
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

    dialogue_starts = [
        event for event in process_events
        if event.get("type") in _DIALOGUE_START_TYPES
    ]
    dialogue_ends = [
        event for event in process_events
        if event.get("type") in _DIALOGUE_END_TYPES
    ]
    has_dialogue_start = bool(dialogue_starts)
    has_dialogue_end = bool(dialogue_ends)
    if has_dialogue_start and not has_dialogue_end:
        issues.append("dialogue.start 无配对 dialogue.end")
    if has_dialogue_end and not has_dialogue_start:
        issues.append("dialogue.end 无配对 dialogue.start")
    if require_header and len(dialogue_starts) != 1:
        issues.append(f"dialogue.start 数量应为 1，实际 {len(dialogue_starts)}")
    if len(dialogue_ends) > 1:
        issues.append(f"dialogue.end 终态重复: {len(dialogue_ends)}")

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
        if t in _HEADER_TYPES:
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
        elif t in _DIALOGUE_START_TYPES | {"coordinator.activation"} | _DIALOGUE_END_TYPES:
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
    evs = [e for e in events if e.get("type") not in _HEADER_TYPES]
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
        and e.get("type") in {"coordinator.activation"} | _DIALOGUE_START_TYPES
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
    issues = validate(evs, require_header=True)
    if issues:
        print("问题:")
        for i in issues:
            print(f"  - {i}")
    else:
        print("校验通过")
    print("\n=== 重放（按行序）===")
    replay(evs)
    build_graph(evs)
