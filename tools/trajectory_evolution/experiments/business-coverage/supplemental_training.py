#!/usr/bin/env python3
"""Isolated business supplemental training; prepare/status never call a model."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from copy import deepcopy
import fcntl
import json
import os
from pathlib import Path
import subprocess

import coldstart

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
DATASET = HERE / "datasets/supplemental-20260929/native"
CONFIG = HERE / "supplemental-deployment-20260929"
OUTPUT = ROOT / "trace_output/business-simulator/training-supplement-20260929"
PLAN = CONFIG / "plan.json"
BINARY = ROOT / "build/trajectory-evolution/trajectory_evolution"
PARALLELISM = 16
_original_save = coldstart.save


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def _writable(path):
    path = Path(path).absolute()
    if not any(path == base or base in path.parents for base in (CONFIG, OUTPUT)):
        raise RuntimeError("supplemental_write_outside_workspace")
    for item in (path, *path.parents):
        if item.is_symlink():
            raise RuntimeError("supplemental_write_symlink")
    return path


def save(path, value):
    _original_save(_writable(path), value)


def tasks():
    rows = [json.loads(line) for line in (DATASET / "train.jsonl").read_text().splitlines() if line.strip()]
    if not rows or len({row["task_id"] for row in rows}) != len(rows):
        raise RuntimeError("supplemental_tasks_missing_or_duplicate")
    if any(row.get("split") != "train" or row.get("review_status") != "reviewed" for row in rows):
        raise RuntimeError("supplemental_requires_reviewed_train_only")
    return rows


@contextmanager
def lifecycle_lock():
    _writable(OUTPUT).mkdir(parents=True, exist_ok=True)
    with (OUTPUT / "lifecycle.lock").open("a") as handle:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("supplemental_training_already_running") from None
        yield


def validate_plan(plan):
    if Path(plan["service"]).resolve() != (CONFIG / "runtime/service.json").resolve():
        raise RuntimeError("supplemental_service_not_isolated")
    service = read(plan["service"])
    if Path(service["data_dir"]).resolve() != (OUTPUT / "evolution").resolve() or service.get("memory_source_dir"):
        raise RuntimeError("supplemental_memory_not_isolated")
    if service["application_id"] != "business-simulator" or service.get("max_level") != "L0":
        raise RuntimeError("supplemental_training_identity_invalid")
    _writable(OUTPUT / "evolution")
    for batch in plan["batches"]:
        if not batch["name"].isdigit() or len(batch["name"]) != 3:
            raise RuntimeError("supplemental_batch_name_invalid")
        config = read(_writable(Path(batch["config"])))
        if config["parallelism"] != PARALLELISM or config["splits"] != ["train"]:
            raise RuntimeError("supplemental_batch_config_invalid")
        for variant in config["variants"]:
            instrument = Path(variant["instrumentation_config"])
            if not instrument.is_absolute():
                instrument = Path(batch["config"]).parent / instrument
            if instrument.resolve() != Path(plan["service"]).resolve():
                raise RuntimeError("supplemental_instrumentation_not_isolated")
    return plan


def prepare():
    if PLAN.is_file():
        return validate_plan(read(PLAN))
    environment = coldstart.environment()
    if not DATASET.exists():
        subprocess.run([str(BINARY), "dataset", "build", "--config",
                        str(DATASET.parent / "build-config.json"),
                        "--output-root", str(DATASET)], cwd=ROOT,
                       env=environment, check=True, stdout=subprocess.DEVNULL)
    rows = tasks()
    subprocess.run([str(BINARY), "dataset", "check", str(DATASET)], cwd=ROOT,
                   env=environment, check=True, stdout=subprocess.DEVNULL)
    _writable(CONFIG).mkdir(parents=True, exist_ok=True)
    deploy = CONFIG / "runtime"
    if not deploy.exists():
        from prepare_dataset import prepare_configuration
        prepare_configuration(_writable(deploy), DATASET)
    service = read(deploy / "service.json")
    for key in ("policy_evaluation_file", "evaluation_levels", "control_learning", "memory_source_dir"):
        service.pop(key, None)
    service.update(data_dir=str(OUTPUT / "evolution"), max_level="L0", auto_evaluate=True,
                   auto_approve_experiences=False, workers=PARALLELISM)
    service["instrumentation"]["level"] = "L0"
    service["learning"].update(min_new_runs=4, min_interval_ms=0, max_batch_runs=32,
                               analysis_parallelism=PARALLELISM, max_analysis_retries=2,
                               retry_backoff_ms=15000)
    service["sharing"].update(enabled=True, min_interval_ms=1000,
                              max_model_calls_per_cycle=32, model_parallelism=PARALLELISM)
    save(deploy / "service.json", service)
    runtime = read(deploy / "runtime.json")
    runtime["evolution"].update(memory_review_parallelism=PARALLELISM,
                                full_curation_new_entry_threshold=64)
    runtime["model"].update(endpoint=environment["AF_LLM_BASE_URL"].rstrip("/") + "/chat/completions",
                             model_id=environment["AF_LLM_MODEL"], timeout_ms=300000, max_retries=4)
    save(deploy / "runtime.json", runtime)
    base = read(deploy / "evaluation.json")
    base.update(splits=["train"], repetitions=1, parallelism=PARALLELISM)
    base["judge"].update(enabled=True, batch_input_bytes=2097152, max_input_bytes=2097152,
                          evidence_field_bytes=256, answer_field_bytes=1024)
    base["artifact_retention"] = {"retain_trial_files": False, "retain_markdown": False,
                                  "retain_jsonl": False}
    batches = []
    for index, start in enumerate(range(0, len(rows), PARALLELISM), 1):
        group = rows[start:start + PARALLELISM]
        name = f"{index:03d}"
        config = deepcopy(base)
        config.update(task_ids=[row["task_id"] for row in group], max_trials=len(group))
        path = deploy / f"training-{name}.json"
        save(path, config)
        batches.append({"name": name, "config": str(path), "task_ids": config["task_ids"]})
    plan = {"mode": "isolated supplemental L0 capture -> judge -> native learn",
            "tasks": len(rows), "turns": sum(1 + len(row.get("followup_inputs", [])) for row in rows),
            "dataset": str(DATASET), "service": str(deploy / "service.json"),
            "batches": batches, "concurrency": PARALLELISM,
            # Resume is not blocked by historical content fingerprints.
            "dataset_files": {}, "original_memory_writeback": False}
    save(PLAN, plan)
    return validate_plan(plan)


def run():
    plan = prepare()
    coldstart.OUTPUT, coldstart.CONFIG = OUTPUT, CONFIG
    coldstart.DATASET, coldstart.PLAN = DATASET, PLAN
    coldstart.coverage.tasks = tasks
    coldstart.save = save
    coldstart.coverage.save = save
    # All native paths are validated before handing control to the existing runner.
    validate_plan(plan)
    recover_partial_captures(plan)
    return coldstart.run(retry_failed=True)


def recover_partial_captures(plan):
    """Package real native trial checkpoints for its existing resume reader."""
    for batch in plan["batches"]:
        destination = _writable(OUTPUT / batch["name"])
        if not destination.is_dir() or (destination / "report.json").exists():
            continue
        # A killed supervisor can leave its evaluator alive. Do not launch a
        # duplicate model wave while that exact output is still being written.
        for command in Path("/proc").glob("[0-9]*/cmdline"):
            try:
                args = command.read_bytes().split(b"\0")
            except OSError:
                continue
            if str(destination).encode() in args and b"--output-root" in args:
                raise RuntimeError("supplemental_capture_still_running")
        started = read(destination / "started.json") if (destination / "started.json").is_file() else {}
        trials = [read(path) for path in sorted((destination / "trials").glob("*.json"))]
        if trials and not started.get("run_id"):
            raise RuntimeError("supplemental_checkpoint_missing_run_identity")
        # No fabricated trials/scores: a partial report only wraps checkpoint
        # files already written by the native evaluator, preserving artifacts.
        save(destination / "report.json", {"schema_version": 1, "complete": False,
             "run_id": started.get("run_id", ""),
             "dataset_version": started.get("dataset_version", ""), "trials": trials})
        save(destination / "manifest.json", {"run_id": started.get("run_id", ""),
             "complete": False, "files": {}})


def status():
    state = read(OUTPUT / "status.json") if (OUTPUT / "status.json").is_file() else {"status": "not_started"}
    return {key: state.get(key) for key in ("status", "phase", "batch", "planned_tasks",
            "planned_turns", "captured_tasks", "captured_turns", "learned", "error", "updated_at")}


def main(argv=None):
    os.umask(0o077)
    os.chdir(ROOT)
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    for action in ("prepare", "run", "status"):
        group.add_argument("--" + action, action="store_true")
    args = parser.parse_args(argv)
    if args.status:
        print(json.dumps(status(), ensure_ascii=False))
        return 0
    with lifecycle_lock():
        if args.prepare:
            plan = prepare()
            print(json.dumps({"status": "prepared", "tasks": plan["tasks"], "turns": plan["turns"],
                              "batches": len(plan["batches"]), "concurrency": PARALLELISM,
                              "output": str(OUTPUT)}, ensure_ascii=False))
            return 0
        return run()


if __name__ == "__main__":
    raise SystemExit(main())
