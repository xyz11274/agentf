#!/usr/bin/env python3
"""Keep an auditable best strategy when GEPA cannot pass its publication gate.

The GEPA gate remains authoritative for release.  This fallback is deliberately
diagnostic: it selects the best observed candidate, evaluates it against L0 on
the complete source ``val`` + ``holdout`` set (200 + 200), and stores only a
compact result under the business-simulator output directory.  Full traces are
created in a temporary directory and are removed after the summary is written.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
from functools import wraps
import hashlib
import gzip
import json
import math
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import tarfile
import time

import layout

ROOT = layout.ROOT
OUTPUT = layout.OUTPUT
SOURCE = layout.SOURCE_DATASET
GEPA_RUNTIME = Path(__file__).resolve().parent / "gepa-deployment/runtime"
BINARY = ROOT / "build/trajectory-evolution/trajectory_evolution"
JOB_ROOT = layout.GEPA / "workspace/agent-evaluation"
USER = "business-training"
BEST_FILE = OUTPUT / "published/best-strategy.json"
COMPARISON_FILE = OUTPUT / "published/best-strategy-comparison.json"
REQUEST_FILE = OUTPUT / "published/best-strategy-request.json"
REGRADE_DIR = OUTPUT / "published/regrade"
EXTERNAL_SEED_REGISTRY = OUTPUT / "audit/targeted-seed-candidates/registry.json"


def _load_json(path: Path):
    opener = gzip.open if path.suffix == '.gz' else open
    with opener(path, 'rt', encoding='utf-8') as stream:
        return json.load(stream)


def _read(path: Path) -> dict:
    value = _load_json(path)
    if not isinstance(value, dict) or value.get("report_kind") != "regrade_trial_index":
        return value
    reports, rows = {}, []
    judge = (value.get("configuration") or {}).get("judge_fingerprint")
    for ref in value.get("trials", []):
        source = ROOT / ref["report_path"]
        if source not in reports:
            native = _load_json(source)
            if judge and (native.get("configuration") or {}).get("judge_fingerprint") != judge:
                raise ValueError("indexed_report_judge_mismatch")
            reports[source] = {row["trial_id"]: row for row in native.get("trials", [])}
        row = reports[source].get(ref["trial_id"])
        if not row or any(row.get(key) != ref.get(key) for key in ("task_id", "variant_id")):
            raise ValueError("indexed_report_trial_mismatch")
        rows.append(row)
    return dict(value, trials=rows)


def _sha256_bytes(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def _sha256(path: Path) -> str:
    return _sha256_bytes(path.read_bytes())


def _atomic_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    raw = (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode()
    temporary = path.with_name(path.name + ".pending")
    temporary.write_bytes(raw)
    os.replace(temporary, path)


def _persist_regrade_bundle(output: Path, selection: dict, attempt: int,
                            split: str = "val-holdout") -> dict | None:
    """Keep one compressed report/checkpoint bundle for judge-only recovery.

    Trial workspaces stay temporary.  The report already contains the compact
    trial evidence, so retaining it plus manifest/artifacts is enough for the
    native ``--regrade-report`` path and avoids copying daemon logs/traces.
    """
    report = output / "report.json"
    manifest = output / "manifest.json"
    if not report.is_file() or not manifest.is_file():
        return None
    REGRADE_DIR.mkdir(parents=True, exist_ok=True)
    node = selection.get("node")
    if isinstance(node, int) and not isinstance(node, bool):
        identity = f"node-{node}"
    else:
        external = selection.get("external_candidate_id") or selection.get("version") or "external"
        identity = "external-" + _sha256_bytes(str(external).encode())[:12]
    target = REGRADE_DIR / f"best-{identity}-{split}.tar.gz"
    pending = target.with_suffix(target.suffix + ".pending")
    if pending.exists():
        pending.unlink()
    with tarfile.open(pending, "w:gz") as archive:
        archive.add(report, arcname="report.json", recursive=False)
        archive.add(manifest, arcname="manifest.json", recursive=False)
        artifacts = output / "artifacts"
        if artifacts.is_dir():
            archive.add(artifacts, arcname="artifacts", recursive=True)
    os.replace(pending, target)
    return {"path": str(target.relative_to(OUTPUT)),
            "bytes": target.stat().st_size, "attempt": attempt}


def _mean(values: list[float]) -> float | None:
    return sum(values) / len(values) if values else None


def _node_metrics(node: dict) -> dict:
    rows = [v for v in (node.get("validation") or {}).values() if isinstance(v, dict)]
    def values(name: str) -> list[float]:
        return [float(v[name]) for v in rows
                if isinstance(v.get(name), (int, float)) and math.isfinite(float(v[name]))]
    return {
        "completed": len(rows),
        "quality": _mean(values("quality")),
        "strict_pass_rate": _mean(values("strict_pass_rate")),
        "invalid_loop": _mean(values("invalid_loops")),
        "latency_ms": _mean(values("duration_ms")),
        "tokens": _mean(values("tokens")),
        "llm_calls": _mean(values("llm_calls")),
    }


def _selection_key(item: tuple[dict, dict]) -> tuple[float, ...]:
    node, metrics = item
    opt = node.get("optimization") or {}
    # A feasible candidate wins; otherwise use the frozen GEPA objective and
    # then measured quality/efficiency.  No threshold is relaxed here.
    def number(value, default: float) -> float:
        try:
            value = float(value)
            return value if math.isfinite(value) else default
        except (TypeError, ValueError):
            return default
    return (
        1.0 if opt.get("feasible") is True else 0.0,
        number(opt.get("score"), -1e30),
        number(metrics.get("quality"), -1e30),
        number(metrics.get("strict_pass_rate"), -1e30),
        -number(metrics.get("invalid_loop"), 1e30),
        -number(metrics.get("latency_ms"), 1e30),
        -number(metrics.get("tokens"), 1e30),
        -float(node.get("id", -1)),
    )


def _candidate_pool(root: Path, tree: dict, limit: int = 3) -> list[dict]:
    """Use frozen GEPA finalists first, then validation-only fallback versions.

    Fallback membership is frozen before any fallback holdout evaluation.  It
    is an architecture showcase pool and never changes the GEPA gate/winner.
    """
    nodes = tree.get("nodes") if isinstance(tree.get("nodes"), list) else []
    seed_validation = (nodes[0].get("validation") or {}) if nodes and isinstance(nodes[0], dict) else {}
    screening_task_ids = list(seed_validation.keys())
    expected_count = len(set(screening_task_ids))
    selected: list[dict] = []
    seen: set[str] = set()

    def add(node_id: int, version: str, source: str) -> None:
        if len(selected) >= limit or not version or version in seen or not 0 <= node_id < len(nodes):
            return
        path = root / "search/versions" / f"{node_id}.json"
        if not path.is_file():
            return
        node = nodes[node_id]
        if str(node.get("version") or version) != version:
            return
        if source == "validation_fallback" and node_id == 0:
            return
        validation = node.get("validation") or {}
        validation_rows = [validation.get(task_id) for task_id in screening_task_ids
                           if isinstance(validation.get(task_id), dict)]
        if expected_count == 0 or len(validation_rows) != expected_count or any(
                not isinstance(value.get("quality"), (int, float)) or
                not math.isfinite(float(value["quality"])) or
                not isinstance(value.get("strict_pass_rate"), (int, float)) or
                not math.isfinite(float(value["strict_pass_rate"]))
                for value in validation_rows):
            return
        metrics = _node_metrics({"validation": {key: validation[key]
                                                for key in screening_task_ids
                                                if isinstance(validation.get(key), dict)}})
        if source == "validation_fallback" and (
                metrics["completed"] != expected_count or metrics["quality"] is None):
            return
        seen.add(version)
        selected.append({
            "job_id": root.name, "node": node_id, "version": version,
            "artifact": _read(path), "selection_source": source,
            "validation": metrics, "optimization": node.get("optimization") or {},
            "screening_task_ids": screening_task_ids,
            "control_coverage": node.get("control_coverage") or {},
            "tree_status": tree.get("status"), "tree_error": tree.get("error"),
            "tree_stop_reason": tree.get("stop_reason"),
            "formal_gate_passed": False,
        })

    final = tree.get("final_holdout")
    entries = final.get("candidates", []) if isinstance(final, dict) else []
    # Preserve C++ frozen validation rank and never promote an arbitrary tree
    # winner into this finalist list.
    for entry in entries[:limit] if isinstance(entries, list) else []:
        if not isinstance(entry, dict):
            continue
        node_id, version = entry.get("node"), entry.get("version")
        if (isinstance(node_id, (int, float)) and not isinstance(node_id, bool)
                and math.isfinite(float(node_id)) and float(node_id).is_integer()
                and isinstance(version, str)):
            add(int(node_id), version, "frozen_final_holdout")

    if len(selected) < limit:
        validation_ranked = []
        for node_id, node in enumerate(nodes):
            if not isinstance(node, dict) or not node.get("version"):
                continue
            validation = node.get("validation") or {}
            validation_rows = [validation.get(task_id) for task_id in screening_task_ids
                               if isinstance(validation.get(task_id), dict)]
            metrics = _node_metrics({"validation": {key: validation[key]
                                                    for key in screening_task_ids
                                                    if isinstance(validation.get(key), dict)}})
            if (expected_count > 0 and metrics["quality"] is not None
                    and len(validation_rows) == expected_count and all(
                        isinstance(value.get("quality"), (int, float)) and
                        math.isfinite(float(value["quality"])) and
                        isinstance(value.get("strict_pass_rate"), (int, float)) and
                        math.isfinite(float(value["strict_pass_rate"]))
                        for value in validation_rows)):
                validation_ranked.append((node, metrics))
        for node, metrics in sorted(validation_ranked,
                                    key=lambda pair: _showcase_key(pair[1]), reverse=True):
            add(int(node["id"]), str(node["version"]), "validation_fallback")
            if len(selected) >= limit:
                break
    return selected


def _completed_pool_candidates(root: Path, tree: dict, existing: list[dict]) -> list[dict]:
    """Return distinct materialized artifacts with a hash-verified pool report."""
    nodes = tree.get("nodes") if isinstance(tree.get("nodes"), list) else []
    seed_validation = (nodes[0].get("validation") or {}) if nodes and isinstance(nodes[0], dict) else {}
    screening_task_ids = list(seed_validation.keys())
    seen = {candidate["version"] for candidate in existing}
    by_version = {}
    for entry in (tree.get("evaluations") or {}).values():
        if not isinstance(entry, dict) or entry.get("status") != "complete":
            continue
        version = entry.get("version")
        relative = entry.get("report")
        if not isinstance(version, str) or version in seen or not isinstance(relative, str):
            continue
        path = Path(relative)
        if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
            continue
        report_path = root / "search" / path
        if not report_path.is_file():
            continue
        expected_hash = entry.get("report_hash")
        if expected_hash and _sha256(report_path) != expected_hash:
            continue
        try:
            pool_report = _read(report_path)
        except (OSError, ValueError):
            continue
        if (not pool_report.get("complete") or
                (entry.get("dataset_version") and
                 pool_report.get("dataset_version") != entry.get("dataset_version"))):
            continue
        split = entry.get("split")
        if split not in {"train", "val"}:
            continue
        candidate_rows = [row for row in pool_report.get("trials", [])
                          if isinstance(row, dict) and row.get("split") == split
                          and row.get("variant_id") == "candidate"]
        observation = (_aggregate(candidate_rows, len(entry.get("task_ids") or candidate_rows))
                       if candidate_rows else {})
        cached_validation = {}
        cached_validation_path = None
        if split == "val":
            expected_val = set(screening_task_ids)
            task_ids = {str(row.get("task_id")) for row in candidate_rows
                        if row.get("task_id") not in (None, "")}
            loaded_versions = [(row.get("evidence") or {}).get("loaded_version")
                               for row in candidate_rows]
            if (len(candidate_rows) == 50 and task_ids == expected_val
                    and all(loaded == version for loaded in loaded_versions)
                    and observation.get("quality_complete")):
                cached_validation = observation
                cached_validation_path = str(report_path)
        prior = by_version.get(version)
        if prior is None:
            by_version[version] = {"entry": entry, "observation": observation,
                                   "cached_validation": cached_validation,
                                   "cached_validation_path": cached_validation_path,
                                   "report_path": str(report_path)}
            continue
        # An exact, complete shared-val50 report takes precedence over any
        # larger-looking pool/train observation. Keep the best pool report
        # separately so it remains useful for candidate ordering.
        if cached_validation:
            prior["cached_validation"] = cached_validation
            prior["cached_validation_path"] = cached_validation_path
        if _showcase_key(observation) > _showcase_key(prior["observation"]):
            prior["observation"] = observation
            if not prior.get("cached_validation"):
                prior["report_path"] = str(report_path)
    result = []
    for node_id, node in enumerate(nodes):
        if node_id == 0 or not isinstance(node, dict):
            continue
        version = node.get("version")
        if not isinstance(version, str) or version in seen or version not in by_version:
            continue
        artifact_path = root / "search/versions" / f"{node_id}.json"
        if not artifact_path.is_file():
            continue
        pool_record = by_version[version]
        parent_id = int(node.get("parent", 0))
        parent = nodes[parent_id] if 0 <= parent_id < len(nodes) else {}
        result.append({
            "job_id": root.name, "node": node_id, "version": version,
            "artifact": _read(artifact_path), "selection_source": "completed_pool_validation",
            "validation": pool_record["cached_validation"] or _node_metrics(node),
            "cached_validation_complete": bool(pool_record["cached_validation"]),
            "cached_validation_report_path": pool_record["cached_validation_path"],
            "pool_report_path": pool_record["report_path"],
            "pool_observation": (pool_record["observation"]
                                 if pool_record["observation"].get("quality") is not None
                                 else _node_metrics(node)),
            "screening_task_ids": screening_task_ids,
            "parent_validation": _node_metrics(parent),
            "screening_evidence": _screening_evidence(node, parent),
            "optimization": node.get("optimization") or {},
            "control_coverage": node.get("control_coverage") or {},
            "tree_status": tree.get("status"), "tree_error": tree.get("error"),
            "tree_stop_reason": tree.get("stop_reason"), "formal_gate_passed": False,
        })
        seen.add(version)
    return result


def _complete_val_metrics(metrics: dict) -> bool:
    return bool(metrics.get("quality_complete")) and metrics.get("completed") in (50, 200)


def _screening_evidence(node: dict, parent: dict) -> dict:
    screening = node.get("screening") or {}
    candidate_rows = screening.get("candidate") or {}
    parent_rows = screening.get("parent") or {}
    task_ids = sorted(set(candidate_rows) & set(parent_rows))
    paired = {}
    if task_ids:
        metrics = ("quality", "strict_pass_rate", "invalid_loops", "tokens", "duration_ms")
        for metric in metrics:
            deltas = [float(candidate_rows[task_id].get(metric, 0) or 0) -
                      float(parent_rows[task_id].get(metric, 0) or 0)
                      for task_id in task_ids]
            paired[metric + "_delta"] = sum(deltas) / len(deltas)
        paired["task_count"] = len(task_ids)
    parent_rules = ((parent.get("control_coverage") or {}).get("rules") or {})
    candidate_rules = ((screening.get("control_coverage") or {}).get("rules") or {})
    new_triggered = {rule_id: rule.get("triggered", 0) for rule_id, rule in candidate_rules.items()
                     if rule_id not in parent_rules and rule.get("triggered", 0) > 0}
    applied = bool(new_triggered) and node.get("reason") != "new_control_not_applied"
    return {**paired, "new_rules_triggered": new_triggered,
            "new_rule_triggered_count": sum(float(value) for value in new_triggered.values()),
            "new_rule_applied": applied,
            "node_reason": node.get("reason"), "parent_node": node.get("parent")}


def _pool_candidate_key(candidate: dict) -> tuple[float, ...]:
    evidence = candidate.get("screening_evidence") or {}
    def number(name: str, default: float = -1e30) -> float:
        try:
            value = float(evidence.get(name, default))
            return value if math.isfinite(value) else default
        except (TypeError, ValueError):
            return default
    return (
        1.0 if evidence.get("new_rule_applied") else 0.0,
        number("quality_delta"), number("strict_pass_rate_delta"),
        -number("invalid_loops_delta", 1e30),
        -number("tokens_delta", 1e30), -number("duration_ms_delta", 1e30),
        number("new_rule_triggered_count", 0.0), -float(candidate.get("node", 0)),
    )


def _finalist_report(root: Path, tree: dict, candidate: dict) -> tuple[dict, str] | None:
    """Load the complete paired L0/finalist holdout report by frozen identity."""
    evaluations = tree.get("evaluations")
    if not isinstance(evaluations, dict):
        return None
    try:
        baseline = _read(root / "baseline.json")
        baseline_version = _artifact_version(baseline)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError):
        return None
    expected_dataset = tree.get("dataset_version")
    for plan_path in (root / "plan.json", root.parent / "plan.json",
                      GEPA_RUNTIME.parent / "plan.json"):
        if not expected_dataset and plan_path.is_file():
            try:
                expected_dataset = _read(plan_path).get("dataset_version")
            except (OSError, ValueError):
                pass
    if not isinstance(expected_dataset, str) or not expected_dataset:
        return None
    matches = [entry for entry in evaluations.values()
               if isinstance(entry, dict) and entry.get("split") == "holdout"
               and entry.get("status") == "complete"
               and entry.get("version") == candidate["version"]
               and entry.get("baseline_version") == baseline_version
               and (not entry.get("dataset_version") or
                    entry.get("dataset_version") == expected_dataset)]
    for entry in matches:
        relative = entry.get("report")
        if not isinstance(relative, str) or not relative:
            continue
        path = Path(relative)
        if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
            continue
        report_path = root / "search" / path
        if not report_path.is_file():
            continue
        raw = report_path.read_bytes()
        expected = entry.get("report_hash")
        if expected and _sha256_bytes(raw) != expected:
            continue
        report = json.loads(raw)
        if (not report.get("complete") or
                report.get("dataset_version") != expected_dataset or
                (entry.get("dataset_version") and
                 report.get("dataset_version") != entry.get("dataset_version"))):
            continue
        trials = report.get("trials") or []
        baseline = [row for row in trials if row.get("variant_id") == "baseline"
                    and row.get("split") == "holdout"]
        candidate_rows = [row for row in trials if row.get("variant_id") == "candidate"
                          and row.get("split") == "holdout"]
        task_ids = {str(row.get("task_id")) for row in candidate_rows
                    if row.get("task_id") not in (None, "")}
        baseline_ids = {str(row.get("task_id")) for row in baseline
                        if row.get("task_id") not in (None, "")}
        _, task_splits = _source_rows()
        expected_holdout = {task_id for task_id, split in task_splits.items()
                            if split == "holdout"}
        entry_ids = {str(task_id) for task_id in entry.get("task_ids", [])}
        def loaded_version(row: dict) -> str | None:
            evidence = row.get("evidence") or {}
            return (evidence.get("loaded_version") or
                    (evidence.get("trajectory") or {}).get("loaded_version"))
        if (len(candidate_rows) != 200 or len(baseline) != 200
                or task_ids != baseline_ids or task_ids != expected_holdout
                or entry_ids != expected_holdout
                or any(loaded_version(row) != candidate["version"] for row in candidate_rows)
                or any(loaded_version(row) != baseline_version for row in baseline)):
            continue
        config = report.get("configuration") or report.get("config") or {}
        variants = config.get("variants") if isinstance(config, dict) else None
        if isinstance(variants, list) and len(variants) == 2:
            by_id = {variant.get("id"): variant for variant in variants
                     if isinstance(variant, dict)}
            if (by_id.get("baseline", {}).get("version") != baseline_version or
                    by_id.get("candidate", {}).get("version") != candidate["version"]):
                continue
        return report, str(report_path)
    return None


def _expected_dataset_version(root: Path, tree: dict) -> str | None:
    expected = tree.get("dataset_version")
    for plan_path in (root / "plan.json", root.parent / "plan.json",
                      GEPA_RUNTIME.parent / "plan.json"):
        if not expected and plan_path.is_file():
            try:
                expected = _read(plan_path).get("dataset_version")
            except (OSError, ValueError):
                pass
    return expected if isinstance(expected, str) and expected else None


def _compatible_validation_dataset(root: Path, expected_dataset: str | None,
                                   report_dataset: str | None,
                                   task_ids: list[str] | set[str]) -> bool:
    """Validate source-val audits against the frozen ID-only GEPA dataset view."""
    if not expected_dataset or not report_dataset:
        return False
    if report_dataset == expected_dataset:
        return True
    plan_path = GEPA_RUNTIME.parent / "plan.json"
    if not plan_path.is_file():
        return False
    try:
        plan = _read(plan_path)
        if (plan.get("dataset_version") != expected_dataset or
                plan.get("source_dataset_version") != report_dataset):
            return False
        view_path = ROOT / plan["dataset_view"]
        if not view_path.is_file() or _sha256(view_path) != plan.get("dataset_view_hash"):
            return False
        view = _read(view_path)
        source_manifest = _read(SOURCE / "manifest.json")
        source_version = source_manifest.get("metadata", {}).get("dataset_version")
        wanted = set(map(str, task_ids))
        return (view.get("source_version") == report_dataset == source_version and
                not view.get("goal_contracts_file") and
                set(map(str, view.get("selection_task_ids", []))) == wanted and
                len(wanted) == 50)
    except (OSError, ValueError, KeyError, TypeError):
        return False


def _cached_val_report(root: Path, tree: dict, version: str,
                       task_ids: list[str]) -> tuple[list[dict], str] | None:
    """Reuse a complete, same-version 50-task val report if the exact IDs match."""
    expected_dataset = _expected_dataset_version(root, tree)
    wanted = set(task_ids)
    if len(wanted) != 50 or expected_dataset is None:
        return None

    def rows_from_report(report_path: Path, variant_id: str,
                         report_relative: str) -> tuple[list[dict], str] | None:
        if not report_path.is_file():
            return None
        try:
            report = _read(report_path)
        except (OSError, ValueError):
            return None
        if (not report.get("complete") or not _compatible_validation_dataset(
                root, expected_dataset, report.get("dataset_version"), wanted)):
            return None
        variants = ((report.get("configuration") or {}).get("variants") or [])
        if not any(isinstance(item, dict) and item.get("id") == variant_id and
                   item.get("version") == version for item in variants):
            return None
        rows = [row for row in report.get("trials", [])
                if row.get("split") == "val" and row.get("variant_id") == variant_id]
        if (len(rows) != 50 or {str(row.get("task_id")) for row in rows} != wanted or
                any((row.get("evidence") or {}).get("loaded_version") != version
                    for row in rows) or not _aggregate(rows, 50).get("quality_complete")):
            return None
        return rows, report_relative

    for entry in (tree.get("evaluations") or {}).values():
        if (not isinstance(entry, dict) or entry.get("split") != "val" or
                entry.get("status") != "complete"):
            continue
        if set(str(value) for value in entry.get("task_ids", [])) != wanted:
            continue
        relative = entry.get("report")
        if not isinstance(relative, str):
            continue
        path = Path(relative)
        if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
            continue
        report_path = root / "search" / path
        if not report_path.is_file():
            continue
        raw = report_path.read_bytes()
        if entry.get("report_hash") and _sha256_bytes(raw) != entry["report_hash"]:
            continue
        if entry.get("version") == version:
            found = rows_from_report(report_path, "candidate", str(report_path))
            if found:
                return found
        if entry.get("baseline_version") == version:
            found = rows_from_report(report_path, "baseline", str(report_path))
            if found:
                return found

    # One explicit authoritative L0 regrade cache is shared by the current
    # business GEPA delivery. Do not scan unrelated audit directories: match
    # source dataset, frozen 50 task IDs, and the job's actual L0 version.
    if Path(root / "baseline.json").is_file():
        audit_path = OUTPUT / "audit/val-regrade-20260929/l0-split/retry-1/report.json"
        audit_relative = str(audit_path.relative_to(OUTPUT))
        found = rows_from_report(audit_path, "candidate", audit_relative)
        if found:
            return found
    return None


def _showcase_key(metrics: dict) -> tuple[float, ...]:
    """Rank shared validation by strict completions, then quality and cost."""
    def number(value, default: float) -> float:
        try:
            value = float(value)
            return value if math.isfinite(value) else default
        except (TypeError, ValueError):
            return default
    strict_passed = metrics.get("strict_passed")
    if strict_passed is None and isinstance(metrics.get("strict_pass_rate"), (int, float)):
        strict_passed = float(metrics["strict_pass_rate"]) * float(metrics.get("completed", 50))
    return (
        number(strict_passed, -1e30),
        number(metrics.get("quality"), -1e30),
        -number(metrics.get("invalid_loop"), 1e30),
        -number(metrics.get("tokens"), 1e30),
        -number(metrics.get("llm_calls"), 1e30),
        -number(metrics.get("latency_ms"), 1e30),
    )


def _official_gate_status() -> dict:
    status_path = layout.GEPA / "status.json"
    runner_status = None
    if status_path.is_file():
        try:
            runner_status = _read(status_path).get("status")
        except (OSError, ValueError):
            runner_status = "unreadable"
    return {
        "runner_status": runner_status or "暂无",
        "showcase_changes_gate_or_publication": False,
    }


def _independent_val_candidates(job_id: str, candidates: list[dict] | None,
                                expected_val_ids: set[str],
                                repair_errors: bool = True) -> list[dict]:
    """Load/repair explicit same-task val audits without touching tree state."""
    accepted = []
    job_root = JOB_ROOT / job_id
    tree = _read(job_root / "search/tree.json") if (job_root / "search/tree.json").is_file() else {}
    expected_dataset = _expected_dataset_version(job_root, tree)
    for supplied in candidates or []:
        if not isinstance(supplied, dict):
            continue
        candidate = dict(supplied)
        version, artifact = candidate.get("version"), candidate.get("artifact")
        task_ids = candidate.get("screening_task_ids") or []
        report_ref = candidate.get("screening_report_path")
        screening_variant = candidate.get("screening_variant_id")
        if screening_variant not in {"best", "candidate"}:
            screening_variant = "best"
        if (not isinstance(version, str) or not isinstance(artifact, dict) or
                not isinstance(report_ref, str) or
                set(map(str, task_ids)) != expected_val_ids or len(task_ids) != 50):
            continue
        try:
            if _candidate_artifact_version(candidate) != version:
                continue
            report_path = Path(report_ref)
            if not report_path.is_absolute():
                report_path = OUTPUT / report_path
            report = _read(report_path)
            variants = ((report.get("configuration") or {}).get("variants") or [])
            configured = any(isinstance(item, dict) and item.get("version") == version and
                             item.get("id") == screening_variant for item in variants)
            rows = [row for row in report.get("trials", [])
                    if row.get("variant_id") == screening_variant
                    and row.get("split") == "val"]
            row_ids = {str(row.get("task_id")) for row in rows}
            valid_identity = (configured and
                              _compatible_validation_dataset(job_root, expected_dataset,
                                                            report.get("dataset_version"),
                                                            expected_val_ids) and
                              row_ids == expected_val_ids and
                              all((row.get("evidence") or {}).get("loaded_version") == version
                                  for row in rows))
            metrics = _aggregate(rows, 50) if valid_identity else {}
            if not metrics.get("quality_complete"):
                if not repair_errors or candidate.get("selection_source") == "external_seed_registry":
                    continue
                repaired = run_comparison(
                    dict(candidate, job_id=job_id), splits=("val",),
                    persist_result=False, task_ids=list(task_ids), include_l0=False,
                    purpose=str(candidate.get("comparison_purpose") or "node5_val50_audit"),
                    resume_report_path=report_path,
                )
                if repaired.get("status") != "complete":
                    continue
                report_ref = repaired.get("report_path") or (repaired.get("report") or {}).get("path")
                report_path = Path(report_ref) if report_ref else report_path
                if not report_path.is_absolute():
                    report_path = OUTPUT / report_path
                report = _read(report_path)
                variants = ((report.get("configuration") or {}).get("variants") or [])
                configured = any(isinstance(item, dict) and item.get("version") == version and
                                 item.get("id") == screening_variant for item in variants)
                rows = [row for row in report.get("trials", [])
                        if row.get("variant_id") == screening_variant and row.get("split") == "val"]
                row_ids = {str(row.get("task_id")) for row in rows}
                if (not configured or not _compatible_validation_dataset(
                        job_root, expected_dataset, report.get("dataset_version"), expected_val_ids) or
                        row_ids != expected_val_ids or
                        any((row.get("evidence") or {}).get("loaded_version") != version
                            for row in rows)):
                    continue
                metrics = _aggregate(rows, 50)
            if not metrics.get("quality_complete"):
                continue
            is_external = candidate.get("selection_source") == "external_seed_registry"
            candidate.update({
                "job_id": job_id,
                "selection_source": ("external_seed_registry" if is_external else
                                     "independent_val_audit"),
                "validation": metrics,
                "validation_evidence_source": ("external_seed_val50" if is_external else
                                               "independent_val_audit"),
                "validation_scope": 50, "screening_report_path": str(report_path),
                "screening_report_sha256": _sha256(report_path),
                "judge_fingerprint": (report.get("configuration") or {}).get("judge_fingerprint"),
                "formal_gate_passed": False,
            })
            accepted.append(candidate)
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError):
            continue
    return accepted


def discover_completed_val_audits(job_id: str) -> list[dict]:
    """Discover independent NodeN val50 audit reports for a read-only freeze."""
    root = JOB_ROOT / job_id
    reports_root = OUTPUT / "published/best-strategy-runs" / job_id
    discovered = []
    if not reports_root.is_dir():
        return discovered
    for result_path in reports_root.glob("node-*-*-node*_val50_audit-val-50-*-candidate/result.json"):
        try:
            result = _read(result_path)
            report_ref = result.get("report_path") or (result.get("report") or {}).get("path")
            if not isinstance(report_ref, str):
                continue
            report_path = Path(report_ref)
            if not report_path.is_absolute():
                report_path = OUTPUT / report_path
            report = _read(report_path)
            screening_variant = result.get("screening_variant_id", "best")
            if screening_variant not in ("best", "candidate"):
                continue
            rows = [row for row in report.get("trials", [])
                    if row.get("variant_id") == screening_variant and row.get("split") == "val"]
            task_ids = sorted({str(row.get("task_id")) for row in rows
                               if row.get("task_id") not in (None, "")})
            name = result_path.parent.name
            node_match = re.search(r"(?:^|-)node(\d+)_val50_audit", name)
            if node_match is None:
                continue
            node_id = int(node_match.group(1))
            version = result.get("version")
            artifact_path = root / "search/versions" / f"{node_id}.json"
            if not isinstance(version, str) or not artifact_path.is_file():
                continue
            artifact = _read(artifact_path)
            discovered.append({
                "job_id": job_id, "node": node_id, "version": version,
                "artifact": artifact, "screening_task_ids": task_ids,
                "screening_report_path": str(report_path),
                "screening_report_sha256": _sha256(report_path),
                "screening_variant_id": screening_variant,
                "judge_fingerprint": result.get("judge_fingerprint"),
                "comparison_purpose": f"node{node_id}_val50_audit",
                "result_path": str(result_path),
            })
        except (OSError, ValueError, TypeError):
            continue
    return discovered


def discover_external_seed_candidates(job_id: str,
                                      registry_path: Path | None = None) -> list[dict]:
    """Read external frozen seed candidates without inventing GEPA nodes.

    Registry entries carry an artifact and an already-complete val50 report.
    Selection still passes through ``_independent_val_candidates`` and its
    exact-ID/version/error checks before a candidate can enter the freeze.
    """
    path = registry_path or EXTERNAL_SEED_REGISTRY
    if not path.is_file():
        return []
    try:
        registry = _read(path)
    except (OSError, ValueError):
        return []
    if registry.get("job_id") not in (None, "", job_id):
        return []
    entries = registry.get("candidates")
    if not isinstance(entries, list):
        return []
    result = []
    for entry in entries:
        if not isinstance(entry, dict) or entry.get("status", "ready") not in {"ready", "complete"}:
            continue
        external_id = entry.get("id")
        version = entry.get("version")
        report_ref = entry.get("val50_report")
        variant_id = entry.get("variant_id", "best")
        if (not isinstance(external_id, str) or not external_id or
                not isinstance(version, str) or not isinstance(report_ref, str) or
                variant_id not in {"best", "candidate"}):
            continue
        artifact = entry.get("artifact")
        artifact_ref = entry.get("artifact_file")
        if isinstance(artifact_ref, str):
            artifact_path = Path(artifact_ref)
            if not artifact_path.is_absolute():
                artifact_path = ROOT / artifact_path
            try:
                artifact = _read(artifact_path)
            except (OSError, ValueError):
                continue
        elif isinstance(artifact, dict):
            artifact_path = None
        else:
            continue
        report_path = Path(report_ref)
        if not report_path.is_absolute():
            report_path = ROOT / report_path
        try:
            report = _read(report_path)
        except (OSError, ValueError):
            continue
        task_ids = entry.get("task_ids")
        if not isinstance(task_ids, list):
            task_ids = sorted({str(row.get("task_id")) for row in report.get("trials", [])
                               if row.get("variant_id") == variant_id and row.get("split") == "val"
                               and row.get("task_id") not in (None, "")})
        result.append({
            "job_id": job_id, "node": None, "external_candidate_id": external_id,
            "version": version, "artifact": artifact,
            "artifact_source_path": (str(artifact_path.relative_to(ROOT))
                                     if artifact_path else "registry_inline_artifact"),
            "artifact_sha256": (_sha256(artifact_path) if artifact_path else
                                _sha256_bytes(json.dumps(artifact, ensure_ascii=False,
                                                        sort_keys=True).encode())),
            "screening_task_ids": task_ids, "screening_report_path": str(report_path),
            "screening_report_sha256": _sha256(report_path),
            "screening_variant_id": variant_id,
            "comparison_purpose": f"external_seed_{external_id}_val50",
            "selection_source": "external_seed_registry",
            **{key: entry[key] for key in ("native_binary", "runtime_requirement",
                "evaluation_feedback_informed", "training_feedback_sources") if key in entry},
            "registry_path": str(path.relative_to(ROOT) if path.is_relative_to(ROOT) else path),
        })
    return result


def _freeze_candidate_artifact(root: Path, freeze_dir: Path, candidate: dict) -> dict:
    """Persist the exact artifact selected before holdout and return its receipt."""
    node = candidate.get("node")
    source = candidate.get("artifact_source_path")
    if isinstance(node, int) and not isinstance(node, bool):
        source_path = root / "search/versions" / f"{node}.json"
        if not source_path.is_file():
            raise RuntimeError("showcase_candidate_artifact_missing")
        artifact = _read(source_path)
        artifact_path = source_path
    else:
        artifact = candidate.get("artifact")
        if not isinstance(artifact, dict):
            raise RuntimeError("showcase_candidate_artifact_missing")
        version_tag = _sha256_bytes(str(candidate["version"]).encode())[:16]
        artifact_path = freeze_dir / "showcase-artifacts" / f"{version_tag}.json"
        if artifact_path.is_file():
            if _read(artifact_path) != artifact:
                raise RuntimeError("showcase_frozen_artifact_changed")
        else:
            _atomic_json(artifact_path, artifact)
    actual_version = _candidate_artifact_version(dict(candidate, artifact=artifact))
    if actual_version != candidate.get("version"):
        raise RuntimeError("showcase_candidate_artifact_version_mismatch")
    try:
        path_ref = str(artifact_path.relative_to(ROOT))
    except ValueError:
        path_ref = str(artifact_path)
    return {
        "artifact_path": path_ref, "artifact_sha256": _sha256(artifact_path),
        "artifact_source_path": source or path_ref,
    }


def choose_best(job_id: str = "", supplemental_candidates: list[dict] | None = None) -> dict:
    """Choose a post-hoc showcase candidate without changing the GEPA gate."""
    root = JOB_ROOT / job_id if job_id else None
    if root is None or not (root / "search/tree.json").is_file():
        candidates = sorted(JOB_ROOT.glob("*/search/tree.json"),
                            key=lambda p: p.stat().st_mtime, reverse=True)
        if not candidates:
            raise RuntimeError("best_strategy_tree_missing")
        tree_file = candidates[0]
        root = tree_file.parent.parent
    tree_file = root / "search/tree.json"
    tree = _read(tree_file)
    job_id = root.name
    pool = _candidate_pool(root, tree)
    expected_candidates = 3
    val_backfill_errors = []
    nodes = tree.get("nodes") if isinstance(tree.get("nodes"), list) else []
    expected_val_ids = (set(map(str, (nodes[0].get("validation") or {}).keys()))
                        if nodes and isinstance(nodes[0], dict) else set())
    supplied_audits = discover_completed_val_audits(job_id)
    for item in discover_external_seed_candidates(job_id):
        if not any(existing.get("version") == item.get("version")
                   for existing in supplied_audits if isinstance(existing, dict)):
            supplied_audits.append(item)
    for item in supplemental_candidates or []:
        if not any(existing.get("version") == item.get("version")
                   for existing in supplied_audits if isinstance(existing, dict)):
            supplied_audits.append(item)
    explicit_candidates = _independent_val_candidates(
        job_id, supplied_audits, expected_val_ids)
    for external in explicit_candidates:
        prior = next((item for item in pool if item.get("version") == external.get("version")), None)
        if prior is None:
            pool.append(external)
        else:
            prior.update({key: external[key] for key in (
                "validation", "validation_evidence_source", "validation_scope",
                "screening_task_ids", "screening_report_path", "screening_report_sha256",
                "selection_source")
                          if key in external})
    if len(pool) < expected_candidates:
        supplemental = _completed_pool_candidates(root, tree, pool)
        supplemental.sort(key=_pool_candidate_key, reverse=True)
        for candidate in supplemental:
            if candidate.get("cached_validation_complete"):
                candidate["validation_evidence_source"] = "reused_complete_pool_val"
                candidate["screening_report_path"] = candidate.get("cached_validation_report_path")
                if candidate.get("screening_report_path"):
                    candidate["screening_report_sha256"] = _sha256(
                        Path(candidate["screening_report_path"]))
                pool.append(candidate)
            else:
                try:
                    measured_val = run_comparison(
                        candidate, splits=("val",), persist_result=False,
                        task_ids=candidate.get("screening_task_ids"), include_l0=False,
                        purpose="showcase_screen_50")
                    val_metrics = (measured_val.get("metrics", {}).get("best", {}).get("val") or {})
                    candidate["validation"] = val_metrics
                    candidate["validation_evidence_source"] = "independent_pool_val"
                    candidate["validation_scope"] = 50
                    candidate["screening_report_path"] = measured_val.get("report_path")
                    candidate["screening_report_sha256"] = (
                        measured_val.get("report", {}).get("report_sha256"))
                    parent_metrics = candidate.get("parent_validation") or {}
                    candidate["screening_delta"] = {
                        "quality_delta": (val_metrics.get("quality") or 0) -
                                         (parent_metrics.get("quality") or 0),
                        "strict_pass_rate_delta": (val_metrics.get("strict_pass_rate") or 0) -
                                                  (parent_metrics.get("strict_pass_rate") or 0),
                        "invalid_loop_reduction": (parent_metrics.get("invalid_loop") or 0) -
                                                  (val_metrics.get("invalid_loop") or 0),
                        "token_reduction": (parent_metrics.get("tokens") or 0) -
                                           (val_metrics.get("tokens") or 0),
                        "latency_reduction_ms": (parent_metrics.get("latency_ms") or 0) -
                                                (val_metrics.get("latency_ms") or 0),
                        "task_count": 50,
                    }
                    if _complete_val_metrics(val_metrics):
                        pool.append(candidate)
                    else:
                        val_backfill_errors.append({"node": candidate["node"],
                                                    "status": measured_val.get("status"),
                                                    "error": measured_val.get("error")})
                except Exception as exc:
                    val_backfill_errors.append({"node": candidate["node"],
                                                "error": type(exc).__name__})
            if len(pool) >= expected_candidates:
                break
    # Freeze membership and rank by the common validation evidence. This lets
    # an explicitly supplied independent val50 audit compete with tree nodes
    # without rewriting tree.validation or treating it as a GEPA finalist.
    pool.sort(key=lambda item: _showcase_key(item.get("validation") or {}), reverse=True)
    pool = pool[:expected_candidates]
    pool_shortfall = max(0, expected_candidates - len(pool))
    if not pool:
        return {
            "job_id": job_id, "node": None, "version": None, "artifact": None,
            "selection_reason": "no_executable_showcase_candidate",
            "showcase_candidate_count": 0, "showcase_candidates": [],
            "candidate_scope": {"expected": expected_candidates, "frozen": 0,
                                "shortfall": expected_candidates, "holdout_complete": 0,
                                "holdout_incomplete": 0,
                                "holdout_shortfall": expected_candidates,
                                "val_backfill_errors": val_backfill_errors,
                                "holdout_errors": []},
            "formal_gate_passed": False,
            "official_gate_status": _official_gate_status(),
            "tree_sha256": _sha256(tree_file), "showcase_only": True,
        }

    # Freeze both the candidate set and the best ID before reading any
    # holdout result. The manifest is a provenance record, not GEPA state.
    best = max(pool, key=lambda candidate: _showcase_key(candidate.get("validation") or {}))
    freeze_dir = OUTPUT / "published/best-strategy-runs" / job_id
    freeze_path = freeze_dir / "showcase-selection-freeze.json"
    if freeze_path.is_file():
        frozen = _read(freeze_path)
        restored = []
        for frozen_candidate in frozen.get("candidate_ids", []):
            version = frozen_candidate.get("version")
            candidate = next((item for item in pool if item.get("version") == version), None)
            if candidate is None:
                artifact_ref = frozen_candidate.get("artifact_path")
                if not isinstance(artifact_ref, str) or not isinstance(version, str):
                    raise RuntimeError("showcase_freeze_unresolvable")
                artifact_path = Path(artifact_ref)
                if not artifact_path.is_absolute():
                    artifact_path = ROOT / artifact_path
                if not artifact_path.is_file():
                    raise RuntimeError("showcase_freeze_artifact_missing")
                if _sha256(artifact_path) != frozen_candidate.get("artifact_sha256"):
                    raise RuntimeError("showcase_frozen_artifact_hash_mismatch")
                artifact = _read(artifact_path)
                if _candidate_artifact_version(dict(frozen_candidate, artifact=artifact)) != version:
                    raise RuntimeError("showcase_frozen_artifact_version_mismatch")
                candidate = {
                    "job_id": job_id, "node": frozen_candidate.get("node"),
                    "external_candidate_id": frozen_candidate.get("external_candidate_id"),
                    "version": version, "artifact": artifact,
                    "selection_source": frozen_candidate.get("selection_source"),
                }
            candidate = dict(candidate)
            candidate.update({"validation": frozen_candidate.get("validation") or {},
                              "selection_source": frozen_candidate.get("selection_source"),
                              "external_candidate_id": frozen_candidate.get("external_candidate_id"),
                              "registry_path": frozen_candidate.get("registry_path"),
                              **{key: frozen_candidate[key] for key in ("native_binary", "runtime_requirement",
                                  "evaluation_feedback_informed", "training_feedback_sources")
                                 if key in frozen_candidate},
                              "artifact_path": frozen_candidate.get("artifact_path"),
                              "artifact_sha256": frozen_candidate.get("artifact_sha256"),
                              "artifact_source_path": frozen_candidate.get("artifact_source_path"),
                              "screening_task_ids": frozen_candidate.get("screening_task_ids") or [],
                              "screening_variant_id": frozen_candidate.get("screening_variant_id"),
                              "screening_report_path": frozen_candidate.get("screening_report_path"),
                              "screening_report_sha256": frozen_candidate.get("screening_report_sha256")})
            restored.append(candidate)
        if (not restored or not any(item.get("version") == frozen.get("best_version")
                                    for item in restored)):
            raise RuntimeError("showcase_freeze_winner_missing")
        pool = restored
        best = next(item for item in pool if item.get("version") == frozen.get("best_version"))
    else:
        candidate_records = []
        for item in pool:
            artifact_receipt = _freeze_candidate_artifact(root, freeze_dir, item)
            candidate_records.append({
                "node": item.get("node"),
                "external_candidate_id": item.get("external_candidate_id"),
                "registry_path": item.get("registry_path"),
                "version": item.get("version"),
                "selection_source": item.get("selection_source"),
                **{key: item[key] for key in ("native_binary", "runtime_requirement",
                    "evaluation_feedback_informed", "training_feedback_sources") if key in item},
                **artifact_receipt,
                "validation": item.get("validation"),
                "screening_task_ids": item.get("screening_task_ids") or [],
                "screening_variant_id": item.get("screening_variant_id"),
                "screening_report_path": item.get("screening_report_path"),
                "screening_report_sha256": item.get("screening_report_sha256"),
            })
            item.update(artifact_receipt)
        best = next(item for item in pool if item.get("version") == best.get("version"))
        _atomic_json(freeze_path, {
            "job_id": job_id,
            "selection_basis": "shared_val_evidence_before_holdout",
            "best_node": best.get("node"),
            "best_candidate_id": best.get("external_candidate_id"),
            "best_version": best.get("version"), "candidate_ids": candidate_records,
            "created_at": datetime.now(timezone.utc).isoformat(),
            "tree_sha256": _sha256(tree_file),
        })

    holdout_complete = []
    holdout_errors = []
    baseline_version = _artifact_version(_read(root / "baseline.json"))
    full_l0_holdout = _baseline400_rows("holdout", baseline_version)
    shared_l0_holdout = _aggregate(full_l0_holdout[0], 200) if full_l0_holdout else None
    for candidate in pool:
        if full_l0_holdout:
            candidate["l0_holdout_evidence_source"] = full_l0_holdout[1]
        loaded = _finalist_report(root, tree, candidate)
        if loaded is not None:
            report, report_path = loaded
            rows = [row for row in report.get("trials", [])
                    if row.get("variant_id") == "candidate" and row.get("split") == "holdout"]
            metrics = _aggregate(rows, 200)
            baseline_rows = [row for row in report.get("trials", [])
                             if row.get("variant_id") == "baseline" and row.get("split") == "holdout"]
            baseline_metrics = _aggregate(baseline_rows, 200)
            if full_l0_holdout:
                baseline_metrics = shared_l0_holdout
            elif baseline_metrics.get("quality_complete"):
                shared_l0_holdout = baseline_metrics
            candidate["holdout_report"] = str(Path(report_path).relative_to(root))
            candidate["holdout_report_sha256"] = _sha256(Path(report_path))
            candidate["holdout_report_data"] = report
            candidate["holdout_evidence_source"] = "reused_frozen_finalist_report"
        else:
            report_path = None
            baseline_metrics = {}
            metrics = {}
        if loaded is None or not metrics.get("quality_complete"):
            # Validation-only fallback candidates are selected before their
            # holdout is observed. They get an independent holdout comparison,
            # but never enter or modify the formal GEPA finalist/gate state.
            try:
                include_l0 = shared_l0_holdout is None
                measured = run_comparison(candidate, splits=("holdout",), persist_result=False,
                                          include_l0=include_l0,
                                          purpose="showcase_holdout")
                metrics = (measured.get("metrics", {}).get("best", {}).get("holdout") or {})
                baseline_metrics = (measured.get("metrics", {}).get("L0", {}).get("holdout") or {}
                                    if include_l0 else shared_l0_holdout or {})
                if baseline_metrics.get("quality_complete") and shared_l0_holdout is None:
                    shared_l0_holdout = baseline_metrics
                candidate["holdout_evidence_source"] = "independent_fallback_holdout"
                candidate["holdout_regrade_bundle"] = measured.get("regrade_bundle")
                candidate["holdout_evaluation_status"] = measured.get("status")
                if not metrics.get("quality_complete"):
                    holdout_errors.append({"node": candidate["node"],
                                           "status": measured.get("status"),
                                           "error": measured.get("error")})
            except Exception as exc:
                metrics, baseline_metrics = {}, {}
                candidate["holdout_evidence_source"] = "independent_fallback_holdout"
                candidate["holdout_evaluation_status"] = "failed"
                holdout_errors.append({"node": candidate["node"],
                                       "error": type(exc).__name__})
            candidate.pop("holdout_report_data", None)
            candidate.pop("holdout_report", None)
            candidate.pop("holdout_report_sha256", None)
        candidate["holdout"] = metrics
        candidate["l0_holdout"] = baseline_metrics
        if metrics.get("quality_complete"):
            holdout_complete.append(candidate)
    # Freeze the winner using validation evidence only. Holdout is an audit
    # report and must never change this ordering, including when a candidate
    # has an incomplete or unusually strong holdout result.
    selection_reason = "best_of_frozen_candidates_by_shared_validation_only"
    best.update({
        "selection_reason": selection_reason,
        "showcase_candidate_count": len(pool),
        "best_selection_basis": "shared_val_evidence_before_holdout",
        "selection_freeze_path": str(freeze_path.relative_to(OUTPUT)),
        "showcase_candidates": [{k: v for k, v in item.items() if k not in {
            "artifact", "holdout_report_data"}} for item in pool],
        "candidate_scope": {"expected": expected_candidates, "frozen": len(pool),
                            "shortfall": pool_shortfall,
                            "holdout_complete": len(holdout_complete),
                            "holdout_incomplete": len(pool) - len(holdout_complete),
                            "holdout_shortfall": max(0, expected_candidates - len(holdout_complete)),
                            "val_backfill_errors": val_backfill_errors,
                            "holdout_errors": holdout_errors},
        "formal_gate_passed": False,
        "official_gate_status": _official_gate_status(),
        "tree_sha256": _sha256(tree_file),
        "showcase_only": True,
    })
    return best


@contextmanager
def _candidate_runtime(candidate: dict):
    """Bind this sequential worker to the runtime frozen with its candidate."""
    global BINARY
    previous = BINARY
    ref = candidate.get("native_binary")
    if candidate.get("runtime_requirement") and not ref:
        raise RuntimeError("candidate_runtime_binding_missing")
    if ref:
        binary = Path(ref)
        if not binary.is_absolute():
            binary = ROOT / binary
        if not binary.is_file():
            raise RuntimeError("candidate_runtime_binary_missing")
        BINARY = binary
    try:
        yield
    finally:
        BINARY = previous


def _with_candidate_runtime(function):
    @wraps(function)
    def bound(selection, *args, **kwargs):
        with _candidate_runtime(selection):
            return function(selection, *args, **kwargs)
    return bound


def _candidate_artifact_version(candidate: dict) -> str:
    with _candidate_runtime(candidate):
        return _artifact_version(candidate["artifact"])


def _artifact_version(artifact: dict) -> str:
    result = subprocess.run(
        [str(BINARY), "artifact-version"],
        input=json.dumps(artifact, ensure_ascii=False, separators=(",", ":")),
        text=True, capture_output=True, timeout=30,
    )
    version = result.stdout.strip()
    if result.returncode or not version.startswith("agent-version-v1:"):
        raise RuntimeError("best_strategy_artifact_invalid")
    return version


def _source_rows() -> tuple[list[dict], dict[str, str]]:
    rows: list[dict] = []
    by_id: dict[str, str] = {}
    for split in ("val", "holdout"):
        path = SOURCE / f"{split}.jsonl"
        for line in path.read_text(encoding="utf-8").splitlines():
            row = json.loads(line)
            rows.append(row)
            by_id[row["task_id"]] = split
    if len(rows) != 400 or len(by_id) != 400 or sum(v == "val" for v in by_id.values()) != 200 or \
            sum(v == "holdout" for v in by_id.values()) != 200:
        raise RuntimeError("best_strategy_source_scope_invalid")
    return rows, by_id


def _comparison_config(temp: Path, selection: dict, rows: list[dict],
                       splits: tuple[str, ...] = ("val", "holdout"),
                       task_ids: list[str] | None = None,
                       include_l0: bool = True) -> Path:
    base = _read(GEPA_RUNTIME / "evaluation.json")
    service = _read(GEPA_RUNTIME / "service.json")
    # Keep the evaluation's state and traces outside the 5 GB deliverable.
    data_dir = temp / "agent-data"
    data_dir.mkdir(parents=True, exist_ok=True)
    service["data_dir"] = str(data_dir)
    memory_source = selection.get('memory_source_dir')
    service["memory_source_dir"] = str((ROOT / memory_source).resolve() if memory_source
                                     else (OUTPUT / "training/evolution").resolve())
    prompt_root = GEPA_RUNTIME / "prompts"
    for key in ("runtime_config", "experience_template_file", "policy_evaluation_file"):
        value = service.get(key)
        if value:
            service[key] = str((GEPA_RUNTIME / value).resolve())
    for key in ("control_learning", "sharing"):
        section = service.get(key) or {}
        for field in tuple(section):
            if field.endswith("prompt_file"):
                section[field] = str((prompt_root / Path(section[field]).name).resolve())
        service[key] = section
    service_file = temp / "service.json"
    _atomic_json(service_file, service)

    command = ["python3", str((ROOT / "examples/agent/business-simulator/evals/run_product_eval.py").resolve()),
               "--evolution-entry"]
    common = base["variants"][0]
    pass_env = list(dict.fromkeys(common.get("pass_env", []) + [
        "AF_LLM_MAX_RETRIES", "AF_LLM_RETRY_BASE_DELAY", "OPENAI_BASE_URL",
        "OPENAI_MODEL", "OPENAI_API_KEY"
    ]))
    baseline = (_read(JOB_ROOT / selection["job_id"] / "baseline.json")
                if include_l0 else None)
    variants = []
    variant_specs = ([ ("L0", baseline), ("best", selection["artifact"]) ]
                     if include_l0 else [("best", selection["artifact"])])
    for variant_id, artifact in variant_specs:
        variants.append({
            "id": variant_id,
            "version": _artifact_version(artifact),
            "artifact": artifact,
            "command": command,
            "instrumentation_config": str(service_file),
            "initial_state_transport": "file",
            "entry": common["entry"],
            "pass_env": pass_env,
        })
    judge = dict(base.get("judge", {}))
    # The 1 KiB head/tail projection hid satisfied requirements in the middle
    # of real answers. All 400 baseline answers fit this single-call window.
    judge["answer_field_bytes"] = 32768
    if judge.get("runtime_config"):
        # The judge has its own runtime configuration and does not inherit the
        # Agent variant's model.  Bind it to the provider/model selected for
        # this run; otherwise a stale plan-only model can turn every otherwise
        # valid trajectory into an HTTP 404.
        judge_runtime = _read((GEPA_RUNTIME / judge["runtime_config"]).resolve())
        runtime_prompts = judge_runtime.get("prompts")
        if isinstance(runtime_prompts, dict):
            # Runtime configuration intentionally accepts only relative prompt
            # paths. Copy the small prompt set beside the temporary runtime so
            # the config remains portable while the large evaluation stays in
            # /tmp and is removed after summarization.
            temporary_prompts = temp / "prompts"
            temporary_prompts.mkdir(parents=True, exist_ok=True)
            for prompt_name, prompt_file in tuple(runtime_prompts.items()):
                if prompt_file:
                    source = GEPA_RUNTIME / "prompts" / Path(prompt_file).name
                    shutil.copy2(source, temporary_prompts / source.name)
                    runtime_prompts[prompt_name] = str(Path("prompts") / source.name)
        model = judge_runtime.get("model")
        if isinstance(model, dict):
            endpoint = os.environ.get("AF_LLM_BASE_URL") or os.environ.get("OPENAI_BASE_URL")
            selected_model = os.environ.get("AF_LLM_MODEL") or os.environ.get("OPENAI_MODEL")
            if endpoint:
                endpoint = endpoint.rstrip("/")
                if not endpoint.endswith("/chat/completions"):
                    endpoint += "/chat/completions"
                model["endpoint"] = endpoint
            if selected_model:
                model["model_id"] = selected_model
            model["api_key_env"] = "OPENAI_API_KEY"
        judge_runtime_file = temp / "judge-runtime.json"
        _atomic_json(judge_runtime_file, judge_runtime)
        judge["runtime_config"] = str(judge_runtime_file)
    for key in ("prompt", "loop_review_prompt", "consistency_prompt", "goal_prompt"):
        if judge.get(key):
            judge[key] = str((GEPA_RUNTIME / judge[key]).resolve())
    config = {
        "schema_version": 1,
        "splits": list(splits),
        "repetitions": 1,
        # The business package supports isolated daemons per task.  Use the
        # documented 16-way evaluator concurrency; provider retries remain
        # per task and do not change the call/token budget.
        "parallelism": 16,
        "max_trials": 800,
        "timeout_ms": 1800000,
        "max_output_bytes": 4194304,
        "max_trace_bytes": 33554432,
        "max_artifact_bytes": 16777216,
        "max_report_bytes": 1073741824,
        "require_trace": True,
        "variants": variants,
        "checks": {},
        "judge": judge,
        "resources": {"nice": 10, "address_space_bytes": 8589934592, "cpu_seconds": 86400},
        "task_ids": (list(task_ids) if task_ids is not None else
                     [row["task_id"] for row in rows if row.get("split") in set(splits)]),
        "artifact_retention": {
            "retain_trial_files": False, "retain_markdown": False,
            "retain_jsonl": False, "retain_report_pb": False,
            "drop_intermediate_pb": True,
        },
    }
    config_file = temp / "evaluation.json"
    _atomic_json(config_file, config)
    return config_file


def _aggregate(rows: list[dict], planned: int | None = None,
               summary: dict | None = None) -> dict:
    # Reuse the canonical progress aggregator so the fallback has exactly the
    # same quality/loop/token/intent/level definitions as normal broadcasts.
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from progress_report import _aggregate as canonical_aggregate
    return canonical_aggregate(rows, planned if planned is not None else len(rows), summary)


def _summarize_report(report: dict, task_splits: dict[str, str],
                      planned_scope: dict[str, int] | None = None) -> dict:
    trials = report.get("trials") or []
    result = {}
    for variant in ("L0", "best"):
        selected = [row for row in trials if row.get("variant_id") == variant]
        by_split = {}
        for split in ("val", "holdout"):
            split_rows = [row for row in selected if row.get("split") == split]
            planned = ((planned_scope or {}).get(split, 0) if planned_scope is not None
                       else 200)
            by_split[split] = _aggregate(split_rows, planned)
        planned_all = (sum(planned_scope.values()) if planned_scope is not None else 400)
        by_split["all"] = _aggregate(selected, planned_all)
        result[variant] = by_split
    return result


def _comparison_is_complete(report: dict, summary: dict, expected_scope: dict[str, int],
                            include_l0: bool) -> bool:
    if not report.get("complete"):
        return False
    variants = ("L0", "best") if include_l0 else ("best",)
    for variant in variants:
        for split, planned in expected_scope.items():
            metrics = summary.get(variant, {}).get(split) or {}
            if (not metrics.get("quality_complete") or metrics.get("completed") != planned
                    or metrics.get("scored") != planned or metrics.get("errors", 0)):
                return False
    return True


def _reuse_candidate_holdout(selection: dict, task_ids: list[str], purpose: str) -> dict | None:
    """Reuse complete same-policy holdout even when its earlier role was best/peer."""
    profile = OUTPUT / 'audit/l0-400-20260929/judge-profile.json'
    judge = _read(profile).get('judge_fingerprint') if profile.is_file() else None
    if not judge:
        return None
    tag = _sha256_bytes(selection['version'].encode())[:10]
    task_hash = _sha256_bytes('\n'.join(sorted(task_ids)).encode())
    source_hash = ':'.join(_sha256(SOURCE / f'{split}.jsonl') for split in ('val', 'holdout'))
    root = OUTPUT / 'published/best-strategy-runs' / selection['job_id']
    for path in sorted(root.glob(f'node-*-{tag}-*-holdout-{len(task_ids)}-*-candidate/result.json')):
        try:
            result = _read(path)
            request = _read(path.with_name('request.json'))
            if (result.get('status') != 'complete' or result.get('version') != selection['version']
                    or request.get('source_dataset_sha256') != source_hash
                    or result.get('request_identity') != {'task_ids_sha256': task_hash, 'include_l0': False}):
                continue
            ref = result.get('report_path') or (result.get('report') or {}).get('path')
            if not ref:
                continue
            report_path = Path(ref)
            report = _read(report_path if report_path.is_absolute() else OUTPUT / report_path)
            rows = [row for row in report.get('trials', []) if row.get('variant_id') == 'best']
            if ((report.get('configuration') or {}).get('judge_fingerprint') != judge
                    or len(rows) != len(task_ids) or {r.get('task_id') for r in rows} != set(task_ids)
                    or any(r.get('split') != 'holdout' or
                           (r.get('evidence') or {}).get('loaded_version') != selection['version'] for r in rows)
                    or not _comparison_is_complete(report, result.get('metrics') or {},
                                                   {'holdout': len(task_ids)}, False)):
                continue
            result.update(report_path=ref, reused_from=str(path), reused_for_purpose=purpose,
                          new_agent_tasks=0)
            return result
        except (OSError, ValueError, TypeError):
            continue
    return None


@_with_candidate_runtime
def run_comparison(selection: dict, retries: int = 2,
                   splits: tuple[str, ...] = ("val", "holdout"),
                   persist_result: bool = True,
                   task_ids: list[str] | None = None,
                   include_l0: bool = True,
                   purpose: str = "final_comparison",
                   resume_report_path: str | Path | None = None) -> dict:
    rows, task_splits = _source_rows()
    selected_ids = (list(task_ids) if task_ids is not None else
                    [row["task_id"] for row in rows if row.get("split") in set(splits)])
    selected_splits = {split: sum(task_splits.get(task_id) == split for task_id in selected_ids)
                      for split in splits}
    if not include_l0 and splits == ("holdout",):
        reused = _reuse_candidate_holdout(selection, selected_ids, purpose)
        if reused is not None:
            return reused
    scope = selected_splits
    task_hash = _sha256_bytes("\n".join(sorted(selected_ids)).encode())
    version_tag = _sha256_bytes(selection["version"].encode())[:10]
    run_root = (OUTPUT / "published/best-strategy-runs" / selection["job_id"] /
                f"node-{selection['node']}-{version_tag}-{purpose}-{'-'.join(splits)}-"
                f"{len(selected_ids)}-{task_hash[:10]}-{'pair' if include_l0 else 'candidate'}")
    run_root.mkdir(parents=True, exist_ok=True)
    request_path = run_root / "request.json"
    result_path = run_root / "result.json"
    request = {
        "status": "running", "purpose": purpose,
        "scope": {**scope, "total": len(selected_ids)},
        "job_id": selection["job_id"], "node": selection["node"],
        "version": selection["version"], "source_dataset": str(SOURCE.relative_to(ROOT)),
        "task_ids_sha256": task_hash,
        "include_l0": include_l0,
        "source_dataset_sha256": _sha256(SOURCE / "val.jsonl") + ":" + _sha256(SOURCE / "holdout.jsonl"),
        "started_at": datetime.now(timezone.utc).isoformat(),
    }
    previous_result = None
    if result_path.is_file():
        try:
            previous_result = _read(result_path)
        except (OSError, ValueError):
            previous_result = None
    previous_report_path = ((previous_result or {}).get("report") or {}).get("path")
    previous_report_file = OUTPUT / previous_report_path if isinstance(previous_report_path, str) else None
    previous_report = None
    if previous_report_file and previous_report_file.is_file():
        try:
            previous_report = _read(previous_report_file)
        except (OSError, ValueError):
            previous_report = None
    if (previous_result and previous_report and previous_result.get("status") == "complete" and
            previous_result.get("version") == selection["version"] and
            previous_result.get("request_identity") == {
                "task_ids_sha256": task_hash, "include_l0": include_l0} and
            _comparison_is_complete(previous_report,
                                    previous_result.get("metrics") or {},
                                    selected_splits, include_l0)):
        previous_result["report_path"] = previous_result.get("report", {}).get("path")
        return previous_result
    first_attempt = int((previous_result or {}).get("attempt", 0)) + 1
    _atomic_json(request_path, request)
    if persist_result:
        _atomic_json(REQUEST_FILE, request)
    last_error = "comparison_not_started"
    last_result = None
    last_report_path = None
    if isinstance(resume_report_path, (str, Path)) and Path(resume_report_path).is_file():
        last_report_path = Path(resume_report_path).resolve()
    elif previous_report_file and previous_report_file.is_file():
        last_report_path = previous_report_file.resolve()
    for attempt in range(first_attempt, first_attempt + retries + 1):
        request.update(status="running", attempt=attempt,
                       attempt_path=str((run_root / f"attempt-{attempt}").relative_to(OUTPUT)))
        _atomic_json(request_path, request)
        try:
            # Stable config and data paths are required for native report
            # resumption; only failed judge rows are re-evaluated on retry.
            config_dir = run_root / "config"
            config_dir.mkdir(parents=True, exist_ok=True)
            config = _comparison_config(config_dir, selection, rows, splits,
                                        selected_ids, include_l0)
            output = run_root / f"attempt-{attempt}" / "evaluation"
            command = [str(BINARY), "dataset", "run", str(SOURCE), "--config", str(config),
                       "--output-root", str(output), "--allow-unreviewed"]
            if last_report_path and last_report_path.is_file():
                command.extend(["--resume-report", str(last_report_path)])
            env = os.environ.copy()
            env.setdefault("TRAJECTORY_UNCONDITIONAL_RESUME", "1")
            env.setdefault("AF_LLM_MAX_RETRIES", "4")
            env.setdefault("AF_LLM_RETRY_BASE_DELAY", "2")
            log_file = run_root / "evaluator.log"
            with log_file.open("a", encoding="utf-8") as stream:
                completed = subprocess.run(
                    command, cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT,
                    timeout=86400,
                )
            report_file = output / "report.json"
            if completed.returncode not in (0, 1, 3) or not report_file.is_file():
                last_error = f"evaluator_exit:{completed.returncode}"
                continue
            report = _read(report_file)
            last_report_path = report_file.resolve()
            summary = _summarize_report(report, task_splits, selected_splits)
            expected_trials = len(selected_ids) * (2 if include_l0 else 1)
            complete = (_comparison_is_complete(report, summary, selected_splits, include_l0)
                        and len(report.get("trials") or []) >= expected_trials)
            result = {
                "status": "complete" if complete else "incomplete",
                "scope": {**scope, "total": len(selected_ids)},
                "job_id": selection["job_id"], "node": selection["node"],
                "version": selection["version"], "attempt": attempt,
                "request_identity": {"task_ids_sha256": task_hash,
                                     "include_l0": include_l0},
                "complete_trials": len(report.get("trials") or []),
                "expected_trials": expected_trials, "metrics": summary,
                "report": {"complete": report.get("complete"),
                           "config_fingerprint": report.get("config_fingerprint"),
                           "dataset_version": report.get("dataset_version"),
                           "report_sha256": _sha256(report_file),
                           "path": str(report_file.relative_to(OUTPUT))},
                "finished_at": datetime.now(timezone.utc).isoformat(),
            }
            try:
                bundle = _persist_regrade_bundle(output, selection, attempt,
                                                 f"{purpose}-{'-'.join(splits)}-{len(selected_ids)}")
                if bundle:
                    result["regrade_bundle"] = bundle
            except (OSError, tarfile.TarError, ValueError) as exc:
                result["regrade_bundle_error"] = type(exc).__name__
            if persist_result:
                _atomic_json(COMPARISON_FILE, result)
                request.update(status=result["status"], finished_at=result["finished_at"],
                               attempts=attempt, complete_trials=result["complete_trials"])
                _atomic_json(REQUEST_FILE, request)
            request.update(status=result["status"], finished_at=result["finished_at"],
                           complete_trials=result["complete_trials"],
                           report_path=result["report"].get("path"))
            _atomic_json(request_path, request)
            _atomic_json(result_path, result)
            result["report_path"] = result["report"].get("path")
            last_result = result
            if complete:
                return result
            last_error = "comparison_incomplete_scoring_errors"
        except (OSError, subprocess.SubprocessError, ValueError, RuntimeError) as exc:
            last_error = type(exc).__name__
        if attempt < first_attempt + retries:
            time.sleep(min(60, 5 * attempt))
    result = dict(last_result or {})
    result.update({
        "status": "incomplete" if last_result else "failed",
        "scope": {**scope, "total": len(selected_ids)},
        "job_id": selection["job_id"], "node": selection["node"],
        "version": selection["version"], "error": last_error,
        "finished_at": datetime.now(timezone.utc).isoformat(),
    })
    if persist_result:
        _atomic_json(COMPARISON_FILE, result)
        request.update(status=result["status"], error=last_error,
                       finished_at=result["finished_at"])
        _atomic_json(REQUEST_FILE, request)
    request.update(status=result["status"], error=last_error,
                   finished_at=result["finished_at"])
    _atomic_json(request_path, request)
    _atomic_json(result_path, result)
    return result


def _screening_report_rows(selection: dict, tree: dict) -> tuple[list[dict], str] | None:
    relative = selection.get("screening_report_path")
    task_ids = selection.get("screening_task_ids") or []
    if not isinstance(relative, str) or len(set(task_ids)) != 50:
        return None
    cached_pool = selection.get("validation_evidence_source") == "reused_complete_pool_val"
    path = ((JOB_ROOT / selection["job_id"] / "search" / relative)
            if cached_pool else OUTPUT / relative)
    if not path.is_file():
        return None
    report = _read(path)
    if (not report.get("complete") or not _compatible_validation_dataset(
            JOB_ROOT / selection["job_id"],
            _expected_dataset_version(JOB_ROOT / selection["job_id"], tree),
            report.get("dataset_version"), set(task_ids))):
        return None
    variant_id = "candidate" if cached_pool else selection.get("screening_variant_id", "best")
    if variant_id not in ("candidate", "best"):
        return None
    rows = [row for row in report.get("trials", [])
            if row.get("variant_id") == variant_id and row.get("split") == "val"]
    wanted = set(task_ids)
    if (len(rows) != 50 or {str(row.get("task_id")) for row in rows} != wanted or
            not _aggregate(rows, 50).get("quality_complete") or
            any((row.get("evidence") or {}).get("loaded_version") != selection["version"]
                for row in rows)):
        return None
    expected_hash = selection.get("screening_report_sha256")
    if expected_hash and _sha256(path) != expected_hash:
        return None
    return rows, (str(path.relative_to(JOB_ROOT / selection["job_id"] / "search"))
                  if cached_pool else str(path.relative_to(OUTPUT)))


def _baseline400_rows(split: str, version: str) -> tuple[list[dict], str] | None:
    """Load an indexed, complete L0 split without rerunning or duplicating traces."""
    directory = OUTPUT / "published/seed004/baseline-cache"
    if not (directory / 'judge-profile.json').is_file():
        directory = OUTPUT / 'baseline-cache'
    if not (directory / 'judge-profile.json').is_file():
        directory = OUTPUT / "audit/l0-400-20260929"
    profile_path = directory / "judge-profile.json"
    profile = _read(profile_path) if profile_path.is_file() else {}
    if profile and profile.get("status") != "complete":
        raise RuntimeError("baseline_judge_regrade_pending_no_agent_rerun")
    index_path = directory / ("val" if split == "val" else "sealed-holdout") / "trial-index.json"
    if not index_path.is_file():
        return None
    index = _read(index_path)
    if profile.get("judge_fingerprint") and index.get("judge_fingerprint") != profile["judge_fingerprint"]:
        raise RuntimeError("baseline_judge_profile_mismatch_no_agent_rerun")
    if (index.get("report_kind") != "baseline_trial_index" or not index.get("complete")
            or index.get("version") != version or index.get("split") != split):
        return None
    source_rows, source_splits = _source_rows()
    wanted = {task for task, part in source_splits.items() if part == split}
    refs = index.get("trials") or []
    if len(wanted) != 200 or len(refs) != 200 or {r.get("task_id") for r in refs} != wanted:
        return None
    by_report = {}
    for ref in refs:
        by_report.setdefault(ref["report_path"], []).append(ref)
    rows = []
    for report_ref, references in by_report.items():
        path = ROOT / report_ref
        if not path.is_file():
            return None
        payload = _read(path)
        if profile.get("judge_fingerprint") and payload.get("configuration", {}).get("judge_fingerprint") != profile["judge_fingerprint"]:
            raise RuntimeError("baseline_report_judge_mismatch_no_agent_rerun")
        candidates = payload.get("trials", [payload] if "task_id" in payload else [])
        by_trial = {row.get("trial_id"): row for row in candidates}
        for ref in references:
            row = by_trial.get(ref["trial_id"])
            if (not row or row.get("task_id") != ref["task_id"] or row.get("split") != split
                    or row.get("variant_id") != ref.get("variant_id", "L0")
                    or (row.get("evidence") or {}).get("loaded_version") != version):
                return None
            rows.append(dict(row, variant_id="L0"))
    metrics = _aggregate(rows, 200)
    if not metrics.get("quality_complete") or metrics.get("scored") != 200 or metrics.get("errors"):
        return None
    return rows, str(index_path.relative_to(OUTPUT))


@_with_candidate_runtime
def _final_validation(selection: dict) -> dict:
    """Reuse the chosen candidate's 50-task screen and L0 cache before the 200-val closeout."""
    root = JOB_ROOT / selection["job_id"]
    tree = _read(root / "search/tree.json")
    screening = _screening_report_rows(selection, tree)
    task_ids = selection.get("screening_task_ids") or []
    l0_version = _artifact_version(_read(root / "baseline.json"))
    full_l0_cache = _baseline400_rows("val", l0_version)
    if full_l0_cache:
        l0_rows, l0_path = full_l0_cache
        _, task_splits = _source_rows()
        val_ids = {task for task, split in task_splits.items() if split == "val"}
        screen_rows, screen_path = screening if screening else ([], None)
        remaining_ids = sorted(val_ids - {row["task_id"] for row in screen_rows})
        measured = run_comparison(selection, splits=("val",), task_ids=remaining_ids,
                                  purpose="final_val_with_cached_l0", include_l0=False)
        report_ref = (measured.get("report") or {}).get("path")
        report_path = OUTPUT / report_ref if report_ref else None
        if not report_path or not report_path.is_file():
            return measured
        report = _read(report_path)
        new_rows = [row for row in report.get("trials", [])
                    if row.get("variant_id") == "best" and row.get("split") == "val"]
        best_rows = screen_rows + new_rows
        best_metrics, l0_metrics = _aggregate(best_rows, 200), _aggregate(l0_rows, 200)
        complete = (len(best_rows) == 200 and {r["task_id"] for r in best_rows} == val_ids
                    and measured.get("status") == "complete"
                    and best_metrics.get("quality_complete") and l0_metrics.get("quality_complete"))
        measured.update({"status": "complete" if complete else "incomplete",
                         "scope": {"val": 200, "total": 200},
                         "complete_trials": len(best_rows) + len(l0_rows), "expected_trials": 400,
                         "metrics": {"L0": {"val": l0_metrics}, "best": {"val": best_metrics}},
                         "val_reuse": {"l0_complete_200": l0_path,
                                       "candidate_screen_50": screen_path,
                                       "new_candidate_tasks": len(remaining_ids), "new_l0_tasks": 0}})
        _atomic_json(COMPARISON_FILE, measured)
        _atomic_json(REQUEST_FILE, {"status": measured["status"], "job_id": selection["job_id"],
                                   "node": selection.get("node"), "version": selection["version"],
                                   "scope": measured["scope"], "val_reuse": measured["val_reuse"]})
        return measured
    l0_cached = _cached_val_report(root, tree, l0_version, task_ids) if screening else None
    if screening is None or l0_cached is None:
        return run_comparison(selection, splits=("val",), purpose="final_val_200")

    all_rows, task_splits = _source_rows()
    val_ids = {task_id for task_id, split in task_splits.items() if split == "val"}
    remaining_ids = sorted(val_ids - set(task_ids))
    if len(remaining_ids) != 150:
        return run_comparison(selection, splits=("val",), purpose="final_val_200")
    remainder = run_comparison(selection, splits=("val",), task_ids=remaining_ids,
                                purpose="final_val_remainder_150", include_l0=True)
    remainder_path = OUTPUT / (remainder.get("report") or {}).get("path", "")
    if not remainder_path.is_file():
        return remainder
    report = _read(remainder_path)
    screen_rows, screen_path = screening
    l0_rows, l0_path = l0_cached
    l0_rows = [dict(row, variant_id="L0") for row in l0_rows]
    best_rows = screen_rows + [row for row in report.get("trials", [])
                               if row.get("variant_id") == "best" and row.get("split") == "val"]
    l0_final_rows = l0_rows + [row for row in report.get("trials", [])
                               if row.get("variant_id") == "L0" and row.get("split") == "val"]
    best_metrics = _aggregate(best_rows, 200)
    l0_metrics = _aggregate(l0_final_rows, 200)
    complete = (bool(report.get("complete")) and len(best_rows) == 200 and
                len(l0_final_rows) == 200 and best_metrics.get("quality_complete") and
                l0_metrics.get("quality_complete"))
    remainder.update({
        "status": "complete" if complete else "incomplete",
        "scope": {"val": 200, "total": 200},
        "complete_trials": len(best_rows) + len(l0_final_rows),
        "expected_trials": 400,
        "metrics": {
            "L0": {"val": l0_metrics, "holdout": selection.get("l0_holdout"),
                   "all": _aggregate(l0_final_rows, 200)},
            "best": {"val": best_metrics, "holdout": selection.get("holdout"),
                     "all": _aggregate(best_rows, 200)},
        },
        "val_reuse": {"candidate_screen_50": screen_path,
                      "l0_screen_cache_50": l0_path,
                      "new_paired_trials": 300},
    })
    _atomic_json(COMPARISON_FILE, remainder)
    request = {"status": remainder["status"], "job_id": selection["job_id"],
               "node": selection["node"], "version": selection["version"],
               "scope": {"val": 200, "total": 200},
               "complete_trials": remainder["complete_trials"],
               "expected_trials": 400, "val_reuse": remainder["val_reuse"]}
    _atomic_json(REQUEST_FILE, request)
    return remainder


def build_best(job_id: str = "", run_full_comparison: bool = True) -> dict:
    selection = choose_best(job_id)
    public_selection = {k: v for k, v in selection.items()
                        if k not in {"artifact", "holdout_report_data"}}
    result = {
        "schema_version": 1,
        "status": "best_observed" if selection.get("artifact") is not None else "best_available",
        "formal_release": False,
        "selection": public_selection,
        "artifact": selection["artifact"],
        "created_at": datetime.now(timezone.utc).isoformat(),
    }
    _atomic_json(BEST_FILE, result)
    if run_full_comparison and selection.get("artifact") is not None:
        validation = _final_validation(selection)
        comparison = {
            **validation,
            "scope": {"val": 200, "holdout": 200, "total": 400},
            "complete_trials": (validation.get("complete_trials", 0)
                                + int((selection.get("holdout") or {}).get("completed", 0))
                                + int((selection.get("l0_holdout") or {}).get("completed", 0))),
            "expected_trials": 800,
            "metrics": {
                key: {
                    "val": validation.get("metrics", {}).get(key, {}).get("val"),
                    "holdout": selection.get("l0_holdout" if key == "L0" else "holdout"),
                }
                for key in ("L0", "best")
            },
            "holdout_evidence_source": selection.get("holdout_evidence_source"),
            "posthoc_showcase_only": True,
            "candidate_scope": selection.get("candidate_scope"),
            "selection_reason": selection.get("selection_reason"),
            "formal_gate_status": {
                "tree_status": selection.get("tree_status"),
                "node_optimization_passed": (selection.get("optimization") or {}).get("passed"),
                "official": selection.get("official_gate_status"),
            },
        }
        val_metrics = validation.get("metrics", {})
        val_quality_complete = all(
            bool((val_metrics.get(variant, {}).get("val") or {}).get("quality_complete"))
            for variant in ("L0", "best"))
        comparison["effective_quality_complete"] = {
            "L0_val": bool((val_metrics.get("L0", {}).get("val") or {}).get("quality_complete")),
            "best_val": bool((val_metrics.get("best", {}).get("val") or {}).get("quality_complete")),
            "L0_holdout": bool(selection.get("l0_holdout", {}).get("quality_complete")),
            "best_holdout": bool(selection.get("holdout", {}).get("quality_complete")),
        }
        comparison["status"] = ("complete" if validation.get("status") == "complete"
                                 and val_quality_complete
                                 and selection.get("holdout", {}).get("quality_complete")
                                 and selection.get("l0_holdout", {}).get("quality_complete")
                                 and not (selection.get("candidate_scope") or {}).get("shortfall")
                                 and not (selection.get("candidate_scope") or {}).get("holdout_shortfall")
                                 else "incomplete")
    else:
        comparison = {
            "status": "incomplete" if selection.get("artifact") is None else "pending",
            "scope": {"val": 200, "holdout": 200, "total": 400},
            "complete_trials": 0,
            "expected_trials": 800,
            "posthoc_showcase_only": True,
            "selection_reason": selection.get("selection_reason"),
            "candidate_scope": selection.get("candidate_scope"),
            "formal_gate_status": {
                "tree_status": selection.get("tree_status"),
                "node_optimization_passed": (selection.get("optimization") or {}).get("passed"),
                "official": selection.get("official_gate_status"),
            },
        }
    result["comparison"] = comparison
    _atomic_json(BEST_FILE, result)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--job-id", default="")
    parser.add_argument("--no-comparison", action="store_true")
    args = parser.parse_args()
    result = build_best(args.job_id, not args.no_comparison)
    print(json.dumps({"status": result["status"], "node": result["selection"].get("node"),
                      "version": result["selection"].get("version"),
                      "comparison": result["comparison"].get("status")}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
