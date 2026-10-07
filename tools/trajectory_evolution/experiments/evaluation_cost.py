#!/usr/bin/env python3
"""Disclose cumulative Agent execution cost across resumable evaluation reports.

Only Agent duration/tokens/llm_calls are counted. Judge and GEPA model/API costs
are excluded. This module does not calculate quality or modify reports/gates.

Example: python3 evaluation_cost.py report-1.json report-2.json \
    --planned candidate/train=32 --output agent-cost.json
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import json
import math
from pathlib import Path

FIELDS = ("duration_ms", "tokens", "llm_calls")


def _number(value):
    return (float(value) if isinstance(value, (int, float)) and not isinstance(value, bool)
            and math.isfinite(value) and value >= 0 else None)


def _run_ids(row):
    evidence = row.get("evidence") or {}
    ids = evidence.get("execution_run_ids") or []
    if not ids and evidence.get("execution_run_id"):
        ids = [evidence["execution_run_id"]]
    if not ids:
        traces = evidence.get("traces") or [evidence.get("trace")]
        ids = [(trace.get("identity") or {}).get("run_id") for trace in traces
               if isinstance(trace, dict)]
    return tuple(sorted({value for value in ids if isinstance(value, str) and value}))


def aggregate_agent_cost(reports, planned_counts=None):
    """Read report paths or in-memory dictionaries; return a small cost sidecar.

    planned_counts optionally maps (variant_id, split) to its planned trial count.
    Otherwise the count is the distinct (task_id, task_version, repetition) slots.
    Repeated trial IDs, including judge-only retries, count once. Different trial
    IDs with identical known Agent run IDs also count once. Repeated observations
    retain the maximum available cost per field, never their sum.
    """
    by_id = {}
    source_reports = []
    row_counts = defaultdict(int)
    slots = defaultdict(set)
    for report in reports:
        if isinstance(report, (str, Path)):
            source_reports.append(str(report))
            payload = json.loads(Path(report).read_text())
        else:
            payload = report
        for row in payload.get("trials", []):
            trial_id = row.get("trial_id")
            if not isinstance(trial_id, str) or not trial_id:
                raise ValueError("trial_id_required_for_cost_deduplication")
            group = (str(row.get("variant_id") or ""), str(row.get("split") or ""))
            task_id = row.get("task_id")
            if not all(group) or not isinstance(task_id, str) or not task_id:
                raise ValueError("variant_split_and_task_required")
            slot = (task_id, row.get("task_version"), row.get("repetition", 0))
            runs = _run_ids(row)
            metrics = row.get("metrics") or {}
            costs = {"duration_ms": _number(row.get("duration_ms")),
                     "tokens": _number(metrics.get("tokens")),
                     "llm_calls": _number(metrics.get("llm_calls"))}
            row_counts[group] += 1
            slots[group].add(slot)
            previous = by_id.get(trial_id)
            if previous is not None:
                if (previous["group"] != group or previous["slot"] != slot or
                        (previous["runs"] and runs and previous["runs"] != runs)):
                    raise ValueError("conflicting_trial_identity")
                previous["runs"] = previous["runs"] or runs
                for field, value in costs.items():
                    if value is not None:
                        previous["costs"][field] = max(previous["costs"].get(field) or 0, value)
            else:
                by_id[trial_id] = {"group": group, "slot": slot, "runs": runs, "costs": costs}

    executions = {}
    unique_ids = defaultdict(int)
    for trial_id, record in by_id.items():
        group = record["group"]
        unique_ids[group] += 1
        key = (group, "runs", record["runs"]) if record["runs"] else (group, "trial", trial_id)
        prior = executions.get(key)
        if prior is None:
            executions[key] = record
        else:
            if prior["slot"] != record["slot"]:
                raise ValueError("same_agent_runs_with_different_task_slots")
            for field, value in record["costs"].items():
                if value is not None:
                    prior["costs"][field] = max(prior["costs"].get(field) or 0, value)

    groups = []
    for group in sorted(row_counts):
        records = [record for record in executions.values() if record["group"] == group]
        planned = (planned_counts or {}).get(group, len(slots[group]))
        if not isinstance(planned, int) or isinstance(planned, bool) or planned < len(slots[group]):
            raise ValueError("planned_count_must_cover_observed_task_slots")
        totals = {field: math.fsum(record["costs"][field] or 0 for record in records)
                  for field in FIELDS}
        missing = {field: sum(record["costs"][field] is None for record in records)
                   for field in FIELDS}
        groups.append({"variant_id": group[0], "split": group[1], "planned_trials": planned,
            "observed_task_slots": len(slots[group]), "report_rows": row_counts[group],
            "unique_trial_ids": unique_ids[group], "agent_executions": len(records),
            "extra_executions": max(0, len(records)-len(slots[group])),
            "deduplicated_report_rows": row_counts[group]-len(records),
            "totals": totals,
            "mean_per_planned_trial_including_retries": {
                field: value/planned if planned else None for field, value in totals.items()},
            "mean_per_agent_execution": {field: value/len(records) for field, value in totals.items()},
            "missing_cost_executions": missing, "cost_complete": not any(missing.values())})
    return {"schema_version": 1, "scope": "agent_execution_only",
            "excluded_costs": ["judge_model_and_API", "GEPA_reflection_and_selection_model_and_API"],
            "deduplication": "unique_trial_id_and_identical_known_agent_run_ids",
            "repeated_cost_observation": "maximum_available_value_per_execution_per_field",
            "source_reports": source_reports, "groups": groups,
            "totals": {field: math.fsum(group["totals"][field] for group in groups) for field in FIELDS}}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", nargs="+", type=Path)
    parser.add_argument("--planned", action="append", default=[], metavar="VARIANT/SPLIT=COUNT")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    planned = {}
    for value in args.planned:
        name, count = value.rsplit("=", 1)
        variant, split = name.split("/", 1)
        planned[(variant, split)] = int(count)
    result = aggregate_agent_cost(args.reports, planned)
    text = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        if args.output.resolve() in {path.resolve() for path in args.reports}:
            parser.error("output must not overwrite an input report")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
