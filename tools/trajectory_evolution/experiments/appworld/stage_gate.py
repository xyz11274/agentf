#!/usr/bin/env python3
"""Read-only AppWorld stage gates.

The delivery order is strict and model-free here:
same-task 32 pilot -> same-seed 100 validation -> GEPA -> three comparison500
candidates.

This module never runs a model. It reuses canonical L0 rows, rejects
incomplete or mixed-basis comparisons, and separates exploratory gain from
the release goal. A zero-loop baseline cannot prove a 15% loop reduction and
is therefore reported as blocked_unverifiable.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import shlex
import sys
from typing import Any, Iterable

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import layout
import common_goal

HERE = Path(__file__).resolve().parent
import importlib.util
_business_metrics_spec = importlib.util.spec_from_file_location(
    "_shared_business_metrics_gate", HERE.parent / "business-coverage/progress_report.py")
_business_metrics_module = importlib.util.module_from_spec(_business_metrics_spec)
_business_metrics_spec.loader.exec_module(_business_metrics_module)
business_metrics = _business_metrics_module
ROOT = HERE.parents[3]
OUTPUT = ROOT / "trace_output/appworld"
GEPA = OUTPUT / "gepa"
# Canonical full-scope baseline produced by ``reproduce.py --all``.
# Stage runners use the same current execution-contract family at each scope.
DEFAULT_L0 = layout.baseline_result(500, output_root=OUTPUT)
# Select the current published strategy by default; callers can always pin a
# different immutable strategy with --seed or APPWORLD_STAGE_SEED.
DEFAULT_SEED = OUTPUT / "published" / "seed0" / "strategy.json"
RUNNER = HERE / "compare.py"

STAGES = {
    "32": {"count": 32, "label": "pilot32", "suffix": "stage32"},
    "100": {"count": 100, "label": "validation100", "suffix": "stage100"},
    "comparison500": {"count": 500, "label": "comparison500", "suffix": "stage500"},
}
# Loop review is an independent evidence stream.  It must never make an
# otherwise scored execution disappear from the quality/resource report.
REQUIRED_NUMERIC = ("quality", "latency_ms", "tokens", "model_calls")
LEVEL_FIELDS = ("L1", "L2", "L2_action", "L2_schedule", "L3")
RESOURCE_FIELDS = ("latency_ms", "tokens", "model_calls")


def _formal_loop_basis(value: Any) -> str | None:
    """Return the business-compatible formal loop-review basis only.

    ``semantic_runtime`` is a deterministic host lower bound and remains
    available as diagnostic evidence in the raw AppWorld report.  It does not
    inspect the complete coordinator/skill round used by the business
    evaluator, so it must never contribute to a stage aggregate or gate.
    """
    if not isinstance(value, str):
        return None
    basis = value.strip()
    if basis == "model_review":
        return basis
    # Qualified/mixed labels are deliberately rejected: a stage gate must
    # prove the exact business contract rather than infer it from a string
    # that might also contain a semantic or native estimate.
    return None


def load(path: Path) -> Any:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def _number(value: Any) -> float | None:
    return (float(value) if isinstance(value, (int, float))
            and not isinstance(value, bool) and math.isfinite(value) else None)


def metric(row: dict[str, Any], name: str) -> Any:
    """Read a compact candidate or unified L0 metric with explicit aliases."""
    if name == "strict":
        return (row.get("verdict") == "passed"
                if row.get("verdict") in {"passed", "failed", "rejected"}
                and _number(metric(row, "quality")) is not None
                and not metric(row, "error") else None)
    aliases: dict[str, tuple[str, ...]] = {
        "reward": ("reward", "appworld_reward"),
        "quality": ("quality",),
        "official_quality": ("official_quality", "quality_score", "auxiliary_test_fraction",
                             "scored_only_auxiliary_test_fraction"),
        "strict": ("strict", "strict_passed"),
        "error": ("error", "error_code"),
        "latency_ms": ("duration_ms", "latency_ms", "latency"),
        "tokens": ("tokens", "known_tokens", "agent_tokens"),
        "model_calls": ("model_calls", "model_call_count", "calls"),
        "avg_invalid_loop": ("avg_invalid_loop", "invalid_loop", "invalid_loop_count"),
        "loop_basis": ("loop_basis", "invalid_loop_basis"),
        "tokens_complete": ("tokens_complete",),
        "L1": ("L1", "l1_applied"),
        "L2": ("L2", "l2_applied"),
        "L2_action": ("L2_action", "l2_action_applied"),
        "L2_schedule": ("L2_schedule", "l2_schedule_applied"),
        "L3": ("L3", "l3_applied"),
    }
    for key in aliases.get(name, (name,)):
        if key in row:
            return row[key]
    values = row.get("metrics")
    if isinstance(values, dict):
        for key in aliases.get(name, (name,)):
            if key in values:
                return values[key]
        if name in LEVEL_FIELDS:
            levels = values.get("levels")
            if isinstance(levels, dict) and name in levels:
                return levels[name]
    if name == "strict":
        return (row.get("verdict") == "passed"
                if row.get("verdict") in {"passed", "failed", "rejected"}
                and _number(metric(row, "quality")) is not None
                and not metric(row, "error") else None)
    if name == "error":
        return row.get("error_code")
    return None


def _row_missing(row: dict[str, Any], require_tokens_complete: bool = True) -> list[str]:
    missing: list[str] = []
    for name in REQUIRED_NUMERIC:
        if _number(metric(row, name)) is None:
            missing.append(name)
    if row.get("verdict") not in {"passed", "failed", "rejected"}:
        missing.append("verdict")
    if require_tokens_complete and metric(row, "tokens_complete") is not True:
        missing.append("tokens_complete")
    if row.get('execution_costs_complete') is False:
        missing.append('execution_costs_complete')
    return missing


def aggregate(rows: Iterable[dict[str, Any]], *, loop_basis: str | None = None,
              require_tokens_complete: bool = False) -> dict[str, Any]:
    """Use the one compact aggregation contract for all benchmark adapters."""
    rows = list(rows)
    result = business_metrics.compact_metrics(rows)
    if require_tokens_complete and result.get("tokens_complete") is not True:
        result["tokens_complete"] = False
        result.setdefault("invalid_rows", []).append({"error": "tokens_incomplete",
                                                        "missing": ["tokens_complete"]})
    if loop_basis is not None and loop_basis != "model_review" and result.get("avg_invalid_loop") is not None:
        result["loop_basis"] = "model_review_unavailable"
        result["avg_invalid_loop"] = None
        result["invalid_loop"] = None
        result["invalid_loop_status"] = "unavailable"
        result["loop_by_task"] = {}
        result["loop_review_coverage"] = 0
    if result.get("loop_basis") == "model_review_unavailable":
        result["loop_basis"] = None
    result["task_ids"] = [row.get("task_id") for row in rows if row.get("task_id")]
    return result


def canonical_l0(path: Path, task_ids: list[str] | None = None) -> dict[str, Any]:
    source = load(path)
    rows = source.get("items", [])
    if not isinstance(rows, list):
        raise ValueError("l0_items_not_list")
    by_id = {row.get("task_id"): row for row in rows
              if isinstance(row, dict) and row.get("task_id")}
    selected = task_ids if task_ids is not None else sorted(by_id)
    missing = [task_id for task_id in selected if task_id not in by_id]
    if missing:
        raise ValueError(f"l0_task_missing:{','.join(missing[:5])}")
    basis = source.get("metrics", {}).get("loop_basis")
    result = aggregate([by_id[task_id] for task_id in selected],
                       loop_basis=basis, require_tokens_complete=True)
    result["loop_coverage"] = source.get("metrics", {}).get("invalid_loop_coverage")
    result["source_status"] = source.get("status")
    return result


def comparison_rows(path: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    value = load(path)
    rows = [row for row in value.get("items", []) if isinstance(row, dict)]
    if not rows:
        raise ValueError(f"comparison_has_no_items:{path}")
    return value, rows


def candidate_output(stage: str, seed: Path, output: Path | None = None) -> Path:
    if output:
        return output
    seed_name = seed.parent.name
    return GEPA / f"{seed_name}-{STAGES[stage]['suffix']}"


def _candidate_loop_basis(value: dict[str, Any]) -> str | None:
    metrics = value.get("metrics")
    if not isinstance(metrics, dict):
        return None
    for name, item in metrics.items():
        if name == "L0_reused" or not isinstance(item, dict):
            continue
        basis = _formal_loop_basis(item.get("loop_basis"))
        if basis:
            return basis
    # Some compact reports store the basis per task rather than in metrics.
    for row in value.get("items", []):
        if isinstance(row, dict):
            basis = _formal_loop_basis(metric(row, "loop_basis"))
            if basis:
                return basis
    return None


def _seed_matches(value: dict[str, Any], expected: Path | None) -> bool:
    """Match the actual evaluated artifact, including reuse of the same path."""
    if expected is None:
        return True
    supplied = value.get("seed") or value.get("seed_strategy")
    if not supplied:
        return False
    try:
        expected = expected.resolve()
        if Path(str(supplied)).resolve() != expected:
            return False
        artifact = json.loads(expected.read_text(encoding="utf-8"))
        if isinstance(value.get("artifact"), dict):
            return value["artifact"] == artifact
        # The existing checkpoint already records the complete artifact. Its
        # exact items must be the rows being gated, never a new in-flight run.
        checkpoint = json.loads(Path(value["progress"]).read_text(encoding="utf-8"))
        rows = value.get("items") or []
        items = {row.get("task_id"): row for row in rows}
        return (isinstance(checkpoint.get("artifact"), dict)
                and checkpoint["artifact"] == artifact
                and len(items) == len(rows)
                and checkpoint.get("items") == items)
    except (OSError, ValueError, TypeError, KeyError):
        return False


def _relative_gain(candidate: float | None, baseline: float | None) -> float | None:
    if candidate is None or baseline is None or baseline == 0:
        return None
    return (candidate - baseline) / abs(baseline)


def _resource_checks(candidate: dict[str, Any], baseline: dict[str, Any]) -> dict[str, Any]:
    checks: dict[str, Any] = {}
    for field in RESOURCE_FIELDS:
        left, right = candidate.get(field), baseline.get(field)
        checks[field] = {
            "ok": (isinstance(left, (int, float)) and
                   isinstance(right, (int, float)) and left <= right),
            "candidate": left, "baseline": right,
            "delta": (left - right if isinstance(left, (int, float))
                      and isinstance(right, (int, float)) else None),
        }
    checks["all"] = all(item["ok"] for key, item in checks.items() if key != "all")
    return checks


def _score_loop_coverage(result: dict[str, Any]) -> bool:
    """Return whether score and formal loop evidence are complete.

    The user confirmed the same three checks for pilot, validation, and final
    release. Resource completeness remains reported without blocking them.
    """
    if result.get("errored", 0) != 0:
        return False
    if not result.get("quality_complete"):
        return False
    if result.get("invalid_loop_status") != "complete":
        return False
    for item in result.get("invalid_rows") or []:
        missing = set(item.get("missing") or ())
        # These are resource accounting fields only; all score, verdict, and
        # loop fields must still be present on every task.
        if missing - {"latency_ms", "tokens", "model_calls",
                      "tokens_complete", "execution_costs_complete"}:
            return False
    return True


def _current_judge_profiles(value: dict[str, Any], l0_source: Path) -> bool:
    """Bind compact profiles to the existing completed native grade sources."""
    import compare
    wanted = layout.judge_projection()
    baseline = load(l0_source)
    if value.get('judge_projection') != wanted or baseline.get('judge_projection') != wanted:
        return False
    if value.get('candidate_source_comparison'):
        # Derived stage views retain real larger native reports. Revalidate
        # their same-artifact trial bindings, not just compact metadata.
        try:
            import reproduce
            expected, progress = reproduce._validated_candidate_subset(
                Path(value['candidate_source_comparison']), Path(value['seed']),
                [row['task_id'] for row in value.get('items') or []], l0_source)
            if (value.get('items') != expected.get('items')
                    or load(Path(value['progress'])) != progress):
                return False
        except (OSError, ValueError, TypeError, KeyError, RuntimeError):
            return False

    def native_ready(source) -> bool:
        if not isinstance(source, str) or not source:
            return False
        path = Path(source)
        manifest = path.with_name('manifest.json')
        return (path.is_file() and manifest.is_file()
                and load(manifest).get('complete') is True
                and compare._report_judge_projection(path) == wanted)

    if not native_ready(baseline.get('source_report')):
        return False
    reference = value.get('progress')
    if not isinstance(reference, str) or not Path(reference).is_file():
        return False
    progress = load(Path(reference))
    shards = list((progress.get('shards') or {}).values())
    return bool(shards) and all(
        shard.get('execution_complete') is True and shard.get('complete') is True
        and shard.get('judge_projection') == wanted
        and native_ready(shard.get('source_report')) for shard in shards)


def _verify_against_l0(stage: str, value: dict[str, Any], rows: list[dict[str, Any]],
                       l0_source: Path, expected_seed: Path | None = None) -> tuple[bool, dict[str, Any]]:
    spec = STAGES.get(stage, {"count": 500})
    candidate_ids = [row.get("task_id") for row in rows]
    unique = len(set(candidate_ids)) == len(rows) and all(candidate_ids)
    if len(rows) != spec["count"] or not unique:
        return False, {"stage": stage, "status": "fail", "gate_status": "fail",
                       "coverage_pass": False, "goal_passed": False,
                       "reason": "task_count_or_duplicate", "completed": len(rows),
                       "expected": spec["count"]}
    if not _current_judge_profiles(value, l0_source):
        return False, {'stage': stage, 'status': 'pending', 'gate_status': 'pending',
                       'coverage_pass': False, 'goal_passed': False,
                       'reason': 'current_native_judge_profile_pending',
                       'completed': len(rows), 'expected': spec['count']}
    baseline = canonical_l0(l0_source, sorted(candidate_ids))
    candidate_basis = _candidate_loop_basis(value)
    candidate = aggregate(rows, loop_basis=candidate_basis,
                          require_tokens_complete=True)
    # Missing status is missing provenance, not an implicit success.
    status_complete = value.get("status") == "complete"
    baseline_status = baseline.get("source_status") == "complete"
    ids_match = set(candidate_ids) == set(baseline["task_ids"])
    seed_match = _seed_matches(value, expected_seed)
    coverage_pass = (status_complete and baseline_status and seed_match and ids_match
                     and candidate["scored"] == spec["count"]
                     and baseline["scored"] == spec["count"]
                     and _score_loop_coverage(candidate) and _score_loop_coverage(baseline))
    score_coverage_pass = (status_complete and baseline_status and seed_match and ids_match
                           and candidate["scored"] == spec["count"]
                           and baseline["scored"] == spec["count"]
                           and candidate["judged"] == spec["count"]
                           and baseline["judged"] == spec["count"]
                           and candidate.get("quality") is not None
                           and baseline.get("quality") is not None)

    goal = common_goal.evaluate(
        baseline, candidate,
        baseline_loop_by_task=baseline["loop_by_task"],
        candidate_loop_by_task=candidate["loop_by_task"],
        baseline_loop_basis=baseline.get("loop_basis"),
        candidate_loop_basis=candidate.get("loop_basis"),
        same_scope=ids_match,
        complete=coverage_pass,
        require_resources=False,
    )
    strict_gain = goal["strict_gain"]
    quality_gain = goal["quality_gain"]
    strict_ok = goal["checks"]["strict_relative_gain_5pct"]
    quality_ok = goal["checks"]["quality_relative_gain_5pct"]
    quality_state = ("pass" if baseline.get("quality") is not None
                     and candidate.get("quality") is not None else "blocked_unverifiable")
    baseline_loops = baseline["loop_by_task"]
    candidate_loops = candidate["loop_by_task"]
    paired_loop_ids = sorted(set(baseline_loops) & set(candidate_loops))
    loop_complete = goal["loop_complete"]
    same_basis = goal["loop_basis_match"]
    baseline_loop = baseline.get("avg_invalid_loop")
    candidate_loop = candidate.get("avg_invalid_loop")
    paired_baseline_loop = (sum(baseline_loops[key] for key in paired_loop_ids) / len(paired_loop_ids)
                            if paired_loop_ids else None)
    paired_candidate_loop = (sum(candidate_loops[key] for key in paired_loop_ids) / len(paired_loop_ids)
                             if paired_loop_ids else None)
    loop_state, loop_ok = goal["loop_state"], goal["checks"]["invalid_loop_reduction_15pct"]
    resources = _resource_checks(candidate, baseline)
    resource_known = (goal["resource_complete"] and all(
        value.get('tokens_complete') and value.get('execution_costs_complete')
        for value in (candidate, baseline)))
    # Resource results are diagnostics under the user's current contract,
    # including comparison500. Missing retry costs stay unknown.
    resource_ok = goal["checks"]["resources_not_increased"] and resource_known
    resource_state = ('blocked_unverifiable' if not resource_known else
                      ('pass' if resource_ok else 'fail'))
    checks = {
        "coverage": coverage_pass,
        "strict_relative_gain_5pct": strict_ok,
        "quality_relative_gain_5pct": quality_ok,
        "invalid_loop_reduction_15pct": loop_ok,
        "resources_not_increased": resource_ok,
    }
    required_checks = [key for key in checks if key != "resources_not_increased"]
    goal_passed = all(checks[key] for key in required_checks)
    gate_status = ("pass" if goal_passed else
                   ("blocked" if loop_state == "blocked_unverifiable"
                    or quality_state == "blocked_unverifiable" else "fail"))
    result = {
        "stage": stage, "status": gate_status, "gate_status": gate_status,
        "comparison": str(value.get("source_comparison", "")),
        "l0_source": str(l0_source), "completed": candidate["completed"],
        "scored": candidate["scored"], "coverage_pass": coverage_pass,
        "score_coverage_pass": score_coverage_pass,
        "seed_match": seed_match, "goal_passed": goal_passed,
        "gate_policy": "score_quality_loop_v1", "required_checks": required_checks,
        "resources_required": False,
        "errors": candidate["invalid_rows"],
        "baseline": {key: baseline.get(key) for key in (
            "strict_passed", "official_strict_passed", "quality", "official_quality", "avg_invalid_loop", "loop_basis",
            "metric_coverage", "loop_review_coverage", "latency_ms", "tokens", "model_calls",
            "total_tokens", "total_model_calls", "tokens_complete", "execution_costs_complete",
            "extra_execution_attempts", "retry_loop_complete", "intent_f1",
            "intent_measured", "intent_task_measured", "intent_turn_keys", "levels", "level_counts")},
        "candidate": {key: candidate.get(key) for key in (
            "strict_passed", "official_strict_passed", "quality", "official_quality", "avg_invalid_loop", "loop_basis",
            "metric_coverage", "loop_review_coverage", "latency_ms", "tokens", "model_calls",
            "total_tokens", "total_model_calls", "tokens_complete", "execution_costs_complete",
            "extra_execution_attempts", "retry_loop_complete", "intent_f1",
            "intent_measured", "intent_task_measured", "intent_turn_keys", "levels", "level_counts")},
        "exploration_gain": {
            "strict_delta": (candidate["strict_passed"] - baseline["strict_passed"]
                             if candidate["strict_passed"] is not None
                             and baseline["strict_passed"] is not None else None),
            "strict_relative_gain": strict_gain,
            "quality_delta": (candidate["quality"] - baseline["quality"]
                              if candidate["quality"] is not None and baseline["quality"] is not None else None),
            "quality_relative_gain": quality_gain,
            "quality_state": quality_state,
            "invalid_loop_reduction": (paired_baseline_loop - paired_candidate_loop
                                       if paired_baseline_loop is not None and paired_candidate_loop is not None else None),
            "paired_loop_coverage": len(paired_loop_ids),
            "paired_loop_task_ids": paired_loop_ids,
            "paired_loop_baseline": paired_baseline_loop,
            "paired_loop_candidate": paired_candidate_loop,
            "loop_complete_coverage": loop_complete,
            "invalid_loop_state": loop_state, "loop_basis_match": same_basis,
            "resources": resources,
            "resource_state": resource_state,
        },
        "goal_checks": checks,
        "selection_uses_stage_scores": False,
        "note": "exploration_gain is informational; GEPA/100/final release requires goal_passed.",
    }
    return goal_passed, result


def verify_stage(stage: str, comparison: Path, l0_source: Path,
                 expected_seed: Path | None = None) -> tuple[bool, dict[str, Any]]:
    value, rows = comparison_rows(comparison)
    value = dict(value)
    value["source_comparison"] = str(comparison)
    return _verify_against_l0(stage, value, rows, l0_source, expected_seed)


def print_plan(seed: Path, l0_source: Path) -> None:
    print("AppWorld staged plan (read-only; no model call started):")
    print("  Pilot/validation gate: same-task coverage, strict +5%, quality +5%, loop -15%.")
    print("  Final comparison500 uses the same three metric gates; resources are reported honestly, including unknown costs.")
    for stage in ("32", "100"):
        out = candidate_output(stage, seed)
        shard_size = int(stage)
        stage_l0 = l0_source
        command = [sys.executable, str(RUNNER), "--seed", str(seed),
                   "--l0-source", str(stage_l0), "--workers", "16",
                   "--shard-size", str(shard_size), "--task-count",
                          str(STAGES[stage]["count"]), "--output", str(out)]
        print(f"  {stage:>3} pilot/validation: " + shlex.join(map(str, command)))
        prerequisite = ""
        if stage == "100":
            prerequisite = " --pilot-comparison <passed-32-comparison.json>"
        gate_l0 = f" --l0-source {stage_l0}"
        pilot_l0 = f" --pilot-l0-source {l0_source}" if stage == "100" else ""
        print(f"      gate: {sys.executable} {__file__} verify --stage {stage} --comparison {out / 'comparison.json'}{gate_l0}{pilot_l0}{prerequisite}")
    print("  GEPA: only after both goal gates pass; train on training-only experience.")
    print("  final: exactly three complete comparison500 comparisons; coverage_pass is not goal_passed.")


def verify_final(comparisons: list[Path], l0_source: Path) -> tuple[bool, dict[str, Any]]:
    """Require three complete candidates; ranking is reporting-only."""
    if len(comparisons) != 3:
        return False, {"status": "fail", "coverage_pass": False,
                       "reason": "exactly_three_candidates_required",
                       "candidate_count": len(comparisons)}
    l0 = load(l0_source)
    l0_ids = {row.get("task_id") for row in l0.get("items", [])
              if isinstance(row, dict) and row.get("task_id")}
    if len(l0_ids) != 500:
        return False, {"status": "fail", "coverage_pass": False,
                       "reason": "canonical_l0_not_500", "l0_count": len(l0_ids)}
    candidates: list[dict[str, Any]] = []
    for path in comparisons:
        value, rows = comparison_rows(path)
        valid, result = _verify_against_l0("comparison500", value, rows, l0_source)
        candidates.append({
            "comparison": str(path),
            "coverage_pass": result.get("coverage_pass", False),
            "goal_passed": result.get("goal_passed", False),
            "gate_status": result.get("gate_status", "fail"),
            "errors": result.get("errors", [])[:10],
            "metrics": result.get("candidate", {}),
        })
    coverage_pass = all(item["coverage_pass"] for item in candidates)
    ranked = sorted([item for item in candidates if item["coverage_pass"]],
                    key=lambda item: (item["metrics"].get("strict_passed", 0),
                                      item["metrics"].get("quality") or float("-inf")),
                    reverse=True)
    result = {
        "status": "pass" if coverage_pass else "fail",
        "coverage_pass": coverage_pass, "goal_passed": False,
        "l0_source": str(l0_source), "candidate_count": len(comparisons),
        "candidates": candidates,
        "best_observed_order": [item["comparison"] for item in ranked],
        "selection_uses_scores_for_training": False,
        "note": "Final-three gate proves complete coverage only; goal_passed remains a separate per-candidate report.",
    }
    return coverage_pass, result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--l0-source", type=Path, default=DEFAULT_L0)
    common.add_argument("--seed", type=Path, default=DEFAULT_SEED)
    sub.add_parser("plan", parents=[common])
    verify = sub.add_parser("verify", parents=[common])
    verify.add_argument("--stage", choices=tuple(STAGES), required=True)
    verify.add_argument("--comparison", type=Path)
    verify.add_argument("--pilot-comparison", type=Path,
                        help="required goal-passed 32-task comparison before the 100-task gate")
    verify.add_argument("--pilot-l0-source", type=Path,
                        help="L0 baseline for the 32-task pilot when verifying stage 100")
    final = sub.add_parser("verify-final", parents=[common])
    final.add_argument("--comparison", action="append", required=True,
                       help="one complete comparison500 comparison; repeat exactly three times")
    args = parser.parse_args()
    l0_source = args.l0_source.resolve()
    seed = args.seed.resolve()
    if args.command == "plan":
        print_plan(seed, l0_source)
        return 0
    if args.command == "verify-final":
        try:
            passed, result = verify_final([Path(item).resolve() for item in args.comparison], l0_source)
        except (OSError, ValueError, json.JSONDecodeError) as error:
            print(json.dumps({"status": "fail", "coverage_pass": False, "reason": str(error)},
                             ensure_ascii=False, indent=2))
            return 1
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0 if passed else 1
    comparison = (args.comparison or candidate_output(args.stage, seed) / "comparison.json").resolve()
    if not l0_source.is_file():
        raise SystemExit(f"missing L0 source: {l0_source}")
    if not comparison.is_file():
        print(json.dumps({"stage": args.stage, "status": "pending", "comparison": str(comparison),
                          "expected_tasks": STAGES[args.stage]["count"]},
                         ensure_ascii=False, indent=2))
        return 2
    if args.stage == "100":
        if not args.pilot_comparison:
            print(json.dumps({"stage": "100", "status": "fail",
                              "reason": "pilot32_goal_gate_required",
                              "hint": "pass --pilot-comparison <goal-passed 32-task comparison.json>"},
                             ensure_ascii=False, indent=2))
            return 1
        pilot = args.pilot_comparison.resolve()
        if not pilot.is_file():
            print(json.dumps({"stage": "100", "status": "pending",
                              "reason": "pilot32_comparison_missing", "comparison": str(pilot)},
                             ensure_ascii=False, indent=2))
            return 2
        pilot_l0_source = (args.pilot_l0_source or l0_source).resolve()
        if not pilot_l0_source.is_file():
            print(json.dumps({"stage": "100", "status": "pending",
                              "reason": "pilot32_l0_source_missing",
                              "l0_source": str(pilot_l0_source)},
                             ensure_ascii=False, indent=2))
            return 2
        try:
            pilot_passed, pilot_result = verify_stage("32", pilot, pilot_l0_source, seed)
        except (OSError, ValueError, json.JSONDecodeError) as error:
            print(json.dumps({"stage": "100", "status": "fail",
                              "reason": f"pilot32_invalid:{error}"},
                             ensure_ascii=False, indent=2))
            return 1
        if not pilot_passed:
            print(json.dumps({"stage": "100", "status": "fail",
                              "reason": "pilot32_goal_gate_not_passed", "pilot": pilot_result},
                             ensure_ascii=False, indent=2))
            return 1
    try:
        passed, result = verify_stage(args.stage, comparison, l0_source, seed)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(json.dumps({"stage": args.stage, "status": "fail", "reason": str(error)},
                         ensure_ascii=False, indent=2))
        return 1
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
