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
from datetime import datetime, timezone
import hashlib
import json
import math
import os
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


def _read(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


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


def _persist_regrade_bundle(output: Path, selection: dict, attempt: int) -> dict | None:
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
    target = REGRADE_DIR / f"best-node-{int(selection['node'])}.tar.gz"
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


def choose_best(job_id: str = "") -> dict:
    """Select one observed version and return its compact provenance."""
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
    ranked: list[tuple[dict, dict]] = []
    for node in tree.get("nodes", []):
        version = node.get("version")
        node_id = node.get("id")
        if not version or not isinstance(node_id, (int, float)):
            continue
        version_file = root / "search/versions" / f"{int(node_id)}.json"
        if not version_file.is_file():
            continue
        metrics = _node_metrics(node)
        if metrics["completed"] == 0:
            continue
        ranked.append((node, metrics))
    if not ranked:
        # The seed is always a valid fallback even when no reflection produced
        # a scored node.  It is loaded from the durable job checkpoint.
        seed = _read(root / "seed.json")
        node = {"id": 0, "status": "seed", "version": "", "optimization": {}}
        version = _artifact_version(seed)
        return {
            "job_id": job_id, "node": 0, "version": version, "artifact": seed,
            "selection_reason": "seed_only_no_scored_candidate",
            "validation": {}, "tree_status": tree.get("status"),
            "tree_error": tree.get("error"), "formal_gate_passed": False,
            "tree_sha256": _sha256(tree_file),
        }
    node, metrics = max(ranked, key=_selection_key)
    node_id = int(node["id"])
    artifact = _read(root / "search/versions" / f"{node_id}.json")
    version = _artifact_version(artifact)
    return {
        "job_id": job_id,
        "node": node_id,
        "version": version,
        "artifact": artifact,
        "selection_reason": "best_observed_validation_objective",
        "selection_key": list(_selection_key((node, metrics))),
        "validation": metrics,
        "optimization": node.get("optimization") or {},
        "control_coverage": node.get("control_coverage") or {},
        "validation_control_coverage": node.get("validation_control_coverage") or {},
        "tree_status": tree.get("status"),
        "tree_error": tree.get("error"),
        "tree_stop_reason": tree.get("stop_reason"),
        "formal_gate_passed": False,
        "tree_sha256": _sha256(tree_file),
    }


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


def _comparison_config(temp: Path, selection: dict, rows: list[dict]) -> Path:
    base = _read(GEPA_RUNTIME / "evaluation.json")
    service = _read(GEPA_RUNTIME / "service.json")
    # Keep the evaluation's state and traces outside the 5 GB deliverable.
    data_dir = temp / "agent-data"
    data_dir.mkdir(parents=True, exist_ok=True)
    service["data_dir"] = str(data_dir)
    service["memory_source_dir"] = str((OUTPUT / "training/evolution").resolve())
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
    baseline = _read(JOB_ROOT / selection["job_id"] / "baseline.json")
    variants = []
    for variant_id, artifact in (("L0", baseline), ("best", selection["artifact"])):
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
        "splits": ["val", "holdout"],
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
        "task_ids": [row["task_id"] for row in rows],
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


def _summarize_report(report: dict, task_splits: dict[str, str]) -> dict:
    trials = report.get("trials") or []
    result = {}
    for variant in ("L0", "best"):
        selected = [row for row in trials if row.get("variant_id") == variant]
        by_split = {}
        for split in ("val", "holdout"):
            split_rows = [row for row in selected if row.get("split") == split]
            by_split[split] = _aggregate(split_rows, 200)
        by_split["all"] = _aggregate(selected, 400)
        result[variant] = by_split
    return result


def run_comparison(selection: dict, retries: int = 2) -> dict:
    rows, task_splits = _source_rows()
    request = {
        "status": "running", "scope": {"val": 200, "holdout": 200, "total": 400},
        "job_id": selection["job_id"], "node": selection["node"],
        "version": selection["version"], "source_dataset": str(SOURCE.relative_to(ROOT)),
        "source_dataset_sha256": _sha256(SOURCE / "val.jsonl") + ":" + _sha256(SOURCE / "holdout.jsonl"),
        "started_at": datetime.now(timezone.utc).isoformat(),
    }
    _atomic_json(REQUEST_FILE, request)
    last_error = "comparison_not_started"
    for attempt in range(1, retries + 2):
        with tempfile.TemporaryDirectory(prefix="business-best-400-") as directory:
            temp = Path(directory)
            try:
                config = _comparison_config(temp, selection, rows)
                output = temp / "evaluation"
                env = os.environ.copy()
                env.setdefault("TRAJECTORY_UNCONDITIONAL_RESUME", "1")
                env.setdefault("AF_LLM_MAX_RETRIES", "4")
                env.setdefault("AF_LLM_RETRY_BASE_DELAY", "2")
                log_file = temp / "evaluator.log"
                with log_file.open("w", encoding="utf-8") as stream:
                    completed = subprocess.run(
                        [str(BINARY), "dataset", "run", str(SOURCE), "--config", str(config),
                         "--output-root", str(output), "--allow-unreviewed"],
                        cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT,
                        timeout=86400,
                    )
                report_file = output / "report.json"
                if completed.returncode not in (0, 1, 3) or not report_file.is_file():
                    last_error = f"evaluator_exit:{completed.returncode}"
                    continue
                report = _read(report_file)
                summary = _summarize_report(report, task_splits)
                complete = bool(report.get("complete")) and len(report.get("trials") or []) >= 800
                result = {
                    "status": "complete" if complete else "incomplete",
                    "scope": {"val": 200, "holdout": 200, "total": 400},
                    "job_id": selection["job_id"], "node": selection["node"],
                    "version": selection["version"], "attempt": attempt,
                    "complete_trials": len(report.get("trials") or []),
                    "expected_trials": 800, "metrics": summary,
                    "report": {"complete": report.get("complete"),
                               "config_fingerprint": report.get("config_fingerprint"),
                               "dataset_version": report.get("dataset_version"),
                               "report_sha256": _sha256(report_file)},
                    "finished_at": datetime.now(timezone.utc).isoformat(),
                }
                # Retain only the compact judge-regrade evidence.  The full
                # trial workspace remains disposable and is removed when this
                # temporary directory exits, keeping the deliverable light.
                try:
                    bundle = _persist_regrade_bundle(output, selection, attempt)
                    if bundle:
                        result["regrade_bundle"] = bundle
                except (OSError, tarfile.TarError, ValueError) as exc:
                    # A storage/archive failure must not erase an otherwise
                    # valid comparison result; surface it for diagnosis.
                    result["regrade_bundle_error"] = type(exc).__name__
                _atomic_json(COMPARISON_FILE, result)
                request.update(status=result["status"], finished_at=result["finished_at"],
                               attempts=attempt, complete_trials=result["complete_trials"])
                _atomic_json(REQUEST_FILE, request)
                return result
            except (OSError, subprocess.SubprocessError, ValueError, RuntimeError) as exc:
                last_error = type(exc).__name__
        if attempt <= retries:
            time.sleep(min(60, 5 * attempt))
    result = {
        "status": "failed", "scope": {"val": 200, "holdout": 200, "total": 400},
        "job_id": selection["job_id"], "node": selection["node"],
        "version": selection["version"], "error": last_error,
        "finished_at": datetime.now(timezone.utc).isoformat(),
    }
    _atomic_json(COMPARISON_FILE, result)
    request.update(status="failed", error=last_error, finished_at=result["finished_at"])
    _atomic_json(REQUEST_FILE, request)
    return result


def build_best(job_id: str = "", run_full_comparison: bool = True) -> dict:
    selection = choose_best(job_id)
    result = {
        "schema_version": 1,
        "status": "best_observed",
        "formal_release": False,
        "selection": {k: v for k, v in selection.items() if k != "artifact"},
        "artifact": selection["artifact"],
        "created_at": datetime.now(timezone.utc).isoformat(),
    }
    _atomic_json(BEST_FILE, result)
    comparison = run_comparison(selection) if run_full_comparison else {
        "status": "pending", "scope": {"val": 200, "holdout": 200, "total": 400},
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
