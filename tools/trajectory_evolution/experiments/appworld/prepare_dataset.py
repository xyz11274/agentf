#!/usr/bin/env python3
"""Freeze a small development study using official task IDs, never scores.

All three variants of each selected scenario are preserved. This is a
development comparison, not an independent test or the full AppWorld benchmark.
Only official train/dev task instructions and required-app metadata are read.
"""
from __future__ import annotations

import argparse
import importlib.metadata
import json
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
WORLD = HERE / "runtime/world"
DATASETS = HERE / "datasets"
TRAIN_SCENARIOS = ["34d9492", "7d7fbf6", "76f2c72", "29caf6f",
                   "cf6abd2", "3c13f5a", "22cc237", "60d0b5b"]
DEV_SCENARIOS = ["68ee2c9", "6c2c621", "530b157", "0d8a4ee"]


def read(path: Path):
    return json.loads(path.read_text())


def write(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n")


def freeze() -> dict:
    target = DATASETS / "selection.json"
    if target.exists():
        return read(target)
    data = WORLD / "data"
    official = {s: (data / "datasets" / (s + ".txt")).read_text().splitlines()
                for s in ("train", "dev", "test_normal", "test_challenge")}
    train = [f"{scenario}_{i}" for scenario in TRAIN_SCENARIOS for i in (1, 2, 3)]
    dev = [f"{scenario}_{i}" for scenario in DEV_SCENARIOS for i in (1, 2, 3)]
    if not set(train).issubset(official["train"]) or not set(dev).issubset(official["dev"]):
        raise ValueError("Selected IDs are missing from this official dataset version")
    if set(train) & set(dev):
        raise ValueError("Train/dev IDs overlap")
    manifest = {
        "dataset": "appworld-office-development",
        "appworld_version": importlib.metadata.version("appworld"),
        "official_repository": "https://github.com/StonyBrookNLP/appworld",
        "world_root": str(WORLD),
        "official_counts": {s: len(ids) for s, ids in official.items()},
        "train_ids": train, "dev_ids": dev,
        "selection_uses_scores": False,
        "selection_basis": "Fixed complete scenarios; file operations, notes-to-files, contact selection and cross-app notification/payment workflows",
        "scope": "Development comparison; repeated development feedback may inform later training. Not independent holdout.",
        "dev_scenarios": DEV_SCENARIOS,
        "train_scenarios": TRAIN_SCENARIOS,
        "native_splits": {"train": "train", "dev": "val"},
        "scoring": "Unmodified official TestTracker.success; TGC counts tasks passing all tests, SGC requires all three scenario variants passing.",
        "covered_goal_apps": ["file_system", "simple_note", "phone", "venmo"],
        "not_covered_goal_apps": ["gmail", "todoist"],
        "license": "Apache-2.0; protected code/data and derivatives require encrypted public redistribution",
        "test_answers_read": False,
    }
    write(target, manifest)
    for split, ids in (("train", train), ("dev", dev)):
        (DATASETS / f"{split}_ids.txt").write_text("\n".join(ids) + "\n")
    return manifest


def task_ref(task_id: str, split: str) -> dict:
    specs = read(WORLD / "data/tasks" / task_id / "specs.json")
    return {
        "task_id": "appworld-" + task_id,
        "group_id": "appworld-scenario-" + task_id.rsplit("_", 1)[0],
        "task_input": specs["instruction"],
        "expected_behavior": "Complete the requested application workflow and pass every original AppWorld state-based evaluation test without unrelated changes.",
        "category": "appworld_cross_app_execution",
        "difficulty": "medium",
        "initial_state": {"schema": "appworld-task-ref.v1", "source_task_id": task_id},
        "constraints": ["Discover app APIs from the provided API documentation and use only the AppWorld API environment.",
                        "Do not access hidden task ground truth, evaluation programs, or host files."],
        "budget": {"max_steps": 300, "max_tool_calls": 300, "timeout_ms": 1200000},
        "split": "train" if split == "train" else "val",
        "review_status": "reviewed",
        "provenance": {"kind": "golden", "learning_exposed": False},
        "attributes": {"appworld": {"source_task_id": task_id, "source_view": split}},
    }


def materialize(binary: Path) -> dict:
    manifest = freeze()
    destination = DATASETS / "native"
    if destination.exists():
        subprocess.run([str(binary), "dataset", "check", str(destination), "--require-reviewed"],
                       check=True, cwd=ROOT, stdout=subprocess.DEVNULL)
        return {"path": str(destination), "reused": True}
    tasks = [task_ref(t, split) for split in ("train", "dev")
             for t in manifest[split + "_ids"]]
    with tempfile.TemporaryDirectory(prefix=".appworld-native-", dir=DATASETS) as td:
        staging = Path(td)
        (staging / "tasks.jsonl").write_text("".join(json.dumps(t, separators=(",", ":")) + "\n" for t in tasks))
        write(staging / "dataset.json", {
            "schema_version": 1, "name": manifest["dataset"],
            "split_seed": "appworld-office-fixed-development-v1",
            "split_percent": [66, 33, 1], "max_tasks": len(tasks), "max_model_calls": 0,
            "budget": {"max_steps": 300, "max_tool_calls": 300, "timeout_ms": 1200000},
            "sources": [{"kind": "golden", "path": "tasks.jsonl"}],
        })
        built = staging / "built"
        p = subprocess.run([str(binary), "dataset", "build", "--config", str(staging / "dataset.json"),
                            "--output-root", str(built)], cwd=ROOT, capture_output=True, text=True)
        if p.returncode:
            raise RuntimeError((p.stderr or p.stdout)[-3000:])
        subprocess.run([str(binary), "dataset", "check", str(built), "--require-reviewed"],
                       cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
        built.rename(destination)
    return {"path": str(destination), "reused": False, "counts": {"train": 24, "val": 12}}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-binary", type=Path, default=ROOT / "trace_output/business-simulator/published/seed004/runtime/trajectory_evolution")
    parser.add_argument("--no-native", action="store_true")
    args = parser.parse_args()
    selection = freeze()
    result = {"selection": str(DATASETS / "selection.json"),
              "counts": {"train": len(selection["train_ids"]), "dev": len(selection["dev_ids"])}}
    if not args.no_native:
        result["native"] = materialize(args.native_binary.resolve())
    print(json.dumps(result, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
