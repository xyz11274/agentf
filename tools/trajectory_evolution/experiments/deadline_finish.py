#!/usr/bin/env python3
"""Time-only cutoff for the business and isolated BFCL experiments.

Default: print a read-only plan. After review, run with --execute in tmux.
At the cutoff, SIGINT only a verified GEPA runner; its existing handler saves
checkpoints. Training and final evaluation are never interrupted. Independent
workers perform final delivery concurrently, using validation-frozen choices.
No score, threshold, experiment configuration, or historical result is edited.

Review (no writes, signals or model calls)::
    /usr/bin/python3 tools/trajectory_evolution/experiments/deadline_finish.py

After explicit review, launch from the repository root in a persistent session::
    /usr/bin/python3 -u tools/trajectory_evolution/experiments/deadline_finish.py --execute \
      --business-runner-entry /tmp/te_resume_business_after_review_budget.py

The cutoff is 2026-09-30 03:00 Beijing; the reporting deadline is 09:00 Beijing.
At the deadline, no new supervisor worker starts; in-flight final evaluations keep checkpoints.
If training survives the cutoff, its existing runner completes training and the
seed validation before search is paused. A stopped runner with no complete seed
validation requires review. There is no automatic invalid-loop stop condition.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import fcntl
import importlib
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
CUTOFF = "2026-09-29T19:00:00+00:00"
DEADLINE = "2026-09-30T01:00:00+00:00"
LINES = {
    "business": {"output": "trace_output/business-simulator", "folder": "business-coverage",
                 "runner": "offline_gepa", "experiment": "offline_experiment",
                 "best": "best_strategy", "user": "business-training"},
    "bfcl": {"output": "trace_output/bfcl-v3/seen-scenarios-v1", "folder": "bfcl-v3",
             "runner": "bfcl_gepa", "experiment": "bfcl_experiment",
             "best": "bfcl_best_strategy", "user": "bfcl-training"},
}


def read(path: Path) -> dict:
    try:
        value = json.loads(path.read_text())
        return value if isinstance(value, dict) else {}
    except FileNotFoundError:
        return {}


def write(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".pending")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")
    os.replace(temporary, path)


def utc(value: str) -> datetime:
    result = datetime.fromisoformat(value.replace("Z", "+00:00"))
    if result.tzinfo is None:
        raise ValueError("time_requires_timezone")
    return result.astimezone(timezone.utc)


def process(pid: int, proc_root: Path = Path("/proc")) -> dict | None:
    """Read identity only; never return or log the process environment."""
    try:
        folder = proc_root / str(pid)
        stat = (folder / "stat").read_text().rsplit(")", 1)[1].split()
        if stat[0] == "Z":
            return None
        return {"pid": pid, "start_ticks": stat[19],
                "argv": (folder / "cmdline").read_bytes().decode().rstrip("\0").split("\0"),
                "cwd": str((folder / "cwd").resolve())}
    except (FileNotFoundError, ProcessLookupError):
        return None


def runner_identity(line: str, pid: int, extra_entry: str = "",
                    proc_root: Path = Path("/proc"), root: Path = ROOT) -> dict:
    info = process(pid, proc_root)
    if not info or info["cwd"] != str(root):
        raise RuntimeError("runner_identity_unverified")
    spec = LINES[line]
    allowed = {str(HERE / spec["folder"] / (name + ".py"))
               for name in (spec["runner"], spec["experiment"])}
    if extra_entry:
        allowed.add(str(Path(extra_entry).resolve()))
    argv = info["argv"]
    script_match = any(str((root / arg).resolve()) in allowed for arg in argv[1:]
                       if arg.endswith(".py"))
    inline_match = False
    if "-c" in argv:
        index = argv.index("-c") + 1
        if index < len(argv):
            code = argv[index]
            inline_match = (f"import {spec['runner']}" in code and
                            "run_to_publication(" in code)
    if not (script_match or inline_match):
        raise RuntimeError("runner_command_unverified")
    if line == "bfcl":
        env = (proc_root / str(pid) / "environ").read_bytes().split(b"\0")
        if b"BFCL_SPLIT_ID=seen-scenarios-v1" not in env:
            raise RuntimeError("bfcl_runner_split_unverified")
    return {"pid": pid, "start_ticks": info["start_ticks"]}


def final_started(tree: dict) -> bool:
    return bool(tree.get("holdout_started") or tree.get("final_holdout"))


def complete_validation(tree: dict) -> bool:
    nodes = tree.get("nodes") or []
    expected = set((nodes[0].get("validation") or {})) if nodes else set()
    if len(expected) != 50:
        return False
    return any(set(node.get("validation") or {}) == expected and all(
        isinstance(row, dict) and all(isinstance(row.get(field), (float, int)) and
        not isinstance(row[field], bool) and math.isfinite(row[field])
        for field in ("quality", "strict_pass_rate"))
        for row in node["validation"].values()) for node in nodes)


def snapshot(line: str, job_id: str = "", root: Path = ROOT) -> dict:
    out = root / LINES[line]["output"]
    runner = read(out / "gepa/status.json")
    progress = runner.get("progress") or {}
    observed = progress.get("job_id") or runner.get("job_id") or ""
    if job_id and observed and job_id != observed:
        raise RuntimeError("runner_job_changed")
    job_id = job_id or observed
    training = read(out / "training/status.json")
    result = {"line": line, "job_id": job_id, "runner_status": runner.get("status"),
              "runner_pid": int(runner.get("pid") or 0),
              "training_status": training.get("status"), "tree": {}}
    if not job_id:
        return result
    if not re.fullmatch(r"[0-9a-f]{64}", job_id):
        raise RuntimeError("job_id_invalid")
    workspace = out / "gepa/workspace/agent-evaluation"
    job = (read(workspace / "state.json").get("jobs") or {}).get(job_id) or {}
    if job.get("user") != LINES[line]["user"] or job.get("level") != "L3":
        raise RuntimeError("job_owner_unverified")
    result["tree"] = read(workspace / job_id / "search/tree.json")
    return result


def action(view: dict, now: datetime, cutoff: datetime, deadline: datetime,
           runner_alive: bool, delivery_alive: bool = False) -> str:
    if now >= deadline:
        return "deadline_reached"
    if delivery_alive:
        return "waiting_final_evaluation"
    if now < cutoff:
        return "waiting_cutoff"
    if not view["job_id"]:
        return "waiting_training" if view["line"] == "bfcl" else "waiting_job"
    tree = view["tree"]
    if runner_alive:
        if final_started(tree) or view["runner_status"] != "running":
            return "waiting_final_evaluation"
        if not complete_validation(tree):
            return "waiting_seed_validation"
        return "pause_search"
    return "start_final_delivery" if complete_validation(tree) else "needs_seed_validation"


@contextmanager
def lock(path: Path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+") as handle:
        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


def pause_search(view: dict, extra_entry: str) -> dict:
    """pidfd prevents a recycled PID from receiving the cutoff signal."""
    if not hasattr(os, "pidfd_open") or not hasattr(signal, "pidfd_send_signal"):
        raise RuntimeError("pidfd_required_for_verified_signal")
    pid = view["runner_pid"]
    descriptor = os.pidfd_open(pid)
    try:
        identity = runner_identity(view["line"], pid, extra_entry)
        # Recheck the current stage immediately before signaling the same process.
        current = snapshot(view["line"], view["job_id"])
        if (current["runner_pid"] != pid or current["runner_status"] != "running"
                or final_started(current["tree"])):
            raise RuntimeError("runner_stage_changed")
        signal.pidfd_send_signal(descriptor, signal.SIGINT)
        return identity
    finally:
        os.close(descriptor)


def external_delivery(line: str, proc_root: Path = Path("/proc")) -> bool:
    """Recognize manual best-strategy launchers before starting another one."""
    for path in proc_root.iterdir():
        if not path.name.isdigit() or int(path.name) == os.getpid():
            continue
        try:
            info = process(int(path.name), proc_root)
            if not info or info["cwd"] != str(ROOT):
                continue
            argv = info["argv"]
            if any(Path(arg).name == LINES[line]["best"] + ".py" for arg in argv):
                return True
            if "-c" in argv and any(f"import {LINES[line]['best']}" in arg for arg in argv):
                return True
        except (OSError, ValueError):
            continue
    return False


def business_selection(best, view: dict, freeze_path: Path | None = None,
                       reuse_prior: bool = True,
                       required_judge: str | None = None,
                       prefer_seeds: bool = False) -> tuple[dict, list[dict]]:
    """Freeze only complete, already measured validation; never read holdout."""
    job_id, tree = view["job_id"], view["tree"]
    job_root = best.JOB_ROOT / job_id
    freeze_path = freeze_path or best.OUTPUT / "published/deadline-finish" / job_id / "selection.json"
    frozen = read(freeze_path)
    if not frozen and reuse_prior:
        # A prior best-strategy delivery has already chosen before holdout.
        frozen = read(best.OUTPUT / "published/best-strategy-runs" / job_id /
                      "showcase-selection-freeze.json")
    if frozen:
        if frozen.get("job_id") != job_id:
            raise RuntimeError("selection_job_mismatch")
        if required_judge and frozen.get("judge_fingerprint") != required_judge:
            raise RuntimeError("selection_judge_mismatch")
        pool = []
        for entry in frozen.get("candidate_ids", []):
            candidate = dict(entry, job_id=job_id)
            artifact_ref = entry.get("artifact_path")
            if artifact_ref:
                artifact_path = Path(artifact_ref)
                if not artifact_path.is_absolute():
                    artifact_path = best.ROOT / artifact_path
            elif isinstance(entry.get("node"), int):
                artifact_path = job_root / "search/versions" / f"{entry['node']}.json"
            else:
                raise RuntimeError("frozen_external_artifact_missing")
            candidate["artifact"] = read(artifact_path)
            if entry.get("artifact_sha256") and best._sha256(artifact_path) != entry["artifact_sha256"]:
                raise RuntimeError("frozen_artifact_changed")
            actual_version = (best._candidate_artifact_version(candidate) if candidate.get("native_binary")
                              or candidate.get("runtime_requirement") else best._artifact_version(candidate["artifact"]))
            if actual_version != candidate["version"]:
                raise RuntimeError("frozen_artifact_changed")
            pool.append(candidate)
        chosen = next((item for item in pool if item["node"] == frozen.get("best_node")
                       and item["version"] == frozen.get("best_version")), None)
        if chosen is None:
            raise RuntimeError("frozen_best_missing")
        if prefer_seeds and not chosen.get("external_candidate_id"):
            raise RuntimeError("frozen_best_is_not_requested_seed")
        if not freeze_path.exists():
            write(freeze_path, frozen)
        return chosen, pool

    nodes = tree.get("nodes") or []
    if not complete_validation(tree):
        raise RuntimeError("complete_seed_validation_missing")
    expected = set(nodes[0]["validation"])
    pool = []
    for item in best._candidate_pool(job_root, tree, limit=len(nodes)):
        rows = nodes[item["node"]].get("validation") or {}
        if set(rows) == expected and complete_validation({"nodes": [{"validation": rows}]}):
            item["validation"]["strict_passed"] = sum(row["strict_pass_rate"] for row in rows.values())
            item["screening_task_ids"] = sorted(expected)
            cached = best._cached_val_report(job_root, tree, item["version"], sorted(expected))
            if cached:
                item["screening_report_path"] = cached[1]
                item["validation_evidence_source"] = "reused_complete_pool_val"
            pool.append(item)
    # Reuse complete, identity-checked pool val50 reports; never run a new screen.
    for item in best._completed_pool_candidates(job_root, tree, pool):
        if item.get("cached_validation_complete"):
            item["validation_evidence_source"] = "reused_complete_pool_val"
            item["screening_report_path"] = item.get("cached_validation_report_path")
            pool.append(item)
    supplied = best.discover_completed_val_audits(job_id)
    supplied.extend(best.discover_external_seed_candidates(job_id))
    for item in best._independent_val_candidates(job_id, supplied, expected, repair_errors=False):
        # Filter before deduplication: a later historical audit for this same
        # version must not replace its valid current-judge regrade.
        if required_judge and item.get("judge_fingerprint") != required_judge:
            continue
        prior = next((old for old in pool if old["version"] == item["version"]), None)
        if prior is None:
            pool.append(item)
        else:
            prior.update(item)
    if not pool:
        seed = nodes[0]
        metrics = best._node_metrics(seed)
        metrics["strict_passed"] = sum(row["strict_pass_rate"] for row in seed["validation"].values())
        pool = [{"job_id": job_id, "node": 0, "version": seed["version"],
                 "artifact": read(job_root / "search/versions/0.json"), "validation": metrics,
                 "selection_source": "seed_validation_fallback"}]
    for item in pool:
        if not item.get("screening_task_ids"):
            item["screening_task_ids"] = sorted(expected)
            cached = best._cached_val_report(job_root, tree, item["version"], sorted(expected))
            if cached:
                item["screening_report_path"] = cached[1]
                item["validation_evidence_source"] = "reused_complete_pool_val"
        actual_version = (best._candidate_artifact_version(item) if item.get("native_binary")
                          or item.get("runtime_requirement") else best._artifact_version(item["artifact"]))
        if actual_version != item["version"]:
            raise RuntimeError("candidate_artifact_mismatch")
    if required_judge:
        compatible = []
        for item in pool:
            ref = item.get("screening_report_path")
            if not ref:
                continue
            path = Path(ref)
            if not path.is_absolute():
                path = best.OUTPUT / path
            if read(path).get("configuration", {}).get("judge_fingerprint") == required_judge:
                item["judge_fingerprint"] = required_judge
                compatible.append(item)
        pool = compatible
        if not pool:
            raise RuntimeError("current_judge_candidate_missing")
    pool.sort(key=lambda item: best._showcase_key(item["validation"]), reverse=True)
    unique = []
    for item in pool:
        if item["version"] not in {prior["version"] for prior in unique}:
            unique.append(item)
    if prefer_seeds:
        eligible = [item for item in unique if item.get("external_candidate_id")]
        if not eligible:
            raise RuntimeError("complete_seed_candidate_missing")
        chosen = eligible[0]
        pool = [chosen] + [item for item in unique if item['version'] != chosen['version']][:2]
    else:
        pool = unique[:3]
        chosen = pool[0]
    for item in pool:
        item.update(best._freeze_candidate_artifact(job_root, freeze_path.parent, item))
    keep = ("node", "external_candidate_id", "version", "selection_source", "validation", "screening_report_path",
            "screening_task_ids", "screening_report_sha256", "screening_variant_id", "validation_evidence_source",
            "artifact_path", "artifact_sha256", "artifact_source_path", "native_binary", "runtime_requirement",
            "evaluation_feedback_informed", "training_feedback_sources")
    write(freeze_path, {"job_id": job_id, "selection_basis": (
        "user_requested_seed_family_ranked_by_val50" if prefer_seeds else "complete_shared_val50_before_holdout"),
        "prefer_seeds": prefer_seeds,
        "selection_task_ids": sorted(expected), "best_node": chosen["node"],
        "best_version": chosen["version"], "best_candidate_id": chosen.get("external_candidate_id"), "candidate_ids": [
            {key: item[key] for key in keep if key in item} for item in pool],
        "judge_fingerprint": required_judge,
        "created_at": datetime.now(timezone.utc).isoformat()})
    return chosen, pool


def business_delivery(best, view: dict, selection=None, result_path: Path | None = None,
                      superseded=None) -> dict:
    existing = read(best.BEST_FILE)
    if ((existing.get("selection") or {}).get("job_id") == view["job_id"] and
            (existing.get("comparison") or {}).get("status") == "complete" and
            (existing.get("evaluation_complete") or existing.get("status") in ("complete", "best_observed"))):
        return existing
    chosen, pool = selection if selection is not None else business_selection(best, view)
    # The required best/L0 comparison gets priority; other frozen candidates are
    # audited afterwards. Their holdout scores cannot change the chosen ID.
    validation = best._final_validation(chosen)
    if superseded is not None and superseded():
        result = {"status": "superseded", "version": chosen["version"],
                  "validation": validation, "reason": "better_val50_candidate_ready"}
        if result_path is not None:
            write(result_path, result)
        return result
    baseline_version = best._artifact_version(read(best.JOB_ROOT / view["job_id"] / "baseline.json"))
    baseline_holdout = best._baseline400_rows("holdout", baseline_version)
    holdout = best.run_comparison(chosen, splits=("holdout",), purpose="deadline_holdout_best",
                                  include_l0=baseline_holdout is None)
    if baseline_holdout:
        baseline_rows, baseline_ref = baseline_holdout
        holdout.setdefault("metrics", {})["L0"] = {"holdout": best._aggregate(baseline_rows, 200)}
        holdout["complete_trials"] = holdout.get("complete_trials", 0) + len(baseline_rows)
        holdout["expected_trials"] = 400
        holdout["l0_cache"] = baseline_ref
        holdout["new_l0_tasks"] = 0
    comparison = {
        "status": "complete" if validation.get("status") == holdout.get("status") == "complete"
                  else "incomplete",
        "scope": {"val": 200, "holdout": 200, "total": 400}, "expected_trials": 800,
        "complete_trials": validation.get("complete_trials", 0) + holdout.get("complete_trials", 0),
        "metrics": {variant: {"val": (validation.get("metrics", {}).get(variant) or {}).get("val"),
                              "holdout": (holdout.get("metrics", {}).get(variant) or {}).get("holdout")}
                    for variant in ("L0", "best")},
        "validation": validation, "holdout": holdout,
        "holdout_used_for_ranking": False,
    }
    write(best.COMPARISON_FILE, comparison)
    audits = [{"node": chosen["node"], "version": chosen["version"],
               "status": comparison.get("status"), "source": "best_final_comparison"}]
    for item in pool:
        if item["version"] == chosen["version"]:
            continue
        if superseded is not None and superseded():
            break
        audit = best.run_comparison(item, splits=("holdout",), include_l0=False,
                                   persist_result=False, purpose="deadline_frozen_audit")
        audits.append({"node": item["node"], "version": item["version"], **audit})
    complete = comparison.get("status") == "complete" and len(audits) == len(pool) and all(
        item.get("status") == "complete" for item in audits)
    result = {"status": "complete" if complete and len(pool) == 3 else "shortfall",
              "formal_release": False, "evaluation_complete": complete,
              "selection": {key: value for key, value in chosen.items() if key != "artifact"},
              "artifact": chosen["artifact"], "comparison": comparison,
              "candidate_count_frozen": len(pool), "shortfall_candidate_count": max(0, 3-len(pool)),
              "holdout_used_for_ranking": False, "candidate_holdout_audits": audits}
    if chosen.get("evaluation_feedback_informed"):
        result["evaluation_basis"] = "training_informed_replay"
        result["independent_holdout"] = False
        comparison["evaluation_basis"] = "training_informed_replay"
        comparison["independent_holdout"] = False
        write(best.COMPARISON_FILE, comparison)
    write(best.BEST_FILE, result)
    write(result_path or best.OUTPUT / "published/deadline-finish" / view["job_id"] / "result.json", result)
    return result


def worker(line: str, job_id: str, receipt: Path) -> int:
    """Each line has its own process, environment and existing resumable API."""
    if line == "bfcl":
        os.environ["BFCL_SPLIT_ID"] = "seen-scenarios-v1"
    sys.path.insert(0, str(HERE / LINES[line]["folder"]))
    gepa = importlib.import_module(LINES[line]["runner"])
    best = importlib.import_module(LINES[line]["best"])
    started = {"line": line, "job_id": job_id, "pid": os.getpid(),
               "status": "running", "started_at": datetime.now(timezone.utc).isoformat()}
    write(receipt, started)
    try:
        with gepa.lifecycle_lock(), lock(ROOT / LINES[line]["output"] / "gepa/runner.lock"):
            view = snapshot(line, job_id)
            pid = view["runner_pid"]
            if pid and process(pid):
                raise RuntimeError("original_runner_still_active")
            environment = gepa.environment()
            os.environ.clear()
            os.environ.update(environment)
            if line == "bfcl":
                result = best.build_best_observed(job_id)
            else:
                result = business_delivery(best, view)
            write(receipt, {**started, "status": "finished", "result_status": result.get("status"),
                           "comparison_status": (result.get("comparison") or {}).get("status"),
                           "finished_at": datetime.now(timezone.utc).isoformat()})
        return 0
    except Exception as exc:
        # Exception text can include model credentials; keep only the class.
        write(receipt, {**started, "status": "error", "error_type": type(exc).__name__,
                       "finished_at": datetime.now(timezone.utc).isoformat()})
        return 1


def supervise(args) -> int:
    cutoff, deadline = utc(args.cutoff), utc(args.deadline)
    if cutoff >= deadline:
        raise ValueError("cutoff_must_precede_deadline")
    state_dir = args.state_dir.resolve()
    state_path = state_dir / "status.json"
    state = read(state_path) if args.execute else {}
    children = {}
    while True:
        now = datetime.now(timezone.utc)
        rows = []
        for line in LINES:
            if line == "business" and getattr(args, "skip_business", False):
                continue
            previous = (state.get("lines") or {}).get(line) or {}
            try:
                view = snapshot(line, getattr(args, line + "_job") or previous.get("job_id", ""))
                receipt_path = state_dir / (line + "-delivery.json")
                receipt = read(receipt_path)
                child = children.get(line)
                if child is not None:
                    child.poll()  # Reap a completed child before checking /proc.
                worker_pid = receipt.get("pid") or previous.get("worker_pid")
                delivery_alive = bool(worker_pid and process(int(worker_pid)))
                if not delivery_alive:
                    delivery_alive = external_delivery(line)
                alive = bool(view["runner_pid"] and process(view["runner_pid"]))
                choice = action(view, now, cutoff, deadline, alive, delivery_alive)
                row = {"line": line, "job_id": view["job_id"], "action": choice,
                       "runner_pid": view["runner_pid"], "tree_status": view["tree"].get("status")}
                if receipt.get("job_id") == view["job_id"] and receipt.get("status") in ("finished", "error"):
                    row.update(action="delivery_" + receipt["status"], result=receipt)
                elif args.execute and choice == "pause_search":
                    identity = runner_identity(line, view["runner_pid"], getattr(args, line + "_runner_entry"))
                    if previous.get("signal_identity") == identity:
                        row.update(action="waiting_checkpoint", signal_identity=identity)
                    else:
                        row.update(action="stop_requested", signal_identity=pause_search(
                            view, getattr(args, line + "_runner_entry")))
                elif args.execute and choice == "start_final_delivery":
                    # Launch both lines independently; a BFCL training wait never
                    # occupies the business final-comparison worker.
                    receipt_path.parent.mkdir(parents=True, exist_ok=True)
                    write(receipt_path, {"status": "launching", "line": line,
                                         "job_id": view["job_id"]})
                    with (state_dir / (line + "-delivery.log")).open("a") as output:
                        child = subprocess.Popen([sys.executable, "-u", str(Path(__file__).resolve()),
                            "--worker", line, "--job-id", view["job_id"], "--receipt", str(receipt_path)],
                            cwd=ROOT, stdout=output, stderr=output, start_new_session=True)
                    children[line] = child
                    row.update(action="delivery_started", worker_pid=child.pid)
                if previous.get("worker_pid") and "worker_pid" not in row:
                    row["worker_pid"] = previous["worker_pid"]
                if previous.get("signal_identity") and "signal_identity" not in row:
                    row["signal_identity"] = previous["signal_identity"]
            except (OSError, ValueError, RuntimeError) as exc:
                row = {"line": line, "action": "needs_review", "error_type": type(exc).__name__}
            rows.append(row)
        state = {"cutoff": cutoff.isoformat(), "deadline": deadline.isoformat(),
                 "updated_at": now.isoformat(), "execute": args.execute,
                 "lines": {row["line"]: row for row in rows}}
        if args.execute:
            write(state_path, state)
        print(json.dumps(state, ensure_ascii=False), flush=True)
        if not args.execute or args.once:
            return 0
        if all(row["action"] in ("delivery_finished", "delivery_error") for row in rows):
            return int(any(row["action"] == "delivery_error" for row in rows))
        if now >= deadline:
            return 3  # Existing final workers retain their checkpoints and continue.
        time.sleep(min(max(args.poll_seconds, 1), 30))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--skip-business", action="store_true",
                        help="Delegate business comparisons to the continuous seed supervisor.")
    parser.add_argument("--cutoff", default=CUTOFF)
    parser.add_argument("--deadline", default=DEADLINE)
    parser.add_argument("--poll-seconds", type=float, default=30)
    parser.add_argument("--state-dir", type=Path, default=ROOT / "trace_output/business-simulator/audit/deadline-finish-20260929")
    for line in LINES:
        parser.add_argument("--" + line + "-job", default="")
        parser.add_argument("--" + line + "-runner-entry", default="")
    parser.add_argument("--worker", choices=tuple(LINES), help=argparse.SUPPRESS)
    parser.add_argument("--job-id", default="", help=argparse.SUPPRESS)
    parser.add_argument("--receipt", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.worker:
        if not re.fullmatch(r"[0-9a-f]{64}", args.job_id) or args.receipt is None:
            parser.error("worker requires a job and receipt")
        return worker(args.worker, args.job_id, args.receipt)
    if not args.execute:
        return supervise(args)
    if not hasattr(os, "pidfd_open") or not hasattr(signal, "pidfd_send_signal"):
        parser.error("--execute requires pidfd support; use /usr/bin/python3")
    with lock(args.state_dir / "supervisor.lock"):
        return supervise(args)


if __name__ == "__main__":
    raise SystemExit(main())
