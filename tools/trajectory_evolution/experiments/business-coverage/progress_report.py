#!/usr/bin/env python3
"""只读实验进度播报。

这个脚本只读取 ``trace_output/business-simulator``，不会启动模型、改写报告或
追加日志。默认打印一张当前快照；``--watch`` 可在 tmux 中每隔指定秒数重复
打印。训练报告只取数字批次目录，自动忽略 ``*-attempt-*`` 重试产物。
"""

from __future__ import annotations

import argparse
from collections import Counter
import json
import math
import re
import sys
import time
from pathlib import Path
from typing import Any, Iterable


HERE = Path(__file__).resolve().parent
ROOT = Path(__file__).resolve().parents[4]
DEFAULT_OUTPUT = ROOT / "trace_output" / "business-simulator"
_NUMBERED_BATCH = re.compile(r"^[0-9]+$")
_NODE = re.compile(r"(?:node|candidate)[-_]?([0-9]+)", re.IGNORECASE)


def _read(path: Path) -> dict[str, Any] | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return None
    return value if isinstance(value, dict) else None


def _number(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)) and math.isfinite(float(value)):
        return float(value)
    return None


def _mean(values: Iterable[float]) -> float | None:
    values = list(values)
    return sum(values) / len(values) if values else None


_INVALID_LOOP_STATUSES = {
    "error", "failed", "invalid", "not_reviewed", "uncertain", "unknown",
}
_VALID_LOOP_STATUSES = {"complete", "completed", "measured", "valid", "accepted"}
_EXECUTION_ERROR_STATUSES = {
    "aborted", "cancelled", "canceled", "crashed", "error", "failed",
    "timeout", "timed_out",
}


def _trial_error_code(trial: dict[str, Any]) -> str | None:
    """Return a stable task error code, if this trial was not fully judged.

    Evaluator reports intentionally retain a semantic assessment alongside a
    failed judge/contract (for diagnostics and retry).  Those values are not
    scores: counting them would turn a judge outage into quality=0 and an
    unreviewed loop list into invalid_loop=0.  Keep the original error code
    where available and use short, deterministic fallbacks for older reports.
    """
    raw = trial.get("error_code")
    if isinstance(raw, str) and raw.strip():
        return raw.strip()
    if str(trial.get("verdict") or "").lower() == "error":
        return "trial_error"
    execution_status = str(trial.get("execution_status") or "").strip().lower()
    if execution_status in _EXECUTION_ERROR_STATUSES:
        return f"execution_{execution_status}"
    evidence = trial.get("evidence")
    if isinstance(evidence, dict):
        if evidence.get("judge_validation_error") is not None:
            return "judge_validation_error"
        for key in ("contract", "judge", "evaluation"):
            section = evidence.get(key)
            if isinstance(section, dict) and str(section.get("status") or "").lower() in {
                    "error", "invalid", "failed"}:
                return f"{key}_invalid"
        assessment = evidence.get("assessment")
        if isinstance(assessment, dict) and str(assessment.get("status") or "").lower() in {
                "error", "invalid"}:
            return "assessment_invalid"
    checks = trial.get("checks")
    if isinstance(checks, list):
        for check in checks:
            if not isinstance(check, dict) or str(check.get("status") or "").lower() != "error":
                continue
            check_id = str(check.get("check_id") or "").lower()
            # Model/contract check failures are evaluator failures even in
            # legacy reports that did not copy error_code to the trial root.
            if (check_id == "model" or "judge" in check_id or "contract" in check_id or
                    check_id.startswith("evaluation")):
                reason = check.get("reason")
                return str(reason).strip() if isinstance(reason, str) and reason.strip() else "evaluation_check_error"
    return None


def _trial_is_partial(trial: dict[str, Any]) -> bool:
    """Whether this row is partial/uncertain and should be shown explicitly."""
    if _trial_error_code(trial) is not None:
        return True
    verdict = str(trial.get("verdict") or "").lower()
    if verdict in {"partial", "uncertain"}:
        return True
    evidence = trial.get("evidence")
    assessment = evidence.get("assessment") if isinstance(evidence, dict) else None
    if not isinstance(assessment, dict):
        return False
    status = str(assessment.get("status") or "").lower()
    if status in {"partial", "incomplete", "uncertain"}:
        return True
    quality = _number(assessment.get("quality"))
    quality_upper = _number(assessment.get("quality_upper"))
    coverage = _number(assessment.get("coverage"))
    return ((quality is not None and quality_upper is not None and quality < quality_upper) or
            (coverage is not None and coverage < 1.0))


def _metric(trial: dict[str, Any], name: str) -> float | None:
    evidence = trial.get("evidence")
    evidence_metrics = evidence.get("metrics") if isinstance(evidence, dict) else None
    for source in (trial.get("metrics"), evidence_metrics):
        if isinstance(source, dict):
            value = _number(source.get(name))
            if value is not None:
                return value
    return None


def _quality(trial: dict[str, Any]) -> float | None:
    if _trial_error_code(trial) is not None:
        return None
    evidence = trial.get("evidence")
    assessment = evidence.get("assessment") if isinstance(evidence, dict) else None
    if isinstance(assessment, dict):
        value = _number(assessment.get("quality"))
        if value is not None:
            return value
    value = _number(trial.get("quality"))
    if value is not None:
        return value
    # 只有缺少 quality 字段时才用严格 verdict，避免覆盖真实裁判分数。
    if trial.get("verdict") == "passed":
        return 1.0
    if trial.get("verdict") in {"failed", "rejected"}:
        return 0.0
    return None


def _invalid_loops(trial: dict[str, Any]) -> float | None:
    if _trial_error_code(trial) is not None:
        return None
    evidence = trial.get("evidence")
    review = evidence.get("loop_review") if isinstance(evidence, dict) else None
    if isinstance(review, dict):
        status = str(review.get("status") or "").strip().lower()
        if status in _INVALID_LOOP_STATUSES or (status and status not in _VALID_LOOP_STATUSES):
            return None
    for key in ("invalid_loops", "invalid_loop_count"):
        value = _number(trial.get(key))
        if value is not None:
            return value
    if isinstance(evidence, dict):
        if isinstance(review, dict):
            value = _number(review.get("count"))
            if value is not None:
                return value
            wasted = review.get("wasted_iterations")
            if isinstance(wasted, list):
                return float(len(wasted))
            value = _number(review.get("wasted_iterations"))
            if value is not None:
                return value
        for key in ("invalid_loops", "invalid_loop_count"):
            value = _number(evidence.get(key))
            if value is not None:
                return value
    return None


def _intent_counts(trial: dict[str, Any]) -> tuple[int, int, int] | None:
    evidence = trial.get("evidence")
    observations = evidence.get("intent_observations") if isinstance(evidence, dict) else None
    if not isinstance(observations, list):
        return None
    tp = fp = fn = 0
    measured = False
    for item in observations:
        if not isinstance(item, dict) or item.get("status") not in {
            "measured", "complete", "partial", "conditional_review"
        }:
            continue
        # These arrays are produced by the evaluator and are the only fields
        # used for the micro-F1 shown here.
        matched = item.get("matched")
        extra = item.get("extra")
        missing = item.get("missing")
        if not all(isinstance(v, list) for v in (matched, extra, missing)):
            continue
        measured = True
        tp += len(matched)
        fp += len(extra)
        fn += len(missing)
    return (tp, fp, fn) if measured else None


def _explicit_level_counts(value: Any) -> dict[str, int]:
    """Extract actual applied L1/L2/L3 receipts, without treating config as use."""
    counts = {"L1": 0, "L2": 0, "L3": 0}

    def walk(item: Any, context: dict[str, Any] | None = None) -> None:
        if isinstance(item, list):
            for child in item:
                walk(child, context)
            return
        if not isinstance(item, dict):
            return
        lower = {str(k).lower(): v for k, v in item.items()}
        level = None
        for key in ("level", "policy_level", "application_level", "requested_level"):
            raw = lower.get(key)
            if isinstance(raw, str) and raw.upper() in counts:
                level = raw.upper()
                break
        status = lower.get("status")
        application_status = lower.get("application_status")
        applied = (lower.get("applied") is True or
                   (isinstance(status, str) and status in {"applied", "accepted", "used"}) or
                   (isinstance(application_status, str) and
                    application_status in {"applied", "accepted", "used"}))
        if level and applied:
            counts[level] += 1
        # Summary counters are accepted only when their key explicitly says
        # application/use; a service configuration's max_level is not a receipt.
        for key, raw in lower.items():
            match = re.fullmatch(r"(?:application[_-]?)?(l[123])(?:[_-]?(?:applications?|applied|uses?|count))?", key)
            if match and any(token in key for token in ("application", "applied", "use", "count")):
                n = _number(raw)
                if n is not None:
                    counts[match.group(1).upper()] += int(n)
        for key, child in item.items():
            # Avoid descending into generic configuration where level names
            # describe capability rather than an observed application.
            if str(key).lower() in {"configuration", "config", "options", "capabilities"}:
                continue
            walk(child, item)

    walk(value)
    return counts


_RECEIPT_LEVELS = {
    "llm": "L1",
    "actions": "L2",
    "schedule": "L2",
    "loop": "L3",
}


def _level_counts(value: Any) -> tuple[dict[str, int], bool]:
    """Count observed applied receipts, returning (counts, evidence_seen).

    The runtime deliberately records the effective boundary on the receipt,
    rather than copying a configured ``requested_level`` into every trial.
    Counting only ``advice.applied`` events avoids treating policy capability
    declarations or rejected applications as successful L1/L2/L3 use.  The
    boolean lets callers distinguish a measured zero from missing telemetry.
    """
    counts = {"L1": 0, "L2": 0, "L3": 0}
    seen = False
    seen_events: set[tuple[Any, ...]] = set()

    def inspect_event(item: Any) -> None:
        nonlocal seen
        if not isinstance(item, dict):
            return
        if item.get("kind") == "advice.applied":
            attrs = item.get("attributes")
            attrs = attrs if isinstance(attrs, dict) else {}
            receipt = attrs.get("receipt")
            receipt = receipt if isinstance(receipt, dict) else {}
            application = item.get("application")
            application = application if isinstance(application, dict) else {}
            receipt_application = receipt.get("application")
            receipt_application = (receipt_application
                                   if isinstance(receipt_application, dict) else {})
            status = application.get("status") or receipt_application.get("status")
            boundary = receipt.get("boundary") or item.get("boundary")
            level = _RECEIPT_LEVELS.get(str(boundary).lower())
            if level and isinstance(status, str):
                # A report may retain both an outer event and a copied event;
                # event_id is authoritative, with stable fields as fallback.
                key = (item.get("event_id"), item.get("execution_id"),
                       item.get("sequence"), item.get("step_id"), boundary)
                if key not in seen_events:
                    seen_events.add(key)
                    seen = True
                    if status == "applied":
                        counts[level] += 1

    # Evidence also contains large output/state/trajectory fields.  Receipts
    # have one stable location, so avoid recursively walking those payloads on
    # every 60-second progress scan.
    trace_values: list[Any] = []
    traces = value.get("traces") if isinstance(value, dict) else None
    if isinstance(traces, list):
        trace_values.extend(traces)
    trace = value.get("trace") if isinstance(value, dict) else None
    if isinstance(trace, dict):
        trace_values.append(trace)
    elif isinstance(trace, list):
        trace_values.extend(trace)
    for trace_item in trace_values:
        if not isinstance(trace_item, dict):
            continue
        events = trace_item.get("events")
        if isinstance(events, list):
            for event in events:
                inspect_event(event)
    return counts, seen


def _trial_rows(report: dict[str, Any], split: str | None = None) -> list[dict[str, Any]]:
    rows = report.get("trials")
    if not isinstance(rows, list):
        return []
    result = []
    for row in rows:
        if not isinstance(row, dict):
            continue
        if split and str(row.get("split", "")).lower() != split:
            continue
        result.append(row)
    return result


def _aggregate(trials: list[dict[str, Any]], planned: int | None = None,
               summary: dict[str, Any] | None = None) -> dict[str, Any]:
    qualities = [v for v in (_quality(t) for t in trials) if v is not None]
    loops = [v for v in (_invalid_loops(t) for t in trials) if v is not None]
    latency = [v for v in (_number(t.get("duration_ms")) for t in trials) if v is not None]
    tokens = [v for v in (_metric(t, "tokens") for t in trials) if v is not None]
    calls = [v for v in (_metric(t, "llm_calls") for t in trials) if v is not None]
    # Intent evidence is part of the judge packet.  Do not let a packet that
    # failed its judge/contract validation improve the aggregate F1 either.
    valid_trials = [t for t in trials if _trial_error_code(t) is None]
    intent = [_intent_counts(t) for t in valid_trials]
    intent = [x for x in intent if x is not None]
    tp, fp, fn = (sum(x[i] for x in intent) for i in range(3)) if intent else (0, 0, 0)
    f1 = (2 * tp / (2 * tp + fp + fn)) if intent and (2 * tp + fp + fn) else None
    summary = summary or {}
    if not loops and (not trials or valid_trials):
        fallback = summary.get("invalid_loop_assessment")
        if isinstance(fallback, dict) and _number(fallback.get("mean")) is not None:
            loops = [float(fallback["mean"])]
    if not intent and (not trials or valid_trials):
        fallback = summary.get("multi_intent_observation")
        if isinstance(fallback, dict) and _number(fallback.get("f1")) is not None:
            f1 = float(fallback["f1"])
    counts = {"L1": 0, "L2": 0, "L3": 0}
    level_evidence = False
    for trial in trials:
        values, receipt_evidence = _level_counts(trial.get("evidence", {}))
        if receipt_evidence:
            level_evidence = True
            for key in counts:
                counts[key] += values[key]
        else:
            # Older/synthetic reports may carry explicit application counters
            # but no trace receipts. Keep that compatibility path while never
            # inferring use from a requested/max level alone.
            values = _explicit_level_counts(trial.get("evidence", {}))
            if any(values.values()):
                level_evidence = True
                for key in counts:
                    counts[key] += values[key]
    verdicts = [t.get("verdict") for t in valid_trials
                if t.get("verdict") in {"passed", "failed", "rejected"}]
    strict = sum(v == "passed" for v in verdicts) if verdicts else None
    error_counts = Counter(
        code for code in (_trial_error_code(t) for t in trials) if code is not None
    )
    partial = sum(_trial_is_partial(t) for t in trials)
    observed_planned = planned if planned is not None else len(trials)
    missing = max(0, observed_planned - len(trials))
    quality_complete = bool(trials) and not error_counts and len(qualities) == len(trials) \
        and len(trials) >= observed_planned
    return {
        "completed": len(trials),
        "planned": observed_planned,
        "scored": len(qualities),
        "unscored": max(0, len(trials) - len(qualities)),
        "quality": _mean(qualities),
        "quality_status": ("complete" if quality_complete else
                           ("partial" if qualities else "unavailable")),
        "quality_complete": quality_complete,
        "partial": partial,
        "missing": missing,
        "errors": sum(error_counts.values()),
        "error_counts": dict(sorted(error_counts.items())),
        "judged": len(verdicts),
        "strict_passed": strict,
        "invalid_loop": _mean(loops),
        "latency_ms": _mean(latency),
        "tokens": _mean(tokens),
        "llm_calls": _mean(calls),
        "intent_f1": f1,
        "intent_measured": len(intent),
        "level_counts": counts if level_evidence else None,
    }


def _is_attempt(path: Path) -> bool:
    return any("-attempt-" in part.lower() or part.lower().startswith("attempt-")
               for part in path.parts)


def _canonical_reports(directory: Path, training: bool = False) -> list[tuple[Path, dict[str, Any]]]:
    if not directory.is_dir():
        return []
    paths = []
    for path in directory.rglob("report.json"):
        if _is_attempt(path):
            continue
        if training and (not path.parent.name.isdigit() or not _NUMBERED_BATCH.fullmatch(path.parent.name)):
            continue
        paths.append(path)
    seen: set[str] = set()
    result = []
    for path in sorted(paths):
        data = _read(path)
        if not data:
            continue
        identity = str(data.get("run_id") or path)
        if identity in seen:
            continue
        seen.add(identity)
        result.append((path, data))
    return result


def _training_snapshot(output: Path) -> dict[str, Any]:
    status = _read(output / "training" / "status.json") or {}
    reports = _canonical_reports(output / "training", training=True)
    rows: list[dict[str, Any]] = []
    for _, report in reports:
        rows.extend(_trial_rows(report, "train"))
    expected = int(_number(status.get("planned_tasks")) or 0) or None
    aggregate = _aggregate(rows, expected, {})
    return {
        "status": status.get("status", "暂无"),
        "phase": status.get("phase", "暂无"),
        "batch": status.get("batch", "暂无"),
        "completed": int(_number(status.get("captured_tasks")) or aggregate["completed"]),
        "planned": expected or aggregate["planned"],
        "reports": len(reports),
        "metrics": aggregate,
    }


def _node_hint(path: Path, report: dict[str, Any]) -> str | None:
    text = "/".join(path.parts)
    match = _NODE.search(text)
    if match:
        return f"node-{int(match.group(1))}"
    for key in ("node", "candidate", "candidate_id", "node_id"):
        value = report.get(key)
        if value not in (None, ""):
            return str(value)
    return None


def _job_node_hints(gepa: Path) -> tuple[dict[str, str], dict[str, str]]:
    """Recover node/parent labels from the tiny GEPA state/tree metadata."""
    state = _read(gepa / "workspace" / "agent-evaluation" / "state.json") or {}
    jobs = state.get("jobs") if isinstance(state.get("jobs"), dict) else {}
    hints: dict[str, str] = {}
    parents: dict[str, str] = {}
    for job_id, job in jobs.items():
        if not isinstance(job, dict):
            continue
        search = job.get("search") if isinstance(job.get("search"), dict) else {}
        selection = search.get("final_holdout_selection") or search.get("gate_winner")
        node = selection.get("node") if isinstance(selection, dict) else None
        if isinstance(node, (int, float)) and not isinstance(node, bool):
            hints[str(job_id)] = f"node-{int(node)}"
        # The search tree is authoritative for the selected node's parent.
        tree_path = gepa / "workspace" / "agent-evaluation" / str(job_id) / "search" / "tree.json"
        tree = _read(tree_path) or {}
        nodes = tree.get("nodes")
        if isinstance(nodes, list):
            for index, item in enumerate(nodes):
                if not isinstance(item, dict):
                    continue
                parent = item.get("parent")
                if isinstance(parent, (int, float)) and not isinstance(parent, bool):
                    parents[f"node-{index}"] = f"node-{int(parent)}"
    return hints, parents


def _holdout_tree_context(tree_path: Path, state: dict[str, Any]) -> dict[str, Any]:
    """读取一个 GEPA tree 的极小留出映射。

    GEPA 的真实报告位于 ``evaluations/*/attempt-N/report.json``，这些 attempt
    目录同时包含搜索期的 train/val 检查点。只从 tree 的 ``evaluations`` 引用
    中挑选 holdout 行，既不会把 validation 冒充留出，也能在报告仍处于恢复
    attempt 时找到最新的真实留出证据。
    """
    tree = _read(tree_path) or {}
    job_id = tree_path.parent.parent.name
    jobs = state.get("jobs") if isinstance(state.get("jobs"), dict) else {}
    job = jobs.get(job_id) if isinstance(jobs, dict) else {}
    job = job if isinstance(job, dict) else {}
    search = job.get("search") if isinstance(job.get("search"), dict) else {}
    selection = search.get("final_holdout_selection") or search.get("gate_winner")
    selection = selection if isinstance(selection, dict) else {}
    selected_version = str(selection.get("version") or "")
    selected_node = selection.get("node")
    if not selected_version:
        selected_version = str(tree.get("selected_version") or "")
    final = tree.get("final_holdout")
    candidates = final.get("candidates") if isinstance(final, dict) else None
    if not selected_version and isinstance(candidates, list):
        # 留出仍在运行时没有 gate_winner；rank 1 是冻结的当前候选，
        # 只用于播报，不改变原生发布选择。
        for item in candidates:
            if isinstance(item, dict) and item.get("version"):
                selected_version = str(item["version"])
                selected_node = item.get("node")
                break
    nodes = tree.get("nodes") if isinstance(tree.get("nodes"), list) else []
    parent_versions: set[str] = set()
    if isinstance(selected_node, (int, float)) and not isinstance(selected_node, bool):
        index = int(selected_node)
        if 0 <= index < len(nodes) and isinstance(nodes[index], dict):
            node = nodes[index]
            parents = node.get("parents")
            if not isinstance(parents, list):
                parents = [node.get("parent")]
            for parent in parents:
                if isinstance(parent, (int, float)) and not isinstance(parent, bool):
                    parent_index = int(parent)
                    if 0 <= parent_index < len(nodes) and isinstance(nodes[parent_index], dict):
                        version = nodes[parent_index].get("version")
                        if version:
                            parent_versions.add(str(version))
    refs: list[dict[str, Any]] = []
    evaluations = tree.get("evaluations")
    if isinstance(evaluations, dict):
        for row in evaluations.values():
            if not isinstance(row, dict) or str(row.get("split", "")) != "holdout":
                continue
            report = row.get("report")
            if not isinstance(report, str) or not report or Path(report).is_absolute():
                continue
            relative = Path(report)
            if any(part in {"", ".", ".."} for part in relative.parts):
                continue
            path = tree_path.parent / relative
            if not path.is_file() or path.name != "report.json":
                continue
            # Final paired rows carry baseline_version. Candidate-only rows are
            # retained only when their version matches the frozen parent/current
            # identity; the labeler below drops unrelated diagnostics.
            refs.append({
                "path": path,
                "candidate_version": str(row.get("version") or ""),
                "baseline_version": str(row.get("baseline_version") or ""),
                "planned": (len(row.get("task_ids"))
                            if isinstance(row.get("task_ids"), list) else None),
                "status": str(row.get("status") or ""),
                "job_id": job_id,
                "selected_version": selected_version,
                "parent_versions": parent_versions,
                "tree": tree,
            })
    return {"job_id": job_id, "selected_version": selected_version,
            "parent_versions": parent_versions, "refs": refs, "tree": tree}


def _variant_versions(report: dict[str, Any]) -> dict[str, str]:
    configuration = report.get("configuration")
    variants = configuration.get("variants") if isinstance(configuration, dict) else None
    result: dict[str, str] = {}
    if isinstance(variants, list):
        for item in variants:
            if isinstance(item, dict) and item.get("id"):
                result[str(item["id"])] = str(item.get("version") or "")
    return result


def _holdout_report_labels(report: dict[str, Any], reference: dict[str, Any]) -> dict[str, list[dict[str, Any]]]:
    """按冻结版本把一份报告拆为 L0/parent/current 三个播报桶。"""
    trials = _trial_rows(report, "holdout")
    variants = _variant_versions(report)
    baseline_version = reference.get("baseline_version") or ""
    candidate_version = reference.get("candidate_version") or ""
    selected_version = reference.get("selected_version") or ""
    parent_versions = set(reference.get("parent_versions") or ())
    contextual = bool(reference.get("job_id")) and bool(
        baseline_version or candidate_version or selected_version or parent_versions)
    buckets: dict[str, list[dict[str, Any]]] = {}
    for trial in trials:
        variant = str(trial.get("variant_id") or "")
        version = variants.get(variant, "")
        lower = variant.lower()
        label: str | None = None
        if "baseline" in lower or lower in {"l0", "control"} or (
                baseline_version and version == baseline_version):
            label = "L0"
        elif "parent" in lower or (version and version in parent_versions):
            label = "parent"
        elif contextual:
            if selected_version and (version == selected_version or
                                     (not version and candidate_version == selected_version)):
                label = "current"
            elif not selected_version and len({t.get("variant_id") for t in trials}) > 1:
                label = "current"
        elif len({t.get("variant_id") for t in trials}) > 1:
            # 保留原有对 synthetic final_holdout.json 的兼容行为。
            label = "current" if "baseline" not in lower else "L0"
        if label:
            buckets.setdefault(label, []).append(trial)
    return buckets


def _holdout_snapshot(output: Path) -> dict[str, Any]:
    reports: list[tuple[Path, dict[str, Any], dict[str, Any]]] = []
    gepa = output / "gepa"
    job_hints, node_parents = _job_node_hints(gepa)
    state = _read(gepa / "workspace" / "agent-evaluation" / "state.json") or {}
    referenced: dict[str, dict[str, Any]] = {}
    # Only tree-referenced holdout rows are considered in a live GEPA workspace.
    # This avoids loading every intermediate attempt and gives us the frozen
    # candidate/parent identity needed for the three-row comparison.
    if gepa.is_dir():
        for tree_path in gepa.rglob("tree.json"):
            context = _holdout_tree_context(tree_path, state)
            for reference in context["refs"]:
                path = reference["path"]
                key = str(path.resolve())
                previous = referenced.get(key)
                # A duplicate tree reference is harmless; keep the richer row.
                if previous is None or reference.get("baseline_version"):
                    referenced[key] = reference
    for path, reference in sorted(
            referenced.items(), key=lambda item: (
                not bool(item[1].get("baseline_version")), item[0])):
        data = _read(Path(path))
        if data and _trial_rows(data, "holdout"):
            reports.append((Path(path), data, reference))
    # Keep the small standalone report convention used by tests and by future
    # canonical final_holdout.json writers. Attempt reports are already covered
    # by tree references and are deliberately not scanned a second time.
    if gepa.is_dir():
        for path in sorted(gepa.rglob("*.json")):
            if path.name not in {"report.json", "final_holdout.json", "holdout.json"} or _is_attempt(path):
                continue
            data = _read(path)
            if not data or not _trial_rows(data, "holdout"):
                continue
            key = str(path.resolve())
            if key not in referenced:
                reports.append((path, data, {"job_id": "", "parent_versions": set()}))
    groups: dict[str, list[dict[str, Any]]] = {}
    sources: dict[str, list[str]] = {}
    planned: dict[str, int] = {}
    seen_variants: set[tuple[str, str, str]] = set()
    for path, report, reference in reports:
        hint = _node_hint(path, report)
        if hint is None:
            # Reports are normally under .../agent-evaluation/<job-id>/...;
            # state.json records the actual GEPA node for this job.
            job_id = path.parent.parent.name if path.parent.name.startswith("attempt-") else ""
            hint = job_hints.get(job_id, "current")
        buckets = _holdout_report_labels(report, reference)
        for label, selected in buckets.items():
            # Retry attempts and multiple Top-K reports can point to the same
            # version. Count one selected report per label/version so 200 tasks
            # remain 200 instead of silently becoming 400/600.
            version = reference.get("selected_version") or reference.get("candidate_version") or label
            dedupe_key = (str(reference.get("job_id") or path), label, str(version))
            if dedupe_key in seen_variants:
                continue
            seen_variants.add(dedupe_key)
            groups.setdefault(label, []).extend(selected)
            expected = reference.get("planned")
            if isinstance(expected, int) and expected > 0:
                planned[label] = max(planned.get(label, 0), expected)
            sources.setdefault(label, []).append(str(path))
    nodes = sorted((k for k in groups if k != "L0"), key=str)
    if "L0" not in groups:
        rows = {"L0": None, "parent": None, "current": None}
    else:
        rows = {"L0": _aggregate(groups["L0"], planned.get("L0")),
                "parent": None, "current": None}
        if "current" in groups:
            rows["current"] = _aggregate(groups["current"], planned.get("current"))
        if "parent" in groups:
            rows["parent"] = _aggregate(groups["parent"], planned.get("parent"))
        # Backward compatibility for old reports that used node-N labels rather
        # than the version-aware labels above.
        if rows["current"] is None and nodes:
            current_node = nodes[-1]
            rows["current"] = _aggregate(groups[current_node], planned.get(current_node))
            parent_node = node_parents.get(current_node)
            if parent_node in groups and rows["parent"] is None:
                rows["parent"] = _aggregate(groups[parent_node], planned.get(parent_node))
        if rows["parent"] is None and len(nodes) > 1:
            fallback_parent = nodes[-2]
            rows["parent"] = _aggregate(groups[fallback_parent], planned.get(fallback_parent))
    return {"reports": len(reports), "rows": rows, "sources": sources}


def _active_node_context(tree: dict[str, Any]) -> dict[str, Any]:
    """Return the active GEPA node and its direct parent for live reporting."""
    nodes = tree.get("nodes") if isinstance(tree.get("nodes"), list) else []
    active = tree.get("active")
    index = int(active) if isinstance(active, (int, float)) and not isinstance(active, bool) else -1
    if not 0 <= index < len(nodes):
        eligible = {"pending", "screened", "accepted", "evaluating", "running"}
        candidates = [i for i, node in enumerate(nodes)
                      if isinstance(node, dict) and node.get("status") in eligible]
        index = max(candidates) if candidates else (len(nodes) - 1)
    current = nodes[index] if 0 <= index < len(nodes) and isinstance(nodes[index], dict) else {}
    parents = current.get("parents")
    if not isinstance(parents, list):
        parents = [current.get("parent")]
    parent_index = next((int(value) for value in parents
                         if isinstance(value, (int, float)) and not isinstance(value, bool)), None)
    parent = (nodes[parent_index] if parent_index is not None and 0 <= parent_index < len(nodes)
              and isinstance(nodes[parent_index], dict) else {})
    return {
        "current_node": f"Node {index}" if index >= 0 else "当前节点",
        "parent_node": f"Node {parent_index}" if parent_index is not None else "父节点",
        "current_version": str(current.get("version") or ""),
        "parent_version": str(parent.get("version") or ""),
        "baseline_version": str(tree.get("baseline_version") or ""),
    }


def _selection_snapshot(output: Path) -> dict[str, Any]:
    """Load the live GEPA selection/validation view (150/50 search is excluded).

    This is intentionally separate from true holdout loading.  It gives the
    periodic process report useful L0/parent/current metrics while a candidate
    is being searched, without presenting validation as holdout evidence.
    """
    gepa = output / "gepa"
    result = {"scope": "GEPA validation/selection（50题）", "reports": 0,
              "current_node": "当前节点", "parent_node": "父节点",
              "rows": {"L0": None, "parent": None, "current": None}}
    if not gepa.is_dir():
        return result
    for tree_path in sorted(gepa.rglob("tree.json")):
        tree = _read(tree_path) or {}
        context = _active_node_context(tree)
        # Prefer the newest tree, which is the only live GEPA tree in normal
        # operation; a later tree wins if a resumed workspace has leftovers.
        result.update({k: context[k] for k in ("current_node", "parent_node")})
        versions = {"L0": context["baseline_version"],
                    "parent": context["parent_version"],
                    "current": context["current_version"]}
        rows: dict[str, list[dict[str, Any]]] = {key: [] for key in versions}
        # A candidate can be evaluated in several validation batches (for
        # example 16 + 34 tasks), followed by a merged 50-task report.  Keep
        # the reports until the label is known: a complete merged report is
        # authoritative, while partial reports are unioned by task identity.
        report_groups: dict[str, list[dict[str, Any]]] = {key: [] for key in versions}
        evaluations = tree.get("evaluations")
        if not isinstance(evaluations, dict):
            continue
        for entry in evaluations.values():
            if not isinstance(entry, dict) or str(entry.get("split", "")) not in {"val", "validation"}:
                continue
            report = entry.get("report")
            if not isinstance(report, str) or not report or Path(report).is_absolute():
                continue
            relative = Path(report)
            if any(part in {"", ".", ".."} for part in relative.parts):
                continue
            path = tree_path.parent / relative
            data = _read(path)
            if not data:
                continue
            variants = _variant_versions(data)
            trials = _trial_rows(data, "val")
            if not trials:
                continue
            version = str(entry.get("version") or "")
            label = next((key for key, value in versions.items()
                          if value and value == version), None)
            if label is None:
                # A candidate-only report may omit the tree version on older
                # resumes; resolve it from the report's variant declaration.
                declared = {str(value) for value in variants.values() if value}
                label = next((key for key, value in versions.items()
                              if value and value in declared), None)
            if label is None:
                continue
            task_ids = {str(task_id) for task_id in entry.get("task_ids", [])
                        if task_id not in (None, "")}
            task_ids.update(str(trial.get("task_id")) for trial in trials
                            if trial.get("task_id") not in (None, ""))
            report_groups[label].append({
                "trials": trials,
                "task_ids": task_ids,
                "status": str(entry.get("status", "")).lower(),
            })
            result["reports"] += 1
        for label, groups in report_groups.items():
            if not groups:
                continue
            all_task_ids = set().union(*(group["task_ids"] for group in groups))
            # Once a complete report covers the whole planned task union,
            # prefer it over its smaller source batches.  This prevents a
            # merged report from being silently averaged with its inputs.
            complete = [group for group in groups
                        if group["status"] in {"complete", "completed", "finished", "done"}
                        and all_task_ids
                        and len(group["task_ids"]) >= len(all_task_ids)]
            if complete:
                selected = max(complete, key=lambda group: len(group["trials"]))
                rows[label] = selected["trials"]
                continue
            seen: set[str] = set()
            for group in groups:
                for trial in group["trials"]:
                    task_id = trial.get("task_id")
                    key = str(task_id) if task_id not in (None, "") else "#" + str(len(rows[label]))
                    if key in seen:
                        continue
                    seen.add(key)
                    rows[label].append(trial)
        planned = {
            label: len(set().union(*(group["task_ids"] for group in groups)))
            for label, groups in report_groups.items()
            if groups and any(group["task_ids"] for group in groups)
        }
        result["rows"] = {
            label: (_aggregate(values, planned.get(label)) if values else None)
            for label, values in rows.items()
        }
    return result


def snapshot(output: Path = DEFAULT_OUTPUT) -> dict[str, Any]:
    training = _training_snapshot(output)
    selection = _selection_snapshot(output)
    holdout = _holdout_snapshot(output)
    comparison = (holdout["rows"] if holdout.get("reports") else selection["rows"])
    comparison_scope = ("真实 holdout" if holdout.get("reports")
                        else selection.get("scope", "GEPA validation/selection"))
    return {"output": str(output), "training": training,
            "training_role": "l0_training_diagnostic_not_gepa_baseline",
            "selection": selection,
            "holdout": holdout,
            "comparison": comparison,
            "comparison_scope": comparison_scope}


def _fmt(value: Any) -> str:
    if value is None:
        return "暂无"
    if isinstance(value, float):
        return f"{value:.3f}"
    return str(value)


def markdown(data: dict[str, Any]) -> str:
    training = data["training"]
    selection = data.get("selection", {})
    comparison = data.get("comparison", {})
    scope = data.get("comparison_scope", "GEPA validation/selection")
    parent_name = selection.get("parent_node", "父节点")
    current_name = selection.get("current_node", "当前节点")
    lines = [
        f"时间：{time.strftime('%Y-%m-%d %H:%M:%S %z')}",
        f"L0训练诊断（非GEPA基线）：{training['status']} / {training['phase']} / batch {training['batch']}；"
        f"训练子集（canonical report）{training['completed']}/{training['planned']}，"
        f"reports={training['reports']}（排除 attempt 重试）",
        f"当前比较口径：{scope}；父节点={parent_name}；当前节点={current_name}；"
        f"真实 holdout={'已生成' if data.get('holdout', {}).get('reports') else '待开始'}",
        "",
        f"| 指标 | L0 | 父节点（{parent_name}） | 当前节点（{current_name}） |",
        "|---|---:|---:|---:|",
    ]
    rows = [comparison.get(label) for label in ("L0", "parent", "current")]
    metrics = (
        ("完成数", lambda row: f"{row['completed']}/{row['planned']}" if row else "待生成"),
        ("质量（评分/未评分）", lambda row: _quality_cell(row) if row else "待生成"),
        ("严格通过数/已判定数", lambda row: _strict_cell(row) if row else "待生成"),
        ("partial / 缺失", lambda row: _partial_cell(row) if row else "待生成"),
        ("错误数", lambda row: _error_cell(row) if row else "待生成"),
        ("平均无效 loop", lambda row: _fmt(row.get("invalid_loop")) if row else "待生成"),
        ("平均时延 ms", lambda row: _fmt(row.get("latency_ms")) if row else "待生成"),
        ("平均 tokens", lambda row: _fmt(row.get("tokens")) if row else "待生成"),
        ("平均 llm_calls", lambda row: _fmt(row.get("llm_calls")) if row else "待生成"),
        ("意图 F1", lambda row: _fmt(row.get("intent_f1")) if row else "待生成"),
        ("L1 实际应用次数", lambda row: _level_value(row, "L1")),
        ("L2 动作/编排实际应用次数", lambda row: _level_value(row, "L2")),
        ("L3 实际应用次数", lambda row: _level_value(row, "L3")),
    )
    for name, render in metrics:
        lines.append(f"| {name} | " + " | ".join(render(row) for row in rows) + " |")
    if not data.get("holdout", {}).get("reports"):
        lines.extend(["", "当前表为 GEPA 搜索/选择指标；真实 holdout 尚未开始，未用 validation 冒充 holdout。"])
    return "\n".join(lines)


def _levels(value: Any) -> str:
    if not value:
        return "暂无可归因回执"
    return "/".join(str(value.get(k, 0)) for k in ("L1", "L2", "L3"))


def _level_value(row: dict[str, Any] | None, level: str) -> str:
    if not row:
        return "待生成"
    counts = row.get("level_counts")
    if not isinstance(counts, dict):
        return "暂无可归因回执"
    return str(counts.get(level, 0))


def _quality_cell(row: dict[str, Any]) -> str:
    status = row.get("quality_status")
    suffix = f"; {status}" if status in {"partial", "unavailable"} else ""
    return f"{_fmt(row.get('quality'))} ({row.get('scored', 0)}/{row.get('unscored', 0)}){suffix}"


def _strict_cell(row: dict[str, Any]) -> str:
    passed = row.get("strict_passed")
    return f"{_fmt(passed)}/{row.get('judged', row.get('scored', 0))}"


def _partial_cell(row: dict[str, Any]) -> str:
    return f"{row.get('partial', 0)} / {row.get('missing', 0)}"


def _error_cell(row: dict[str, Any]) -> str:
    count = row.get("errors", 0)
    details = row.get("error_counts")
    if not count or not isinstance(details, dict):
        return str(count)
    return f"{count} ({', '.join(f'{key}:{value}' for key, value in details.items())})"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--json", action="store_true", help="输出 JSON 快照")
    parser.add_argument("--watch", nargs="?", const=60.0, type=float,
                        help="持续播报；默认每 60 秒一次")
    args = parser.parse_args(argv)
    while True:
        started = time.monotonic()
        data = snapshot(args.output)
        if args.json:
            print(json.dumps(data, ensure_ascii=False, separators=(",", ":")), flush=True)
        else:
            print(markdown(data), flush=True)
        if args.watch is None:
            return 0
        # Keep output cadence bounded by the requested interval even when a
        # large report takes a few seconds to scan.
        time.sleep(max(0.1, args.watch - (time.monotonic() - started)))


if __name__ == "__main__":
    raise SystemExit(main())
