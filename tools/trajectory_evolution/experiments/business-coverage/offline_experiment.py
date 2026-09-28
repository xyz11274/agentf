#!/usr/bin/env python3
"""面向非程序员的业务离线实验入口。

常用命令：
  python3 .../offline_experiment.py --fresh   # 清理旧 GEPA 树并运行到发布判定
  python3 .../offline_experiment.py --resume  # 从训练/GEPA checkpoint 继续
  python3 .../offline_experiment.py --status  # 查看当前阶段和关键门禁指标
  python3 .../offline_experiment.py --check   # 只做无模型启动检查
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import shutil
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import layout  # noqa: E402
import coldstart  # noqa: E402
import offline_gepa as gepa  # noqa: E402


def _read(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}


FULL_LOCK = layout.OUTPUT.parent / ".business-simulator-full.lock"


@contextmanager
def full_lifecycle_lock():
    """Serialize cleanup, training, GEPA and publication as one lifecycle."""
    FULL_LOCK.parent.mkdir(parents=True, exist_ok=True)
    handle = FULL_LOCK.open("a+")
    try:
        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        handle.close()
        raise RuntimeError("business_experiment_already_running")
    try:
        yield
    finally:
        fcntl.flock(handle, fcntl.LOCK_UN)
        handle.close()


def _receipt_failed_run() -> None:
    """Keep a tiny failure receipt before deleting a failed GEPA tree."""
    status = layout.GEPA / "status.json"
    if not status.is_file():
        return
    audit = layout.OUTPUT / "audit" / "failed-runs"
    audit.mkdir(parents=True, exist_ok=True)
    state = _read(status)
    job_id = state.get("progress", {}).get("job_id", "unknown")
    target = audit / str(job_id)
    target.mkdir(parents=True, exist_ok=True)
    (target / "status.json").write_text(json.dumps(state, ensure_ascii=False, indent=2) + "\n")
    log = layout.GEPA / "learning.log"
    if log.is_file():
        lines = log.read_text(errors="replace").splitlines()[-200:]
        (target / "learning-tail.log").write_text("\n".join(lines) + ("\n" if lines else ""))


def _remove_target(path: Path) -> None:
    if not path.exists() and not path.is_symlink():
        return
    if path.is_symlink() or (path.exists() and not path.is_dir()):
        raise RuntimeError("cleanup_target_invalid:" + str(path))
    shutil.rmtree(path)


def clean_full_outputs() -> None:
    """Remove regenerable old stages while preserving source, audit and archive."""
    _receipt_failed_run()
    targets = (
        layout.GEPA,
        layout.TRAINING,
        layout.PUBLISHED,
        layout.TRAINING_DATASET,
        HERE / "coldstart-deployment",
        HERE / "gepa-deployment",
    )
    # A stale lock file is harmless; an acquired lock means a child evaluator
    # still owns the stage and deleting it would destroy its checkpoint.
    lock_paths = []
    for target in (layout.GEPA, layout.TRAINING):
        lock_paths.extend((target / name for name in (
            "runner.lock", "workspace/service.lock",
            "workspace/agent-evaluation/evaluation.lock")))
    held = []
    try:
        for lock_path in lock_paths:
            if not lock_path.is_file():
                continue
            handle = lock_path.open("a+")
            try:
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as exc:
                handle.close()
                raise RuntimeError("business_experiment_stage_active") from exc
            held.append(handle)
        for target in targets:
            _remove_target(target)
    finally:
        for handle in held:
            fcntl.flock(handle, fcntl.LOCK_UN)
            handle.close()
    # The task split view is regenerated from the immutable source dataset.
    if layout.GEPA_VIEW.is_file():
        layout.GEPA_VIEW.unlink()


def run_training_from_scratch() -> dict:
    coldstart.prepare()
    for attempt in range(1, 4):
        rc = coldstart.run(retry_failed=attempt > 1)
        state = _read(layout.TRAINING / "status.json")
        if rc == 0 and state.get("status") == "completed":
            break
        if state.get("error") == "requested_stop":
            raise RuntimeError("training_paused")
        if state.get("error") not in ("batch_no_usable_trials", "capture_failed",
                                       "learning_failed", "learning_status_missing"):
            raise RuntimeError("training_failed:" + str(state.get("error", rc)))
        print("TRAINING_RECOVER", json.dumps({"attempt": attempt,
                                               "error": state.get("error")}, ensure_ascii=False),
              flush=True)
    else:
        raise RuntimeError("training_recovery_limit")
    compact = coldstart.compact_completed_outputs()
    return {"status": "completed", "training": state, "compaction": compact}


def _gepa_checkpoint_state() -> str:
    """Return the GEPA recovery boundary without reading large trial files.

    ``plan.json`` is the durable configuration boundary.  A status/tree without
    it is still evidence that a GEPA run started, but cannot be safely resumed:
    rebuilding a plan could silently change the evaluator or dataset.  Keep
    that state for diagnosis instead of letting ``--resume`` delete it.
    """
    plan = gepa.PLAN.is_file()
    status = gepa.STATUS.is_file()
    state = (gepa.WORKSPACE / "agent-evaluation" / "state.json").is_file()
    search = (gepa.WORKSPACE / "agent-evaluation").is_dir() and any(
        path.is_dir() for path in
        (gepa.WORKSPACE / "agent-evaluation").glob("*/search"))
    if plan:
        return "planned"
    if status or state or search:
        return "orphaned"
    return "empty"


def _resume_gepa_or_start() -> dict:
    """Continue a prepared GEPA tree; create one only when none exists."""
    checkpoint = _gepa_checkpoint_state()
    if checkpoint == "orphaned":
        raise RuntimeError("gepa_resume_plan_missing")
    if checkpoint == "empty":
        return gepa.fresh()

    # A published/candidate-ready runner is already past model execution.  A
    # resume must not launch another GEPA job; only replay the idempotent
    # publication boundary.  The publisher validates the same job and bundle.
    runner = _read(gepa.STATUS)
    if runner.get("status") == "published":
        return {"status": "published", "publication": runner.get("publication", {})}
    if runner.get("status") == "candidate_ready":
        from offline_publish import publish
        publication = publish()
        runner.update(status="published", publication=publication)
        gepa.coverage.save(gepa.STATUS, runner)
        return {"status": "published", "publication": publication}

    # Select the durable GEPA job whose search tree contains the most progress.
    # ``latest`` can point at an auto-created seed evaluation after an
    # interrupted resume; that empty tree must never displace the real search
    # checkpoint.  Only the compact tree header is read here.
    state = _read(gepa.WORKSPACE / "agent-evaluation/state.json")
    candidates = []
    for job_id, job in (state.get("jobs") or {}).items():
        if (job.get("user") != gepa.USER or job.get("level") != "L3"
                or job.get("status") not in gepa.FRESH_RETRYABLE_JOB_STATES):
            continue
        tree_file = gepa.WORKSPACE / "agent-evaluation" / job_id / "search/tree.json"
        try:
            tree = _read(tree_file)
        except (OSError, ValueError):
            continue
        candidates.append((int(tree.get("rounds", 0)),
                          int(tree.get("trials_used", 0)),
                          int(job.get("queued_ms", 0)), job_id))
    initial_job_id = max(candidates)[-1] if candidates else ""
    # run_to_publication reuses the frozen plan/tree and its own recovery
    # supervisor.  It does not call reset_fresh_outputs or delete checkpoints.
    publication = (gepa.run_to_publication(initial_job_id=initial_job_id)
                   if initial_job_id else gepa.run_to_publication())
    if publication.get("status") == "best_observed":
        return publication
    return {"status": "published", "publication": publication}


def status() -> int:
    runner = _read(layout.GEPA / "status.json")
    job = gepa.progress()
    decision = _read(layout.GEPA / "workspace/agent-evaluation" /
                     job.get("job_id", "") / "search/decision.json") if job.get("job_id") else {}
    gate = decision.get("gate", job.get("gate", {}))
    print(json.dumps({
        "stage": runner.get("status", "待准备"),
        "job_status": job.get("job_status", "暂无"),
        "completed": job.get("completed", "暂无"),
        "published": decision.get("published", job.get("published_in_offline_workspace", False)),
        "gate_passed": gate.get("passed", "暂无"),
        "gate_reasons": gate.get("reasons", []),
    }, ensure_ascii=False, indent=2))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--fresh", action="store_true", help="清理旧 GEPA 产物并重新运行")
    group.add_argument("--resume", action="store_true", help="从现有 checkpoint 继续完整链路")
    group.add_argument("--status", action="store_true", help="播报当前状态和门禁")
    group.add_argument("--check", action="store_true", help="只检查环境，不调用模型")
    args = parser.parse_args()
    if args.status:
        return status()
    if args.check:
        source = layout.SOURCE_DATASET
        counts = {split: len((source / f"{split}.jsonl").read_text().splitlines())
                  for split in ("train", "val", "holdout")}
        print(json.dumps({"ok": counts == {"train": 400, "val": 200, "holdout": 200},
                          "source_counts": counts, "output_root": str(layout.OUTPUT)},
                         ensure_ascii=False))
        return 0
    with full_lifecycle_lock():
        if args.fresh:
            clean_full_outputs()
        elif not (layout.TRAINING / "status.json").is_file():
            raise RuntimeError("resume_checkpoint_missing")
        training = run_training_from_scratch()
        result = _resume_gepa_or_start() if args.resume else gepa.fresh()
        result["training"] = training
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
