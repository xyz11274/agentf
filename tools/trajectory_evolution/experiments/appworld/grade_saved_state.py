"""Diagnose every captured task using its saved official database, without an agent.

Original native reports are immutable. This output is a diagnostic sidecar, not
a replacement score report or a claim that an execution error has been fixed.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import math
import os
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
PYTHON = HERE / "runtime/venv/bin/python"
WORLD = HERE / "runtime/world"


def grade_one(task_id: str, experiment: str) -> None:
    os.environ["APPWORLD_ROOT"] = str(WORLD)
    os.environ["APPWORLD_CACHE"] = str(HERE / "runtime/cache")
    from appworld.evaluator import evaluate_task

    tracker = evaluate_task(task_id, experiment_name=experiment, save_report=False)
    total = int(tracker.num_tests)
    print(json.dumps({"task_id": task_id, "strict_passed": bool(tracker.success),
                      "passed_tests": int(tracker.pass_count), "total_tests": total,
                      "quality_score": tracker.pass_count / total if total else None}))


def locate(row: dict, report_path: Path, evidence_roots=()) -> tuple[str | None, str | None]:
    output = row.get("evidence", {}).get("output", {})
    experiment = output.get("official_experiment_name")
    raw_id = row["task_id"].removeprefix("appworld-")
    if experiment:
        target = WORLD / "experiments/outputs" / experiment / "tasks" / raw_id / "dbs"
        return (experiment, None) if target.is_dir() else (None, "saved_database_missing")
    roots = evidence_roots or (report_path.parent.parent / "official-evidence",)
    candidates = list({path.resolve() for root in roots for path in Path(root).glob(
        "agentflow-*/tasks/" + raw_id + "/dbs")})
    if len(candidates) != 1:
        return None, "saved_database_missing" if not candidates else "ambiguous_saved_database"
    experiment = candidates[0].parents[2].name
    link = WORLD / "experiments/outputs" / experiment
    if not link.is_symlink() or link.resolve() != candidates[0].parents[2].resolve():
        return None, "official_output_link_mismatch"
    return experiment, None


def score(row: dict, report_path: Path, evidence_roots=()) -> dict:
    original = row.get("evidence", {}).get("output", {})
    result = {"task_id": row["task_id"], "trial_id": row.get("trial_id"),
              "source_report": str(report_path),
              "original_evaluation_status": original.get("evaluation_status"),
              "original_error_code": original.get("error_code"),
              "original_strict_passed": original.get("strict_passed"),
              "original_quality_score": original.get("quality_score"),
              "model_calls": original.get("model_calls"),
              "tokens": original.get("tokens"),
              "latency_ms": original.get("latency_ms")}
    experiment, error = locate(row, report_path, evidence_roots)
    if error:
        return {**result, "diagnostic_status": "errored", "diagnostic_error": error}
    env = {key: value for key, value in os.environ.items()
           if not any(term in key.upper() for term in
                      ("PROXY", "API_KEY", "TOKEN", "SECRET", "PASSWORD"))}
    env.update(APPWORLD_ROOT=str(WORLD), APPWORLD_CACHE=str(HERE / "runtime/cache"))
    try:
        completed = subprocess.run([str(PYTHON), str(Path(__file__).resolve()),
                                    "--grade-one", row["task_id"].removeprefix("appworld-"),
                                    experiment], env=env, text=True, capture_output=True,
                                   timeout=90, check=False)
        if completed.returncode:
            return {**result, "diagnostic_status": "errored",
                    "diagnostic_error": "official_grader_exit_" + str(completed.returncode)}
        grade = json.loads(completed.stdout.strip().splitlines()[-1])
    except (subprocess.TimeoutExpired, ValueError, IndexError, OSError) as error:
        return {**result, "diagnostic_status": "errored", "diagnostic_error": type(error).__name__}
    unchanged = None
    if original.get("evaluation_status") == "completed":
        original_quality = original.get("quality_score")
        unchanged = (grade["strict_passed"] == bool(original.get("strict_passed"))
                     and isinstance(original_quality, (int, float))
                     and math.isclose(grade["quality_score"], original_quality,
                                      rel_tol=1e-12, abs_tol=1e-12))
    return {**result, "diagnostic_status": "completed", "official_experiment_name": experiment,
            "saved_state_grade": grade, "original_completed_grade_matches": unchanged}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--grade-one", nargs=2, metavar=("TASK_ID", "EXPERIMENT"))
    parser.add_argument("--reports", nargs="+", type=Path)
    parser.add_argument("--trial-roots", nargs="+", type=Path)
    parser.add_argument("--evidence-root", action="append", type=Path, default=[])
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--output", type=Path, default=ROOT /
                        "trace_output/appworld/gepa/diagnostics/saved-state-grades.json")
    args = parser.parse_args()
    if args.grade_one:
        grade_one(*args.grade_one)
        return 0
    paths = args.reports or [ROOT / "trace_output/appworld" / suffix for suffix in
                             ("training/captures/train-01/report.json",
                              "training/captures/train-02/report.json",
                              "gepa/captures/dev-L0/report.json")]
    if args.trial_roots:
        inputs = [(json.loads(path.read_text()), path.resolve()) for root in args.trial_roots
                  for path in root.glob("*.json")]
    else:
        inputs = [(row, path.resolve()) for path in paths
                  for row in json.loads(path.read_text())["trials"]]
    with ThreadPoolExecutor(max_workers=max(1, min(16, args.workers))) as pool:
        rows = list(pool.map(lambda item: score(*item, args.evidence_root), inputs))
    summary = {"report_kind": "saved_state_diagnostic", "additional_agent_calls": 0,
               "grade_comparison_float_tolerance": 1e-12,
               "official_grading_only": True, "original_reports_modified": False,
               "attempted": len(rows),
               "graded": sum(row["diagnostic_status"] == "completed" for row in rows),
               "mismatched_completed_grades": sum(row.get("original_completed_grade_matches")
                                                  is False for row in rows), "tasks": rows}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(".tmp")
    temporary.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n")
    temporary.replace(args.output)
    print(json.dumps({key: value for key, value in summary.items() if key != "tasks"}))
    return 0 if summary["graded"] == len(rows) and not summary["mismatched_completed_grades"] else 1


if __name__ == "__main__":
    sys.exit(main())
