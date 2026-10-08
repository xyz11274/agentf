#!/usr/bin/env python3
"""Run true multi-turn retail capture and native trajectory learning."""
from __future__ import annotations
import argparse
from concurrent.futures import ThreadPoolExecutor
from copy import deepcopy
from datetime import datetime, timezone
import fcntl
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
OUTPUT = ROOT / "trace_output/tau2-retail-verified"
TRAINING = OUTPUT / "training"
GEPA = OUTPUT / "gepa"
PUBLISHED = OUTPUT / "published"
RUNTIME = HERE / "runtime/training"
DEPLOYMENT = RUNTIME
DATASET = HERE / "datasets/native"
BASELINE_DATASET = HERE / "datasets/gepa-view.json"
SOURCE = HERE / "datasets/source"
BINARY = ROOT / "trace_output/business-simulator/published/seed004/runtime/trajectory_evolution"
HOST = HERE / "tau2_host.py"
PYTHON = HERE / "runtime/venv/bin/python"
PLAN = RUNTIME / "plan.json"
STATUS = TRAINING / "status.json"
HOST_VERSION = "tau2-l0-verified-864350a-v1"
sys.path.insert(0, str(HERE.parent / "bfcl-v3"))
from bfcl_experiment import (environment as _environment, _privacy_scan,
                            learning_result_usable, _isolated_learning_input)


def read(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def save(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".new")
    temporary.write_text(json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
    temporary.replace(path)


write = save


def environment(*, require_model=True):
    env = _environment(require_model=require_model)
    env["AGENTFLOW_PYTHON"] = str(PYTHON)
    env["PATH"] = str(PYTHON.parent) + os.pathsep + env.get("PATH", "")
    env["TRAJECTORY_UNCONDITIONAL_RESUME"] = "1"
    env.setdefault("EVOLUTION_TAU2_TOKEN", secrets.token_urlsafe(32))
    env.setdefault("TAU2_SEED", "300")
    env.setdefault("TAU2_MAX_STEPS", "100")
    env.setdefault("TAU2_MAX_ERRORS", "10")
    env.setdefault("TAU2_USER_MAX_TOKENS", "32768")
    return env


def ids(label):
    return ["tau2-retail-" + s for s in (SOURCE / f"{label}_ids.txt").read_text().splitlines() if s]


def _evaluation_config(task_ids, parallelism, *, version=HOST_VERSION, variant_id="candidate"):
    val, holdout = set(ids("val")), set(ids("holdout"))
    splits = sorted({"val" if t in val else "holdout" if t in holdout else "train" for t in task_ids})
    return {
        "schema_version": 1, "splits": splits, "task_ids": task_ids,
        "repetitions": 1, "parallelism": parallelism, "max_trials": len(task_ids),
        "timeout_ms": 900000, "max_output_bytes": 1048576,
        "max_trace_bytes": 16777216, "max_report_bytes": 536870912, "require_trace": True,
        "resources": {"nice": 10, "address_space_bytes": 8589934592, "cpu_seconds": 1200},
        "variants": [{"id": variant_id, "version": version,
            "command": ["{config_dir}/../venv/bin/python", "-B", "{config_dir}/../../tau2_host.py", "--run-entry"],
            "instrumentation_config": "service.json", "initial_state_transport": "entry",
            "entry": {"input": {name: {"$eval": name} for name in
                      ("messages", "user_id", "conversation_id", "run_key", "turn", "initial_state", "constraints", "budget")},
                      "output_pointer": "/response"},
            "pass_env": ["AF_LLM_API_KEY", "AF_LLM_BASE_URL", "AF_LLM_MODEL", "AF_LLM_TIMEOUT",
                         "AF_LLM_MAX_RETRIES", "AF_LLM_RETRY_JITTER", "AF_LLM_THINKING_MODE",
                         "OPENAI_API_KEY", "OPENAI_BASE_URL", "OPENAI_MODEL", "TAU2_USER_MODEL",
                         "TAU2_SEED", "TAU2_MAX_STEPS", "TAU2_MAX_ERRORS", "TAU2_USER_MAX_TOKENS",
                         "AGENTFLOW_PYTHON", "AGENTFLOW_DAEMON", "PYTHONPATH", "LD_LIBRARY_PATH"]}],
        "checks": {"*": [{"id": "strict", "path": "/output/tau2_reward", "op": "equals", "value": 1.0}]},
        "judge": {"enabled": False, "assessment_mode": "objectives", "batch_input_bytes": 262144,
                  "max_batch_turns": 4, "max_evidence_lookups": 2, "evidence_field_bytes": 256,
                  "answer_field_bytes": 32768, "max_repair_attempts": 2,
                  "prompt": "prompts/task-judge-system.txt", "loop_review_prompt": "prompts/task-loop-review.txt",
                  "consistency_prompt": "prompts/task-consistency.txt", "runtime_config": "runtime.json"},
        "artifact_retention": {"retain_trial_files": False, "retain_markdown": False,
                               "retain_jsonl": False, "retain_report_pb": True, "drop_intermediate_pb": True},
    }


def prepare(*, train_parallelism=6, baseline_parallelism=2, require_model=True):
    env = environment(require_model=require_model)
    for path in (TRAINING, GEPA, PUBLISHED, RUNTIME): path.mkdir(parents=True, exist_ok=True)
    if not (RUNTIME / "prompts").is_dir():
        shutil.copytree(ROOT / "tools/trajectory_evolution/config/prompts", RUNTIME / "prompts")
    runtime = read(ROOT / "tools/trajectory_evolution/config/standard-runtime.json")
    runtime["model"].update(endpoint=env.get("AF_LLM_BASE_URL", "").rstrip("/") + "/chat/completions",
        model_id=env.get("AF_LLM_MODEL", ""), api_key_env="OPENAI_API_KEY", thinking_mode="disabled",
        timeout_ms=120000, max_retries=4, max_output_tokens=16384, sessionlog_max_output_tokens=16384)
    runtime["evaluation"].update(require_chinese_reason=False)
    runtime["evolution"].update(require_chinese_reason=False, memory_review_parallelism=4)
    save(RUNTIME / "runtime.json", runtime)
    service = read(ROOT / "tools/trajectory_evolution/config/boundary-service.json")
    service.update(application_id="tau2-retail-verified", data_dir=str((TRAINING / "evolution").relative_to(ROOT)),
        port=0, workers=16, max_level="L0", auto_evaluate=True,
        principals=[{"user_id": "tau2-training", "token_env": "EVOLUTION_TAU2_TOKEN"}],
        runtime_config="runtime.json", experience_template_file="prompts/runtime-experience.txt")
    service["instrumentation"] = {"level": "L0", "capabilities": ["model", "actions", "loop", "schedule"]}
    service["learning"].update(min_new_runs=1, min_interval_ms=0, max_batch_runs=12, max_batch_bytes=33554432,
                                analysis_parallelism=4, max_analysis_retries=2, retry_backoff_ms=15000)
    service["sharing"].update(enabled=True, min_interval_ms=1000, max_model_calls_per_cycle=32, model_parallelism=4)
    save(RUNTIME / "service.json", service)
    training_ids = ids("train")
    batches = []
    for start in range(0, len(training_ids), 12):
        name = f"{len(batches)+1:03d}"
        config = RUNTIME / f"training-{name}.json"
        task_ids = training_ids[start:start+12]
        save(config, _evaluation_config(task_ids, train_parallelism))
        batches.append({"name": name, "task_ids": task_ids, "config": str(config)})
    save(RUNTIME / "baseline-val.json", _evaluation_config(ids("val"), baseline_parallelism))
    save(RUNTIME / "baseline-holdout.json", _evaluation_config(ids("holdout"), baseline_parallelism))
    save(RUNTIME / "preflight.json", _evaluation_config(ids("val")[:1], 1))
    plan = {"dataset": str(DATASET), "baseline_dataset": str(BASELINE_DATASET), "service": str(RUNTIME / "service.json"),
            "runtime": str(RUNTIME / "runtime.json"), "host_version": HOST_VERSION,
            "counts": {"train": 44, "pool": 15, "val": 15, "holdout": 40},
            "train_parallelism": train_parallelism, "baseline_parallelism": baseline_parallelism,
            "batches": batches, "baseline_config": str(RUNTIME / "baseline-val.json"),
            "baseline_holdout_config": str(RUNTIME / "baseline-holdout.json"),
            "preflight_config": str(RUNTIME / "preflight.json")}
    save(PLAN, plan)
    if not STATUS.is_file(): save(STATUS, {"status": "prepared", "completed_tasks": 0, "learned_batches": []})
    return plan


def _terminal_output(trial):
    evidence = trial.get("evidence") or {}
    output = evidence.get("output") or {}
    if isinstance(output, dict) and isinstance(output.get("final"), dict): return output["final"]
    traces = evidence.get("traces") or ([evidence["trace"]] if evidence.get("trace") else [])
    for trace in reversed(traces):
        for event in reversed(trace.get("events") or []):
            if event.get("kind") == "run.end" and isinstance(event.get("output"), dict): return event["output"]
    return output if isinstance(output, dict) else {}


def capture(dataset, config, destination, env, *, resume_report=None):
    report = destination / "report.json"
    if report.is_file() and read(report).get("complete"):
        return read(report)
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        previous = destination.with_name(destination.name + "-attempt-01")
        suffix = 1
        while previous.exists():
            suffix += 1
            previous = destination.with_name(destination.name + f"-attempt-{suffix:02d}")
        destination.rename(previous)
        if (previous / "report.json").is_file(): resume_report = previous / "report.json"
    command = [str(BINARY), "dataset", "run", str(dataset), "--config", str(config), "--output-root", str(destination)]
    if resume_report and Path(resume_report).is_file(): command.extend(["--resume-report", str(resume_report)])
    with (destination.parent / (destination.name + ".capture.log")).open("a", encoding="utf-8") as log:
        process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        started = time.monotonic()
        while process.poll() is None:
            save(destination.parent / (destination.name + ".progress.json"), {
                "phase": "native_capture", "pid": process.pid, "elapsed_s": round(time.monotonic()-started),
                "output": str(destination), "updated_at": datetime.now(timezone.utc).isoformat()})
            time.sleep(10)
    if not report.is_file(): raise RuntimeError("native_capture_report_missing: " + str(destination))
    _privacy_scan(report, env)
    result = read(report)
    if process.returncode not in (0, 1, 3): raise RuntimeError("native_capture_failed: " + str(process.returncode))
    return result


def preflight(plan, env):
    destination = GEPA / "baseline/preflight"
    prior = destination / "report.json"
    if prior.is_file():
        previous = read(prior).get("trials") or []
        if len(previous) != 1 or previous[0].get("execution_status") != "completed" or _terminal_output(previous[0]).get("tau2_reward") not in (0.0, 1.0):
            archived = destination.with_name("preflight-invalid-"+datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S"))
            destination.rename(archived)
    report = capture(BASELINE_DATASET, Path(plan["preflight_config"]), destination, env)
    trials = report.get("trials") or []
    if len(trials) != 1: raise RuntimeError("native_preflight_missing_trial")
    trial = trials[0]; output = _terminal_output(trial)
    healthy = trial.get("execution_status") == "completed" and trial.get("verdict") != "error" and output.get("tau2_reward") in (0.0, 1.0) and not output.get("error_code")
    result = {"healthy": healthy, "task_id": trial.get("task_id"), "execution_status": trial.get("execution_status"),
              "verdict": trial.get("verdict"), "tau2_reward": output.get("tau2_reward"), "error_code": output.get("error_code"),
              "official_all": True, "generic_judge_enabled": False}
    save(GEPA / "baseline/preflight-status.json", result)
    if not healthy: raise RuntimeError("native_preflight_not_healthy")
    return result


def metrics_from_trials(trials):
    outputs = [_terminal_output(trial) for trial in trials]
    scored = [(trial, out) for trial, out in zip(trials, outputs)
              if trial.get("execution_status") == "completed" and trial.get("verdict") != "error"
              and out.get("tau2_reward") in (0.0, 1.0) and not str(out.get("error_code", "")).startswith("external_")]
    def numeric(value):
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    def average(values):
        values = [float(value) for value in values if numeric(value)]
        return sum(values) / len(values) if values else None
    loops = [(trial.get("evidence") or {}).get("trajectory", {}).get("summary", {}).get("invalid_loop_lower_bound")
             for trial, out in scored]
    result = {"completed": sum(t.get("execution_status") in ("completed", "failed", "timeout") for t in trials),
              "scored": len(scored), "errored": len(trials)-len(scored),
              "strict_passed": sum(out["tau2_reward"] == 1.0 for _, out in scored),
              "quality": average(out["tau2_reward"] for _, out in scored),
              "avg_invalid_loop": average(loops), "invalid_loop_method": "rule_lower_bound",
              "invalid_loop_coverage": sum(numeric(value) for value in loops),
              "host_repeat_diagnostic": average(out.get("invalid_loop_count") for _, out in scored),
              "avg_latency_ms": average(out.get("latency_ms", trial.get("duration_ms")) for trial, out in zip(trials, outputs)),
              "db_passed": sum(out.get("db_passed") is True for _, out in scored),
              "communicate_passed": sum(out.get("communicate_passed") is True for _, out in scored),
              "intent_f1": None, "intent_annotation": "No official per-turn intent labels"}
    costs = {}
    for role, field, complete, calls in (("agent", "tokens", "tokens_complete", "model_calls"),
                                          ("user", "user_tokens", "user_tokens_complete", "user_model_calls")):
        known = [out[field] for out in outputs if numeric(out.get(field)) and out.get(complete) is True]
        call_values = [out[calls] for out in outputs if numeric(out.get(calls))]
        costs[role] = {"token_coverage": len(known), "executions": len(outputs),
                       "known_tokens": sum(known) if known else None,
                       "tokens": sum(known) if len(known) == len(outputs) and outputs else None,
                       "avg_tokens": average(known) if len(known) == len(outputs) and outputs else None,
                       "calls": sum(call_values) if len(call_values) == len(outputs) and outputs else None}
    total = [out["tokens"]+out["user_tokens"] for out in outputs
             if numeric(out.get("tokens")) and numeric(out.get("user_tokens"))
             and out.get("tokens_complete") is True and out.get("user_tokens_complete") is True]
    costs["total"] = {"token_coverage": len(total), "executions": len(outputs),
                       "tokens": sum(total) if len(total) == len(outputs) and outputs else None,
                       "known_tokens": sum(total) if total else None,
                       "avg_tokens": average(total) if len(total) == len(outputs) and outputs else None,
                       "calls": costs["agent"]["calls"] + costs["user"]["calls"]
                       if costs["agent"]["calls"] is not None and costs["user"]["calls"] is not None else None}
    result.update(costs=costs, tokens=costs["agent"]["tokens"], avg_tokens=costs["agent"]["avg_tokens"],
                  token_coverage=costs["agent"]["token_coverage"], model_calls=costs["agent"]["calls"],
                  avg_model_calls=costs["agent"]["calls"]/len(outputs) if outputs and costs["agent"]["calls"] is not None else None)
    for field in ("l1_applied", "l2_action_applied", "l2_schedule_applied", "l2_applied", "l3_applied"):
        values = [out[field] for out in outputs if numeric(out.get(field))]
        result[field] = sum(values) if len(values) == len(outputs) and outputs else None
    return result


def _state(path, **fields):
    value = read(path) if path.is_file() else {}
    value.update(fields, updated_at=datetime.now(timezone.utc).isoformat(), pid=os.getpid())
    save(path, value)
    return value


def _learn_batch(batch, destination, report, env):
    training_set = set(ids("train"))
    if any(trial.get("task_id") not in training_set for trial in report.get("trials", [])):
        raise RuntimeError("Refusing to learn a non-training task")
    eligibility = deepcopy(report)
    for trial in eligibility.get("trials", []):
        out = _terminal_output(trial)
        if trial.get("execution_status") != "completed" or out.get("tau2_reward") not in (0.0, 1.0) or str(out.get("error_code", "")).startswith("external_"):
            trial["verdict"] = "error"
    eligible = [trial for trial in eligibility.get("trials", []) if trial.get("verdict") != "error"]
    if not eligible:
        return {"usable": True, "admissible_trials": 0, "captured_errors": len(report.get("trials", [])), "no_memory_evidence": True}
    learning_input, temporary = _isolated_learning_input(destination, eligibility)
    try:
        for attempt in range(2):
            process = subprocess.run([str(BINARY), "learn", "--config", str(RUNTIME / "service.json"),
                "--training-evaluation", str(learning_input), "--dataset", str(DATASET)],
                cwd=ROOT, env=env, text=True, capture_output=True)
            try: learned = json.loads(process.stdout)
            except ValueError: learned = {"status": "native_learning_status_missing", "exit_code": process.returncode}
            save(destination / "learning-status.json", learned)
            with (destination / "learning.log").open("a") as log: log.write(process.stderr)
            if learning_result_usable(learned, process.returncode):
                return {"usable": True, "admissible_trials": len(eligible), "captured_errors": len(report.get("trials", []))-len(eligible)}
        return {"usable": False, "admissible_trials": len(eligible), "error": learned.get("status", "native_learning_unresolved")}
    finally:
        if temporary and temporary.exists(): shutil.rmtree(temporary)


def run_training(plan, env):
    learned = set((read(STATUS) if STATUS.is_file() else {}).get("learned_batches", []))
    errors = []
    for batch in plan["batches"]:
        if batch["name"] in learned: continue
        destination = TRAINING / "batches" / batch["name"]
        _state(STATUS, status="running", phase="capture", batch=batch["name"])
        try:
            report = capture(DATASET, Path(batch["config"]), destination, env)
            if not report.get("complete"):
                report = capture(DATASET, Path(batch["config"]), destination, env)
            if not report.get("complete") or {trial.get("task_id") for trial in report.get("trials", [])} != set(batch["task_ids"]):
                raise RuntimeError("Training capture incomplete; its completed tasks remain resumable")
            _state(STATUS, phase="learning", metrics=metrics_from_trials(report.get("trials", [])))
            learning = _learn_batch(batch, destination, report, env)
            if learning["usable"]: learned.add(batch["name"])
            else: errors.append({"batch": batch["name"], "error": learning.get("error")})
        except Exception as error:
            errors.append({"batch": batch["name"], "error": str(error)})
        all_trials = []
        for item in plan["batches"]:
            path = TRAINING / "batches" / item["name"] / "report.json"
            if path.is_file(): all_trials.extend(read(path).get("trials", []))
        _state(STATUS, phase="batch_completed", learned_batches=sorted(learned), errors=errors,
               completed_tasks=len(all_trials), metrics=metrics_from_trials(all_trials))
    return _state(STATUS, status="training_completed" if len(learned)==len(plan["batches"]) else "training_with_unresolved",
                  phase="gepa_ready" if len(learned)==len(plan["batches"]) else "learning_recovery_available")


def run_baseline(plan, env):
    state_path = GEPA / "baseline/status.json"
    reports = {}
    for label, config in (("val", plan["baseline_config"]), ("holdout", plan["baseline_holdout_config"])):
        _state(state_path, status="running", phase=label)
        try:
            resume = GEPA / "baseline/preflight/report.json" if label == "val" else None
            report = capture(BASELINE_DATASET, Path(config), GEPA / "baseline" / label, env, resume_report=resume)
            if not report.get("complete"):
                report = capture(BASELINE_DATASET, Path(config), GEPA / "baseline" / label, env)
            reports[label] = {"report": str(GEPA / "baseline" / label / "report.json"),
                              "metrics": metrics_from_trials(report.get("trials", []))}
        except Exception as error: reports[label] = {"error": str(error)}
        _state(state_path, results=reports)
    return _state(state_path, status="completed" if all("metrics" in value for value in reports.values()) else "incomplete", results=reports)


def run(plan, env, *, include_training=False):
    lock_path = TRAINING / "controller.lock"
    with lock_path.open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        progress = GEPA / "baseline/preflight.progress.json"
        while progress.is_file():
            pid = read(progress).get("pid")
            try:
                if not isinstance(pid, int): break
                os.kill(pid, 0)
            except OSError: break
            _state(TRAINING / "controller-status.json", phase="waiting_for_native_preflight", native_pid=pid)
            time.sleep(10)
        preflight(plan, env)
        if include_training:
            with ThreadPoolExecutor(max_workers=2) as executor:
                training = executor.submit(run_training, plan, env)
                baseline = executor.submit(run_baseline, plan, env)
                result = {"training": training.result(), "baseline": baseline.result()}
        else:
            result = {"training": {"status": "deferred", "reason": "Published Seed004 transfer comparison has priority"},
                      "baseline": run_baseline(plan, env)}
        save(TRAINING / "controller-result.json", result)
        return result


def check():
    checks = {"binary": BINARY.is_file(), "python": PYTHON.is_file(), "host": HOST.is_file(),
              "native_dataset": (DATASET / "dataset.pb").is_file(), "gepa_view": BASELINE_DATASET.is_file(),
              "counts": read(SOURCE / "manifest.json").get("counts") == {"train":44,"pool":15,"val":15,"holdout":40}}
    if all(checks.values()):
        for data in (DATASET, BASELINE_DATASET):
            subprocess.run([str(BINARY), "dataset", "check", str(data), "--require-reviewed"], cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
    return {"ok": all(checks.values()), "checks": checks, "api_calls": 0}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--preflight", action="store_true")
    parser.add_argument("--start", action="store_true")
    parser.add_argument("--resume", action="store_true", help="Reuse all completed captures and learning batches without version gates")
    parser.add_argument("--status", action="store_true")
    parser.add_argument("--train-only", action="store_true")
    parser.add_argument("--baseline-only", action="store_true")
    parser.add_argument("--train-and-baseline", action="store_true")
    parser.add_argument("--train-parallelism", type=int, default=6)
    parser.add_argument("--baseline-parallelism", type=int, default=2)
    args = parser.parse_args()
    if args.check:
        result=check(); print(json.dumps(result)); raise SystemExit(0 if result["ok"] else 1)
    if args.status:
        print(json.dumps({"training": read(STATUS) if STATUS.is_file() else {"status":"not_started"},
            "baseline": read(GEPA / "baseline/status.json") if (GEPA / "baseline/status.json").is_file() else {"status":"not_started"},
            "preflight": read(GEPA / "baseline/preflight-status.json") if (GEPA / "baseline/preflight-status.json").is_file() else {"status":"pending"}}, ensure_ascii=False)); return
    if not 1 <= args.train_parallelism <= 16 or not 1 <= args.baseline_parallelism <= 16 or (args.train_and_baseline and args.train_parallelism + args.baseline_parallelism > 16):
        raise ValueError("Training + baseline Agent concurrency must be at most 16")
    plan = prepare(train_parallelism=args.train_parallelism, baseline_parallelism=args.baseline_parallelism)
    if args.preflight: print(json.dumps(preflight(plan, environment()), ensure_ascii=False)); return
    if args.train_only or args.baseline_only:
        preflight(plan, environment())
        result = run_training(plan, environment()) if args.train_only else run_baseline(plan, environment())
        print(json.dumps(result, ensure_ascii=False)); return
    if args.start or args.resume or args.train_and_baseline:
        print(json.dumps(run(plan, environment(), include_training=args.train_and_baseline), ensure_ascii=False)); return
    print(json.dumps(plan, ensure_ascii=False))


if __name__ == "__main__": main()
