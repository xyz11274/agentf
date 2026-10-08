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
import subprocess
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
COMMON_EXPERIMENTS = HERE.parent
if str(COMMON_EXPERIMENTS) not in sys.path:
    sys.path.insert(0, str(COMMON_EXPERIMENTS))

import layout  # noqa: E402
import coldstart  # noqa: E402
import offline_gepa as gepa  # noqa: E402

ROOT = layout.ROOT


def _read(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}


def _save(path: Path, value: dict) -> None:
    """Write the compact lifecycle receipt atomically."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".new")
    temporary.write_text(json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n",
                         encoding="utf-8")
    temporary.replace(path)


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
    """Remove regenerable stages while preserving every historical publication."""
    _receipt_failed_run()
    targets = (
        layout.GEPA,
        layout.TRAINING,
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


def _prepare_training() -> None:
    """Prepare the immutable business training view for the common runner."""
    coldstart.prepare()


def run_training_from_scratch(*, prepared: bool = False) -> dict:
    if not prepared:
        _prepare_training()
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


class _BusinessDatasetAdapter:
    """Dataset-only hooks for the shared lifecycle controller."""

    name = "business-simulator"
    output_root = layout.OUTPUT

    @staticmethod
    def split_counts() -> dict[str, int]:
        return {split: len((layout.SOURCE_DATASET / f"{split}.jsonl").read_text().splitlines())
                for split in ("train", "val", "holdout")}

    @staticmethod
    def materialize(view: str) -> dict:
        path = layout.SOURCE_DATASET / f"{view}.jsonl"
        return {"view": view, "path": str(path), "exists": path.is_file()}

    @staticmethod
    def baseline(scope: str) -> dict:
        # The paired L0 artifact is created by the current seed stage.  Do not
        # expose a historical seed004 comparison as adapter baseline evidence.
        return {"scope": scope,
                "path": str(layout.GEPA / "workspace/agent-evaluation/lifecycle-seed/baseline.json")}


def _business_memory_path() -> Path:
    return (layout.TRAINING / "evolution/by-user/business-training/memory"
            / "evolution-memory.json")


def _business_seed_path() -> Path:
    return layout.GEPA / "workspace/agent-evaluation/lifecycle-seed/strategy.json"


def _business_candidate_identity() -> dict[str, str]:
    """Bind the common receipt to the current memory and compiled seed."""
    memory = _business_memory_path()
    identity = {
        "memory_revision": str(_read(memory).get("revision", "")),
        "seed_artifact": str(_business_seed_path()),
    }
    artifact = _read(_business_seed_path())
    if artifact:
        import best_strategy
        identity["seed_version"] = best_strategy._artifact_version(artifact)
    return identity


def _business_seed_selection(label: str) -> dict:
    """Compile one seed from the current training memory only.

    A historical published strategy is deliberately never a fallback: mixing
    it with this run's memory would make the 32/100 gate unverifiable.
    """
    import best_strategy
    memory = _business_memory_path()
    if not memory.is_file():
        raise RuntimeError("business_training_seed_unavailable")
    request = {
        "memory_path": str(memory),
        "identity": {"user_id": "business-training", "agent_id": "business-simulator",
                     "conversation_id": "evaluation-version", "run_id": "evaluation-version"},
        "shadow_options": {
            "target_scopes": ["agent:business-simulator/actor:default"],
            "max_policy_entries": 64, "max_advice_items": 2,
            "max_advice_bytes": 2048, "max_query_bytes": 32768,
            "max_memory_bytes": 16777216, "max_snapshot_bytes": 262144,
            "max_match_terms": 128, "min_matched_terms": 2,
            "require_validated_evidence": True,
        },
        "options": {"requested_level": "L3",
                     "host_capabilities": ["context.request", "action.filter", "loop.decide"],
                     "fallback": "strict", "allowed_users": ["business-training"],
                     "allowed_conversations": ["evaluation-version"],
                     "max_context_bytes": 2048, "request_context_pointer": "/system",
                     "request_context_mode": "text"},
    }
    proc = subprocess.run([str(best_strategy.BINARY), "runtime", "compile"],
                          input=json.dumps(request, ensure_ascii=False), text=True,
                          capture_output=True, cwd=ROOT, timeout=30)
    if proc.returncode:
        raise RuntimeError("business_training_seed_unavailable")
    try:
        policy = json.loads(proc.stdout)
    except (TypeError, ValueError) as exc:
        raise RuntimeError("business_training_seed_unavailable") from exc
    if not isinstance(policy, dict) or policy.get("effective_level") != "L3":
        raise RuntimeError("business_training_seed_unavailable")
    artifact = {"schema_version": 1, "agent": {"revision": "business-simulator"},
                "user_id": "business-training", "level": "L3", "policy": policy}
    version = best_strategy._artifact_version(artifact)
    job_id = f"lifecycle-{label}"
    artifact_path = _business_seed_path()
    target = artifact_path.with_name("baseline.json")
    target.parent.mkdir(parents=True, exist_ok=True)
    baseline = {"schema_version": 1, "agent": {"revision": "business-simulator"},
                "user_id": "business-training", "level": "L0", "policy": None}
    _save(artifact_path, artifact)
    _save(target, baseline)
    return {"artifact": artifact, "artifact_path": str(artifact_path),
            "job_id": job_id, "node": "seed",
            "memory_source_dir": str(layout.TRAINING / "evolution"), "version": version}


def _business_stage_ids(count: int) -> list[str]:
    rows = [json.loads(line) for line in
            (layout.SOURCE_DATASET / "val.jsonl").read_text(encoding="utf-8").splitlines()
            if line.strip()]
    task_ids = sorted(row["task_id"] for row in rows)
    if len(task_ids) < count:
        raise RuntimeError(f"business_val_scope_too_small:{count}")
    return task_ids[:count]


def _business_task_loop_evidence(result: dict, variant: str, split: str = "val") -> tuple[dict, str | None]:
    """Extract paired task loops from the native report, never from a scalar mean."""
    from progress_report import _invalid_loops, _trial_error_code
    reference = result.get("report_path") or (result.get("report") or {}).get("path")
    if not isinstance(reference, str):
        return {}, None
    path = Path(reference)
    if not path.is_absolute():
        path = (layout.OUTPUT / path if not reference.startswith("trace_output/")
                else layout.ROOT / path)
    report = _read(path)
    rows = [row for row in (report.get("trials") or [])
            if row.get("variant_id") == variant and row.get("split") == split]
    loops, bases = {}, set()
    for row in rows:
        task_id = row.get("task_id")
        value = _invalid_loops(row)
        review = (row.get("evidence") or {}).get("loop_review")
        if (not isinstance(task_id, str) or not task_id or task_id in loops
                or _trial_error_code(row) is not None or value is None
                or not isinstance(review, dict)
                or str(review.get("status") or "").lower() not in {
                    "complete", "completed", "measured", "valid", "accepted"}):
            return {}, None
        loops[task_id] = value
        bases.add(str(row.get("invalid_loop_basis") or "model_review"))
    if not loops or len(bases) != 1:
        return {}, None
    basis = next(iter(bases))
    return loops, basis if basis == "model_review" else None


def _business_goal_gate(result: dict) -> tuple[bool, dict]:
    """Apply the shared relative goal to a paired native report."""
    from common_goal import evaluate
    metrics = result.get("metrics") or {}
    baseline = (metrics.get("L0") or {}).get("val") or {}
    candidate = (metrics.get("best") or {}).get("val") or {}
    def normalize(row):
        value = dict(row)
        value.setdefault("avg_invalid_loop", value.get("invalid_loop"))
        value.setdefault("model_calls", value.get("llm_calls", value.get("calls")))
        value.setdefault("latency_ms", value.get("duration_ms"))
        return value
    baseline, candidate = normalize(baseline), normalize(candidate)
    # A scalar average is insufficient to prove paired loop reduction.  The
    # native report must provide the exact task-keyed loop evidence and the
    # formal basis on both sides; otherwise common_goal reports a blocked gate.
    loop_b, basis_b = _business_task_loop_evidence(result, "L0")
    loop_c, basis_c = _business_task_loop_evidence(result, "best")
    complete = (isinstance(loop_b, dict) and isinstance(loop_c, dict)
                and set(loop_b) == set(loop_c))
    goal = evaluate(baseline, candidate,
                    baseline_loop_by_task=loop_b if complete else {},
                    candidate_loop_by_task=loop_c if complete else {},
                    baseline_loop_basis=basis_b if complete else None,
                    candidate_loop_basis=basis_c if complete else None,
                    same_scope=True, complete=bool(result.get("status") == "complete"),
                    require_intent_when_measured=False)
    return goal["passed"], {"passed": goal["passed"], "reasons": [name for name, ok in goal["checks"].items() if not ok],
                            "baseline": baseline, "candidate": candidate, "goal": goal}


def _legacy_business_state(state: dict) -> dict:
    """Project only durable legacy training evidence into common state."""
    # Native GEPA history stays in status.json; the lifecycle receipt keeps
    # only its small controller fields and stage evidence.
    state = {key: state[key] for key in ("status", "phase") if key in state}
    results = state.get("stage_results")
    if not isinstance(results, dict):
        results = {}
        state["stage_results"] = results
    training = _read(layout.TRAINING / "status.json")
    if training.get("status") == "completed":
        for name in ("prepare", "train"):
            results.setdefault(name, {
                "status": "complete",
                "data": {"legacy_evidence": "training/status.json"},
            })
    # A training-memory directory is not seed evidence.  The seed stage must
    # compile the current memory and record its exact artifact; otherwise an
    # old receipt could let pilot32/validation100 run against no candidate.
    return state


def _run_common_business() -> dict:
    """Run the business adapter under the same stage contract as AppWorld."""
    from common_lifecycle import LifecycleController, StageResult

    # Native GEPA owns status.json. Keep common lifecycle state in a sibling
    # receipt so controller writes cannot clobber candidate/job recovery.
    state_path = layout.GEPA / "lifecycle.json"
    state = _read(state_path)
    if "stage_results" not in state:
        state = _legacy_business_state(_read(layout.GEPA / "status.json"))
        _save(state_path, state)
    current_candidate = _business_candidate_identity()
    stored_candidate = state.get("candidate") or {}
    if stored_candidate != current_candidate:
        # Memory revisions and seed replacement invalidate candidate-specific
        # gates, while completed prepare/train evidence remains reusable.
        state["stage_results"] = {
            key: value for key, value in (state.get("stage_results") or {}).items()
            if key in {"prepare", "train"}
        }
        state.pop("artifacts", None)
        state.pop("metrics", None)
        state.pop("last_error", None)
        state["status"], state["phase"] = "pending", "pending"
    state["candidate"] = dict(current_candidate)
    _save(state_path, state)
    native = {}

    def callback(stage: str, context: dict) -> StageResult:
        if stage == "prepare":
            _prepare_training()
            return StageResult("complete", data={"callback": "business.coldstart.prepare"})
        if stage == "train":
            result = run_training_from_scratch(prepared=True)
            return StageResult("complete", data={"callback": "business.coldstart.run",
                                                   "training": result})
        if stage == "seed":
            source = gepa.checked_stage1()
            selection = _business_seed_selection("seed")
            current_candidate["seed_version"] = selection["version"]
            context["state"]["candidate"] = dict(current_candidate)
            return StageResult("complete", artifact=selection["artifact_path"],
                               data={"callback": "business.seed.freeze",
                                     "memory_source": str(source),
                                     "version": selection["version"]})
        if stage in ("pilot32", "validation100"):
            count = 32 if stage == "pilot32" else 100
            selection = _business_seed_selection("seed")
            if current_candidate.get("seed_version") != selection["version"]:
                raise RuntimeError("business_candidate_changed")
            task_ids = _business_stage_ids(count)
            result = gepa.run_comparison(selection, splits=("val",), task_ids=task_ids,
                                         include_l0=True, purpose=f"lifecycle_stage{count}")
            passed, gate = _business_goal_gate(result)
            comparison = result.get("report_path") or (result.get("report") or {}).get("path")
            return StageResult("complete", gate_passed=passed,
                               metrics=(result.get("metrics", {}).get("best", {}).get("val") or {}),
                               artifact=str(comparison) if comparison else None,
                               data={"callback": f"business.stage{count}",
                                     "comparison": comparison, "gate": gate,
                                     "task_count": count})
        if stage == "gepa_prepare":
            plan = gepa.prepare()
            return StageResult("complete", data={"callback": "business.offline_gepa.prepare",
                                                   "plan": plan})
        if stage == "gepa_run":
            # Reuse the durable native job when resuming; the helper selects
            # the most progressed checkpoint and only creates a job after a
            # genuinely fresh plan. This keeps common resume unconditional
            # without accidentally starting a second GEPA tree.
            native_result = _resume_gepa_or_start(publish=False)
            native.update(native_result if isinstance(native_result, dict) else {})
            native_status = native.get("status")
            if native_status not in {"candidate_ready", "best_available", "best_observed", "published"}:
                return StageResult("failed", reason=f"native_gepa_status:{native_status or 'missing'}",
                                    data={"callback": "business.offline_gepa.run",
                                          "native_status": native_status})
            return StageResult("complete", data={"callback": "business.offline_gepa.run",
                                                   "native_status": native.get("status")})
        if stage == "comparison500":
            job_id = (native.get("job_id") or gepa.progress().get("job_id"))
            if not job_id:
                raise RuntimeError("business_gepa_job_missing")
            best_result = __import__("best_strategy").build_best(job_id, run_full_comparison=True)
            value = best_result.get("comparison") or {}
            fixed_gate = value.get("fixed_500_gate") if isinstance(value, dict) else None
            passed = isinstance(fixed_gate, dict) and fixed_gate.get("passed") is True
            comparison = layout.PUBLISHED / "best-strategy.json"
            return StageResult("complete", gate_passed=passed,
                               artifact=str(comparison),
                               metrics=(value.get("metrics", {}).get("best", {}).get("all")
                                        if isinstance(value, dict) else None),
                               data={"callback": "business.fixed_comparison500",
                                     "comparison": str(comparison),
                                     "fixed_500_gate": fixed_gate})
        if stage == "publish":
            from offline_publish import publish
            publication = publish()
            return StageResult("complete", artifact=str(layout.PUBLISHED / "best-strategy.json"),
                               data={"callback": "business.publication_receipt",
                                     "publication": publication})
        raise RuntimeError("business_unknown_common_stage")

    result = LifecycleController(_BusinessDatasetAdapter(), callback, state_path).run()
    return result


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


def _resume_gepa_or_start(*, publish: bool = False) -> dict:
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
        if not publish:
            return {"status": "candidate_ready", "job_id": gepa.progress().get("job_id")}
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
    publication = (gepa.run_to_publication(initial_job_id=initial_job_id, publish=publish)
                   if initial_job_id else gepa.run_to_publication(publish=publish))
    if publication.get("status") in {"best_observed", "best_available"}:
        return publication
    return {"status": "published", "publication": publication}


def status() -> int:
    runner = _read(layout.GEPA / "status.json")
    lifecycle = _read(layout.GEPA / "lifecycle.json")
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
        "lifecycle": {key: lifecycle[key] for key in (
            "status", "phase", "stage_results", "last_error") if key in lifecycle},
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
        print(json.dumps({"ok": counts == {"train": 672, "val": 200, "holdout": 200},
                          "source_counts": counts, "output_root": str(layout.OUTPUT)},
                         ensure_ascii=False))
        return 0
    with full_lifecycle_lock():
        if args.fresh:
            clean_full_outputs()
        elif not (layout.TRAINING / "status.json").is_file():
            raise RuntimeError("resume_checkpoint_missing")
        # Both fresh and resume now use the shared stage contract. The
        # business callbacks retain their native capture/learn/GEPA logic;
        # the controller owns only order, durable state and gate dependencies.
        result = _run_common_business()
        train_result = ((result.get("stage_results", {}).get("train", {}).get("data", {})
                         if isinstance(result.get("stage_results"), dict) else {}).get("training"))
        if train_result is not None:
            # Preserve the previous CLI's compact top-level training receipt.
            result["training"] = train_result
    print(json.dumps(result, ensure_ascii=False, indent=2))
    # A rejected/incomplete 500-task acceptance is an honest terminal result,
    # but the one-command train→publish contract was not fulfilled.
    return 0 if result.get("status") in ("complete", "published") else 1


if __name__ == "__main__":
    raise SystemExit(main())
