#!/usr/bin/env python3
"""One-command BFCL v3 trajectory-evolution experiment.

Examples for operators who do not write code:

  python3 bfcl_experiment.py --fresh
  python3 bfcl_experiment.py --resume
  python3 bfcl_experiment.py --status
  python3 bfcl_experiment.py --check

The command is safe to run in tmux.  A task-level model/checker failure is
recorded and learning continues from usable trials; an infrastructure failure
pauses at the last completed batch.
"""

from __future__ import annotations

import argparse
from collections import Counter
from contextlib import contextmanager
from copy import deepcopy
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import signal
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
from bfcl_paths import (OUTPUT, SOURCE, TRAINING_DATASET, COLDSTART_DEPLOYMENT,
                        SPLIT_ID, LOCK_TAG)

TRAINING = OUTPUT / "training"
GEPA = OUTPUT / "gepa"
PUBLISHED = OUTPUT / "published"
# Dataset and immutable deployment inputs live beside this experiment code,
# never inside trace_output.  trace_output is reserved for regenerable
# evidence, checkpoints, memory and publication receipts.
DATASET = TRAINING_DATASET
DEPLOYMENT_ROOT = COLDSTART_DEPLOYMENT
DEPLOYMENT = DEPLOYMENT_ROOT / "runtime"
PLAN = DEPLOYMENT_ROOT / "plan.json"
STATUS = TRAINING / "status.json"
FULL_LOCK = OUTPUT.parent / ("." + LOCK_TAG + "-full.lock")
BINARY = ROOT / "build/trajectory-evolution/trajectory_evolution"
HOST = HERE / "bfcl_v3_host.py"

sys.path.insert(0, str(HERE))
from bfcl_v3_adapter import materialize  # noqa: E402


def read(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def save(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".new")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


@contextmanager
def full_lifecycle_lock():
    """Serialize BFCL cleanup, training, GEPA and publication as one run."""
    FULL_LOCK.parent.mkdir(parents=True, exist_ok=True)
    handle = FULL_LOCK.open("a+")
    try:
        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as exc:
        handle.close()
        raise RuntimeError("bfcl_experiment_already_running") from exc
    try:
        yield
    finally:
        fcntl.flock(handle, fcntl.LOCK_UN)
        handle.close()


def _remove_exact(path: Path, expected: Path) -> None:
    """Remove only a validated experiment-owned path during --fresh."""
    if not path.exists() and not path.is_symlink():
        return
    if path.is_symlink() or path.resolve() != expected.resolve():
        raise RuntimeError("bfcl_cleanup_path_invalid:" + str(path))
    if path.is_dir():
        shutil.rmtree(path)
    else:
        path.unlink()


def _load_env_file(path: Path, env: dict[str, str]) -> None:
    if not path.is_file():
        return
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key, value = key.strip(), value.strip()
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key):
            continue
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
            value = value[1:-1]
        env.setdefault(key, value)


def environment(*, require_model: bool = True) -> dict[str, str]:
    env = os.environ.copy()
    _load_env_file(ROOT / "examples/agent/business-simulator/.env", env)
    if env.get("AF_LLM_API_KEY"):
        env["OPENAI_API_KEY"] = env["AF_LLM_API_KEY"]
    if env.get("AF_LLM_MODEL"):
        env["OPENAI_MODEL"] = env["AF_LLM_MODEL"]
    if env.get("AF_LLM_BASE_URL"):
        env["OPENAI_BASE_URL"] = env["AF_LLM_BASE_URL"]
    for proxy_name in ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy",
                       "all_proxy", "BUSINESS_BROWSER_PROXY"):
        env.pop(proxy_name, None)
    env["NO_PROXY"] = env["no_proxy"] = "*"
    if env.get("AF_LLM_BASE_URL"):
        env["TRAJECTORY_MODEL_ENDPOINT_OVERRIDE"] = env["AF_LLM_BASE_URL"].rstrip("/") + "/chat/completions"
    if require_model and not all(env.get(name) for name in
                                 ("AF_LLM_API_KEY", "AF_LLM_MODEL", "AF_LLM_BASE_URL")):
        raise RuntimeError("bfcl_model_environment_missing")
    env.setdefault("EVOLUTION_BFCL_TOKEN", secrets.token_urlsafe(32))
    env["TRAJECTORY_UNCONDITIONAL_RESUME"] = "1"
    env.setdefault("AF_LLM_THINKING_MODE", "disabled")
    env.setdefault("AF_LLM_TIMEOUT", "120")
    env.setdefault("AF_LLM_MAX_RETRIES", "4")
    env.setdefault("AF_LLM_RETRY_JITTER", "0.25")
    env.setdefault("BFCL_POINT_TIMEOUT_MS", "1000")
    env.setdefault("BFCL_BASELINE_PATH", "/root/lai-code/baselines/gorilla/berkeley-function-call-leaderboard")
    interpreter = Path(env.get("AGENTFLOW_PYTHON", "/root/miniconda3/envs/agentflow-dev/bin/python3"))
    if interpreter.is_file():
        env["AGENTFLOW_PYTHON"] = str(interpreter)
        env["PATH"] = str(interpreter.parent) + os.pathsep + env.get("PATH", "")
        library = interpreter.parent.parent / "lib"
        if library.is_dir():
            env["LD_LIBRARY_PATH"] = str(library) + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    return env


def _source_ids(name: str) -> list[str]:
    return [line.strip() for line in (SOURCE / f"{name}_ids.txt").read_text().splitlines() if line.strip()]


def _host_revision(env: dict[str, str]) -> str:
    agent_files = [ROOT / "examples/agent/business-simulator/agent.md",
                   ROOT / "examples/agent/business-simulator/harness/bfcl_skill.py",
                   ROOT / "examples/agent/business-simulator/harness/bfcl_tool_runtime.py",
                   ROOT / "examples/agent/business-simulator/harness/business_sim/tool_scope.py",
                   ROOT / "examples/agent/business-simulator/harness/skills/bfcl-functions/SKILL.md"]
    values = {
        "host": hashlib.sha256(HOST.read_bytes()).hexdigest(),
        "adapter": hashlib.sha256((HERE / "bfcl_v3_adapter.py").read_bytes()).hexdigest(),
        "source": read(SOURCE / "manifest.json")["source_zip_sha256"],
        "model": env.get("AF_LLM_MODEL", ""),
        "endpoint": env.get("AF_LLM_BASE_URL", ""),
        "agent": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                  for path in agent_files if path.is_file()},
    }
    return "bfcl-host-v1:" + hashlib.sha256(json.dumps(values, sort_keys=True).encode()).hexdigest()


def _evaluation_config(task_ids: list[str], revision: str, parallelism: int) -> dict:
    native_splits = sorted({"holdout" if task_id in set(_source_ids("holdout")) else
                            "val" if task_id in set(_source_ids("selection")) else "train"
                            for task_id in task_ids})
    return {
        "schema_version": 1,
        "splits": native_splits,
        "task_ids": task_ids,
        "repetitions": 1,
        "parallelism": parallelism,
        "max_trials": len(task_ids),
        "timeout_ms": 900000,
        "max_output_bytes": 1048576,
        "max_trace_bytes": 8388608,
        "max_report_bytes": 536870912,
        "require_trace": True,
        "resources": {"nice": 10, "address_space_bytes": 8589934592, "cpu_seconds": 1200},
        "variants": [{
            "id": "bfcl-l0",
            "version": revision,
            # Keep the command portable and within the evaluator's relative
            # resource boundary; absolute /root paths are rejected by the
            # native privacy validator.
            "command": ["python3", "-B", "{config_dir}/../../bfcl_v3_host.py", "--evolution-entry"],
            "instrumentation_config": "service.json",
            "initial_state_transport": "entry",
            "entry": {
                "input": {name: {"$eval": name} for name in
                          ("messages", "user_id", "conversation_id", "run_key", "turn",
                           "initial_state", "constraints", "budget")},
                "output_pointer": "/response",
            },
            "pass_env": [
                "AF_LLM_API_KEY", "AF_LLM_BASE_URL", "AF_LLM_MODEL", "AF_LLM_TIMEOUT",
                "AF_LLM_MAX_RETRIES", "AF_LLM_RETRY_JITTER", "OPENAI_API_KEY", "OPENAI_BASE_URL", "OPENAI_MODEL",
                "BFCL_POINT_TIMEOUT_MS", "BFCL_BASELINE_PATH", "AGENTFLOW_DAEMON",
                "PYTHONPATH", "LD_LIBRARY_PATH",
            ],
        }],
        # A BFCL task is a multi-run conversation.  The native evaluator
        # stores the final run output under /output/final; using /output here
        # silently turns every strict result into "missing evidence".
        "checks": {"*": [{"id": "strict", "path": "/output/final/bfcl_valid",
                             "op": "equals", "value": True}]},
        # BFCL already supplies a deterministic multi-turn checker and the
        # host emits the exact strict/intent/loop metrics.  The generic
        # objective judge expects business-style observed iterations and can
        # turn a strict pass into evaluation_judge_failed, contaminating
        # training and GEPA inputs.  Keep its schema documented but disabled
        # for this benchmark; deterministic checks remain authoritative.
        "judge": {
            "enabled": False, "assessment_mode": "objectives",
            "batch_input_bytes": 262144, "max_batch_turns": 4,
            "max_evidence_lookups": 2, "evidence_field_bytes": 256,
            "answer_field_bytes": 2048, "max_repair_attempts": 2,
            "prompt": "prompts/task-judge-system.txt",
            "loop_review_prompt": "prompts/task-loop-review.txt",
            "consistency_prompt": "prompts/task-consistency.txt",
            "runtime_config": "runtime.json",
        },
        "artifact_retention": {
            "retain_trial_files": False, "retain_markdown": False,
            "retain_jsonl": False, "retain_report_pb": True,
            "drop_intermediate_pb": True,
        },
    }


def prepare(*, clean: bool) -> dict:
    if clean and SPLIT_ID:
        raise RuntimeError("bfcl_seen_split_use_start_or_resume")
    if clean:
        _remove_exact(OUTPUT, ROOT / "trace_output/bfcl-v3")
        _remove_exact(DATASET, HERE / "datasets/training-reviewed")
        _remove_exact(DEPLOYMENT_ROOT, HERE / "coldstart-deployment")
    if PLAN.is_file() and DATASET.is_dir() and STATUS.is_file():
        return read(PLAN)
    if OUTPUT.exists() or (DATASET.exists() and not SPLIT_ID) or DEPLOYMENT_ROOT.exists():
        raise RuntimeError("bfcl_partial_prepare_requires_fresh_or_resume")
    env = environment()
    TRAINING.mkdir(parents=True)
    if not DATASET.is_dir():
        materialize(DATASET, source=SOURCE)
    subprocess.run([str(BINARY), "dataset", "check", str(DATASET), "--require-reviewed"],
                   cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
    DEPLOYMENT.mkdir(parents=True)
    prompts = ROOT / "tools/trajectory_evolution/config/prompts"
    shutil.copytree(prompts, DEPLOYMENT / "prompts")
    runtime = read(ROOT / "tools/trajectory_evolution/config/standard-runtime.json")
    runtime["model"].update(
        endpoint=env["AF_LLM_BASE_URL"].rstrip("/") + "/chat/completions",
        model_id=env["AF_LLM_MODEL"], api_key_env="OPENAI_API_KEY",
        thinking_mode="disabled", timeout_ms=120000, max_retries=4,
        max_output_tokens=16384, sessionlog_max_output_tokens=16384,
    )
    runtime["evolution"]["memory_review_parallelism"] = 16
    save(DEPLOYMENT / "runtime.json", runtime)
    service = read(ROOT / "tools/trajectory_evolution/config/boundary-service.json")
    service.update(application_id="bfcl-v3", data_dir=str((TRAINING / "evolution").relative_to(ROOT)),
                   port=0, workers=16, max_level="L0", auto_evaluate=True,
                   principals=[{"user_id": "bfcl-training", "token_env": "EVOLUTION_BFCL_TOKEN"}],
                   runtime_config="runtime.json", experience_template_file="prompts/runtime-experience.txt")
    service["instrumentation"] = {"level": "L0", "capabilities": ["model", "actions", "loop", "schedule"]}
    service["learning"].update(min_new_runs=4, min_interval_ms=0, max_batch_runs=32,
                                max_batch_bytes=16777216, analysis_parallelism=16,
                                max_analysis_retries=2, retry_backoff_ms=15000)
    service["sharing"].update(enabled=True, min_interval_ms=1000,
                               max_model_calls_per_cycle=32, model_parallelism=16)
    save(DEPLOYMENT / "service.json", service)
    revision = _host_revision(env)
    train_ids = _source_ids("train")
    task_map = {}
    for line in (DATASET / "train.jsonl").read_text(encoding="utf-8").splitlines():
        if line.strip():
            task = json.loads(line)
            task_map[str(task["task_id"])] = task
    batches = []
    current, turns = [], 0
    for task_id in train_ids:
        task = task_map.get(task_id)
        if task is None:
            raise RuntimeError("bfcl_training_task_missing:" + task_id)
        size = 1 + len(task.get("followup_inputs", []))
        if current and (len(current) >= 16 or turns + size > 64):
            name = f"{len(batches) + 1:03d}"
            config = _evaluation_config(current, revision, 16)
            path = DEPLOYMENT / f"training-{name}.json"
            save(path, config)
            batches.append({"name": name, "task_ids": current,
                            "config": str(path.relative_to(ROOT))})
            current, turns = [], 0
        current.append(task_id)
        turns += size
    if current:
        name = f"{len(batches) + 1:03d}"
        config = _evaluation_config(current, revision, 16)
        path = DEPLOYMENT / f"training-{name}.json"
        save(path, config)
        batches.append({"name": name, "task_ids": current,
                        "config": str(path.relative_to(ROOT))})
    plan = {
        "schema_version": 1,
        "split_id": SPLIT_ID or "legacy",
        "source": str(SOURCE.relative_to(ROOT)),
        "mode": "BFCL v3 train400 -> memory -> GEPA search150/selection50 -> holdout200",
        "dataset": str(DATASET.relative_to(ROOT)),
        "source_zip_sha256": read(SOURCE / "manifest.json")["source_zip_sha256"],
        "host_revision": revision,
        "counts": {name: len(_source_ids(name)) for name in ("train", "search", "selection", "holdout")},
        "batch_size": 16,
        "max_batch_turns": 64,
        "parallelism": 16,
        "batches": batches,
        "service": str((DEPLOYMENT / "service.json").relative_to(ROOT)),
        "artifact_budget_bytes": 5 * 1024 ** 3,
        "created_at": datetime.now(timezone.utc).isoformat(),
    }
    save(PLAN, plan)
    save(STATUS, {"status": "prepared", "phase": "preflight", "completed_tasks": 0,
                  "learned_batches": [], "updated_at": datetime.now(timezone.utc).isoformat()})
    return plan


def repack_unstarted_plan() -> dict:
    """Merge only unstarted training tasks to the business 16-way schedule.

    Completed reports/memory are immutable.  This lets a paused early plan be
    accelerated without replaying learned tasks or mixing two task batches.
    """
    plan = read(PLAN)
    if int(plan.get("batch_size", 0) or 0) >= 16 and int(plan.get("max_batch_turns", 0) or 0) >= 64:
        return plan
    state = read(STATUS)
    learned = {str(name) for name in state.get("learned_batches", [])}
    learned_ids = set()
    kept = []
    for batch in plan.get("batches", []):
        name = str(batch["name"])
        destination = TRAINING / "batches" / name
        if name in learned:
            learned_ids.update(batch.get("task_ids", []))
            kept.append(batch)
        elif destination.exists() and any(destination.iterdir()):
            raise RuntimeError("bfcl_repack_partial_batch_requires_review")
        old_config = DEPLOYMENT / f"training-{name}.json"
        if name not in learned and old_config.exists():
            old_config.unlink()
    task_map = {}
    for line in (DATASET / "train.jsonl").read_text(encoding="utf-8").splitlines():
        if line.strip():
            task = json.loads(line)
            task_map[str(task["task_id"])] = task
    remaining = [task_id for task_id in _source_ids("train") if task_id not in learned_ids]
    batches = list(kept)
    current, turns = [], 0
    for task_id in remaining:
        task = task_map.get(task_id)
        if task is None:
            raise RuntimeError("bfcl_repack_task_missing:" + task_id)
        size = 1 + len(task.get("followup_inputs", []))
        if current and (len(current) >= 16 or turns + size > 64):
            name = f"{len(batches) + 1:03d}"
            path = DEPLOYMENT / f"training-{name}.json"
            save(path, _evaluation_config(current, plan["host_revision"], 16))
            batches.append({"name": name, "task_ids": current,
                            "config": str(path.relative_to(ROOT))})
            current, turns = [], 0
        current.append(task_id)
        turns += size
    if current:
        name = f"{len(batches) + 1:03d}"
        path = DEPLOYMENT / f"training-{name}.json"
        save(path, _evaluation_config(current, plan["host_revision"], 16))
        batches.append({"name": name, "task_ids": current,
                        "config": str(path.relative_to(ROOT))})
    plan.update(batches=batches, batch_size=16, max_batch_turns=64, parallelism=16,
                repacked_at=datetime.now(timezone.utc).isoformat(),
                repacked_completed_batches=sorted(learned))
    save(PLAN, plan)
    return plan


def _terminal_output(trial: dict) -> dict:
    evidence = trial.get("evidence") or {}
    traces = evidence.get("traces") or ([evidence["trace"]] if evidence.get("trace") else [])
    for trace in reversed(traces):
        for event in reversed(trace.get("events") or []):
            if event.get("kind") == "run.end" and isinstance(event.get("output"), dict):
                return event["output"]
    output = evidence.get("output")
    if isinstance(output, dict):
        if isinstance(output.get("final"), dict):
            return output["final"]
        return output
    return {}


def metrics_from_trials(trials: list[dict]) -> dict:
    completed = [trial for trial in trials if trial.get("execution_status") == "completed"]
    outputs = [_terminal_output(trial) for trial in completed]
    # A trial with verdict=error has execution cost, but no reliable score or
    # loop observation.  Keep it in the cost totals while excluding it from
    # quality/loop aggregates.  In particular, an evaluation trace mismatch
    # may still contain a stale final output; treating that output as a scored
    # failure would make the native judge error vote in BFCL quality.
    scored = [(trial, output) for trial, output in zip(completed, outputs)
              if trial.get("verdict") != "error" and isinstance(output.get("bfcl_valid"), bool)]
    scored_outputs = [output for _, output in scored]
    strict = sum(bool(value.get("bfcl_valid")) for value in scored_outputs)
    def average(name: str):
        values = [float(item[name]) for item in scored_outputs
                  if isinstance(item.get(name), (int, float))]
        return sum(values) / len(values) if values else None
    evidence_metrics = [trial.get("evidence", {}).get("metrics", {}) for trial in completed]
    tokens = sum(int(item.get("tokens", 0) or 0) for item in outputs)
    if not tokens:
        tokens = sum(int(item.get("tokens", 0) or 0) for item in evidence_metrics)
    calls = sum(int(item.get("model_calls", 0) or 0) for item in outputs)
    if not calls:
        calls = sum(int(item.get("llm_calls", 0) or 0) for item in evidence_metrics)
    # Latency is a cost signal, so include errored executions when available;
    # only quality and loop observations use the scored subset.
    latency_values = [float(item["latency_ms"]) for item in outputs
                      if isinstance(item.get("latency_ms"), (int, float))]
    latency = (sum(latency_values) / len(latency_values)) if latency_values else None
    if latency is None:
        latency = (sum(float(t.get("duration_ms", 0) or 0) for t in completed) / len(completed)
                   if completed else None)
    return {
        "completed": len(completed), "strict_passed": strict,
        "scored": len(scored_outputs),
        "errored": len(completed) - len(scored_outputs),
        "quality": strict / len(scored_outputs) if scored_outputs else None,
        "avg_invalid_loop": average("invalid_loop_count"),
        "avg_latency_ms": latency,
        "tokens": tokens, "avg_tokens": tokens / len(completed) if completed else None,
        "model_calls": calls, "avg_model_calls": calls / len(completed) if completed else None,
        "function_f1": average("function_f1"), "intent_f1": average("intent_f1"),
        "intent_recognition_rate": average("intent_recognition_rate"),
        "l1_applied": sum(int(item.get("l1_applied", 0) or 0) for item in outputs),
        "l2_applied": sum(int(item.get("l2_applied", 0) or 0) for item in outputs),
        "l3_applied": sum(int(item.get("l3_applied", 0) or 0) for item in outputs),
    }


def _privacy_scan(path: Path, env: dict[str, str]) -> None:
    data = path.read_bytes()
    secrets_found = [value.encode() for key, value in env.items()
                     if re.search(r"KEY|TOKEN|SECRET|PASSWORD", key, re.I)
                     and len(value) >= 16 and not value.isdigit()]
    if any(value in data for value in secrets_found):
        raise RuntimeError("bfcl_privacy_scan_hit")
    if re.search(rb"\b(?:ark|sk)-[A-Za-z0-9_-]{16,}\b|Bearer\s+[A-Za-z0-9._~+/-]{16,}", data, re.I):
        raise RuntimeError("bfcl_credential_pattern_hit")


_LEARNING_TERMINAL_STATUSES = frozenset(("completed", "completed_with_unresolved"))
_LEARNING_PENDING_FIELDS = (
    "pending_runs", "unpublished_runs", "training_pending_users",
    "memory_pending_reviews", "agent_evaluation_pending_jobs", "shared_pending_users",
)


def learning_result_usable(learning: dict | None, returncode: int) -> bool:
    """Use native JSON status as the learning checkpoint boundary.

    Native learn may return 3 for isolated task failures.  A terminal pipeline
    with no pending queues remains usable; malformed, pending or manager-failed
    results must be retried so they never become a false completed batch.
    """
    if returncode not in (0, 3) or not isinstance(learning, dict) or not learning:
        return False
    if learning.get("shared_status") not in _LEARNING_TERMINAL_STATUSES:
        return False
    if learning.get("agent_evaluation_error"):
        return False
    outcomes = learning.get("run_outcomes")
    if not isinstance(outcomes, dict) or not outcomes:
        return False
    for value in outcomes.values():
        if isinstance(value, bool) or not isinstance(value, (int, float)) or value < 0:
            return False
    for name in _LEARNING_PENDING_FIELDS:
        value = learning.get(name, 0)
        if isinstance(value, bool) or not isinstance(value, (int, float)) or value < 0 or value:
            return False
    for name in ("training_failed_users", "memory_management_failed_users",
                 "shared_quarantined_users"):
        value = learning.get(name, 0)
        if isinstance(value, bool) or not isinstance(value, (int, float)) or value < 0 or value:
            return False
    managers = learning.get("memory_management", {})
    if managers is not None:
        if not isinstance(managers, dict):
            return False
        for manager in managers.values():
            if not isinstance(manager, dict):
                return False
            if manager.get("status") in ("pending", "running", "failed", "waiting_for_model"):
                return False
            pending = manager.get("pending", 0)
            if isinstance(pending, bool) or not isinstance(pending, (int, float)) or pending < 0 or pending:
                return False
    return True


def _compact_batch(destination: Path) -> None:
    """Drop regenerable per trial files while keeping canonical reports."""
    for name in ("trials", "report.md", "results.jsonl"):
        path = destination / name
        if path.is_dir():
            shutil.rmtree(path)
        elif path.is_file():
            path.unlink()


def _isolated_learning_input(destination: Path, report: dict) -> tuple[Path, Path | None]:
    """Build a throw-away report that excludes judge/transport error trials.

    The capture report remains the canonical cost and audit record.  Native
    ``learn`` imports every admissible trace it receives, so passing the
    canonical report would publish errored traces into private memory even
    though their score is excluded above.  Re-sign only the temporary report
    and leave the original report/manifest untouched.
    """
    trials = report.get("trials") or []
    eligible = [trial for trial in trials if trial.get("verdict") != "error"]
    if len(eligible) == len(trials):
        return destination, None
    isolated = destination / ".learning-input"
    if isolated.exists():
        shutil.rmtree(isolated)
    isolated.mkdir(parents=True)
    filtered = dict(report)
    filtered["trials"] = eligible
    save(isolated / "report.json", filtered)
    manifest = read(destination / "manifest.json")
    manifest["files"] = dict(manifest.get("files") or {})
    manifest["files"]["report.json"] = hashlib.sha256(
        (isolated / "report.json").read_bytes()).hexdigest()
    save(isolated / "manifest.json", manifest)
    report_pb = destination / "report.pb"
    if report_pb.is_file():
        shutil.copy2(report_pb, isolated / "report.pb")
    return isolated, isolated


def compact_completed_outputs() -> dict:
    """Compact only after all 400 training tasks have learned successfully."""
    state = read(STATUS)
    if state.get("status") != "training_completed" or int(state.get("completed_tasks", 0)) < 400:
        raise RuntimeError("training_not_complete_for_compaction")
    removed_files = removed_bytes = 0
    def remove(path: Path) -> None:
        nonlocal removed_files, removed_bytes
        if not path.exists() or path.is_symlink():
            return
        if path.is_file():
            removed_files += 1; removed_bytes += path.stat().st_size; path.unlink(); return
        for child in path.rglob("*"):
            if child.is_file():
                removed_files += 1; removed_bytes += child.stat().st_size
        shutil.rmtree(path)
    for batch_dir in sorted((TRAINING / "batches").glob("[0-9][0-9][0-9]")):
        _compact_batch(batch_dir)
    evolution = TRAINING / "evolution" / "by-user" / "bfcl-training"
    for name in ("traces", "evaluation", "batches"):
        remove(evolution / name)
    summary = {"status": "compacted", "removed_files": removed_files,
               "removed_bytes": removed_bytes,
               "canonical_reports": len(list((TRAINING / "batches").glob("*/report.json")))}
    save(TRAINING / "compaction.json", summary)
    return summary


def run_training(*, retry_failed: bool = False) -> dict:
    plan = read(PLAN)
    env = environment()
    lock_path = OUTPUT / "runner.lock"
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    lock = lock_path.open("a")
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    state = read(STATUS)
    learned = set(state.get("learned_batches") or [])
    # A resumed run has crossed a checkpoint boundary; do not keep a prior
    # requested-stop marker as if it were a current failure.
    state.pop("error", None)
    stop: list[str] = []
    signal.signal(signal.SIGTERM, lambda *_: stop.append("requested_stop"))
    signal.signal(signal.SIGINT, lambda *_: stop.append("requested_stop"))
    try:
        for batch in plan["batches"]:
            if batch["name"] in learned:
                continue
            destination = TRAINING / "batches" / batch["name"]
            report = destination / "report.json"
            state.update(status="running", phase="capture", batch=batch["name"], pid=os.getpid(),
                         updated_at=datetime.now(timezone.utc).isoformat())
            save(STATUS, state)
            resume = []
            if report.is_file():
                try:
                    report_complete = bool(read(report).get("complete"))
                except (OSError, ValueError):
                    report_complete = False
                if not report_complete and retry_failed:
                    attempt = 1
                    previous = destination.with_name(destination.name + f"-attempt-{attempt:02d}")
                    while previous.exists():
                        attempt += 1
                        previous = destination.with_name(destination.name + f"-attempt-{attempt:02d}")
                    destination.rename(previous)
                    resume = ["--resume-report", str(previous / "report.json")]
                    report = destination / "report.json"
            if not report.is_file():
                if destination.exists():
                    if not retry_failed:
                        raise RuntimeError("bfcl_incomplete_batch_requires_resume")
                    attempt = 1
                    previous = destination.with_name(destination.name + f"-attempt-{attempt:02d}")
                    while previous.exists():
                        attempt += 1
                        previous = destination.with_name(destination.name + f"-attempt-{attempt:02d}")
                    destination.rename(previous)
                    resume = ["--resume-report", str(previous / "report.json")]
                command = [str(BINARY), "dataset", "run", str(DATASET), "--config", batch["config"],
                           "--output-root", str(destination), *resume]
                process = subprocess.run(command, cwd=ROOT, env=env)
                if process.returncode not in (0, 1, 3) or not report.is_file():
                    raise RuntimeError("bfcl_capture_failed")
            _privacy_scan(report, env)
            value = read(report)
            if not value.get("complete"):
                raise RuntimeError("bfcl_capture_incomplete")
            state.update(phase="learning", completed_tasks=sum(len(read(TRAINING / "batches" / done / "report.json").get("trials", []))
                                                                for done in learned) + len(value.get("trials", [])),
                         metrics=metrics_from_trials(value.get("trials", [])))
            save(STATUS, state)
            learning_destination, isolated_input = _isolated_learning_input(destination, value)
            try:
                if value.get("trials") and not any(
                        trial.get("verdict") != "error" for trial in value["trials"]):
                    # A batch containing only errors has no admissible memory
                    # evidence.  It is still a completed capture and its
                    # tokens/calls remain in the canonical report metrics.
                    learning = subprocess.CompletedProcess([], 0, json.dumps({
                        "shared_status": "completed",
                        "run_outcomes": {"errored_trials": 0},
                        "training_no_admissible_trials": True,
                    }), "")
                else:
                    learning = subprocess.run(
                        [str(BINARY), "learn", "--config", plan["service"],
                         "--training-evaluation", str(learning_destination),
                         "--dataset", str(DATASET)],
                        cwd=ROOT, env=env, text=True, capture_output=True)
                try:
                    learned_status = json.loads(learning.stdout)
                except ValueError:
                    raise RuntimeError("bfcl_learning_status_missing") from None
                save(destination / "learning-status.json", learned_status)
                if not learning_result_usable(learned_status, learning.returncode):
                    raise RuntimeError("bfcl_learning_failed")
            finally:
                if isolated_input is not None and isolated_input.exists():
                    shutil.rmtree(isolated_input)
            learned.add(batch["name"])
            _compact_batch(destination)
            state.update(status="running", phase="batch_completed", learned_batches=sorted(learned),
                         completed_tasks=sum(len(read(TRAINING / "batches" / done / "report.json").get("trials", []))
                                             for done in learned),
                         artifact_bytes=sum(p.stat().st_size for p in OUTPUT.rglob("*") if p.is_file()),
                         updated_at=datetime.now(timezone.utc).isoformat())
            if state["artifact_bytes"] > plan["artifact_budget_bytes"]:
                raise RuntimeError("bfcl_artifact_budget_exceeded")
            save(STATUS, state)
            print(json.dumps({"event": "learned", "batch": batch["name"],
                              "completed_tasks": state["completed_tasks"],
                              "artifact_bytes": state["artifact_bytes"]}, ensure_ascii=False), flush=True)
            if stop:
                raise RuntimeError("requested_stop")
        state.update(status="training_completed", phase="gepa_pending",
                     completed_tasks=sum(len(read(TRAINING / "batches" / done / "report.json").get("trials", []))
                                         for done in learned),
                     updated_at=datetime.now(timezone.utc).isoformat())
        save(STATUS, state)
        state["compaction"] = compact_completed_outputs()
        save(STATUS, state)
        return state
    except Exception as error:
        state.update(status="paused", error=str(error) if isinstance(error, RuntimeError) else type(error).__name__,
                     updated_at=datetime.now(timezone.utc).isoformat())
        save(STATUS, state)
        raise
    finally:
        fcntl.flock(lock, fcntl.LOCK_UN)
        lock.close()


def run_training_with_recovery() -> dict:
    """Retry only recoverable native failures, preserving each checkpoint."""
    recoverable = {"bfcl_incomplete_batch_requires_resume", "bfcl_capture_failed",
                   "bfcl_capture_incomplete", "bfcl_learning_status_missing",
                   "bfcl_learning_failed"}
    last_error = None
    for attempt in range(1, 4):
        try:
            return run_training(retry_failed=attempt > 1)
        except RuntimeError as error:
            last_error = error
            state = read(STATUS) if STATUS.is_file() else {}
            code = str(state.get("error", error))
            if code == "requested_stop":
                raise RuntimeError("training_paused") from error
            if code not in recoverable or attempt >= 3:
                raise
            print(json.dumps({"event": "training_recover", "attempt": attempt,
                              "error": code}, ensure_ascii=False), flush=True)
    raise RuntimeError("bfcl_training_recovery_limit") from last_error


def check() -> dict:
    manifest = read(SOURCE / "manifest.json")
    with (SOURCE / "bfcl_v3_splits.zip").open("rb") as stream:
        digest = hashlib.sha256(stream.read()).hexdigest()
    checks = {
        "binary": BINARY.is_file(), "host": HOST.is_file(),
        "source_hash": digest == manifest["source_zip_sha256"],
        "counts": manifest.get("counts") == {"train": 400, "search": 150,
                                                "selection": 50, "holdout": 200},
        "dataset_outside_output": DATASET.resolve() not in (OUTPUT.resolve(),)
            and OUTPUT.resolve() not in DATASET.resolve().parents,
        "checker": False, "fixture_dependencies": False,
        "python_compile": False, "native_dataset": False,
    }
    # Capture uses the interpreter selected for agentflowd.  Running these
    # probes with the shell's Python can report a false green when a lazy BFCL
    # fixture dependency exists in one environment but not the other.
    probe_python = Path(os.environ.get("AGENTFLOW_PYTHON", sys.executable))
    if not probe_python.is_file():
        probe_python = Path(sys.executable)
    compile_result = subprocess.run([str(probe_python), "-m", "py_compile", str(HOST),
                                     str(HERE / "bfcl_v3_adapter.py"), str(Path(__file__))])
    checks["python_compile"] = compile_result.returncode == 0
    baseline_env = os.environ.copy()
    baseline_env["PYTHONPATH"] = "/root/lai-code/baselines/gorilla/berkeley-function-call-leaderboard"
    checker = subprocess.run([str(probe_python), "-c",
        "from bfcl_eval.eval_checker.multi_turn_eval.multi_turn_checker import multi_turn_checker"],
        env=baseline_env)
    checks["checker"] = checker.returncode == 0
    # Import every official multi-turn fixture module before capture.  The
    # checker module alone does not import MathAPI (or other lazy backends),
    # so a missing dependency can otherwise surface only after hundreds of
    # model calls and be mistaken for Agent failures.
    baseline_env["BFCL_SOURCE_ZIP"] = str(SOURCE / "bfcl_v3_splits.zip")
    fixture_probe = (
        "import importlib,json,os,zipfile; "
        "from bfcl_eval.constants.executable_backend_config import CLASS_FILE_PATH_MAPPING; "
        "classes=set(); z=zipfile.ZipFile(os.environ['BFCL_SOURCE_ZIP']); "
        "[classes.update(json.loads(line).get('involved_classes',[])) "
        " for name in z.namelist() if name.endswith('.jsonl') "
        " for line in z.read(name).decode().splitlines() if line.strip()]; "
        "[importlib.import_module(CLASS_FILE_PATH_MAPPING[c]) for c in sorted(classes)]"
    )
    dependencies = subprocess.run([str(probe_python), "-c", fixture_probe], env=baseline_env)
    checks["fixture_dependencies"] = dependencies.returncode == 0
    if DATASET.is_dir():
        checks["native_dataset"] = subprocess.run([str(BINARY), "dataset", "check", str(DATASET),
                                                    "--require-reviewed"], cwd=ROOT,
                                                   stdout=subprocess.DEVNULL).returncode == 0
    else:
        # A clean checkout has no generated native view yet; source integrity
        # and compilation are the meaningful preflight gates before prepare().
        checks["native_dataset"] = True
    result = {"ok": all(checks.values()), "checks": checks,
              "artifact_bytes": sum(p.stat().st_size for p in OUTPUT.rglob("*") if p.is_file()) if OUTPUT.exists() else 0,
              "artifact_budget_bytes": 5 * 1024 ** 3}
    if OUTPUT.exists():
        save(OUTPUT / "preflight.json", result)
    if not result["ok"]:
        raise RuntimeError("bfcl_preflight_failed")
    return result


def status() -> dict:
    state = read(STATUS) if STATUS.is_file() else {"status": "not_prepared", "completed_tasks": 0}
    trials = []
    if (TRAINING / "batches").is_dir():
        for report in sorted((TRAINING / "batches").glob("*/report.json")):
            try:
                trials.extend(read(report).get("trials", []))
            except (OSError, ValueError):
                continue
    state["l0"] = metrics_from_trials(trials)
    state["parent"] = state.get("parent", "待生成")
    state["current"] = state.get("current", "待生成")
    if (GEPA / "status.json").is_file():
        try:
            import bfcl_gepa
            state["gepa"] = {
                "runner": read(GEPA / "status.json"),
                "progress": bfcl_gepa.progress(),
            }
        except Exception as error:
            state["gepa"] = {"status": "unavailable", "error": type(error).__name__}
    if (PUBLISHED / "latest.json").is_file():
        try:
            state["published"] = read(PUBLISHED / "latest.json")
        except (OSError, ValueError):
            state["published"] = {"status": "invalid"}
    state["artifact_bytes"] = sum(p.stat().st_size for p in OUTPUT.rglob("*") if p.is_file()) if OUTPUT.exists() else 0
    return state


def _resume_gepa_or_start() -> dict:
    import bfcl_gepa
    if not bfcl_gepa.PLAN.is_file():
        if bfcl_gepa.STATUS.is_file() or bfcl_gepa.WORKSPACE.exists():
            raise RuntimeError("gepa_resume_plan_missing")
        if SPLIT_ID:
            with bfcl_gepa.lifecycle_lock():
                bfcl_gepa.prepare(**bfcl_gepa.FRESH_SEARCH_LIMITS)
                return bfcl_gepa.run_to_publication()
        return bfcl_gepa.fresh()
    runner = read(bfcl_gepa.STATUS) if bfcl_gepa.STATUS.is_file() else {}
    if runner.get("status") == "published":
        return {"status": "published", "publication": runner.get("publication", {})}
    if runner.get("status") == "candidate_ready":
        from bfcl_publish import publish
        publication = publish()
        runner.update(status="published", publication=publication,
                      published_at=datetime.now(timezone.utc).isoformat())
        save(bfcl_gepa.STATUS, runner)
        return {"status": "published", "publication": publication}
    return {"status": "published", "publication": bfcl_gepa.run_to_publication()}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--start", action="store_true", help="Start isolated seen split, or resume its existing checkpoint")
    group.add_argument("--fresh", action="store_true")
    group.add_argument("--resume", action="store_true")
    group.add_argument("--status", action="store_true")
    group.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    if args.status:
        print(json.dumps(status(), ensure_ascii=False, indent=2)); return 0
    if args.check:
        print(json.dumps(check(), ensure_ascii=False, indent=2)); return 0
    with full_lifecycle_lock():
        if args.start:
            if not SPLIT_ID:
                raise RuntimeError("bfcl_start_requires_isolated_split")
            if not (PLAN.is_file() and STATUS.is_file()):
                prepare(clean=False)
        elif args.fresh:
            prepare(clean=True)
        elif not (PLAN.is_file() and STATUS.is_file() and DATASET.is_dir()):
            raise RuntimeError("resume_checkpoint_missing")
        if args.resume:
            repack_unstarted_plan()
        print(json.dumps(check(), ensure_ascii=False), flush=True)
        training_state = run_training_with_recovery()
        if training_state.get("status") != "training_completed":
            raise RuntimeError("training_stage_not_complete")
        result = _resume_gepa_or_start()
        print(json.dumps({"training": training_state, "gepa": result},
                         ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
