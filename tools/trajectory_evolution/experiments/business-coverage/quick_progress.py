#!/usr/bin/env python3
"""Fast read-only GEPA snapshot; never scans historical reports."""
from __future__ import annotations
import glob, json, os, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
GEPA = ROOT / "trace_output" / "business-simulator" / "gepa"

def load(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None

def number(value):
    return value if isinstance(value, (int, float)) else None

def tree():
    paths = glob.glob(str(GEPA / "workspace" / "agent-evaluation" / "*" / "search" / "tree.json"))
    if not paths:
        return {}
    return load(Path(max(paths, key=lambda p: os.stat(p).st_mtime))) or {}

def row(node, baseline=False):
    node = node or {}
    opt = node.get("optimization") or {}
    c = opt.get("criteria") if isinstance(opt, dict) else {}
    c = c if isinstance(c, dict) else {}
    coverage = node.get("control_coverage") or {}
    caps = coverage.get("capabilities") if isinstance(coverage, dict) else {}
    caps = caps if isinstance(caps, dict) else {}
    def applied(key):
        value = caps.get(key) or {}
        return value.get("applied", 0) if isinstance(value, dict) else 0
    return {
        "state": node.get("status", "待生成"),
        "done": f"{len(node.get('validation') or {})}/50" if node.get("validation") else "待生成",
        "quality": number(c.get("quality")),
        "loops": number(c.get("invalid_loops")),
        "latency": number(c.get("duration_ms")),
        "tokens": number(c.get("tokens")),
        "L1": 0 if baseline else applied("llm"),
        "L2a": 0 if baseline else applied("actions"),
        "L2s": 0 if baseline else applied("schedule"),
        "L3": 0 if baseline else applied("loop"),
    }

def fmt(value, scale=1):
    if value is None:
        return "暂无"
    return f"{value / scale:.2f}" if isinstance(value, float) else str(value)

def score_row(scores):
    values = list(scores.values()) if isinstance(scores, dict) else []
    if not values:
        return {"state": "待生成", "done": "待生成"}
    def avg(key):
        nums = [v.get(key) for v in values
                if isinstance(v, dict) and isinstance(v.get(key), (int, float))]
        return sum(nums) / len(nums) if nums else None
    strict = sum(1 for v in values if isinstance(v, dict) and
                 (v.get("strict_pass_rate", 0) or 0) >= 1)
    return {"state": "已完成", "done": f"{len(values)}", "quality": avg("quality"),
            "strict": f"{strict}/{len(values)}", "loops": avg("invalid_loops"),
            "latency": avg("duration_ms"), "tokens": avg("tokens")}

def main():
    status = load(GEPA / "status.json") or {}
    t = tree(); nodes = t.get("nodes") if isinstance(t.get("nodes"), list) else []
    active = t.get("active")
    current = nodes[int(active)] if isinstance(active, (int, float)) and int(active) < len(nodes) else (nodes[-1] if nodes else {})
    parent = nodes[int(current["parent"])] if isinstance(current.get("parent"), (int, float)) and int(current["parent"]) < len(nodes) else {}
    labels = [f"Node {int(parent['id'])}" if parent else "父节点", f"Node {int(current['id'])}" if current else "当前节点"]
    l0 = score_row(t.get("baseline_validation"))
    if l0.get("state") == "待生成":
        l0 = row(nodes[0] if nodes else {}, True)
    parent_value = row(parent)
    current_value = row(current)
    pool_parent = {"state": "待正式报告"}
    pool_current = {"state": "待正式报告"}
    screening = current.get("screening") if isinstance(current, dict) else None
    if isinstance(screening, dict) and isinstance(screening.get("parent"), dict):
        pool_parent = score_row(screening["parent"])
    if isinstance(screening, dict) and isinstance(screening.get("candidate"), dict):
        pool_current = score_row(screening["candidate"])
    values = [l0, parent_value, current_value]
    print(f"UTC {time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime())} | GEPA={status.get('status', t.get('status', '暂无'))} | round={t.get('rounds', '暂无')}")
    print(f"| 指标 | L0 | 父节点 pool（{labels[0]}，同题） | 父节点（{labels[0]}） | 当前节点（{labels[1]}） |")
    print("|---|---:|---:|---:|---:|")
    items = [("状态", "state", 1), ("完成题数", "done", 1), ("质量", "quality", 1), ("严格通过", "strict", 1), ("平均无效 loop", "loops", 1), ("平均时延(s)", "latency", 1000), ("平均 token", "tokens", 1), ("意图 F1（题级）", None, 1), ("L1 实际应用", "L1", 1), ("L2 动作实际应用", "L2a", 1), ("L2 编排实际应用", "L2s", 1), ("L3 loop 实际应用", "L3", 1)]
    for label, key, scale in items:
        vals = ["待正式报告" if key is None else
                (v.get(key) if key in ("state", "done", "strict") else fmt(v.get(key), scale))
                for v in values]
        pool = "待正式报告" if key is None else (pool_parent.get(key) if key in ("state", "done", "strict") else fmt(pool_parent.get(key), scale))
        live = "待正式报告" if key is None else (pool_current.get(key) if key in ("state", "done", "strict") else fmt(pool_current.get(key), scale))
        cells = [label, "0" if label.startswith("L") else vals[0], pool,
                 vals[1], live if current.get("status") == "screened" else vals[2]]
        print("| " + " | ".join("暂无" if cell is None else str(cell) for cell in cells) + " |")

if __name__ == "__main__":
    main()
