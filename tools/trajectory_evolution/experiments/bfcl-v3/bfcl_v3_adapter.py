#!/usr/bin/env python3
"""Materialize compact native EvaluationTask views from the frozen BFCL v3 archive.

The source zip remains the only copy of prompts, function schemas, ground truth and
fixture state.  Native tasks carry only row identity and public benchmark metadata;
the isolated BFCL host resolves the row at execution time.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
from zipfile import ZipFile

from bfcl_v3_dataset import TRAIN_NAME, VAL_NAME


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
SOURCE = HERE / "datasets/source"
BINARY = ROOT / "build/trajectory-evolution/trajectory_evolution"
EXPECTED = {"train": 400, "search": 150, "selection": 50, "holdout": 200}


def _read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def _rows() -> dict[str, dict]:
    archive_path = SOURCE / "bfcl_v3_splits.zip"
    with ZipFile(archive_path) as archive:
        rows = []
        for member in (TRAIN_NAME, VAL_NAME):
            rows.extend(json.loads(line) for line in archive.read(member).splitlines() if line.strip())
    result = {str(row["id"]): row for row in rows}
    if len(result) != 800:
        raise ValueError("bfcl_source_identity_invalid")
    return result


def selected_rows() -> dict[str, list[dict]]:
    indexed = _rows()
    result = {}
    seen: set[str] = set()
    for split, expected in EXPECTED.items():
        ids = [line.strip() for line in (SOURCE / f"{split}_ids.txt").read_text().splitlines()
               if line.strip()]
        if len(ids) != expected or len(set(ids)) != expected or any(item not in indexed for item in ids):
            raise ValueError(f"bfcl_{split}_ids_invalid")
        if seen.intersection(ids):
            raise ValueError("bfcl_split_identity_overlap")
        seen.update(ids)
        result[split] = [indexed[item] for item in ids]
    if len(seen) != 800:
        raise ValueError("bfcl_split_coverage_invalid")
    return result


def _user_text(turn: list[dict]) -> str:
    parts = [str(message.get("content", "")).strip() for message in turn
             if message.get("role") == "user" and str(message.get("content", "")).strip()]
    if not parts:
        # BFCL missing-function cases contain one schema-refresh turn with no
        # new user message.  The isolated host consumes this marker locally
        # and sends the updated function list without exposing the marker to
        # the model.
        return "[BFCL function availability updated; continue the pending request]"
    value = "\n".join(parts)
    # Native dataset privacy checks treat realistic access codes/passwords in
    # benchmark prompts as credentials.  The BFCL host resolves the immutable
    # row at runtime, so persisted task views use stable placeholders while
    # the isolated model request still receives the original benchmark text.
    value = re.sub(r"(?i)(access\s+code\s+)[A-Za-z0-9_-]+", r"\1<BFCL_CODE>", value)
    value = re.sub(r"(?i)(access\s+token\s*[:=]?\s*)[A-Za-z0-9_.-]+", r"\1<BFCL_TOKEN>", value)
    value = re.sub(r"(?i)(password\s*[=:]\s*['\"]?)[A-Za-z0-9_.-]+", r"\1<BFCL_PASSWORD>", value)
    return value


def evaluation_task(row: dict, split: str) -> dict:
    inputs = [_user_text(turn) for turn in row["turns"]]
    source_member = TRAIN_NAME if split == "train" else VAL_NAME
    scenario = str(row.get("scenario", "unknown"))
    difficulty = "hard" if len(inputs) >= 5 or "long_context" in str(row.get("subset")) else (
        "medium" if len(inputs) >= 3 else "easy")
    # Search and selection have a documented 2/2 scenario split.  Include the
    # view name in group_id so native split isolation remains explicit.
    return {
        "task_id": str(row["id"]),
        "group_id": f"bfcl-v3:{split}:scenario:{scenario}",
        "task_input": inputs[0],
        "followup_inputs": inputs[1:],
        "expected_behavior": (
            "Each turn must emit only valid calls to the provided functions, with exact arguments; "
            "state changes and execution results must match the BFCL v3 deterministic checker."
        ),
        "category": str(row.get("subset") or row.get("test_category") or "bfcl_multi_turn"),
        "difficulty": difficulty,
        "initial_state": {
            "schema": "bfcl-v3-row-ref.v1",
            "row_id": str(row["id"]),
            "source_member": source_member,
        },
        "constraints": [
            "Use only functions exposed by the benchmark row.",
            "Do not invent unavailable functions or missing parameters.",
        ],
        "budget": {
            "max_steps": max(32, 16 * len(inputs)),
            "max_tool_calls": max(32, 16 * len(inputs)),
            "timeout_ms": 900000,
        },
        "split": {"search": "train", "selection": "val"}.get(split, split),
        "review_status": "reviewed",
        "provenance": {"kind": "golden", "learning_exposed": False},
        "attributes": {
            "bfcl": {
                "source_view": split,
                "scenario": scenario,
                "subset": str(row.get("subset", "")),
                "turns": len(inputs),
            }
        },
    }


def materialize(destination: Path, *, check: bool = True) -> dict:
    destination = destination.resolve()
    if destination.exists():
        raise FileExistsError(f"refusing to overwrite BFCL native view: {destination}")
    source_manifest = _read_json(SOURCE / "manifest.json")
    splits = selected_rows()
    tasks = [evaluation_task(row, split) for split, rows in splits.items() for row in rows]
    counts = Counter(task["split"] for task in tasks)
    if counts != Counter({"train": 550, "val": 50, "holdout": 200}):
        raise AssertionError("bfcl_native_split_mapping_invalid")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".bfcl-native-", dir=destination.parent) as folder:
        staging = Path(folder)
        tasks_file = staging / "tasks.jsonl"
        tasks_file.write_text("".join(json.dumps(task, ensure_ascii=False, separators=(",", ":")) + "\n"
                                      for task in tasks), encoding="utf-8")
        config = {
            "schema_version": 1,
            "name": "bfcl-v3-multi-turn",
            "split_seed": "bfcl-v3-frozen-splits-v1",
            "split_percent": [68, 7, 25],
            "max_tasks": len(tasks),
            "max_model_calls": 0,
            "budget": {"max_steps": 128, "max_tool_calls": 128, "timeout_ms": 900000},
            "sources": [{"kind": "golden", "path": "tasks.jsonl"}],
        }
        (staging / "dataset.json").write_text(json.dumps(config, separators=(",", ":")), encoding="utf-8")
        built = staging / "frozen"
        process = subprocess.run([str(BINARY), "dataset", "build", "--config",
                                  str(staging / "dataset.json"), "--output-root", str(built)],
                                 cwd=ROOT, text=True, capture_output=True)
        if process.returncode:
            details = _read_json(built / "manifest.json") if (built / "manifest.json").is_file() else {}
            raise RuntimeError("bfcl_native_build_failed:" + json.dumps(
                {"process": (process.stderr or process.stdout)[-1000:],
                 "issues": details.get("issues", [])[:8]}, ensure_ascii=False))
        if check:
            subprocess.run([str(BINARY), "dataset", "check", str(built), "--require-reviewed"],
                           cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
        selection = {
            "schema_version": 1,
            "source_zip_sha256": source_manifest["source_zip_sha256"],
            "source_manifest_sha256": hashlib.sha256((SOURCE / "manifest.json").read_bytes()).hexdigest(),
            "requested_counts": EXPECTED,
            "native_counts": dict(counts),
            "mapping": {"train": "train", "search": "train", "selection": "val", "holdout": "holdout"},
            "raw_fixture_persisted_in_tasks": False,
        }
        (built / "bfcl-selection.json").write_text(
            json.dumps(selection, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        built.rename(destination)
    return selection


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args(argv)
    print(json.dumps(materialize(args.output), ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
