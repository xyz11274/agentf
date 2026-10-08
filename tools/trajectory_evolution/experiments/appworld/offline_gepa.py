#!/usr/bin/env python3
"""AppWorld's native GEPA lifecycle.

This entry point follows the same boundary as the business experiment:
training evidence is imported first, a Seed0 artifact is compiled from that
store, GEPA searches the training pool, validation selects candidates, and
only then can the fixed comparison500 comparison be requested.  The script is
read-only unless ``--prepare`` or ``--run`` is explicitly supplied; neither
command changes an existing published artifact.

The model-facing search is submitted to the native ``serve`` API.  No Python
implementation re-scores or rewrites native GEPA state.
"""
from __future__ import annotations

from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
import argparse
import copy
import hashlib
import json
import os
import secrets
import select
import subprocess
import time
import urllib.request

import layout as controller

ROOT = controller.ROOT
HERE = controller.HERE
OUTPUT = ROOT / "trace_output/appworld"
TRAINING = OUTPUT / "training"
GEPA = OUTPUT / "gepa"
PUBLISHED = OUTPUT / "published"
DATASETS = HERE / "datasets"
DATASET = TRAINING / "datasets/gepa"
CONFIG = GEPA / "runtime"
WORKSPACE = GEPA / "workspace"
PLAN = GEPA / "plan.json"
STATUS = GEPA / "status.json"
USER = controller.OWNER
AGENT = controller.AGENT
BINARY = controller.BINARY


def read(path: Path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def save(path: Path, value) -> None:
    path = Path(path)
    if path == STATUS and path.is_file() and "seed_search" not in value:
        previous = read(path)
        if isinstance(previous.get("seed_search"), dict):
            value = dict(value, seed_search=previous["seed_search"])
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".new")
    temporary.write_text(json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n",
                         encoding="utf-8")
    temporary.replace(path)


def _recorded_job(artifact: dict) -> str | None:
    """Reuse the submitted search when its frozen seed is the requested seed."""
    submission = GEPA / "submission.json"
    if not submission.is_file():
        return None
    job_id = read(submission).get("job_id")
    if not job_id:
        return None
    state_path = WORKSPACE / "agent-evaluation/state.json"
    if not state_path.is_file():
        raise RuntimeError("offline_gepa_checkpoint_unavailable")
    job = (read(state_path).get("jobs") or {}).get(job_id)
    if (not isinstance(job, dict) or job.get("user") != USER
            or job.get("level") != "L3" or job.get("gepa") is not True):
        raise RuntimeError("offline_gepa_checkpoint_unavailable")
    frozen_seed = WORKSPACE / "agent-evaluation" / job_id / "seed.json"
    if not frozen_seed.is_file():
        raise RuntimeError("offline_gepa_checkpoint_seed_missing")
    # Equality uses the saved artifact itself, without adding signatures or
    # checking manager/runtime versions. Changed runtime configuration can
    # therefore resume the original native job and search tree.
    return job_id if read(frozen_seed) == artifact else None


def build_dataset(recorded_job: str | None = None) -> dict:
    """Build the canonical disjoint train/validation/comparison view."""
    from dataset import materialize
    frozen = {}
    if recorded_job is not None:
        plan = read(PLAN)
        tree = read(WORKSPACE / 'agent-evaluation' / recorded_job / 'search/tree.json')
        count = (plan.get('split_counts') or {}).get('train')
        if type(count) not in (int, float) or isinstance(count, bool) or int(count) != count or count <= 0:
            raise RuntimeError('appworld_frozen_gepa_training_scope_invalid')
        from dataset import rows
        frozen = {'frozen_training_tasks': [row['task_id'] for row in rows('train')[:int(count)]],
                  'expected_dataset_version': tree.get('dataset_version')}
    destination = Path(materialize(BINARY, view='gepa', **frozen)['path'])
    return read(destination / 'manifest.json')


def _completed_training_superset(frozen_manifest: dict, count: int) -> bool:
    """A completed newer TRAIN pack can prove an unchanged frozen subset."""
    try:
        status = read(TRAINING / 'full-training-status.json')
        current = TRAINING / 'datasets/train'
        if (status.get('status') != 'complete'
                or Path(status.get('dataset', '')).resolve() != current.resolve()
                or status.get('deferred_reviews') or status.get('last_error')
                or status.get('errors') or status.get('errored')
                or status.get('last_learning', {}).get('usable') is not True):
            return False
        manifest = read(current / 'manifest.json')
        rows_by_pack = []
        for folder, metadata in ((DATASET, frozen_manifest), (current, manifest)):
            payload = (folder / 'train.jsonl').read_bytes()
            if hashlib.sha256(payload).hexdigest() != metadata.get('files', {}).get('train.jsonl'):
                return False
            rows = [json.loads(line) for line in payload.splitlines() if line.strip()]
            task_ids = [row.get('task_id') for row in rows]
            if (not rows or any(not isinstance(task, str) or not task for task in task_ids)
                    or len(set(task_ids)) != len(task_ids)
                    or any(row.get('split') != 'train' or row.get('review_status') != 'reviewed'
                           for row in rows)
                    or len(rows) != metadata.get('summary', {}).get('splits', {}).get('train')):
                return False
            rows_by_pack.append({row['task_id']: row for row in rows})
        frozen, completed = rows_by_pack
        planned = len(completed)
        batch_size = status.get('batch_size')
        if (len(frozen) != count or not frozen.keys() <= completed.keys()
                or type(batch_size) is not int or batch_size <= 0):
            return False
        batches = (planned + batch_size - 1) // batch_size
        expected = {'planned_tasks': planned, 'completed_tasks': planned,
                    'planned_batches': batches, 'completed_batches': batches,
                    'imported_batches': batches}
        if any(isinstance(status.get(key), bool) or status.get(key) != value
               for key, value in expected.items()):
            return False
        def task_spec(row):
            value = copy.deepcopy(row)
            # Native versions include the materialized input-file fingerprint;
            # appending TRAIN changes these two derived fields for every row.
            value.pop('task_version', None)
            provenance = value.get('provenance')
            if isinstance(provenance, dict):
                provenance.pop('source_fingerprints', None)
            return value
        return all(task_spec(row) == task_spec(completed[task])
                   for task, row in frozen.items())
    except (OSError, ValueError, TypeError, AttributeError):
        return False


def _recorded_training_ready(job_id: str) -> bool:
    """Keep an admitted search bound to its completed frozen TRAIN scope."""
    plan = read(PLAN)
    tree = read(WORKSPACE / 'agent-evaluation' / job_id / 'search/tree.json')
    count = (plan.get('split_counts') or {}).get('train')
    if (type(count) not in (int, float) or isinstance(count, bool)
            or count <= 0 or int(count) != count
            or not tree.get('dataset_version')
            or tree.get('dataset_version') != plan.get('dataset_version')
            or Path(plan.get('dataset', '')).resolve() != DATASET.resolve()):
        return False
    manifest = read(DATASET / 'manifest.json')
    if (manifest.get('metadata', {}).get('dataset_version') != tree['dataset_version']
            or manifest.get('summary', {}).get('splits', {}).get('train') != count):
        return False
    receipt = plan.get('training_readiness')
    if not isinstance(receipt, dict):
        lifecycle = read(GEPA / 'lifecycle.json')
        receipt = {'train_tasks': lifecycle.get('dataset', {}).get('split_counts', {}).get('train'),
                   'stage_result': lifecycle.get('stage_results', {}).get('train')}
    preserved = (receipt.get('train_tasks') == count
            and isinstance(receipt.get('stage_result'), dict)
            and receipt['stage_result'].get('status') == 'complete'
            and receipt['stage_result'].get('data', {}).get('callback') == 'appworld.training')
    if preserved:
        return True
    # Legacy plans did not persist training_readiness. A later common preflight
    # may update lifecycle counts; prove the original scope from the completed
    # actual task pack instead of comparing those mutable counts alone.
    return _completed_training_superset(manifest, int(count))


def _training_ready(recorded_job: str | None = None) -> Path:
    status = TRAINING / "full-training-status.json"
    if recorded_job is not None:
        if not _recorded_training_ready(recorded_job):
            raise RuntimeError("appworld_frozen_training_receipt_unavailable")
    elif not status.is_file() or read(status).get("status") != "complete":
        raise RuntimeError("appworld_training_not_complete")
    memory = TRAINING / "evolution/by-user" / USER / "memory/evolution-memory.json"
    if not memory.is_file():
        raise RuntimeError("appworld_training_memory_missing")
    entries = read(memory).get("entries", [])
    if not any((entry.get("candidate") or {}).get("status") ==
               "EVOLUTION_CANDIDATE_STATUS_PROPOSED" for entry in entries):
        raise RuntimeError("appworld_training_has_no_proposed_experience")
    return memory


def _copy_training_configs() -> None:
    CONFIG.mkdir(parents=True, exist_ok=True)
    source = controller.RUNTIME
    for name in ("runtime.json", "service.json"):
        value = read(source / name)
        save(CONFIG / name, value)
    prompts = ROOT / "tools/trajectory_evolution/config/prompts"
    target = CONFIG / "prompts"
    if not target.exists():
        target.symlink_to(prompts, target_is_directory=True)
    # Native runtime resolves learning prompt paths relative to this config.
    # Keep the private templates used during training available on GEPA's
    # identical runtime contract instead of dropping them while copying JSON.
    private_prompts = source / "appworld-prompts"
    private_target = CONFIG / "appworld-prompts"
    if private_prompts.is_dir() and not private_target.exists():
        private_target.symlink_to(private_prompts.resolve(), target_is_directory=True)


def require_seed_gates(seed_artifact: Path) -> dict:
    """Every search entry requires complete same-seed 32/100 target proofs."""
    import stage_gate
    import reproduce
    reproduce.ensure_search_allowed(GEPA)
    seed = Path(seed_artifact).resolve()
    artifact = read(seed)
    for count in (32, 100):
        comparison = reproduce.stage_path(seed, count, gepa_root=GEPA)
        baseline = reproduce._stage_l0_source(count, comparison, output_root=GEPA.parent)
        try:
            passed, result = stage_gate.verify_stage(str(count), comparison, baseline, seed)
        except (OSError, ValueError, RuntimeError) as error:
            raise RuntimeError(f"appworld_seed_stage{count}_goal_gate_required") from error
        if not passed or result.get("goal_passed") is not True:
            raise RuntimeError(f"appworld_seed_stage{count}_goal_gate_not_passed")
    if read(seed) != artifact:
        raise RuntimeError("appworld_seed_changed_during_goal_verification")
    return artifact


def _baseline_evaluation_file(stage: Path) -> Path:
    """Use the native configuration beside the canonical graded source."""
    result_path = stage / "result.json"
    if result_path.is_file():
        source_report = read(result_path).get("source_report")
        if source_report:
            source = Path(source_report).resolve()
            if source.name != "report.json" or not source.is_relative_to(stage.resolve()):
                raise RuntimeError("appworld_external_baseline_source_outside_stage")
            # Keep the same nearest-first convention as compare's existing
            # grading projection. Native reuse verifies its judge fingerprint.
            for folder in (source.parent, *list(source.parents)[1:4]):
                if not folder.is_relative_to(stage.resolve()):
                    break
                config = folder.with_name(folder.name + ".regrade.config.json")
                if config.is_file():
                    return config
    return stage / "capture.config.json"


def prepare(seed_artifact: Path | None = None, *, max_rounds: int = 30,
            max_model_calls: int = 512) -> dict:
    artifact = require_seed_gates(Path(seed_artifact or PUBLISHED / "seed0/strategy.json"))
    recorded_job = _recorded_job(artifact)
    memory = _training_ready(recorded_job) if recorded_job else _training_ready()
    dataset = build_dataset(recorded_job) if recorded_job else build_dataset()
    _copy_training_configs()
    service = read(CONFIG / "service.json")
    service.update(application_id=AGENT, data_dir=str(WORKSPACE),
                   # Native service expects the source *store directory*;
                   # the immutable JSON file remains recorded in the plan.
                   memory_source_dir=str(TRAINING / "evolution"), runtime_config="runtime.json",
                   max_level="L3", workers=16, auto_evaluate=True,
                   auto_approve_experiences=True, evaluation_levels=["L3"])
    service["policy_evaluation_file"] = "agent-evaluation.json"
    service.setdefault("control_learning", {})["enabled"] = True
    service.setdefault("instrumentation", {})["level"] = "L3"
    service["instrumentation"]["capabilities"] = ["model", "actions", "loop"]
    service.setdefault("shadow_options", {}).update(
        require_validated_evidence=True, max_policy_entries=64,
        max_advice_items=2, max_advice_bytes=2048)
    service["sharing"] = dict(service.get("sharing") or {}, enabled=False)
    save(CONFIG / "service.json", service)

    baseline = read(PUBLISHED / "L0.json")
    evaluation = controller.evaluation_config(controller.ids("train"), baseline, 16)
    evaluation.pop("task_ids", None)
    evaluation.update(splits=["train", "val", "holdout"], parallelism=16,
                      max_trials=8000, timeout_ms=1800000)
    for variant in evaluation.get("variants", []):
        variant["instrumentation_config"] = str(CONFIG / "service.json")
        variant.setdefault("pass_env", [])
        if "APPWORLD_EVIDENCE_ROOT" not in variant["pass_env"]:
            variant["pass_env"].append("APPWORLD_EVIDENCE_ROOT")
    # Preserve the baseline's actual process limits for native same-contract
    # reuse. Explicit values also avoid the manager's lower fallback limits.
    baseline_evaluation = _baseline_evaluation_file(GEPA / "l0-current-stage500")
    if baseline_evaluation.is_file():
        source_resources = read(baseline_evaluation).get("resources")
        if source_resources is not None:
            evaluation["resources"] = copy.deepcopy(source_resources)
    # Judge identity includes the runtime file's original bytes. The copied
    # management JSON is semantically identical but may use other whitespace;
    # keep all task grades on the exact already-frozen runtime source.
    evaluation["judge"]["runtime_config"] = str(controller.RUNTIME / "runtime.json")
    evaluation["judge"]["enabled"] = True
    evaluation["judge"]["max_model_calls"] = 2048
    evaluation["artifact_retention"] = {
        "retain_trial_files": False, "retain_markdown": False,
        "retain_jsonl": False, "retain_report_pb": True,
        "drop_intermediate_pb": True,
    }
    save(CONFIG / "evaluation.json", evaluation)

    agent = artifact.get("agent") or {}
    save(CONFIG / "agent-evaluation.json", {
        "schema_version": 1, "dataset": os.path.relpath(DATASET, CONFIG),
        "evaluation_config": "evaluation.json", "agent": agent,
        "min_interval_ms": 0, "max_jobs": 256, "max_jobs_per_hour": 12,
        "training": {"enabled": False, "min_experiences": 1, "batch_size": 16,
                      "max_batches": 25},
        "gate": {"min_trials": 2, "memory_min_pairs": 2, "min_coverage": 1.0,
                 "max_quality_drop": 0.0, "max_pass_rate_drop": 0.0,
                 "min_invalid_loop_reduction": 0.15,
                 "resources_report_only": True},
        "gepa_config": "gepa.json",
    })
    base = read(ROOT / "tools/trajectory_evolution/config/gepa.json")
    base.update(runtime_config="runtime.json",
                prompt_file=str((ROOT / 'tools/trajectory_evolution/config/prompts/gepa-reflection.txt').resolve()),
                max_rounds=max_rounds, max_model_calls=max_model_calls,
                max_trials=8000, timeout_ms=50 * 3600000, minibatch_size=32,
                merge_minibatch_size=16, max_feedback_trials=8, patience=0,
                acceptance="pareto", reflection_strategy="combee",
                duplication_factor=1, holdout_top_k=3,
                holdout_max_report_bytes=2147483648,
                candidate_selection_strategy="pareto", frontier_type="instance")
    # A fresh end-to-end run has no external baseline yet: native GEPA then
    # evaluates it normally. An existing capture selects authoritative reuse,
    # including a pending result, and must never fall back to a duplicate run.
    baseline_dataset = TRAINING / "datasets/comparison500"
    if baseline_evaluation.is_file() and (baseline_dataset / "manifest.json").is_file():
        base.update(
            holdout_baseline_result_file=str(GEPA / "l0-current-stage500/result.json"),
            holdout_baseline_dataset=str(baseline_dataset),
            holdout_baseline_evaluation_file=str(baseline_evaluation))
    base.setdefault("objective", {}).update(invalid_loop_weight=0.2, token_weight=0,
                                              latency_weight=0, invalid_loop_scale=1,
                                              token_scale=10000, latency_scale_ms=60000)
    save(CONFIG / "gepa.json", base)
    plan = {
        "schema_version": 1, "stage": "train -> seed -> 32/100 gates -> GEPA -> top3 comparison500",
        "dataset": str(DATASET), "dataset_version": dataset.get("metadata", {}).get("dataset_version"),
        "split_counts": dataset.get("summary", {}).get("splits"),
        "source_memory": str(memory), "seed": str(seed_artifact or PUBLISHED / "seed0/strategy.json"),
        "search": {"pool": "official train", "validation": "canonical val.jsonl",
                   "final_holdout": "frozen comparison500", "max_rounds": max_rounds,
                   "max_model_calls": max_model_calls, "top_k": 3},
        "parallelism": 16, "prepared_at": datetime.now(timezone.utc).isoformat(),
    }
    if recorded_job:
        previous_plan = read(PLAN)
        receipt = previous_plan.get('training_readiness')
        if not isinstance(receipt, dict):
            lifecycle = read(GEPA / 'lifecycle.json')
            receipt = {'source': str(GEPA / 'lifecycle.json'),
                       'train_tasks': lifecycle.get('dataset', {}).get('split_counts', {}).get('train'),
                       'stage_result': lifecycle.get('stage_results', {}).get('train')}
        plan['training_readiness'] = receipt
    GEPA.mkdir(parents=True, exist_ok=True)
    save(PLAN, plan)
    if recorded_job:
        previous = read(STATUS) if STATUS.is_file() else {"status": "prepared"}
        save(STATUS, {**previous, **progress(recorded_job), "updated_at": plan["prepared_at"]})
    else:
        save(STATUS, {"status": "prepared", "phase": "gepa_pending", "updated_at": plan["prepared_at"]})
    return plan


def progress(job_id: str | None = None) -> dict:
    state_path = WORKSPACE / "agent-evaluation/state.json"
    if not state_path.is_file():
        return {"phase": "waiting_for_gepa", "nodes": 0}
    state = read(state_path)
    jobs = [(key, item) for key, item in (state.get("jobs") or {}).items()
            if item.get("user") == USER and item.get("level") == "L3"]
    if not jobs:
        return {"phase": "waiting_for_gepa", "nodes": 0}
    if job_id is None and (GEPA / "submission.json").is_file():
        job_id = read(GEPA / "submission.json").get("job_id")
    if job_id:
        selected = next((pair for pair in jobs if pair[0] == job_id), None)
        if selected is None:
            return {"phase": "gepa_checkpoint_unavailable", "job_id": job_id, "nodes": 0}
        job_id, job = selected
    else:
        job_id, job = max(jobs, key=lambda pair: pair[1].get("queued_ms", 0))
    result = {"job_id": job_id, "job_status": job.get("status"),
              "gepa": job.get("gepa", False)}
    tree = WORKSPACE / "agent-evaluation" / job_id / "search/tree.json"
    if tree.is_file():
        value = read(tree)
        result.update(nodes=len(value.get("nodes", [])), rounds=value.get("rounds", 0),
                      tree_status=value.get("status"), stop_reason=value.get("stop_reason"),
                      node_states=dict(Counter(item.get("status") for item in value.get("nodes", []))))
    return result


def run(seed_artifact: Path | None = None) -> dict:
    plan = read(PLAN)
    artifact_path = Path(seed_artifact or plan["seed"]).resolve()
    artifact = require_seed_gates(artifact_path)
    recorded_job = _recorded_job(artifact)
    env = controller.environment()
    env["TRAJECTORY_UNCONDITIONAL_RESUME"] = "1"
    env["APPWORLD_EVIDENCE_ROOT"] = str(GEPA / "official-evidence")
    env["EVOLUTION_APPWORLD_TOKEN"] = secrets.token_urlsafe(32)
    config = CONFIG / "service.json"
    with (GEPA / "learning.log").open("a") as log:
        process = subprocess.Popen([str(BINARY), "serve", "--config", str(config), "--no-auto-evaluate"],
                                   cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=log, text=True)
    try:
        ready, _, _ = select.select([process.stdout], [], [], 30)
        if not ready:
            raise RuntimeError("offline_gepa_management_start_timeout")
        line = process.stdout.readline().strip()
        prefix = "trajectory-integration.v1 listening=127.0.0.1:"
        if not line.startswith(prefix):
            raise RuntimeError("offline_gepa_management_start_failed")
        port = int(line[len(prefix):])
        def invoke(operation: str, **fields) -> dict:
            request = urllib.request.Request(f"http://127.0.0.1:{port}/v1/invoke",
                data=json.dumps({"operation": operation,
                                 "request_id": secrets.token_hex(16), **fields}).encode(),
                headers={"Content-Type": "application/json",
                         "Authorization": "Bearer " + env["EVOLUTION_APPWORLD_TOKEN"]})
            try:
                with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(request, timeout=30) as response:
                    response_value = json.load(response)
            except urllib.error.HTTPError as error:
                # Only native fixed error codes enter the diagnostic; never
                # echo response values, request evidence or authorization.
                import re
                code = 'management_http_' + str(error.code)
                try:
                    rejection = json.load(error)
                    value = rejection.get('error')
                    candidate = value.get('code') if isinstance(value, dict) else value
                    if isinstance(candidate, str) and re.fullmatch('[a-z][a-z0-9_]{0,127}', candidate):
                        code = candidate
                except (ValueError, OSError, TypeError, AttributeError):
                    pass
                raise RuntimeError('offline_gepa_' + operation.rsplit('.', 1)[-1] + '_failed:' + code) from None
            if response_value.get("ok") is not True:
                raise RuntimeError("offline_gepa_" + operation.rsplit(".", 1)[-1] + "_failed")
            return response_value.get("result") or {}

        if recorded_job:
            # Native restart already requeues a running checkpoint. Failed
            # terminal attempts need its explicit retry operation, never a
            # fresh submission whose service context may create another tree.
            current = invoke("agent_evaluation.status")
            job = (current.get("jobs") or {}).get(recorded_job)
            if not isinstance(job, dict):
                raise RuntimeError("offline_gepa_checkpoint_unavailable")
            if job.get("status") in {"error", "interrupted", "inconclusive", "cancelled", "superseded"}:
                invoke("agent_evaluation.retry", job_id=recorded_job)
            job_id = recorded_job
        else:
            submitted = invoke("agent_evaluation.submit", artifact=artifact)
            job_id = submitted.get("job_id")
            if not job_id:
                raise RuntimeError("offline_gepa_submit_failed")
            save(GEPA / "submission.json", submitted)
        while process.poll() is None:
            current = progress(job_id)
            save(STATUS, {"status": "running", **current,
                          "updated_at": datetime.now(timezone.utc).isoformat()})
            if current.get("job_status") in {"passed", "rejected", "error", "interrupted",
                                              "inconclusive", "cancelled"}:
                break
            time.sleep(5)
        result = progress(job_id)
        save(STATUS, {"status": "stopped", **result,
                      "updated_at": datetime.now(timezone.utc).isoformat()})
        return result
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()
        process.stdout.close()



def _final_baseline_items(rows: list[dict], report: dict, compare) -> list[dict]:
    """Resolve projected baseline receipts through their retained cost ledger."""
    references = [(row.get('evidence') or {}).get('source_reference') for row in rows]
    if not any(references):
        return [compare.compact_row(row, 'native-final-L0') for row in rows]
    if not all(isinstance(ref, dict) for ref in references):
        raise RuntimeError('gepa_final_baseline_source_mixed')
    descriptor = (report.get('summary') or {}).get('baseline_source') or {}
    result_file = descriptor.get('result_file')
    if not result_file:
        config = read(CONFIG / 'gepa.json')
        result_file = config.get('holdout_baseline_result_file')
    if not result_file:
        raise RuntimeError('gepa_final_baseline_source_missing')
    path = Path(result_file)
    if not path.is_absolute():
        path = CONFIG / path
    source = read(path)
    if source.get('status') != 'complete' or not source.get('source_report'):
        raise RuntimeError('gepa_final_baseline_source_incomplete')
    if source.get('judge_projection') != controller.judge_projection():
        raise RuntimeError('gepa_final_baseline_source_judge_changed')
    source_report = Path(source['source_report']).resolve()
    original_items = source.get('items') or []
    originals = {item.get('task_id'): item for item in original_items}
    if len(originals) != len(original_items):
        raise RuntimeError('gepa_final_baseline_source_scope_invalid')
    items = []
    for row, reference in zip(rows, references):
        original = originals.get(row.get('task_id'))
        if (not isinstance(original, dict)
                or Path(str(reference.get('report') or '')).resolve() != source_report
                or reference.get('trial_id') != row.get('trial_id')
                or original.get('trial_id') != row.get('trial_id')
                or reference.get('variant_id') != 'candidate'
                or reference.get('full_execution_evidence') != 'original_source'
                or original.get('verdict') != row.get('verdict')):
            raise RuntimeError('gepa_final_baseline_source_mismatch')
        # The native source validator has already checked full execution and
        # scores. Keep all original compact fields, including measured levels,
        # unknown failed attempts and retry costs; missing trace is never zero.
        item = copy.deepcopy(original)
        item['source_shard'] = 'native-final-L0'
        items.append(item)
    return items


def publish(*, proof_only: bool = False, source_tree: Path | None = None,
            prepare_only: bool = False) -> dict:
    """Build the frozen top-K proof, then optionally cross the publish boundary.

    The native service writes ``published-version.json`` only for an accepted
    search.  We copy that immutable artifact to the small published area and
    retain the native tree as the detailed provenance source.
    """
    pinned_tree = Path(source_tree).resolve() if source_tree is not None else None
    if pinned_tree is not None:
        native_root = (WORKSPACE / 'agent-evaluation').resolve()
        if (not pinned_tree.is_relative_to(native_root)
                or pinned_tree.relative_to(native_root).parts[1:] != ('search', 'tree.json')):
            raise RuntimeError('offline_gepa_publish_tree_invalid')
    current = progress(pinned_tree.parent.parent.name) if pinned_tree is not None else progress()
    job_id = current.get("job_id")
    if current.get("job_status") not in {"passed", "rejected"} or not job_id:
        raise RuntimeError("offline_gepa_not_passed")
    tree_path = WORKSPACE / "agent-evaluation" / job_id / "search/tree.json"
    bundle_path = tree_path.parent / "published-version.json"
    if not tree_path.is_file():
        raise RuntimeError("offline_gepa_publish_tree_missing")
    tree = read(tree_path)
    final_holdout = tree.get("final_holdout") or {}
    candidates = final_holdout.get("candidates") or []
    manifest = read(DATASET / "manifest.json") if (DATASET / "manifest.json").is_file() else {}
    holdout_count = int((manifest.get("summary") or {}).get("splits", {}).get("holdout", 0) or 0)
    if holdout_count <= 0:
        raise RuntimeError("offline_gepa_holdout_manifest_missing")
    # Native GEPA evaluates each final candidate against the frozen L0 in a
    # paired report (baseline + candidate), so ``trials`` is twice the task
    # count.  Checking the actual manifest count avoids accepting a partial
    # or candidate-only replay as a complete comparison500 proof.
    expected_final_trials = holdout_count * 2
    requested_top_k = final_holdout.get('requested_top_k')
    if (tree.get("status") != "completed" or final_holdout.get("status") != "completed"
            or isinstance(requested_top_k, bool)
            or requested_top_k not in (1, 2, 3)
            or not 1 <= len(candidates) <= requested_top_k
            or len({item.get('node') for item in candidates}) != len(candidates)
            or len({item.get('version') for item in candidates}) != len(candidates)
            or any(item.get('rank') != index + 1 for index, item in enumerate(candidates))
            or any(item.get("status") != "complete" or item.get("trials") != expected_final_trials
                   for item in candidates)):
        raise RuntimeError("offline_gepa_top3_holdout_incomplete")
    # Apply the same canonical strict/quality/loop contract as the pilots,
    # reporting resource costs separately. Native acceptance
    # alone only guarantees non-regression, not the user's +5% release goal.
    import compare
    import stage_gate
    proof_root = GEPA / 'final-comparison'
    def save_proof(path, value):
        if not prepare_only:
            save(path, value)
    canonical_ids = {row['task_id'] for row in (
        json.loads(line) for line in (DATASET / 'holdout.jsonl').read_text().splitlines() if line.strip())}
    if len(canonical_ids) != holdout_count:
        raise RuntimeError('gepa_final_dataset_scope_invalid')
    proofs = []
    baseline_rows = None
    for finalist in candidates:
        matches = [entry for entry in (tree.get('evaluations') or {}).values()
                   if entry.get('split') == 'holdout' and entry.get('status') == 'complete'
                   and entry.get('version') == finalist.get('version')]
        report = None
        for entry in reversed(matches):
            relative = Path(str(entry.get('report') or ''))
            source = (tree_path.parent / relative).resolve()
            if relative.is_absolute() or not source.is_relative_to(tree_path.parent.resolve()) or not source.is_file():
                continue
            loaded = read(source)
            if loaded.get('complete') is True:
                report = loaded
                break
        if report is None:
            raise RuntimeError(f"gepa_final_native_report_missing:{finalist.get('node')}")
        projection = compare._report_judge_projection(source)
        if projection != controller.judge_projection():
            raise RuntimeError('gepa_final_native_judge_profile_invalid')
        trials = report.get('trials') or []
        raw_l0 = [row for row in trials if row.get('variant_id') == 'baseline' and row.get('split') == 'holdout']
        raw_candidate = [row for row in trials if row.get('variant_id') == 'candidate' and row.get('split') == 'holdout']
        for rows in (raw_l0, raw_candidate):
            if len(rows) != holdout_count or {row.get('task_id') for row in rows} != canonical_ids:
                raise RuntimeError('gepa_final_same_task_coverage_invalid')
        l0_items = _final_baseline_items(raw_l0, report, compare)
        items = [compare.compact_row(row, f"native-final-{finalist.get('node')}") for row in raw_candidate]
        l0_by_task = {row['task_id']: row for row in l0_items}
        if baseline_rows is None:
            baseline_rows = l0_by_task
            historical_baseline = GEPA / 'l0-current-stage500/result.json'
            baseline_source = (read(historical_baseline)['source_report']
                               if historical_baseline.is_file() else str(source))
            save_proof(proof_root / 'L0.json', {'status': 'complete', 'items': l0_items,
                                          'source_report': baseline_source,
                                          'judge_projection': projection,
                                          'metrics': compare.summarize(l0_items, holdout_count)})
        else:
            fields = ('verdict', 'error_code', 'reward', 'quality', 'avg_invalid_loop', 'invalid_loop_basis',
                      'tokens', 'tokens_complete', 'model_calls', 'latency_ms',
                      'l1_applied', 'l2_applied', 'l2_action_applied', 'l2_schedule_applied', 'l3_applied')
            if any(any(l0_by_task[task].get(field) != baseline_rows[task].get(field) for field in fields)
                   for task in canonical_ids):
                raise RuntimeError('gepa_final_baseline_changed')
        artifact_path = tree_path.parent / 'versions' / f"{int(finalist['node'])}.json"
        artifact = read(artifact_path)
        if controller.version(artifact) != finalist.get('version'):
            raise RuntimeError('gepa_final_artifact_version_changed')
        progress_path = proof_root / f"node-{finalist.get('node')}-progress.json"
        save_proof(progress_path, {'shards': {'0': {'execution_complete': True, 'complete': True,
             'judge_projection': projection, 'source_report': str(source)}}})
        comparison = {'status': 'complete', 'seed': str(artifact_path), 'items': items,
                      'judge_projection': projection, 'progress': str(progress_path),
                      'metrics': {'candidate': compare.summarize(items, holdout_count)},
                      'scope': 'same_task_native_top3_comparison500',
                      'source_kind': 'native_gepa_final', 'source_tree': str(tree_path.resolve()),
                      'source_job': job_id, 'selected_node': finalist['node'],
                      'version': finalist['version'], 'source_report': str(source),
                      'source_l0': str((proof_root / 'L0.json').resolve())}
        passed, gate = stage_gate._verify_against_l0('comparison500', comparison, items, proof_root / 'L0.json')
        comparison_path = proof_root / f"node-{finalist.get('node')}.json"
        save_proof(comparison_path, comparison)
        proofs.append({'node': finalist.get('node'), 'version': finalist.get('version'),
                       'validation_rank': finalist.get('rank'), 'goal_passed': passed,
                       'gate': gate, 'artifact': str(artifact_path),
                       'comparison': str(comparison_path.resolve()), 'comparison_value': comparison})
    def rank(item):
        metrics = item['gate'].get('candidate') or {}
        def number(key, default=0):
            value = metrics.get(key)
            return float(value) if isinstance(value, (int, float)) else default
        return (item['goal_passed'], number('strict_passed'), number('quality'),
                -number('avg_invalid_loop', float('inf')),
                -number('tokens', float('inf')), -number('latency_ms', float('inf')))
    best = max(proofs, key=rank)
    artifact = read(Path(best['artifact']))
    if artifact.get('level') != 'L3':
        raise RuntimeError('gepa_best_final_artifact_invalid')
    native_bundle = read(bundle_path) if bundle_path.is_file() else {}
    native_gate_passed = (native_bundle.get('evaluation') or {}).get('gate', {}).get('passed') is True
    target = PUBLISHED / 'strategy/strategy.json'
    manifest = {
        'release_status': ('proof_ready' if proof_only else
                           ('published_goal_passed' if best['goal_passed'] else 'evaluation_only_best')),
        'goal_status': 'goal_passed' if best['goal_passed'] else 'goal_not_met',
        'goal_passed': best['goal_passed'], 'native_gate_passed': native_gate_passed,
        # Keep the selected tree artifact in the proof; only the final
        # publish call copies it into the small public directory.
        'proof_artifact': str(Path(best['artifact']).resolve()),
        'strategy': str(target) if not proof_only else None,
        'version': controller.version(artifact),
        'source_job': job_id, 'source_tree': str(tree_path), 'selected_node': best['node'],
        'selection': 'best observed among the complete same-task final candidates',
        'final_holdout_top_k': requested_top_k, 'evaluated_candidate_count': len(candidates),
        'final_holdout': final_holdout,
        'candidate_goal_checks': [{key: value for key, value in item.items()
                                   if key != 'comparison_value'} for item in proofs],
        'source_kind': 'native_gepa_final', 'source_comparison': best['comparison'],
        'source_strategy': str(Path(best['artifact']).resolve()),
        'metric_basis': 'business_verdict_objectives_quality_per_trial_resource_means',
        'model_calls': 0,
        'model_calls_scope': 'publication_operation_only',
    }
    if prepare_only:
        return {'artifact': artifact, 'comparison': best['comparison_value'], 'manifest': manifest}
    save(proof_root / "manifest.json", manifest)
    if proof_only:
        return manifest
    import reproduce
    manifest = reproduce.deliver_best_available(Path(best['artifact']), Path(best['comparison']))
    save(STATUS, {"status": "published", "publication": manifest,
                  "updated_at": datetime.now(timezone.utc).isoformat()})
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--publish", action="store_true")
    parser.add_argument("--proof", action="store_true",
                        help="run top-3 comparison and write proof only; do not publish")
    parser.add_argument("--status", action="store_true")
    parser.add_argument("--view", action="store_true", help="build/check GEPA dataset without model calls")
    parser.add_argument("--seed-artifact", type=Path)
    parser.add_argument("--max-rounds", type=int, default=30)
    parser.add_argument("--max-model-calls", type=int, default=512)
    args = parser.parse_args()
    if (args.prepare or args.run or args.publish or args.proof) and os.environ.get(
            "AGENTFLOW_SHARED_LIFECYCLE") != "1":
        parser.error("mutating AppWorld stages must be entered through reproduce.py shared lifecycle")
    if args.view:
        print(json.dumps(build_dataset(), ensure_ascii=False)); return 0
    if args.prepare:
        print(json.dumps(prepare(args.seed_artifact, max_rounds=args.max_rounds,
                                 max_model_calls=args.max_model_calls), ensure_ascii=False)); return 0
    if args.run:
        print(json.dumps(run(args.seed_artifact), ensure_ascii=False)); return 0
    if args.publish:
        print(json.dumps(publish(), ensure_ascii=False)); return 0
    if args.proof:
        print(json.dumps(publish(proof_only=True), ensure_ascii=False)); return 0
    print(json.dumps(progress(), ensure_ascii=False)); return 0


if __name__ == "__main__":
    raise SystemExit(main())
