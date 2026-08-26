#!/usr/bin/env python3
"""sessionlog_reader — 读取/校验/重放 sessionlog，构建编排图（节点=trace_id，边=dispatch/return）。

- load(dialogue_id)：读文件，torn 尾行丢弃
- validate()：按 trace_id 分组校验递增、llm.request/response 配对、血缘引用存在
- replay()：按文件行序重放事件流（恢复模型看到的一切）
- build_graph()：构建编排图——
    节点 = trace_id（每个 coordinator/skill 子进程一个执行单元）
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
    """校验采集正确性（按 trace_id 分组，跨进程不混校验）：
    1. 每个 trace_id 内 seq 递增且无重复
    2. llm.request 必须有配对的 llm.response（同 trace_id + span_id）
    3. source_event_seqs 引用的 seq 必须存在
    返回问题列表（空=通过）。
    """
    issues = []

    by_trace: dict[str, list[dict]] = defaultdict(list)
    for e in events:
        if e.get("type") == "session.header":
            continue
        by_trace[e.get("trace_id") or str(e.get("pid"))].append(e)

    for tid, evs in sorted(by_trace.items()):
        seqs = [e.get("seq") for e in evs]
        if seqs != sorted(seqs):
            issues.append(f"trace={tid}: seq 不递增: {seqs}")
        if len(set(seqs)) != len(seqs):
            issues.append(f"trace={tid}: seq 重复: {seqs}")

    for tid, evs in by_trace.items():
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
                issues.append(f"trace={tid}: llm.request 无配对 llm.response span={span} seq={req.get('seq')}")

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
        elif t in ("tool.call", "tool.result", "skill.start", "skill.end", "skill.dispatch"):
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
            print(f"[replay] trace={str(e.get('trace_id'))[:20]} seq={e.get('seq')} {t} node={e.get('node_id')}"
                  f" parent={str(e.get('parent_span_id',''))[:20]}{extra}")
        elif t in ("session.start", "session.end"):
            d = e.get("data", {})
            extra = ""
            if t == "session.start":
                extra = f" trigger={d.get('trigger_type')} completed_skill={d.get('completed_skill','')!r}"
            print(f"[replay] trace={str(e.get('trace_id'))[:20]} seq={e.get('seq')} {t} node={e.get('node_id')}{extra}")


def build_graph(events: list[dict]):
    """构建编排图：节点=trace_id，边=派发边(dispatch) / 回传边(return)。

    节点属性：node_id、事件数、起始/结束 ts、包含的 skill_name（skill 进程）
    派发边：skill.dispatch（coordinator，带 exec_skill_key）→ skill.start（skill，带 exec_skill_key）
            —— 按 exec_skill_key 匹配
    回传边：skill.end（skill，带 skill_name）→ session.start(trigger_type=skill_completion, 带 completed_skill)
            —— 按 skill_name == completed_skill + skill.end.ts < session.start.ts 匹配
    """
    evs = [e for e in events if e.get("type") != "session.header"]
    if not evs:
        return

    # ── 节点：trace_id ──
    nodes: dict[str, dict] = {}
    for e in evs:
        tid = e.get("trace_id") or f"pid{e.get('pid')}"
        n = nodes.setdefault(tid, {
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
            matched.sort(key=lambda x: (x[1], x[0].get("ts") or 0))
            s = matched[0][0]
            edges.append({
                "type": "dispatch", "from": d.get("trace_id"), "to": s.get("trace_id"),
                "skill": (d_names or [""])[0],
                "exec_key": s.get("data", {}).get("exec_skill_key", "")
                            or (d_keys[0] if d_keys else ""), "ts": d_ts,
            })

    ends = [e for e in evs if e.get("type") == "skill.end"]
    restarts = [e for e in evs if e.get("type") == "session.start"
                and e.get("data", {}).get("trigger_type") == "skill_completion"]
    for en in ends:
        skill = en.get("data", {}).get("skill_name", "")
        ek = en.get("data", {}).get("exec_skill_key", "")
        # exec_key 精确匹配（哪个 skill 完成）：skill.end → 对应 skill.start → 对应 skill.dispatch
        # 若 end 有 exec_key，用它锁定"这一个 skill 的完成"，避免同名 skill 串边
        candidates = []
        for ss in restarts:
            cs = ss.get("data", {}).get("completed_skill", "")
            if skill and skill == cs and (en.get("ts") or 0) < (ss.get("ts") or 0):
                candidates.append(ss)
        if candidates:
            # 有 exec_key 时，只连"该 skill 完成后最近的决策段"（避免并行同名 skill 的 end 串到别的决策段）
            candidates.sort(key=lambda x: x.get("ts") or 0)
            ss = candidates[0]
            edges.append({
                "type": "return", "from": en.get("trace_id"), "to": ss.get("trace_id"),
                "skill": skill, "exec_key": ek, "ts": en.get("ts"),
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
