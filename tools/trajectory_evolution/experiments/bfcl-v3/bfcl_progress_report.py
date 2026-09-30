#!/usr/bin/env python3
"""Read-only BFCL progress and holdout comparison report.

The business experiment and BFCL use the same native report schema.  Reuse the
business reporter so L0/parent/current labeling, strict counts, metrics and
level receipts stay identical; only the output root is different.
"""
from __future__ import annotations

import sys
from pathlib import Path
import time
import json

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
BUSINESS = ROOT / "tools/trajectory_evolution/experiments/business-coverage"
sys.path.insert(0, str(BUSINESS))

import progress_report as _business_report  # noqa: E402
import bfcl_experiment as _bfcl_experiment  # noqa: E402


def snapshot():
    data = _business_report.snapshot(_bfcl_experiment.OUTPUT)
    # The generic reporter intentionally ignores benchmark-specific terminal
    # output.  Merge the authoritative BFCL host aggregates so training
    # diagnostics expose intent/F1 and host loop counts without changing the
    # formal holdout comparison logic.
    try:
        host = (_bfcl_experiment.status() or {}).get("l0") or {}
    except Exception:
        host = {}
    if host:
        training = data.setdefault("training", {})
        try:
            plan = json.loads(_bfcl_experiment.PLAN.read_text())
            training["planned"] = int((plan.get("counts") or {}).get("train", 400))
        except Exception:
            training["planned"] = 400
        metrics = training.setdefault("metrics", {})
        mapping = {
            "quality": "quality", "strict_passed": "strict_passed",
            "avg_invalid_loop": "invalid_loop", "avg_latency_ms": "latency_ms",
            "avg_tokens": "tokens", "avg_model_calls": "llm_calls",
            "intent_f1": "intent_f1", "intent_recognition_rate": "intent_recognition_rate",
        }
        for source, target in mapping.items():
            if source in host:
                metrics[target] = host.get(source)
        metrics["intent_measured"] = host.get("completed", 0)
        metrics["level_counts"] = {
            "L1": int(host.get("l1_applied", 0) or 0),
            "L2": int(host.get("l2_applied", 0) or 0),
            "L3": int(host.get("l3_applied", 0) or 0),
        }
        data["training"]["host_metrics"] = host
    return data


def markdown(data: dict) -> str:
    # Before holdout there is no valid L0/parent/current comparison.  Keep the
    # output to a compact training diagnostic table instead of printing an
    # empty formal comparison table.
    if not data.get("holdout", {}).get("reports"):
        training = data.get("training", {})
        metrics = training.get("metrics", {})
        levels = metrics.get("level_counts") or {}
        fmt = lambda value: "暂无" if value is None else (f"{value:.3f}" if isinstance(value, float) else str(value))
        rows = [
            ("已完成题数", f"{training.get('completed', 0)}/{training.get('planned', 0)}"),
            ("已生成训练报告", training.get("reports", 0)),
            ("有效评分题数", training.get("host_metrics", {}).get("scored", "暂无")),
            ("错误题数", training.get("host_metrics", {}).get("errored", "暂无")),
            ("质量（训练诊断）", fmt(metrics.get("quality"))),
            ("严格通过数", fmt(metrics.get("strict_passed"))),
            ("平均无效 loop", fmt(metrics.get("invalid_loop"))),
            ("平均 token", fmt(metrics.get("tokens"))),
            ("平均模型调用", fmt(metrics.get("llm_calls"))),
            ("平均时延 ms", fmt(metrics.get("latency_ms"))),
            ("意图识别率", fmt(metrics.get("intent_recognition_rate"))),
            ("意图 F1", fmt(metrics.get("intent_f1"))),
            ("L1 实际应用", levels.get("L1", 0)),
            ("L2 实际应用", levels.get("L2", 0)),
            ("L3 实际应用", levels.get("L3", 0)),
        ]
        lines = [
            f"时间：{time.strftime('%Y-%m-%d %H:%M:%S %z')}",
            f"L0训练诊断（非GEPA基线）：{training.get('status', '暂无')} / "
            f"{training.get('phase', '暂无')} / batch {training.get('batch', '暂无')}",
            "",
            "| 训练诊断指标 | 数据 |",
            "|---|---:|",
        ]
        lines.extend(f"| {name} | {value} |" for name, value in rows)
        lines.extend(["", "正式 holdout 尚未开始，因此不展示 L0/父节点/当前节点正式对照表。"])
        return "\n".join(lines)
    return _business_report.markdown(data)


def main(argv: list[str] | None = None) -> int:
    import argparse
    import json
    import time

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--watch", nargs="?", const=60.0, type=float)
    args = parser.parse_args(argv)
    while True:
        started = time.monotonic()
        data = snapshot()
        print(json.dumps(data, ensure_ascii=False, separators=(",", ":"))
              if args.json else markdown(data), flush=True)
        if args.watch is None:
            return 0
        time.sleep(max(0.1, args.watch - (time.monotonic() - started)))


if __name__ == "__main__":
    raise SystemExit(main())
