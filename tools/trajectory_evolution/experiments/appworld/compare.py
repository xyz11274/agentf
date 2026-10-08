#!/usr/bin/env python3
"""Run a fixed AppWorld seed on comparison500 with compact, resumable evidence.

One native capture is retained per shard for interruption recovery and failure
review. Compact paired results reuse that execution; judge failures never
discard its trace or schedule healthy Agent tasks again.
"""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import nullcontext
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent
import importlib.util
_business_metrics_spec = importlib.util.spec_from_file_location(
    "_shared_business_metrics_compare", HERE.parent / "business-coverage/progress_report.py")
_business_metrics_module = importlib.util.module_from_spec(_business_metrics_spec)
_business_metrics_spec.loader.exec_module(_business_metrics_module)
business_metrics = _business_metrics_module
ROOT = HERE.parents[3]
import layout as c  # noqa: E402

MERGED = c.baseline_result(500)
DEFAULT_SEED = ROOT / "trace_output/appworld/published/seed0/strategy.json"
DEFAULT_OUT = ROOT / "trace_output/appworld/gepa/seed0-comparison500"
DATASET = ROOT / "trace_output/appworld/training/datasets/comparison500"


def save(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".new")
    tmp.write_text(json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n")
    tmp.replace(path)


def preserve_review_failure(destination: Path, label: str, report: Path,
                           config: Path, error: BaseException,
                           *, status: str = "regrade_unavailable",
                           reasons: list[str] | None = None) -> str:
    """Keep a bounded, replayable source when the judge phase fails.

    Agent execution evidence is otherwise held in a temporary directory. A
    judge outage must not erase the only trace that can be regraded later;
    successful shards remain compact and do not retain this duplicate.
    """
    target = destination / "review-failures" / label
    target.mkdir(parents=True, exist_ok=True)
    source = target / "report.json"
    durable = report.resolve().is_relative_to(destination.resolve())
    if durable:
        source = report.resolve()
    else:
        shutil.copyfile(report, source)
    # Native ``--regrade-report`` validates the source report against a
    # sibling manifest. Preserve it (or derive the minimal immutable form)
    # before the temporary capture directory is removed.
    manifest = report.parent / "manifest.json"
    if manifest.is_file():
        if not durable:
            shutil.copyfile(manifest, target / "manifest.json")
    else:
        raise RuntimeError('native_capture_manifest_missing')
    retained_config = config.resolve() if durable else target / 'capture.config.json'
    if not durable:
        shutil.copyfile(config, retained_config)
    detail = {
        "status": status,
        "error_type": type(error).__name__,
        "error": str(error)[:2000],
        "source_report": str(source),
        "capture_config": str(retained_config),
    }
    if reasons:
        detail["replay_reasons"] = sorted(set(str(reason) for reason in reasons))
    if isinstance(error, c.RegradeError):
        detail.update(returncode=error.returncode, stderr=error.stderr, stdout=error.stdout)
    save(target / "error.json", detail)
    return str(source)


def replay_reasons(row: dict, *, review_enabled: bool) -> list[str]:
    """Return reasons a completed capture must retain raw evidence for regrade.

    Native captures can contain a model check while the derived assessment or
    loop review is incomplete.  Such a shard is execution-complete, but its
    metrics are not final: dropping ``report.json`` would make a later judge
    retry impossible.  Keep this test independent of the compact row so it
    can distinguish missing evidence from a legitimate zero/failed objective.
    """
    if not review_enabled:
        return []
    reasons: list[str] = []
    evidence = row.get("evidence") or {}
    review_status = review_row_status(evidence.get("loop_review"))
    if review_status != "complete":
        reasons.append(f"loop_review:{review_status}")
    if c._judge_error_code(row) is not None:
        reasons.append("assessment:judge_error")
    elif c.assessed_quality(row) is None:
        # A missing/malformed objectives assessment is different from a
        # valid quality of 0.0.  Preserve the native report for a regrade.
        reasons.append("assessment:missing_or_unmeasurable")
    return reasons


def review_row_status(review: object) -> str:
    """Return the row-level review status without treating malformed complete as complete."""
    if not isinstance(review, dict):
        return "missing"
    status = str(review.get("status") or "").strip().lower()
    if c.review_measurable(review):
        return "complete"
    if status in c._VALID_LOOP_STATUSES:
        return "uncertain"
    if status in {"uncertain", "not_reviewed", "unknown"}:
        return "uncertain"
    if status in {"error", "failed", "invalid"}:
        return "error"
    return status or "other"


def review_distribution_complete(distribution: dict, expected: int) -> bool:
    """Return true only when every retained execution has a final judge review."""
    if expected < 0 or not isinstance(distribution, dict):
        return False
    return (expected > 0 and distribution.get("complete", 0) == expected
            and all(count == 0 for key, count in distribution.items() if key != "complete"))


def candidate_review_complete(items: list[dict]) -> bool:
    return bool(items) and all(
        item.get('review_status') == 'complete' and not item.get('error_code')
        and c._number(item.get('quality')) is not None for item in items)


def _preexecution_capture_failure(row: dict) -> bool:
    """Identify a wrapper failure proven to run no Agent code.

    A failed evaluation-entry wrapper can terminate before the Agent starts
    (for example exit 137 while constructing the capture boundary).  Such a
    row has no metrics, no output, and explicit proof of zero observed runs.
    Keep it in the audit trail and count the retry attempt, but do not turn
    the absent pre-execution cost into an unknown Agent resource value once a
    later native retry supplies a complete execution.
    """
    evidence = row.get("evidence") or {}
    failure = evidence.get("failure_capture") or {}
    runner_error = str(evidence.get("runner_error") or "")
    return bool(
        row.get("execution_status") == "error"
        and not isinstance(row.get("metrics"), dict)
        and not isinstance(evidence.get("output"), dict)
        and failure.get("complete") is False
        and isinstance(failure.get("observed_runs"), (int, float))
        and not isinstance(failure.get("observed_runs"), bool)
        and failure.get("observed_runs") == 0
        and evidence.get("exit_code") in (137, 137.0, -9, -9.0)
        # Only the capture-boundary failure is provably pre-execution.  A
        # handler failure is raised after the native receiver has started and
        # may have already run Agent steps; its missing metrics therefore make
        # the execution cost unknown rather than zero/exempt.
        and runner_error.startswith("evaluation_entry_capture_")
    )


trace_level_counts = c.trace_level_counts


def compact_row(row: dict, source: str) -> dict:
    output = c.output(row)
    evidence = row.get("evidence") or {}
    single_metrics = c.metrics([row])
    judge_error = c._judge_error_code(row)
    error = c._trial_error_code(row)
    execution_error = c._execution_error_code(row)
    # Formal scores use the same native verdict and error contract as
    # business. The official sandbox reward remains independent auxiliary
    # evidence when a judge failure makes the formal result unavailable.
    quality = c.assessed_quality(row)
    loop_review = evidence.get("loop_review") if isinstance(evidence.get("loop_review"), dict) else {}
    observed_iterations = loop_review.get("observed_iterations")
    loop_basis = single_metrics.get("loop_basis")
    # The host semantic counter is a diagnostic lower bound.  Formal loop
    # accounting must use the business-compatible model review only.
    waste_count = single_metrics.get("avg_invalid_loop") if loop_basis == "model_review" else None
    trace_levels, trace_levels_observed = trace_level_counts(evidence)
    return {
        "task_id": row.get("task_id"),
        "group_id": row.get("group_id"),
        "trial_id": row.get("trial_id"),
        "execution_status": row.get("execution_status"),
        "verdict": row.get("verdict"),
        "error_code": error,
        "execution_error_code": execution_error,
        "judge_error_code": judge_error,
        "error_type": output.get("error_type"),
        "error_detail": output.get("error_detail"),
        "reward": output.get("appworld_reward"),
        "quality_score": output.get("quality_score"),
        "official_quality": output.get("quality_score"),
        "quality": quality,
        "quality_status": single_metrics.get("quality_status"),
        "strict_passed": single_metrics.get("strict_passed"),
        "official_scored": single_metrics.get("official_scored", 0) == 1,
        "known_agent_tokens": output.get("known_agent_tokens"),
        "tokens": single_metrics.get("tokens"),
        "tokens_complete": single_metrics.get("tokens_complete", False),
        "model_calls": single_metrics.get("model_calls"),
        "latency_ms": single_metrics.get("latency_ms"),
        "tool_error_count": output.get("tool_error_count"),
        "repeated_code_count": output.get("repeated_code_count"),
        # Formal loop scores use the same model-review contract as business.
        # The host semantic counter is retained in the source row as a
        # deterministic lower-bound diagnostic and never substitutes for it.
        "invalid_loop_count": (single_metrics.get("avg_invalid_loop")
                                if loop_basis == "model_review" else None),
        "loop_observed_iterations": observed_iterations,
        "loop_waste_count": waste_count,
        "loop_keep_count": (float(observed_iterations) - float(waste_count)
                             if isinstance(observed_iterations, (int, float))
                             and isinstance(waste_count, (int, float)) else None),
        "invalid_loop_basis": "model_review" if loop_basis == "model_review" else "model_review_unavailable",
        "avg_invalid_loop": single_metrics.get("avg_invalid_loop"),
        "intent_f1": single_metrics.get("intent_f1"),
        "intent_counts": single_metrics.get("intent_counts"),
        "intent_measured": single_metrics.get("intent_measured", 0),
        "intent_task_measured": single_metrics.get("intent_task_measured", 0),
        "intent_turn_keys": single_metrics.get("intent_turn_keys", []),
        "l1_applied": trace_levels["L1"] if trace_levels_observed else None,
        "l2_applied": trace_levels["L2"] if trace_levels_observed else None,
        "l2_action_applied": trace_levels["L2_action"] if trace_levels_observed else None,
        "l2_schedule_applied": trace_levels["L2_schedule"] if trace_levels_observed else None,
        "l3_applied": trace_levels["L3"] if trace_levels_observed else None,
        "level_count_basis": "trace advice.applied by boundary" if trace_levels_observed else "unavailable",
        "loaded_version": evidence.get("loaded_version"),
        "source_shard": source,
    }


def _grade_only_cost_manifest(path: Path, reference: dict, dataset_version) -> dict | None:
    """Read an existing native lineage node whose report was cleaned up.

    A complete native manifest can prove that every job reused or regraded an
    existing trial. It contributes no Agent attempt; its native source link
    still has to lead to actual execution rows before costs become complete.
    """
    value = c.read(path)
    expected_sha = reference.get('report_sha256')
    if (value.get('complete') is not True or
            value.get('run_id') != reference.get('run_id') or
            not isinstance(dataset_version, str) or not dataset_version or
            not isinstance(expected_sha, str) or len(expected_sha) != 64 or
            (value.get('files') or {}).get('report.json') != expected_sha or
            value.get('dataset_version') != dataset_version):
        return None
    summary = value.get('summary') or {}
    counts = [summary.get(key) for key in
              ('reused_trials', 'regraded_trials', 'planned_trials', 'not_started_trials')]
    if any(isinstance(number, bool) or not isinstance(number, (int, float)) or
           not math.isfinite(number) or number < 0 or int(number) != number
           for number in counts):
        return None
    reused, regraded, planned, not_started = counts
    parent = summary.get('resume_source') or {}
    if (planned <= 0 or reused + regraded != planned or not_started != 0 or
            summary.get('agent_reexecuted') is True or not parent.get('run_id') or
            not isinstance(parent.get('report_sha256'), str) or
            len(parent['report_sha256']) != 64):
        return None
    return value


def execution_cost_rows(items: list[dict], report_path: Path, root: Path, *,
                        source_roots: list[str] | None = None,
                        native_report: dict | None = None) -> list[dict]:
    """Count real executions along native resume lineage, never judge retries.

    This changes only compact results. Native trial IDs identify an execution;
    reused/regraded rows keep that ID and therefore incur no second Agent cost.
    An unavailable ancestor or incomplete attempt leaves resource totals unknown.
    """
    report_path, root = Path(report_path), Path(root)
    final = c.read(report_path) if native_report is None else native_report
    reports = {str(final.get('run_id') or ''): final}
    report_paths = {str(final.get('run_id') or ''): report_path}
    manifest_nodes = set()
    candidates = set(root.glob('capture*/report.json'))
    candidates.update(root.glob('regrade/**/report.json'))
    candidates.update(root.glob('review-failures/**/report.json'))
    if root.name.startswith('shard-'):
        candidates.update((root.parent.parent / 'review-failures' / root.name).glob('**/report.json'))
    manifest_roots = [root / 'regrade', root / 'review-failures']
    if root.name.startswith('shard-'):
        manifest_roots.append(root.parent.parent / 'review-failures' / root.name)
    candidates.update(path for folder in manifest_roots for path in folder.glob('**/manifest.json')
                      if not path.with_name('report.json').is_file())
    # Larger baseline stages can resume the actual smaller-stage executions.
    if root.name.startswith('l0-'):
        for stage in root.parent.glob('l0-*-stage*'):
            candidates.update(stage.glob('capture*/report.json'))
            candidates.update(stage.glob('regrade/**/report.json'))
    # Interrupted native runs have atomic trials and started.json, but no
    # final report. Their real attempt costs remain in the resume lineage.
    candidates.update(path for path in root.glob('capture*/started.json')
                      if not path.with_name('report.json').is_file())
    # Cross-stage imports retain references to whole native sources. Their
    # original resume ancestors stay in the owning stage, including a pilot
    # shard inherited through a directory link; never turn that missing local
    # copy into a zero-cost execution.
    for original in source_roots or []:
        owner = Path(original)
        for folder in dict.fromkeys((owner, owner.resolve())):
            candidates.update(folder.glob('capture*/report.json'))
            candidates.update(folder.glob('regrade/**/report.json'))
            candidates.update(path for path in folder.glob('capture*/started.json')
                              if not path.with_name('report.json').is_file())
            reviews = folder.parent.parent / 'review-failures' / folder.name
            candidates.update(reviews.glob('**/report.json'))
            candidates.update(path for base in (folder / 'regrade', reviews)
                              for path in base.glob('**/manifest.json')
                              if not path.with_name('report.json').is_file())
    chain, seen, complete = [], set(), True
    unrecorded_attempts = set()
    lineage_hints = {}
    def candidate_order(path, parent):
        # Existing tiny manifests identify promising parents without parsing
        # unrelated reports. A hint only changes ordering: absent/wrong hints
        # retain the original full-body fallback and all cost validation.
        if path not in lineage_hints:
            hint = path if path.name in ('manifest.json', 'started.json') else path.with_name('manifest.json')
            try:
                lineage_hints[path] = c.read(hint).get('run_id') if hint.is_file() else None
            except (OSError, ValueError, TypeError, AttributeError):
                lineage_hints[path] = None
        return (path.name != 'started.json', lineage_hints[path] != parent, str(path))
    current = final
    while True:
        run_id = str(current.get('run_id') or '')
        if run_id and run_id in seen:
            complete = False
            break
        seen.add(run_id)
        chain.append(current)
        unrecorded_attempts.update(current.get('_unrecorded_attempt_tasks') or [])
        if current.get('_unrecorded_attempt_scope_complete') is False:
            # A storage failure can remove finished work before its atomic
            # trial is written. Unobserved attempts keep run totals unknown.
            complete = False
        parent_reference = ((current.get('summary') or {}).get('resume_source') or
                            (current.get('configuration') or {}).get('execution_source') or {})
        parent = parent_reference.get('run_id')
        if not parent:
            break
        if parent not in reports:
            # Interrupted checkpoints are local and identify their run in a
            # tiny started sidecar. Avoid reading unrelated large baseline
            # reports before reaching the actual partial ancestor.
            for path in sorted(candidates, key=lambda path: candidate_order(path, parent)):
                if path.name == 'manifest.json':
                    value = _grade_only_cost_manifest(path, parent_reference,
                                                      final.get('dataset_version'))
                    if value is None:
                        continue
                    manifest_nodes.add(value['run_id'])
                else:
                    value = _partial_cost_source(path) if path.name == 'started.json' else c.read(path)
                candidates.remove(path)
                if value.get('run_id'):
                    reports[value['run_id']] = value
                    report_paths[value['run_id']] = path
                if parent in reports:
                    break
        if parent not in reports:
            complete = False
            break
        if run_id in manifest_nodes:
            source_path = report_paths[parent]
            source_manifest = (reports[parent] if source_path.name == 'manifest.json' else
                               c.read(source_path.with_name('manifest.json')))
            if (source_manifest.get('complete') is not True or
                    source_manifest.get('run_id') != parent or
                    source_manifest.get('dataset_version') != final.get('dataset_version') or
                    (source_manifest.get('files') or {}).get('report.json') !=
                    parent_reference.get('report_sha256')):
                complete = False
                break
        current = reports[parent]
    attempts = {}
    for native in reversed(chain):
        for row in native.get('trials') or []:
            task, trial = row.get('task_id'), row.get('trial_id')
            if task and trial:
                # A degraded capture can be followed by a complete native
                # retry and then a judge-only regrade with the same trial id.
                # Keep the newest row in the resume lineage so its
                # authoritative metrics are not masked by the first failed
                # capture.  The existing checkpoint's execution_attempts is
                # retained below for transparent retry accounting.
                attempts.setdefault(task, {})[trial] = row
    final_ids = {row.get('task_id'): row.get('trial_id') for row in final.get('trials') or []}
    result = []
    for original in items:
        item = dict(original)
        task = item.get('task_id')
        native_rows = list(attempts.get(task, {}).values())
        # A native resume can legitimately assign a fresh trial_id to the
        # rerun while preserving the task_id.  Bind the final compact row to
        # any authoritative row for that task in the resume lineage; requiring
        # the stale pre-retry id would mark valid token/call evidence unknown.
        native_trial_ids = {row.get('trial_id') for row in native_rows}
        bound = bool(native_rows) and final_ids.get(task) in native_trial_ids
        exempt = [row for row in native_rows if _preexecution_capture_failure(row)]
        costs = [compact_row(row, item.get('source_shard', 'capture'))
                 for row in native_rows if row not in exempt]
        lineage_complete = complete and bound and task not in unrecorded_attempts
        # Rebuild every canonical compact field from the authoritative native
        # source before applying retry-cost aggregation.  Resource totals for
        # a resumed task are derived below and must therefore win over the
        # single-trial values in ``current``.
        if costs:
            current = next((cost for cost in costs
                            if cost.get('trial_id') == item.get('trial_id')), costs[-1])
            item.update(current)
        # Refresh existing audit markers as well when a prior checkpoint had
        # recorded an incomplete single-trial lineage and the authoritative
        # report is now complete.  Bare compact fixtures without these fields
        # keep their minimal shape for callers that only need metric rows.
        refresh_accounting = (len(native_rows) > 1 or not lineage_complete or
                              any(key in item for key in (
                                  'execution_attempts', 'execution_costs_complete',
                                  'retry_loop_complete')))
        if refresh_accounting:
            item['execution_attempts'] = max(
                int(item.get('execution_attempts', 1) or 1),
                len(native_rows) + int(task in unrecorded_attempts))
            item['execution_cost_exemptions'] = len(exempt)
            item['execution_costs_complete'] = bool(costs) and lineage_complete and all(
                cost.get('tokens_complete') is True and
                all(c._number(cost.get(key)) is not None
                    for key in ('latency_ms', 'tokens', 'model_calls')) for cost in costs)
            item['retry_loop_complete'] = lineage_complete and all(
                cost.get('invalid_loop_basis') == 'model_review' and
                c._number(cost.get('avg_invalid_loop')) is not None for cost in costs)
            for key in ('latency_ms', 'tokens', 'model_calls'):
                values = [c._number(cost.get(key)) for cost in costs]
                known = sum(value for value in values if value is not None)
                item[key] = known if lineage_complete and values and all(
                    value is not None for value in values) else None
                if key == 'latency_ms':
                    item['known_execution_latency_ms'] = known
            item['known_agent_tokens'] = sum(
                c._number(cost.get('known_agent_tokens')) or 0 for cost in costs)
            item['tokens_complete'] = item['execution_costs_complete']
        result.append(item)
    return result


def summarize(items: list[dict], planned: int) -> dict:
    """Aggregate compact evidence through the single AppWorld/business path."""
    return business_metrics.compact_metrics(items, planned)


def refresh_progress_costs(progress: dict, output: Path) -> dict:
    """Refresh derived costs from existing native reports without executing anything."""
    for index, state in progress.get('shards', {}).items():
        if state.get('execution_complete') is not True and state.get('complete') is not True:
            continue
        label = state.get('label') or f'shard-{int(index):02d}'
        checkpoint = output / 'shards' / f'{label}.json'
        saved = c.read(checkpoint) if checkpoint.is_file() else {}
        source = state.get('source_report') or saved.get('source_report')
        if not source:
            continue
        selected = saved.get('task_ids') or [task for task, row in progress.get('items', {}).items()
                                           if row.get('source_shard') == label]
        rows = [progress['items'][task] for task in selected]
        rows = execution_cost_rows(rows, Path(source), output / 'shards' / label,
                                   source_roots=state.get('source_cost_roots'))
        progress['items'].update({row['task_id']: row for row in rows})
        if saved:
            saved['items'] = rows
            save(checkpoint, saved)
    return progress


def summarize_existing(path: Path) -> dict:
    """Recompute compact cost accounting only; no Agent, judge or native command."""
    path = path.resolve()
    value = c.read(path)
    if value.get('source_report'):
        value['items'] = execution_cost_rows(value['items'], Path(value['source_report']), path.parent)
        value['metrics'] = summarize(value['items'], len(value['items']))
        value['model_calls'] = value['metrics']['total_model_calls']
    else:
        baseline = summarize_existing(Path(value['source_l0']))
        progress_path = Path(value['progress'])
        progress = refresh_progress_costs(c.read(progress_path), path.parent)
        value['items'] = [progress['items'][row['task_id']] for row in value['items']]
        baseline_rows = {row['task_id']: row for row in baseline['items']}
        planned = progress.get('planned_tasks', len(value['items']))
        for key in value['metrics']:
            rows = ([baseline_rows[row['task_id']] for row in value['items']]
                    if key == 'L0_reused' else value['items'])
            value['metrics'][key] = summarize(rows, planned)
        save(progress_path, progress)
    save(path, value)
    return value


def _require_current_host_version(value: dict, source: Path) -> None:
    """Reject an L0 source produced by a different executable host revision."""
    expected = c.host_version()
    actual = value.get('host_version')
    if actual != expected:
        raise RuntimeError(
            f"l0_source_host_version_mismatch:{source}:{actual or 'missing'}:{expected}")


def _report_variant_version(report: dict) -> str | None:
    configuration = report.get('configuration') if isinstance(report, dict) else None
    variants = configuration.get('variants') if isinstance(configuration, dict) else None
    if not isinstance(variants, list):
        return None
    versions = {item.get('version') for item in variants
                if isinstance(item, dict) and isinstance(item.get('version'), str)}
    return versions.pop() if len(versions) == 1 else None


def _archive_incompatible_baseline(output: Path, actual: str | None) -> None:
    """Keep old evidence readable while forcing a fresh baseline capture."""
    label = re.sub(r'[^A-Za-z0-9_.-]+', '_', actual or 'missing')
    for name in ('result.json', 'capture.config.json', 'capture', 'regrade'):
        source = output / name
        if not source.exists():
            continue
        target = output / f'{name}-legacy-{label}'
        suffix = 1
        while target.exists():
            suffix += 1
            target = output / f'{name}-legacy-{label}-{suffix:02d}'
        source.rename(target)


def _report_judge_projection(source: Path, fallback: dict | None = None) -> dict | None:
    """Read existing grading configuration; never invent a signature."""
    # A native GEPA source must validate its original context even if an
    # unrelated nearby config exists. Never downgrade a failed native check
    # into the ordinary sidecar/fallback path.
    if Path(source).resolve().is_relative_to((c.GEPA / 'workspace/agent-evaluation').resolve()):
        return _native_gepa_judge_projection(source)
    for folder in (source.parent, *list(source.parents)[1:4]):
        if not folder.name:
            continue
        config = folder.with_name(folder.name + '.regrade.config.json')
        if config.is_file():
            return c.judge_projection(c.read(config))
        # Native capture recovery keeps the immutable shard configuration in
        # its parent instead of creating a judge-only configuration sibling.
        # Read that existing source configuration; a missing one stays unknown.
        config = folder / 'config.json'
        if config.is_file():
            return c.judge_projection(c.read(config))
    return fallback


def _native_gepa_evaluation_context(config_path: Path, dataset_version: str) -> str:
    """Verify the existing native context; never persist a new fingerprint."""
    config = c.read(config_path)
    def contents(value):
        path = Path(value)
        return (path if path.is_absolute() else config_path.parent / path).read_bytes()
    value = config_path.read_bytes() + dataset_version.encode()
    for variant in config.get('variants') or []:
        if variant.get('instrumentation_config'):
            value += contents(variant['instrumentation_config'])
    judge = config.get('judge') or {}
    if judge.get('enabled'):
        if judge.get('goal_prompt'):
            value += contents(judge['goal_prompt'])
        value += contents(judge['prompt'])
        if judge.get('loop_review_prompt'):
            value += contents(judge['loop_review_prompt'])
        value += contents(judge['runtime_config'])
        if judge.get('consistency_prompt'):
            value += b'\njudge-consistency-v1\n' + contents(judge['consistency_prompt'])
    environment = c.environment()
    for name in ('OPENAI_BASE_URL', 'OPENAI_MODEL'):
        if name in environment:
            value += (name + '=' + environment[name] + '\n').encode()
    value += b'\nturn-acceptance-v2:judge-contract-v3:action-requests-v1'
    if judge.get('loop_review_prompt') and judge.get('consistency_prompt'):
        value += b'\nloop-consistency-v2:separate-goal-loop-reviews'
    if judge.get('assessment_mode', 'legacy') == 'objectives':
        value += b'\ngoal-assessment-v1:judge-summary-consistency-v1'
    prefix = (b'gepa-baseline-evaluation-v1:boundary-current-request-v1:verified-controls-v1:'
              b'failure-replan-v2:skill-schedule-v2:control-outcomes-v5:aggressive-controls-v1:'
              b'retrieval-evidence-v4\n')
    return hashlib.sha256(prefix + value).hexdigest()


def _native_gepa_judge_projection(source: Path) -> dict | None:
    """Read a final native source only after its original bindings validate."""
    try:
        import reproduce
        source = Path(source).resolve()
        workspace = (c.GEPA / 'workspace/agent-evaluation').resolve()
        relative = source.relative_to(workspace)
        parts = relative.parts
        if (len(parts) != 6 or parts[1:3] != ('search', 'evaluations')
                or not parts[4].startswith('attempt-') or parts[5] != 'report.json'):
            return None
        search = workspace / parts[0] / 'search'
        tree = c.read(search / 'tree.json')
        entry = (tree.get('evaluations') or {}).get(parts[3]) or {}
        header = reproduce._baseline_native_header(source) or {}
        manifest = c.read(source.with_name('manifest.json'))
        configuration = header.get('configuration') or {}
        config_path = c.GEPA / 'runtime/evaluation.json'
        context = _native_gepa_evaluation_context(config_path, tree['dataset_version'])
        candidate = next((item for item in configuration.get('variants') or []
                          if item.get('id') == 'candidate'), {})
        node = next(item for item in tree.get('nodes') or []
                    if item.get('version') == entry.get('version'))
        artifact = c.read(search / 'versions' / f"{int(node['id'])}.json")
        if (tree.get('status') != 'completed' or entry.get('status') != 'complete'
                or entry.get('split') != 'holdout'
                or (search / entry.get('report', '')).resolve() != source
                or context != tree.get('evaluation_context')
                or context != entry.get('evaluation_context')
                or entry.get('candidate_evaluation_context') != tree.get('candidate_evaluation_context')
                or candidate.get('version') != entry.get('version')
                or c.version(artifact) != entry.get('version')
                or header.get('complete') is not True or manifest.get('complete') is not True
                or not header.get('run_id') or manifest.get('run_id') != header.get('run_id')
                or header.get('dataset_version') != tree.get('dataset_version')
                or manifest.get('dataset_version') != tree.get('dataset_version')
                or not entry.get('report_hash')
                or (manifest.get('files') or {}).get('report.json') != entry.get('report_hash')
                or configuration.get('model_enabled') is not True
                or configuration.get('allow_unreviewed') is not False
                or not configuration.get('judge_fingerprint')):
            return None
        digest = hashlib.sha256()
        with source.open('rb') as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b''):
                digest.update(block)
        if digest.hexdigest() != entry['report_hash']:
            return None
        config = c.read(config_path)
        projection = c.judge_projection(config)
        # The L0 grade owns the frozen judge identity; the current config's
        # context above proves it is the original config, not a new fallback.
        baseline_path = c.GEPA / 'l0-current-stage500/result.json'
        if baseline_path.is_file():
            baseline = c.read(baseline_path)
            baseline_source = Path(baseline.get('source_report') or '')
            baseline_header = reproduce._baseline_native_header(baseline_source) or {}
            if (baseline.get('status') != 'complete'
                    or baseline.get('judge_projection') != projection
                    or _report_judge_projection(baseline_source) != projection
                    or (baseline_header.get('configuration') or {}).get('judge_fingerprint')
                       != configuration['judge_fingerprint']):
                return None
        else:
            # A from-scratch run grades L0 in the first native paired final
            # report. It has no historical compact L0 file; the same checked
            # context and full report then prove both variants' judge profile.
            config_file = c.GEPA / 'runtime/gepa.json'
            native_config = c.read(config_file) if config_file.is_file() else {}
            if (native_config.get('holdout_baseline_result_file')
                    or (header.get('summary') or {}).get('baseline_source', {}).get('result_file')
                    or next((item.get('version') for item in configuration.get('variants') or []
                             if item.get('id') == 'baseline'), None) != tree.get('baseline_version')):
                return None
        return projection
    except (OSError, ValueError, TypeError, KeyError, RuntimeError, StopIteration):
        return None


def inherit_candidate_source(source: Path, output: Path, task_ids: list[str],
                             artifact: dict) -> None:
    """Reference authentic closed prefix shards in a new comparison scope.

    Only compact checkpoints/configuration are written here. Original native
    reports, trial IDs, execution traces and all cost ancestors remain owned
    by the source stage; the ordinary runner schedules the uncovered suffix.
    """
    from training import same_capture
    source = Path(source).resolve()
    owner = source.parent
    if owner == output.resolve():
        raise RuntimeError('comparison_candidate_source_same_output')
    if (output / 'progress.json').is_file():
        previous = c.read(output / 'progress.json')
        if previous.get('resume_candidate_source') != str(source):
            raise RuntimeError('comparison_candidate_source_changed')
        return
    if output.exists() and next(output.iterdir(), None) is not None:
        raise RuntimeError('comparison_candidate_source_output_not_empty')
    comparison, progress = c.read(source), c.read(owner / 'progress.json')
    _require_current_host_version(comparison, source)
    selected_ids = sorted(row.get('task_id') for row in comparison.get('items', []))
    if (comparison.get('status') != 'complete' or not selected_ids
            or len(selected_ids) != len(set(selected_ids))
            or selected_ids != task_ids[:len(selected_ids)]
            or len(selected_ids) >= len(task_ids)
            or progress.get('planned_tasks') != len(selected_ids)
            or progress.get('artifact') != artifact):
        raise RuntimeError('comparison_candidate_source_scope_or_artifact_changed')
    tasks = {task['task_id']: task for task in
             (json.loads(line) for line in (DATASET / 'holdout.jsonl').read_text().splitlines()
              if line.strip())}
    dataset_version = (c.read(DATASET / 'manifest.json').get('metadata') or {}).get('dataset_version')
    if not dataset_version:
        raise RuntimeError('comparison_candidate_source_dataset_missing')
    observed_items = {row['task_id']: row for row in comparison['items']}
    prepared, covered = [], []
    for index, path in enumerate(sorted((owner / 'shards').glob('shard-[0-9][0-9].json'))):
        label = f'shard-{index:02d}'
        saved = c.read(path)
        original_root = owner / 'shards' / label
        config = c.read(original_root / 'config.json')
        variants = [v for v in config.get('variants', []) if v.get('id') == 'candidate']
        chosen = saved.get('task_ids') or []
        report_path = Path(saved.get('source_report') or '')
        if (path.stem != label or saved.get('artifact') != artifact
                or saved.get('execution_complete') is not True
                or not chosen or len(chosen) != len(set(chosen))
                or chosen != selected_ids[len(covered):len(covered) + len(chosen)]
                or len(variants) != 1 or variants[0].get('artifact') != artifact
                or variants[0].get('command') != [str(c.PYTHON), '-B', str(c.HOST), '--run-entry']
                or not report_path.is_file()
                or not report_path.with_name('manifest.json').is_file()
                or c.read(report_path.with_name('manifest.json')).get('complete') is not True):
            raise RuntimeError('comparison_candidate_source_shard_invalid')
        native = c.read(report_path)
        capture_path = original_root / 'capture/report.json'
        if (not capture_path.is_file() or not capture_path.with_name('manifest.json').is_file()
                or c.read(capture_path.with_name('manifest.json')).get('complete') is not True):
            raise RuntimeError('comparison_candidate_source_capture_missing')
        capture = c.read(capture_path)
        rows = native.get('trials') or []
        if (native.get('complete') is not True or capture.get('complete') is not True
                or native.get('dataset_version') != dataset_version
                or len(rows) != len(chosen) or {r.get('task_id') for r in rows} != set(chosen)
                or not same_capture(capture, native, chosen)
                or any(not c.framework_capture_complete(r) or c._execution_error_code(r)
                       or r.get('task_version') != tasks.get(r.get('task_id'), {}).get('task_version')
                       or r.get('variant_id') != 'candidate'
                       or (r.get('evidence') or {}).get('loaded_version') != variants[0].get('version')
                       for r in rows)):
            raise RuntimeError('comparison_candidate_source_native_identity_changed')
        roots = list(dict.fromkeys([str(original_root),
                                    *(saved.get('source_cost_roots') or [])]))
        items = execution_cost_rows([compact_row(r, label) for r in rows], report_path,
                                    original_root, source_roots=roots)
        score_keys = ('trial_id', 'execution_status', 'verdict', 'error_code', 'quality',
                      'strict_passed', 'avg_invalid_loop', 'invalid_loop_basis', 'loaded_version')
        if any(any(item.get(key) != observed_items[item['task_id']].get(key)
                   for key in score_keys) for item in items):
            raise RuntimeError('comparison_candidate_source_compact_score_changed')
        for item in items:
            status = review_row_status(next(r for r in rows if r['task_id'] == item['task_id'])
                                       .get('evidence', {}).get('loop_review'))
            item.update(review_status=status, loop_review_status=status,
                        loop_review_source=str(report_path))
        checkpoint = dict(saved, items=items, source_report=str(report_path),
                          source_cost_roots=roots, source_checkpoint=str(path))
        prepared.append((label, config, capture_path.parent.resolve(), checkpoint))
        covered.extend(chosen)
    if covered != selected_ids:
        raise RuntimeError('comparison_candidate_source_prefix_incomplete')
    # All validation finishes before creating target evidence references.
    for label, config, capture, checkpoint in prepared:
        folder = output / 'shards' / label
        folder.mkdir(parents=True, exist_ok=True)
        save(folder / 'config.json', config)
        (folder / 'capture').symlink_to(capture, target_is_directory=True)
        save(output / 'shards' / f'{label}.json', checkpoint)
    save(output / 'progress.json', dict(progress, resume_candidate_source=str(source)))



def retained_candidate_shards(progress: dict, task_ids: list[str], shard_dir: Path,
                              artifact: dict, shard_size: int) -> list[list[str]] | None:
    """Keep verified saved prefix partitions, then schedule only the new suffix."""
    previous_count = progress.get('planned_tasks')
    if (not isinstance(previous_count, int) or previous_count > len(task_ids)
            or previous_count < 1):
        return None
    if isinstance(progress.get('artifact'), dict) and progress['artifact'] != artifact:
        return None
    partitions, covered = [], []
    paths = sorted(shard_dir.glob('shard-[0-9][0-9].json'))
    for index, path in enumerate(paths):
        if path.stem != f'shard-{index:02d}':
            break
        try:
            saved = c.read(path)
        except (OSError, ValueError, TypeError):
            break
        if not (saved.get('execution_complete') is True or saved.get('complete') is True):
            break
        items = saved.get('items') or []
        selected = saved.get('task_ids') or [row.get('task_id') for row in items]
        if (not isinstance(selected, list) or not selected
                or len(set(selected)) != len(selected)
                or {row.get('task_id') for row in items} != set(selected)
                or len(items) != len(selected)
                or task_ids[len(covered):len(covered) + len(selected)] != selected):
            return None
        partitions.append(selected)
        covered.extend(selected)
    if not covered:
        return None
    # Reuse the same native artifact/config/loaded-version proof as ordinary
    # recovery. A partition cannot authorize any score or execution by itself.
    verified = recover_candidate_progress(progress, partitions, shard_dir, artifact)
    if set(verified.get('items', {})) != set(covered):
        return None
    remaining = task_ids[len(covered):]
    return partitions + [remaining[i:i + shard_size] for i in range(0, len(remaining), shard_size)]


def recover_candidate_progress(progress: dict, shards: list[list[str]], shard_dir: Path,
                               artifact: dict) -> dict:
    """Resume matching candidate evidence; schedule unknown/different rows again.

    The strategy object is already a published artifact, not a new signature.
    Legacy rows may prove their strategy using their native loaded version or
    retained original capture config. Missing proof never blocks a resumed run.
    """
    current_version = None

    def same_strategy(saved: dict, items: list[dict], config_path: Path) -> bool:
        nonlocal current_version
        for name in ('artifact', 'seed_artifact'):
            previous = saved.get(name)
            if isinstance(previous, dict):
                return previous == artifact
        if config_path.is_file():
            try:
                original = json.loads(config_path.read_text())
                for variant in original.get('variants', []):
                    if (isinstance(variant, dict) and variant.get('id') == 'candidate'
                            and isinstance(variant.get('artifact'), dict)):
                        return variant['artifact'] == artifact
            except (OSError, ValueError, TypeError):
                pass
        loaded = [item.get('loaded_version') for item in items]
        if not loaded or not all(isinstance(value, str) and value for value in loaded):
            return False
        if current_version is None:
            try:
                current_version = c.version(artifact)
            except Exception:
                return False
        return all(value == current_version for value in loaded)

    restored = dict(progress, shards={}, items={}, artifact=artifact)
    old_items = progress.get('items') if isinstance(progress.get('items'), dict) else {}
    old_shards = progress.get('shards') if isinstance(progress.get('shards'), dict) else {}
    for index, selected in enumerate(shards):
        label = f'shard-{index:02d}'
        candidates = []
        checkpoint = shard_dir / f'{label}.json'
        if checkpoint.is_file():
            try:
                candidates.append(json.loads(checkpoint.read_text()))
            except (OSError, ValueError, TypeError):
                pass
        previous = old_shards.get(str(index)) or {}
        if isinstance(previous, dict) and (previous.get('execution_complete') is True
                                            or previous.get('complete') is True):
            candidates.append(dict(progress, complete=review_distribution_complete(
                                       previous.get('review_distribution', {}), len(selected)),
                                   execution_complete=True,
                                   items=[old_items[task] for task in selected if task in old_items],
                                   review_distribution=previous.get('review_distribution', {}),
                                   source_report=previous.get('source_report'),
                                   source_cost_roots=previous.get('source_cost_roots'),
                                   judge_projection=previous.get('judge_projection')))
        for saved in candidates:
            if not isinstance(saved, dict):
                continue
            items = saved.get('items')
            if not isinstance(items, list) or any(not isinstance(item, dict) for item in items):
                continue
            task_ids = [item.get('task_id') for item in items]
            execution_complete = (saved.get('execution_complete') is True
                                  or saved.get('complete') is True)
            if (not execution_complete or len(task_ids) != len(selected)
                    or not all(isinstance(task, str) and task for task in task_ids)
                    or len(set(task_ids)) != len(selected) or set(task_ids) != set(selected)):
                continue
            config_path = shard_dir.parent / 'review-failures' / label / 'capture.config.json'
            if not same_strategy(saved, items, config_path):
                continue
            projection = saved.get('judge_projection')
            if not isinstance(projection, dict):
                actual_config = config_path if config_path.is_file() else shard_dir / label / 'config.json'
                projection = c.judge_projection(c.read(actual_config)) if actual_config.is_file() else None
            review_distribution = saved.get('review_distribution', {})
            restored['shards'][str(index)] = {
                'label': label, 'task_count': len(selected),
                'complete': (review_distribution_complete(review_distribution, len(selected))
                             and candidate_review_complete(items)),
                'execution_complete': True,
                'updated_at': time.time(),
                'scored': sum(not item.get('error_code') and c._number(item.get('quality')) is not None
                              for item in items),
                'review_distribution': review_distribution,
                'judge_projection': projection,
                'source_report': saved.get('source_report'),
                'source_cost_roots': saved.get('source_cost_roots') or [],
            }
            restored['items'].update({item['task_id']: item for item in items})
            break
    return restored


def recover_candidate_reviews(progress: dict, shards: list[list[str]], output: Path,
                              artifact: dict, env: dict) -> dict:
    """Repair retained judge gaps without scheduling another Agent execution."""
    for index, selected in enumerate(shards):
        state = progress.get('shards', {}).get(str(index))
        if not state or (state.get('execution_complete') is not True
                         and state.get('complete') is not True):
            continue
        items = [progress['items'][task] for task in selected]
        projection_changed = state.get('judge_projection') != c.judge_projection()
        if not projection_changed and candidate_review_complete(items):
            state.update(execution_complete=True, complete=True)
            continue
        label = f'shard-{index:02d}'
        retained = output / 'review-failures' / label
        source, config = retained / 'report.json', retained / 'capture.config.json'
        if state.get('source_report'):
            source = Path(state['source_report'])
        if (retained / 'error.json').is_file():
            detail = c.read(retained / 'error.json')
            if not source.is_file():
                source = Path(detail.get('source_report') or source)
            config = Path(detail.get('capture_config') or config)
        if not source.is_file():
            source = output / 'shards' / label / 'capture/report.json'
        if not config.is_file():
            config = output / 'shards' / label / 'config.json'
        if not source.is_file() or not config.is_file():
            raise RuntimeError(f'{label}_retained_judge_evidence_missing')
        # _review_rows bounds recovery within this invocation and preserves
        # healthy judge rows. Historical failed directories cannot exhaust
        # every future normal resume after a local repair or profile change.
        retry_root = retained / 'regrade'
        original = c.read(source)
        if {row.get('task_id') for row in original.get('trials', [])} != set(selected):
            raise RuntimeError(f'{label}_retained_judge_scope_changed')
        effective = c.read(config)
        effective.setdefault('judge', {}).update(c.judge_projection())
        c.save(config, effective)
        try:
            rows, reviewed_source = _review_rows(
                original['trials'], report_path=source, dataset=DATASET,
                config_path=config, output_root=retained / 'regrade', env=env,
                return_source=True, source_projection=state.get('judge_projection'))
        except Exception as error:
            recoverable = isinstance(error, c.RegradeError) or (
                isinstance(error, RuntimeError) and str(error) in {
                    'comparison_judge_recovery_unavailable',
                    'comparison_review_judge_unavailable'})
            if not recoverable:
                raise
            state.update(execution_complete=True, complete=False,
                         review_error=type(error).__name__)
            save(retained / 'error.json', {
                'status': 'regrade_unavailable', 'error_type': type(error).__name__,
                'error': str(error)[:2000], 'source_report': str(source),
                'capture_config': str(config)})
            save(output / 'progress.json', progress)
            continue
        distribution = {}
        repaired = []
        for row in rows:
            item = compact_row(row, label)
            status = review_row_status((row.get('evidence') or {}).get('loop_review'))
            distribution[status] = distribution.get(status, 0) + 1
            item.update(review_status=status, loop_review_status=status,
                        loop_review_source=str(reviewed_source))
            repaired.append(item)
        repaired = execution_cost_rows(repaired, reviewed_source, output / 'shards' / label,
                                       source_roots=state.get('source_cost_roots'))
        review_complete = (review_distribution_complete(distribution, len(selected))
                           and candidate_review_complete(repaired))
        state.update(review_distribution=distribution,
                     judge_projection=c.judge_projection(), source_report=str(reviewed_source),
                     execution_complete=True, complete=review_complete,
                     scored=sum(not item.get('error_code') and
                                c._number(item.get('quality')) is not None for item in repaired))
        progress['items'].update({item['task_id']: item for item in repaired})
        save(output / 'shards' / f'{label}.json', {
            'label': label, 'complete': review_complete, 'execution_complete': True,
            'task_ids': sorted(selected),
            'items': repaired, 'review_distribution': distribution, 'artifact': artifact,
            'judge_projection': c.judge_projection(), 'source_report': str(reviewed_source)})
        if state.get('source_cost_roots'):
            saved_checkpoint = c.read(output / 'shards' / f'{label}.json')
            saved_checkpoint['source_cost_roots'] = state['source_cost_roots']
            save(output / 'shards' / f'{label}.json', saved_checkpoint)
        save(output / 'progress.json', progress)
    return progress


def comparison_main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--seed", type=Path, default=DEFAULT_SEED)
    parser.add_argument("--l0-source", type=Path, default=MERGED,
                        help="500-row L0 report used only for task ids and final comparison")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--resume-candidate-source", type=Path,
                        help="reuse authentic closed prefix shards from this comparison.json")
    parser.add_argument("--shard-size", type=int, default=32)
    parser.add_argument("--workers", type=int, default=16, help="total concurrent Agent executions across shards")
    parser.add_argument("--max-shards", type=int, default=0)
    parser.add_argument("--task-count", type=int, default=0,
                        help="run only the first sorted tasks for a bounded development comparison")
    parser.add_argument("--task-id", action="append", default=[],
                        help="run explicit task ids; repeat for a retry subset")
    args = parser.parse_args()
    if args.shard_size <= 0 or args.workers <= 0:
        raise SystemExit("shard-size and workers must be positive")
    args.output = args.output.resolve()
    seed_path = args.seed.resolve()
    l0_source = args.l0_source.resolve()
    merged = json.loads(l0_source.read_text(encoding="utf-8"))
    # The baseline writer owns this compact result. Reading or refreshing its
    # native cost chain here can overwrite a concurrently completed regrade
    # with the old snapshot, and delays otherwise independent Agent work.
    _require_current_host_version(merged, l0_source)
    l0_rows = {row["task_id"]: row for row in merged.get("items", [])}
    dataset_ids = {
        json.loads(line)["task_id"] for line in
        (DATASET / "holdout.jsonl").read_text(encoding="utf-8").splitlines()
        if line.strip()
    }
    # The production source is the immutable 500-row L0 report. A bounded
    # 32/100 pilot may instead reuse a same-task L0 report that already has
    # formal model-review loop evidence; never allow that shortcut for a full
    # scope run or for IDs outside the frozen comparison500 dataset.
    bounded_source = bool(args.task_count or args.task_id)
    if dataset_ids != set(l0_rows):
        if not bounded_source or not set(l0_rows).issubset(dataset_ids):
            raise RuntimeError(
                f"comparison500_task_set_mismatch:dataset={len(dataset_ids)}:l0={len(l0_rows)}"
            )
        if len(l0_rows) < 1:
            raise RuntimeError("bounded_l0_source_empty")
    seed = json.loads(seed_path.read_text(encoding="utf-8"))
    seed_metric_key = "seed_" + seed_path.parent.name.replace("-", "_")
    task_ids = sorted(l0_rows)
    if args.task_id:
        missing = [task_id for task_id in args.task_id if task_id not in l0_rows]
        if missing:
            raise RuntimeError("l0_task_missing:" + ",".join(missing))
        task_ids = list(dict.fromkeys(args.task_id))
    elif args.task_count:
        if args.task_count <= 0 or args.task_count > len(task_ids):
            raise SystemExit("task-count must be between 1 and the L0 task count")
        task_ids = task_ids[:args.task_count]
    shards = [task_ids[i:i + args.shard_size] for i in range(0, len(task_ids), args.shard_size)]
    if args.max_shards:
        shards = shards[:args.max_shards]
    shard_parallelism = min(2, len(shards))
    shard_workers = max(1, args.workers // shard_parallelism)
    progress_path = args.output / "progress.json"
    if args.resume_candidate_source:
        if args.max_shards:
            raise RuntimeError('comparison_candidate_source_max_shards_unsupported')
        inherit_candidate_source(args.resume_candidate_source, args.output, task_ids, seed)
    progress = json.loads(progress_path.read_text()) if progress_path.is_file() else {
        "scope": "same_task_seed_comparison500_vs_reused_l0", "seed": str(seed_path),
        "planned_tasks": len(task_ids), "workers_per_shard": shard_workers,
        "total_parallelism": shard_workers * shard_parallelism, "shards": {}, "items": {},
    }
    shard_dir = args.output / "shards"
    shard_dir.mkdir(parents=True, exist_ok=True)
    retained = (None if args.max_shards else
                retained_candidate_shards(progress, task_ids, shard_dir, seed, args.shard_size))
    if progress.get("planned_tasks") not in (None, len(task_ids)) and retained is None:
        raise RuntimeError(f"checkpoint_task_count_mismatch:{progress.get('planned_tasks')}:{len(task_ids)}")
    if retained is not None:
        shards = retained
        shard_parallelism = min(2, len(shards))
        shard_workers = max(1, args.workers // shard_parallelism)
        progress.update(planned_tasks=len(task_ids), workers_per_shard=shard_workers,
                        total_parallelism=shard_workers * shard_parallelism)
    # Both worker checkpoints and parent progress need the same provenance.
    # A changed strategy only invalidates its candidate rows; L0 stays reused.
    progress = recover_candidate_progress(progress, shards, shard_dir, seed)
    progress['seed'] = str(seed_path)
    save(progress_path, progress)
    env = c.environment()
    env.update(TRAJECTORY_EVALUATION_PARALLELISM=str(shard_workers))
    progress = recover_candidate_reviews(progress, shards, args.output, seed, env)

    def run_shard(index: int, selected: list[str]):
        label = f"shard-{index:02d}"
        retained_root = shard_dir / label
        retained_root.mkdir(parents=True, exist_ok=True)
        with nullcontext(retained_root) as td:
            root = Path(td).resolve()
            config = c.evaluation_config(selected, seed, shard_workers)
            config.update(splits=["holdout"], task_ids=selected, parallelism=shard_workers,
                          max_trials=len(selected), timeout_ms=1_800_000,
                          max_report_bytes=(c.FULL_SCOPE_REPORT_BYTES
                                            if len(task_ids) > 100 else 268_435_456))
            config_path = root / "config.json"
            capture_projection = c.judge_projection(config)
            if config_path.is_file():
                previous = c.read(config_path)
                capture_projection = c.judge_projection(previous)
                if previous.get('variants') != config.get('variants'):
                    # A different candidate needs a different execution;
                    # retain the actual previous evidence rather than mix it.
                    number = 1
                    previous_root = root.with_name(label + f'-previous-{number:02d}')
                    while previous_root.exists():
                        number += 1
                        previous_root = root.with_name(label + f'-previous-{number:02d}')
                    root.rename(previous_root)
                    root.mkdir(parents=True)
                    capture_projection = c.judge_projection(config)
            c.save(config_path, config)
            local_env = dict(env, TRAJECTORY_EVIDENCE_ROOT=str(root / "official-evidence"),
                             APPWORLD_EVIDENCE_ROOT=str(root / "official-evidence"))
            capture_dir = root / "capture"
            capture_report = capture_dir / "report.json"
            capture_manifest = capture_dir / "manifest.json"
            report = None
            if capture_report.is_file() and capture_manifest.is_file():
                try:
                    retained = c.read(capture_report)
                    retained_ids = {row.get('task_id') for row in retained.get('trials') or []}
                    if (retained.get('complete') is True and
                            c.read(capture_manifest).get('complete') is True and
                            len(retained_ids) == len(selected) and
                            retained_ids == set(selected)):
                        # Judge retries must reuse the immutable Agent execution.
                        report = retained
                except (OSError, ValueError, TypeError):
                    report = None
            if report is None:
                report = c.shared.capture(DATASET, config_path, capture_dir, local_env)
            reviewed_projection = capture_projection
            authoritative_source = root / "capture" / "report.json"
            report, authoritative_source = recover_baseline_capture(
                report, authoritative_source, dataset=DATASET, config_path=config_path,
                output_root=root, env=local_env)
            rows = report.get("trials", [])
            # Agent execution and model loop review are separate phases. The
            # review reuses this saved report and never runs the host again.
            # A judge outage must not discard valid execution results.
            review_status = "disabled"
            review_source = None
            review_error: BaseException | None = None
            review_distribution = {
                "complete": 0, "uncertain": 0, "error": 0,
                "missing": len(rows), "other": 0,
            }
            review_enabled = os.environ.get("APPWORLD_LOOP_REVIEW", "1") != "0"
            # Capture already has the business-style judge enabled.  Do not
            # spend a second full judge pass (and create a second, divergent
            # verdict) when the native rows already contain model checks.
            # Regrade only legacy/disabled captures that have no model check.
            has_model_check = lambda item: any(
                isinstance(check, dict) and check.get("check_id") == "model"
                for check in (item.get("checks") or []))
            # A native model check is not sufficient evidence: the same
            # capture can contain an objective transport failure or an
            # incomplete loop_review.  Regrade those rows from the saved
            # report before compacting so every task gets a chance to obtain
            # formal model-review quality and loop metrics without rerunning
            # the Agent/environment.
            needs_regrade = review_enabled and (
                capture_projection != c.judge_projection(config)
                or
                any(not has_model_check(item) for item in rows)
                or any(replay_reasons(item, review_enabled=True) for item in rows)
            )
            if review_enabled and not needs_regrade:
                review_distribution = {"complete": 0, "uncertain": 0, "error": 0,
                                       "missing": 0, "other": 0}
                for item in rows:
                    review = (item.get("evidence") or {}).get("loop_review")
                    row_status = review_row_status(review)
                    review_distribution[row_status] = review_distribution.get(row_status, 0) + 1
                review_status = "complete" if review_distribution.get("complete", 0) == len(rows) else "partial"
            elif review_enabled:
                try:
                    reviewed_rows, reviewed_source = _review_rows(
                        rows, report_path=authoritative_source, dataset=DATASET,
                        config_path=config_path, output_root=root / 'regrade',
                        env=local_env, return_source=True, source_projection=capture_projection)
                    reviewed = dict(report, trials=reviewed_rows)
                    # Native regrade owns verdict/error/assessment together.
                    # Accept its complete rows only after proving that execution,
                    # costs and task scope still match the retained capture.
                    report = _validated_regrade(report, reviewed)
                    authoritative_source = reviewed_source
                    rows = report.get("trials", [])
                    reviewed_projection = c.judge_projection(config)
                    review_distribution = {"complete": 0, "uncertain": 0, "error": 0,
                                           "missing": 0, "other": 0}
                    for row in rows:
                        status = review_row_status((row.get("evidence") or {}).get("loop_review"))
                        review_distribution[status] = review_distribution.get(status, 0) + 1
                    review_status = ("complete" if review_distribution.get("complete", 0) == len(rows)
                                     else "partial")
                except Exception as error:
                    review_status = f"unavailable:{type(error).__name__}"
                    review_error = error
                    review_distribution = {
                        "complete": 0, "uncertain": 0, "error": 0,
                        "missing": 0, "other": 0,
                    }
                    review_distribution["error"] = len(rows)
            # A successful native capture still needs replayable evidence when
            # any row has an incomplete loop review or failed/missing
            # objectives assessment.  The temporary directory is removed on
            # leaving this context, so preserve the report/config before that
            # happens.  This also covers native model-check rows: a model
            # check's presence alone does not prove its derived fields are
            # complete.
            if review_enabled:
                preserve_reasons: list[str] = []
                for row in rows:
                    preserve_reasons.extend(replay_reasons(row, review_enabled=True))
                if preserve_reasons and review_source is None:
                    preserve_error = review_error or RuntimeError(review_status)
                    try:
                        review_source = preserve_review_failure(
                            args.output, label, authoritative_source,
                            config_path, preserve_error,
                            status="regrade_required", reasons=preserve_reasons)
                    except Exception as preserve_failure:
                        review_status += f":preserve_failed:{type(preserve_failure).__name__}"
            items = execution_cost_rows([compact_row(row, label) for row in rows],
                                        authoritative_source, root)
            for row, item in zip(rows, items):
                review = (row.get("evidence") or {}).get("loop_review")
                item_status = review_row_status(review) if os.environ.get("APPWORLD_LOOP_REVIEW", "1") != "0" else "disabled"
                if item_status == "missing" and review_status.startswith("unavailable:"):
                    item_status = "error"
                item["review_status"] = item_status
                item["loop_review_status"] = item_status
                item["loop_review_batch_status"] = review_status
                if review_source:
                    item["loop_review_source"] = review_source
            expected = set(selected)
            actual = [item.get("task_id") for item in items]
            if (report.get("complete") is not True or len(actual) != len(expected)
                    or len(set(actual)) != len(expected) or set(actual) != expected):
                raise RuntimeError(
                    f"{label}_task_coverage_invalid:expected={len(expected)}:actual={len(actual)}"
                )
            execution_complete = True
            review_complete = (review_distribution_complete(review_distribution, len(expected))
                               and candidate_review_complete(items))
            checkpoint = args.output / "shards" / f"{label}.json"
            save(checkpoint, {"label": label, "complete": review_complete,
                              "execution_complete": execution_complete,
                              "task_ids": sorted(expected), "items": items,
                              "review_distribution": review_distribution,
                              "host_version": c.host_version(), "seed": str(seed_path),
                              "artifact": seed, "judge_projection": reviewed_projection,
                              "source_report": str(authoritative_source)})
            return (label, items, review_complete, review_distribution,
                    reviewed_projection, str(authoritative_source))

    # Existing executions have already had one bounded recovery pass above.
    # A partial judge result must not send that shard through run_shard again.
    pending = [(i, selected) for i, selected in enumerate(shards)
               if progress.get("shards", {}).get(str(i), {}).get("execution_complete") is not True]
    active_parallelism = min(2, len(pending), args.workers)
    if active_parallelism:
        shard_workers = max(1, args.workers // active_parallelism)
        env['TRAJECTORY_EVALUATION_PARALLELISM'] = str(shard_workers)
    progress.update(workers_per_shard=shard_workers,
                    total_parallelism=shard_workers * active_parallelism)
    save(progress_path, progress)
    with ThreadPoolExecutor(max_workers=max(1, active_parallelism)) as pool:
        futures = {pool.submit(run_shard, i, selected): i for i, selected in pending}
        for future in as_completed(futures):
            index = futures[future]
            label, items, complete, review_distribution, reviewed_projection, reviewed_source = future.result()
            expected = set(shards[index])
            actual = [item.get("task_id") for item in items]
            if (len(actual) != len(expected) or
                    len(set(actual)) != len(expected) or set(actual) != expected):
                raise RuntimeError(
                    f"{label}_task_coverage_invalid_after_run:expected={len(expected)}:actual={len(actual)}"
                )
            progress.setdefault("shards", {})[str(index)] = {
                "label": label, "task_count": len(shards[index]), "complete": complete,
                "execution_complete": True,
                "updated_at": time.time(), "scored": sum(
                    not x.get("error_code") and c._number(x.get("quality")) is not None for x in items),
                "review_distribution": review_distribution,
                "judge_projection": reviewed_projection, "source_report": reviewed_source,
            }
            for item in items:
                progress.setdefault("items", {})[item["task_id"]] = item
            save(progress_path, progress)

    progress = refresh_progress_costs(progress, args.output)
    save(progress_path, progress)
    items = [progress["items"][task_id] for task_id in task_ids if task_id in progress.get("items", {})]
    if len(items) != len(set(task_ids)) or len({item.get("task_id") for item in items}) != len(items):
        raise RuntimeError(f"comparison500_partial_or_duplicate:{len(items)}/{len(task_ids)}")
    metrics = summarize(items, len(task_ids))
    baseline, baseline_ready, profiles_ready = comparison_baseline_state(
        l0_source, task_ids, progress)
    result_metrics = {seed_metric_key: metrics}
    if baseline_ready and profiles_ready:
        current_l0 = {row['task_id']: row for row in baseline['items']}
        result_metrics['L0_reused'] = summarize(
            [current_l0[task_id] for task_id in task_ids], len(task_ids))
    review_distribution = {}
    for item in items:
        status = item.get("review_status", "missing")
        review_distribution[status] = review_distribution.get(status, 0) + 1
    result = {"scope": "same_task_seed_subset_vs_reused_l0", "status": "complete" if baseline_ready and profiles_ready and metrics["quality_complete"] and metrics["invalid_loop_status"] == "complete" else "partial",
              "host_version": c.host_version(), "seed": str(seed_path), "source_l0": str(l0_source),
              "baseline_status": "complete" if baseline_ready else "awaiting_l0",
              "judge_profile_status": "complete" if profiles_ready else "awaiting_current_review",
              "baseline_source_report": baseline.get('source_report'),
              "judge_projection": c.judge_projection(), "metrics": result_metrics,
              "review_distribution": review_distribution,
              "items": items, "progress": str(progress_path), "scores_used_for_training_or_selection": False}
    save(args.output / "comparison.json", result)
    print(json.dumps({"status": result["status"], "completed": len(items), "metrics": result["metrics"]}, ensure_ascii=False))
    return 0


def comparison_baseline_state(source: Path, task_ids: list[str], progress: dict) -> tuple[dict, bool, bool]:
    """Read the final baseline without changing its authoritative result.

    A retained candidate may finish while L0 is being regraded. Keep that
    execution as partial evidence until the same ordinary command can finalize
    against complete grades under the shared projection.
    """
    baseline = c.read(source)
    _require_current_host_version(baseline, source)
    rows = baseline.get('items') or []
    identities = [row.get('task_id') for row in rows]
    if len(identities) != len(set(identities)) or not set(task_ids).issubset(identities):
        raise RuntimeError('comparison_l0_final_task_coverage_changed')
    wanted = c.judge_projection()
    baseline_profile_current = baseline.get('judge_projection') == wanted
    if baseline.get('source_report'):
        report = Path(baseline['source_report'])
        manifest = report.with_name('manifest.json')
        observed = _report_judge_projection(report)
        baseline_profile_current = (baseline_profile_current and observed == wanted
                                    and report.is_file() and manifest.is_file()
                                    and c.read(manifest).get('complete') is True)
    baseline_ready = baseline.get('status') == 'complete' and baseline_profile_current
    states = list((progress.get('shards') or {}).values())
    profiles_ready = (baseline_profile_current and bool(states)
                      and all(state.get('judge_projection') == wanted for state in states))
    return baseline, baseline_ready, profiles_ready


def _validated_regrade(original: dict, reviewed: dict) -> dict:
    """Use native grades whole, while keeping execution and costs immutable."""
    from training import same_capture
    task_ids = [row.get("task_id") for row in original.get("trials") or []]
    if not same_capture(original, reviewed, task_ids):
        raise RuntimeError("comparison_regrade_changed_execution_or_scope")
    return reviewed


def _review_rows(rows: list[dict], *, report_path: Path, dataset: Path,
                 config_path: Path, output_root: Path, env: dict,
                 return_source: bool = False,
                 source_projection: dict | None = None) -> list[dict] | tuple[list[dict], Path]:
    """Reuse native grades and retry only failed judging of saved executions.

    Every derived report must keep the original scope, execution and costs.
    Incomplete attempts remain available, but never block a later recovery.
    """
    original = {"trials": rows}
    wanted_projection = c.judge_projection(c.read(config_path)) if config_path.is_file() else None
    projection = _report_judge_projection(report_path, source_projection)
    # Minimal fixtures with no configured projection preserve their generic
    # contract. A real configured profile with unknown provenance is regraded.
    if projection is None and not any((wanted_projection or {}).values()):
        projection = wanted_projection

    def has_model_check(row):
        return any(isinstance(check, dict) and check.get("check_id") == "model"
                   for check in row.get("checks") or [])

    def needs_review(row):
        return not has_model_check(row) or bool(replay_reasons(row, review_enabled=True))

    # Initialization can leave a skeleton with no evidence to retain.
    if (output_root.is_dir() and not (output_root / 'report.json').exists()
            and not (output_root / 'manifest.json').exists()
            and not any((output_root / 'trials').glob('*.json'))
            and not any(output_root.glob('retry*/report.json'))
            and not any((output_root / 'artifacts').rglob('*.*'))):
        shutil.rmtree(output_root)

    def retry_number(path):
        suffix = path.name.removeprefix('retry').lstrip('-')
        return int(suffix) if suffix.isdigit() else 0

    previous = sorted((path for path in output_root.glob('retry*')
                       if path.is_dir() and (path.name == 'retry'
                       or path.name.removeprefix('retry-').isdigit())), key=retry_number)
    report, source = original, report_path
    # A retained shard can already have a complete native review from the
    # original controller. Recovery writes under review-failures, so its own
    # output directory alone cannot discover that immutable healthy pair.
    retained_root = report_path.parent.parent
    retained_review = retained_root / 'regrade' / 'report.json'
    if retained_review.is_file() and retained_review != report_path:
        from training import complete_native_review
        native_source = c.read(report_path)
        selected = complete_native_review(
            native_source, retained_root,
            [row.get('task_id') for row in original['trials']],
            source_compatible=lambda path: _report_judge_projection(
                path, source_projection) == wanted_projection,
            prefer_earliest=True)
        if selected is not None:
            reviewed, reviewed_path = selected
            retained_projection = _report_judge_projection(reviewed_path, source_projection)
            if retained_projection == wanted_projection:
                report = _validated_regrade(original, reviewed)
                source, projection = reviewed_path, retained_projection
                if not any(needs_review(row) for row in report['trials']):
                    return (report['trials'], source) if return_source else report['trials']
    for target in [output_root, *previous]:
        replay = target / 'report.json'
        manifest = target / 'manifest.json'
        if (not replay.is_file() or not manifest.is_file()
                or c.read(manifest).get('complete') is not True):
            continue
        cached_projection = _report_judge_projection(replay, source_projection)
        if cached_projection is None and not any((wanted_projection or {}).values()):
            cached_projection = wanted_projection
        if cached_projection != wanted_projection:
            continue
        # A historical recovery directory can contain a rerun of a failed
        # task with a new trial_id.  It is valid audit evidence, but it is not
        # a judge-only regrade of this capture and must never replace the
        # original execution.  Skip it and continue with the last compatible
        # report; the old directory remains available for diagnosis.
        cached = c.read(replay)
        try:
            candidate = _validated_regrade(original, cached)
        except RuntimeError as error:
            if str(error) != 'comparison_regrade_changed_execution_or_scope':
                raise
            original_ids = {row.get('task_id'): row.get('trial_id') for row in original['trials']}
            cached_ids = {row.get('task_id'): row.get('trial_id') for row in cached.get('trials', [])}
            if original_ids.keys() == cached_ids.keys() and original_ids != cached_ids:
                continue
            raise
        report = candidate
        source = replay
        projection = cached_projection
    number = max((retry_number(path) for path in previous), default=0) + 1

    def next_target():
        nonlocal number
        if not output_root.exists():
            return output_root
        target = output_root / f'retry-{number:02d}'
        number += 1
        return target

    if projection == wanted_projection and not any(needs_review(row) for row in report['trials']):
        return (report['trials'], source) if return_source else report['trials']
    # Missing model checks or a non-error unmeasurable review require a full
    # regrade. A valid model check with a judge failure can resume directly.
    requires_regrade = lambda current: any(
        not has_model_check(row)
        or (needs_review(row) and c._judge_error_code(row) is None)
        for row in current['trials'])
    if projection != wanted_projection or requires_regrade(report):
        target = next_target()
        reviewed = c.regrade_report(source, dataset, config_path, target, env=env)
        report = _validated_regrade(original, reviewed)
        source = target / 'report.json'
        projection = wanted_projection

    for retry_index in range(2):
        if not any(needs_review(row) for row in report['trials']):
            break
        if any('evaluation_judge_http_429' in str(check.get('reason') or '')
               for row in report['trials'] for check in row.get('checks') or []):
            time.sleep(min(30, 15 * (2 ** retry_index)))
        grading_env = dict(env)
        grading_env['TRAJECTORY_EVALUATION_PARALLELISM'] = '4'
        # Execution recovery may relax batch/storage metadata. Judge-only
        # recovery must retain the native grader identity check: an old
        # controller can otherwise copy healthy grades after the CLI changes.
        grading_env.pop('TRAJECTORY_UNCONDITIONAL_RESUME', None)
        if requires_regrade(report):
            # Native resume reuses non-error model checks. Regrade genuinely
            # missing evidence rather than silently copying it as complete.
            target = next_target()
            reviewed = c.regrade_report(source, dataset, config_path, target,
                                        env=grading_env, parallelism=4)
        else:
            if any(c._execution_error_code(row) or not c.framework_capture_complete(row)
                   for row in report['trials']):
                raise RuntimeError('comparison_review_requires_capture_recovery')
            target = next_target()
            output_root.parent.mkdir(parents=True, exist_ok=True)
            grading_config_path = target.with_name(target.name + '.regrade.config.json')
            if config_path.is_file():
                grading_config = c.read(config_path)
                # Keep the saved configuration identity. Worker backpressure
                # belongs in the environment: changing this field changes
                # native config_fingerprint even though grading is identical.
                for folder in (source.parent, *list(source.parents)[1:4]):
                    if not folder.name:
                        continue
                    source_config = folder.with_name(folder.name + '.regrade.config.json')
                    if not source_config.is_file():
                        continue
                    actual = c.read(source_config)
                    workers = actual.get('parallelism')
                    if (c.judge_projection(actual) == wanted_projection
                            and type(workers) in (int, float)
                            and workers == int(workers) and 1 <= workers <= 16):
                        grading_config['parallelism'] = workers
                    break
                c.save(grading_config_path, grading_config)
            else:
                grading_config_path = config_path
            command = [str(c.BINARY), 'dataset', 'run', str(dataset), '--config', str(grading_config_path),
                       '--output-root', str(target), '--model-config', str(c.RUNTIME / 'runtime.json'),
                       '--resume-report', str(source)]
            with (output_root.parent / 'review-recovery.log').open('a+', encoding='utf-8') as log:
                log.seek(0, os.SEEK_END)
                log_start = log.tell()
                completed = subprocess.run(command, cwd=ROOT, env=grading_env, stdout=log,
                                           stderr=subprocess.STDOUT, check=False)
                log.flush()
                log.seek(log_start)
                context_changed = 'evaluation_checkpoint_context_mismatch' in log.read()
            replay, manifest = target / 'report.json', target / 'manifest.json'
            if (completed.returncode not in (0, 1, 3) or not replay.is_file()
                    or not manifest.is_file() or c.read(manifest).get('complete') is not True):
                if not context_changed:
                    raise RuntimeError('comparison_judge_recovery_unavailable')
                # A native identity change needs a full evidence regrade,
                # never a relaxed resume that relabels the old healthy rows.
                target = next_target()
                reviewed = c.regrade_report(source, dataset, config_path, target,
                                            env=grading_env, parallelism=4)
            else:
                reviewed = c.read(replay)
        report = _validated_regrade(original, reviewed)
        source = target / 'report.json'
    return (report['trials'], source) if return_source else report['trials']


def _assert_review_complete(rows: list[dict]) -> None:
    incomplete = [row for row in rows
                  if replay_reasons(row, review_enabled=True)]
    if incomplete:
        raise RuntimeError(f"l0_baseline_review_incomplete:{len(incomplete)}")


def l0_evaluation_service() -> Path:
    """A read-only L0 execution boundary cannot ingest validation evidence."""
    from copy import deepcopy
    service = deepcopy(c.read(c.RUNTIME / 'service.json'))
    service.update(data_dir=str(c.GEPA / 'l0-evolution'), max_level='L0',
                   auto_evaluate=False, auto_approve_experiences=False)
    service.pop('memory_source_dir', None)
    service.pop('policy_evaluation_file', None)
    # Native learning schedules have no enabled field; auto_evaluate is the
    # execution-time switch that keeps this L0 store out of training.
    service.setdefault('learning', {}).pop('enabled', None)
    service.setdefault('sharing', {})['enabled'] = False
    service.setdefault('control_learning', {})['enabled'] = False
    service['instrumentation'] = {'level': 'L0', 'capabilities': ['model', 'actions', 'loop']}
    service['runtime_options'].update(action_rules=[], loop_rules=[], schedule_rules=[])
    target = c.RUNTIME / 'l0-evaluation-service.json'
    c.save(target, service)
    return target


def recover_baseline_capture(report: dict, report_path: Path, *, dataset: Path,
                             config_path: Path, output_root: Path, env: dict) -> tuple[dict, Path]:
    """Resume only failed execution/capture rows, retaining the native source.

    Judge errors with a complete capture are regraded later. Healthy Agent
    executions are never rerun. Each invocation makes at most two new native
    requests; old failed attempts never block recovery after a config repair.
    """
    def needs_capture(row):
        evidence = row.get('evidence') or {}
        traces = evidence.get('traces') or [evidence.get('trace')]
        closed = all(isinstance(trace, dict)
                     and any(event.get('kind') == 'run.start' for event in trace.get('events') or [])
                     and any(event.get('kind') == 'run.end' for event in trace.get('events') or [])
                     for trace in traces)
        return bool(c._execution_error_code(row) or not c.framework_capture_complete(row)
                    or not closed)

    expected = {row.get('task_id') for row in report.get('trials') or []}
    if all(not needs_capture(row) for row in report.get('trials') or []):
        return report, report_path
    prior = sorted((path for path in output_root.glob('capture-retry-*')
                    if path.is_dir() and path.name.rsplit('-', 1)[-1].isdigit()),
                   key=lambda path: int(path.name.rsplit('-', 1)[-1]))
    for target in prior:
        replay = target / 'report.json'
        if not replay.is_file():
            continue
        cached = c.read(replay)
        if {row.get('task_id') for row in cached.get('trials') or []} != expected:
            raise RuntimeError('l0_baseline_recovery_scope_changed')
        report, report_path = cached, replay
    number = max((int(path.name.rsplit('-', 1)[-1]) for path in prior), default=0) + 1
    for attempt in range(2):
        target = output_root / f'capture-retry-{number + attempt:02d}'
        replay = target / 'report.json'
        rerun = [row['trial_id'] for row in report.get('trials') or []
                 if needs_capture(row)]
        if not rerun:
            return report, report_path
        command = [str(c.BINARY), 'dataset', 'run', str(dataset), '--config', str(config_path),
                   '--output-root', str(target), '--resume-report', str(report_path)]
        for trial_id in rerun:
            command.extend(['--resume-rerun-trial', trial_id])
        with (output_root / 'capture-recovery.log').open('a', encoding='utf-8') as log:
            completed = subprocess.run(command, cwd=ROOT, env=env, stdout=log,
                                       stderr=subprocess.STDOUT, check=False)
        if completed.returncode not in (0, 1, 3) or not replay.is_file():
            raise RuntimeError('l0_baseline_capture_recovery_unavailable')
        report, report_path = c.read(replay), replay
    if any(needs_capture(row) for row in report.get('trials') or []):
        raise RuntimeError('l0_baseline_capture_recovery_exhausted')
    return report, report_path


def _partial_cost_source(started_path: Path) -> dict:
    """Read native checkpoint costs; never manufacture an evaluation report."""
    folder = started_path.parent
    started = c.read(started_path)
    rows = [c.read(path) for path in sorted((folder / 'trials').glob('*.json'))]
    settings = c.read(folder.parent / 'capture.config.json')
    saved = {row.get('trial_id') for row in rows}
    work = {path.name for path in (folder / '.work').glob('*') if path.is_dir()} - saved
    # Native assigns IDs from its frozen job index. AppWorld baselines have
    # one variant/repetition and use dataset order (the canonical sorted IDs).
    unknown = set()
    if work:
        if settings.get('repetitions') != 1 or len(settings.get('variants') or []) != 1:
            raise RuntimeError('l0_partial_cost_job_scope_invalid')
        for index, task in enumerate(settings.get('task_ids') or []):
            trial = hashlib.sha256(f"{started.get('run_id')}:{index}".encode()).hexdigest()[:32]
            if trial in work:
                unknown.add(task)
                work.remove(trial)
        if work:
            raise RuntimeError('l0_partial_cost_job_identity_unknown')
    value = {'run_id': started.get('run_id'), 'trials': rows,
             '_unrecorded_attempt_tasks': sorted(unknown)}
    progress_path = folder.parent / 'capture.progress.json'
    progress = c.read(progress_path) if progress_path.is_file() else {}
    declared = (progress.get('partial_unrecorded_attempts') or {}).get(folder.name)
    if declared:
        if (declared.get('run_id') != started.get('run_id')
                or Path(declared.get('source_dir') or '').resolve() != folder.resolve()
                or not isinstance(declared.get('task_ids'), list)
                or any(task not in (settings.get('task_ids') or [])
                       for task in declared['task_ids'])
                or not isinstance(declared.get('complete'), bool)):
            raise RuntimeError('l0_partial_unrecorded_attempt_identity_mismatch')
        unknown.update(declared['task_ids'])
        value['_unrecorded_attempt_tasks'] = sorted(unknown)
        value['_unrecorded_attempt_scope_complete'] = declared['complete']
    parent = (progress.get('partial_resume_sources') or {}).get(folder.name)
    if parent:
        value['summary'] = {'resume_source': {'run_id': parent['run_id']}}
    return value


def _partial_baseline_checkpoint(stage: Path, task_ids: list[str], config: dict,
                                 tasks: list[dict], dataset: Path):
    """Select an interrupted native batch without merging independent runs."""
    folders = [stage / 'capture', *stage.glob('capture-retry-*')]
    folders = sorted((folder for folder in folders
                      if (folder / 'started.json').is_file()
                      and not (folder / 'report.json').is_file()),
                     key=lambda folder: (folder / 'started.json').stat().st_mtime_ns,
                     reverse=True)
    if not folders:
        return None
    previous = c.read(stage / 'capture.config.json')
    # Report storage capacity may increase during recovery; it has no effect
    # on task execution, verdicts, judge budgets or retained trial contents.
    before, after = dict(previous), dict(config)
    old_limit, new_limit = before.pop('max_report_bytes', 0), after.pop('max_report_bytes', 0)
    if before != after or new_limit < old_limit:
        raise RuntimeError('l0_partial_checkpoint_config_changed')
    version = (c.read(dataset / 'manifest.json').get('metadata') or {}).get('dataset_version')
    versions = {task['task_id']: task.get('task_version') for task in tasks}
    expected_tasks = set(task_ids)
    expected_variant = config['variants'][0]['id']
    for folder in folders:
        started = c.read(folder / 'started.json')
        if (not started.get('run_id') or started.get('complete') is not False
                or started.get('dataset_version') != version
                or started.get('planned_trials') != len(task_ids)):
            raise RuntimeError('l0_partial_checkpoint_context_mismatch')
        rows, seen = [], set()
        for path in sorted((folder / 'trials').glob('*.json')):
            row = c.read(path)
            task = row.get('task_id')
            if (task not in expected_tasks or task in seen or path.stem != row.get('trial_id')
                    or row.get('task_version') != versions.get(task)
                    or row.get('variant_id') != expected_variant or row.get('repetition') != 1):
                raise RuntimeError('l0_partial_checkpoint_identity_mismatch')
            if (row.get('evidence') or {}).get('artifacts'):
                raise RuntimeError('l0_partial_checkpoint_artifacts_unsupported')
            seen.add(task)
            # Keep native failure rows too: native reruns execution failures
            # and regrades judge failures while cost accounting retains both.
            rows.append(row)
        if rows:
            return folder, started, rows
    return None


def _resume_partial_baseline(checkpoint, *, stage: Path, dataset: Path,
                             config_path: Path, config: dict, env: dict):
    """Resume exact atomic trials through the existing native checkpoint protocol."""
    from training import same_capture
    folder, started, rows = checkpoint
    metadata = {'splits': config['splits'], 'repetitions': config['repetitions'],
        'parallelism': config['parallelism'], 'model_enabled': config.get('judge', {}).get('enabled', True),
        'allow_unreviewed': False,
        'variants': [{key: variant[key] for key in ('id', 'version')}
                     for variant in config['variants']]}
    partial = {'schema_version': 1, 'report_kind': 'task_evaluation.v1',
        'dataset_version': started['dataset_version'], 'run_id': started['run_id'],
        'evaluator_version': ('task-evaluator-v2-goals'
            if config.get('judge', {}).get('assessment_mode') == 'objectives' else 'task-evaluator-v1'),
        'configuration': metadata, 'complete': False, 'trials': rows}
    number = 1
    target = stage / f'capture-retry-{number:02d}'
    while target.exists():
        number += 1
        target = stage / f'capture-retry-{number:02d}'
    # started.json omits the resume parent. Keep its actual native identity
    # in the existing progress sidecar so a second interruption cannot lose
    # earlier failed or unfinished attempt costs after the envelope expires.
    progress_path = stage / 'capture.progress.json'
    progress = c.read(progress_path) if progress_path.is_file() else {}
    progress.setdefault('partial_resume_sources', {})[target.name] = {
        'source_dir': str(folder), 'run_id': started['run_id']}
    save(progress_path, progress)
    # The temporary envelope contains untouched native trial dictionaries.
    # The durable source remains started.json/trials in the original folder.
    with tempfile.TemporaryDirectory(prefix='.checkpoint-', dir=stage) as temporary:
        source = Path(temporary) / 'report.json'
        # Native reports use compact JSON; indentation can more than double
        # long traces and exhaust the existing checkpoint byte limit.
        source.write_text(json.dumps(partial, ensure_ascii=False,
                                     separators=(',', ':')) + '\n', encoding='utf-8')
        save(source.with_name('manifest.json'), {'run_id': started['run_id'],
            'dataset_version': started['dataset_version'], 'complete': False,
            'files': {'report.json': hashlib.sha256(source.read_bytes()).hexdigest()}})
        command = [str(c.BINARY), 'dataset', 'run', str(dataset), '--config', str(config_path),
            '--output-root', str(target), '--model-config', str(c.RUNTIME / 'runtime.json'),
            '--resume-report', str(source)]
        with (stage / 'capture-recovery.log').open('a', encoding='utf-8') as log:
            completed = subprocess.run(command, cwd=ROOT,
                env=dict(env, TRAJECTORY_UNCONDITIONAL_RESUME='1'), stdout=log,
                stderr=subprocess.STDOUT, check=False)
    path = target / 'report.json'
    if completed.returncode not in (0, 1, 3) or not path.is_file():
        raise RuntimeError('l0_partial_checkpoint_resume_unavailable')
    report = c.read(path)
    reusable = [row for row in rows if row.get('execution_status') == 'completed'
                and c.framework_capture_complete(row) and c._trace_complete(row)
                and not c._execution_error_code(row)]
    reused_ids = {row['task_id'] for row in reusable}
    retained = dict(report, trials=[row for row in report.get('trials') or []
                                    if row.get('task_id') in reused_ids])
    if (not same_capture({'trials': reusable}, retained, list(reused_ids))
            and not _partial_capture_roundtrip_equal(reusable, retained.get('trials') or [])):
        raise RuntimeError('l0_partial_checkpoint_changed_execution')
    return report, path


def _partial_capture_roundtrip_equal(previous: list[dict], current: list[dict]) -> bool:
    """Allow one native floating-point ULP only in auxiliary quality_score.

    Native JSON/protobuf roundtrips can move 0.9166666666666665 to its adjacent
    double. Duration, metrics, task identities and every other trace/output
    field remain exact. This compares saved evidence without rewriting it.
    """
    def output_equal(left, right):
        if left == right:
            return True
        if not isinstance(left, dict) or not isinstance(right, dict) or left.keys() != right.keys():
            return False
        for key in left:
            a, b = left[key], right[key]
            if a == b:
                continue
            if (key != 'quality_score' or not isinstance(a, float) or not isinstance(b, float)
                    or not math.isfinite(a) or not math.isfinite(b)
                    or abs(a - b) > max(math.ulp(a), math.ulp(b))):
                return False
        return True

    def trace_equal(left, right):
        if left == right:
            return True
        if not isinstance(left, dict) or not isinstance(right, dict) or left.keys() != right.keys():
            return False
        if any(left[key] != right[key] for key in left if key != 'events'):
            return False
        a, b = left.get('events'), right.get('events')
        if not isinstance(a, list) or not isinstance(b, list) or len(a) != len(b):
            return False
        for before, after in zip(a, b):
            if before == after:
                continue
            if (not isinstance(before, dict) or not isinstance(after, dict)
                    or before.keys() != after.keys() or before.get('kind') != 'run.end'
                    or any(before[key] != after[key] for key in before if key != 'output')
                    or not output_equal(before.get('output'), after.get('output'))):
                return False
        return True

    before = {(row.get('task_id'), row.get('trial_id')): row for row in previous}
    if len(current) != len(before):
        return False
    for row in current:
        old = before.get((row.get('task_id'), row.get('trial_id')))
        if old is None or any(old.get(key) != row.get(key) for key in
                ('metrics', 'execution_status', 'duration_ms', 'variant_id', 'task_version')):
            return False
        a, b = old.get('evidence') or {}, row.get('evidence') or {}
        if (any(a.get(key) != b.get(key) for key in ('loaded_version', 'trajectory'))
                or not output_equal(a.get('output'), b.get('output'))
                or not trace_equal(a.get('trace'), b.get('trace'))):
            return False
        if a.get('traces') != b.get('traces'):
            left, right = a.get('traces'), b.get('traces')
            if (not isinstance(left, list) or not isinstance(right, list) or len(left) != len(right)
                    or not all(trace_equal(x, y) for x, y in zip(left, right))):
                return False
    return True


def _baseline_report_judge_fingerprint(path: Path):
    """Read a bounded metadata hint; selected sources still receive full validation."""
    import codecs
    decoder = json.JSONDecoder()
    try:
        with path.open('rb') as stream:
            prefix = ''
            text_decoder = codecs.getincrementaldecoder('utf-8')()
            for _ in range(64):
                chunk = stream.read(4096)
                if not chunk:
                    break
                prefix += text_decoder.decode(chunk)
                try:
                    position = len(prefix) - len(prefix.lstrip())
                    if prefix[position] != '{':
                        break
                    position += 1
                    while True:
                        position += len(prefix[position:]) - len(prefix[position:].lstrip())
                        key, position = decoder.raw_decode(prefix, position)
                        if not isinstance(key, str):
                            raise ValueError('report_metadata_key_invalid')
                        position += len(prefix[position:]) - len(prefix[position:].lstrip())
                        if prefix[position] != ':':
                            raise ValueError('report_metadata_separator_invalid')
                        position += 1
                        position += len(prefix[position:]) - len(prefix[position:].lstrip())
                        # Do not traverse execution evidence to locate metadata.
                        if key == 'trials':
                            raise ValueError('report_metadata_after_trials')
                        value, position = decoder.raw_decode(prefix, position)
                        if key == 'configuration' and isinstance(value, dict):
                            return value.get('judge_fingerprint')
                        position += len(prefix[position:]) - len(prefix[position:].lstrip())
                        if prefix[position] != ',':
                            raise ValueError('report_metadata_not_found')
                        position += 1
                except (json.JSONDecodeError, IndexError):
                    continue
                except ValueError:
                    break
    except (OSError, UnicodeError, RecursionError):
        pass
    # Unknown layouts retain the former full-read semantics. No hint is cached;
    # source/profile/manifest/capture/cost checks below remain authoritative.
    return (c.read(path).get('configuration') or {}).get('judge_fingerprint')


def _prior_baseline_source(output_root: Path, task_ids: list[str], config: dict,
                           tasks: list[dict], *, larger_only: bool = False) -> tuple[dict, Path] | None:
    """Find a completed L0 stage and its exact, whole native report."""
    from copy import deepcopy
    from training import same_capture
    requested = set(task_ids)
    task_versions = {task['task_id']: task.get('task_version') for task in tasks}
    stages = []
    for count in (32, 100, 500):
        stage = c.baseline_result(count, output_root=output_root.parent.parent).parent
        result_path = stage / 'result.json'
        if not result_path.is_file():
            continue
        result = c.read(result_path)
        items = result.get('items') or []
        ids = {item.get('task_id') for item in items}
        compatible_scope = (len(items) > len(task_ids) and requested <= ids
                            if larger_only else 0 < len(items) <= len(task_ids) and ids <= requested)
        if (result.get('status') == 'complete' and
                result.get('host_version') == c.host_version() and
                compatible_scope and len(ids) == len(items)
                and (not larger_only or result.get('judge_projection') == c.judge_projection(config))):
            stages.append((len(items), stage, result))
    if not stages:
        return None
    select = min if larger_only else max
    _, stage, result = select(stages, key=lambda item: item[0])
    expected_items = {item['task_id']: item for item in result['items']}
    expected = set(expected_items)
    previous_config = c.read(stage / 'capture.config.json')

    def execution_settings(value):
        value = deepcopy(value)
        for key in ('task_ids', 'max_trials', 'parallelism', 'max_report_bytes'):
            value.pop(key, None)
        value.pop('judge', None)
        return value

    if execution_settings(previous_config) != execution_settings(config):
        # A historical smaller baseline may have been produced before the
        # current unified execution contract.  It remains audit evidence, but
        # cannot seed the new scope; let the caller perform a clean capture
        # instead of turning a harmless configuration boundary into a stage
        # failure.
        return None
    # A compact result can outlive the native report it points at.  Host and
    # projection checks alone are insufficient: an old report may carry the
    # current host marker while still containing a different L0 artifact (and
    # therefore a different execution policy).  Bind every reusable native
    # source to the current config before accepting it as a baseline.
    expected_variant = next(
        (variant.get('version') for variant in config.get('variants', [])
         if variant.get('id') == 'candidate'), None)
    expected_projection = c.judge_projection(config)
    # A regrade/retry can legitimately receive a new native judge fingerprint
    # while retaining the exact same projected judge contract.  The previous
    # implementation pinned reuse to the first capture fingerprint, which
    # made a complete, current regrade report look unusable and blocked scope
    # expansion (for example 32 -> 500) before any new task could run.
    # Accept fingerprints observed in the current stage's reports; the
    # projection/host/variant checks below remain authoritative.
    current_judge_fingerprints = set()
    for candidate in stage.glob('**/report.json'):
        try:
            fingerprint = _baseline_report_judge_fingerprint(candidate)
        except (OSError, TypeError, ValueError):
            fingerprint = None
        if fingerprint:
            current_judge_fingerprints.add(fingerprint)

    def source_projection(path: Path) -> dict | None:
        projection = _report_judge_projection(path)
        if projection is not None:
            return projection
        # Native capture reports do not repeat the full judge settings.  The
        # capture sidecar is the authoritative config for the direct capture
        # path; retry/regrade paths are handled by _report_judge_projection.
        sidecar = path.parent.parent / 'capture.config.json'
        if sidecar.is_file():
            return c.judge_projection(c.read(sidecar))
        return None

    def source_matches_config(native: dict, path: Path) -> bool:
        metadata = native.get('configuration') or {}
        variants = metadata.get('variants') or []
        actual_variant = next(
            (variant.get('version') for variant in variants
             if variant.get('id') == 'candidate'), None)
        if expected_variant and actual_variant != expected_variant:
            return False
        if (current_judge_fingerprints
                and metadata.get('judge_fingerprint') not in current_judge_fingerprints):
            return False
        # Regrading uses fewer workers while keeping the exact captured
        # execution and judge fingerprint; parallelism is scheduling metadata.
        for key in ('repetitions', 'splits'):
            if metadata.get(key) != config.get(key):
                return False
        if metadata.get('model_enabled') != bool((config.get('judge') or {}).get('enabled', True)):
            return False
        if metadata.get('allow_unreviewed') is not False:
            return False
        return source_projection(path) == expected_projection

    retries = sorted(stage.glob('capture-retry-*/report.json'),
                     key=lambda path: int(path.parent.name.rsplit('-', 1)[-1]), reverse=True)
    capture = None
    for path in [*retries, stage / 'capture/report.json']:
        if not path.is_file() or not path.with_name('manifest.json').is_file():
            continue
        native = c.read(path)
        rows = native.get('trials') or []
        if (native.get('complete') is True and len(rows) == len(expected)
                and {row.get('task_id') for row in rows} == expected
                and all(c.framework_capture_complete(row) and not c._execution_error_code(row)
                        for row in rows)):
            capture = native
            break
    if capture is None:
        raise RuntimeError('l0_baseline_reuse_capture_source_missing')
    reviewed = sorted((stage / 'regrade').glob('retry*/report.json'),
                      key=lambda path: int(path.parent.name.removeprefix('retry').lstrip('-') or 0),
                      reverse=True)
    candidates = [*( [Path(result['source_report'])] if result.get('source_report') else []),
                  *reviewed, stage / 'regrade/report.json', *retries, stage / 'capture/report.json']
    for path in dict.fromkeys(candidates):
        if not path.is_file() or not path.with_name('manifest.json').is_file():
            continue
        if c.read(path.with_name('manifest.json')).get('complete') is not True:
            continue
        native = c.read(path)
        rows = native.get('trials') or []
        if (native.get('complete') is not True or len(rows) != len(expected)
                or {row.get('task_id') for row in rows} != expected):
            continue
        if not source_matches_config(native, path):
            continue
        if not same_capture(capture, native, list(expected)):
            continue
        if any(replay_reasons(row, review_enabled=True) for row in rows):
            continue
        if any(row.get('task_version') != task_versions.get(row['task_id']) for row in rows):
            raise RuntimeError('l0_baseline_reuse_task_changed')
        compact = execution_cost_rows([compact_row(row, 'l0-baseline') for row in rows], path, stage)
        if {row['task_id']: row for row in compact} != expected_items:
            continue
        if not all(any(check.get('check_id') == 'model' for check in row.get('checks') or [])
                   for row in rows):
            continue
        # One native source reference is sufficient; do not invent a merged
        # report or duplicate the source payload in the compact result.
        if not larger_only and result.get('source_report') != str(path):
            result['source_report'] = str(path)
            c.save(stage / 'result.json', result)
        return native, path
    raise RuntimeError('l0_baseline_reuse_complete_native_source_missing')



def _own_baseline_source(output_root: Path, task_ids: list[str], config: dict,
                         tasks: list[dict], original_capture: dict | None = None) -> tuple[dict, Path, list[dict]] | None:
    """Validate this scope's canonical native source before searching larger scopes."""
    from copy import deepcopy
    from training import same_capture
    try:
        result_path = output_root / 'result.json'
        if not result_path.is_file():
            return None
        result = c.read(result_path)
        requested = set(task_ids)
        items = result.get('items') or []
        if (result.get('status') != 'complete' or result.get('host_version') != c.host_version()
                or result.get('judge_projection') != c.judge_projection(config)
                or len(items) != len(task_ids)
                or {row.get('task_id') for row in items} != requested):
            return None
        source = Path(result.get('source_report') or '')
        # Lexical ownership permits an existing archived-raw symlink inside
        # this scope, while rejecting an unrelated stage's source reference.
        if not (Path(os.path.abspath(source)).is_relative_to(output_root.absolute())
                or source.resolve().is_relative_to(output_root.resolve())):
            return None
        manifest_path = source.with_name('manifest.json')
        saved_config = output_root / 'capture.config.json'
        if not source.is_file() or not manifest_path.is_file() or not saved_config.is_file():
            return None
        def execution_settings(value):
            value = deepcopy(value)
            for key in ('task_ids', 'max_trials', 'parallelism', 'max_report_bytes', 'judge'):
                value.pop(key, None)
            return value
        def closed_capture(row):
            evidence = row.get('evidence') or {}
            traces = evidence.get('traces') or [evidence.get('trace')]
            return (c.framework_capture_complete(row) and not c._execution_error_code(row)
                    and all(isinstance(trace, dict)
                            and any(event.get('kind') == 'run.start' for event in trace.get('events') or [])
                            and any(event.get('kind') == 'run.end' for event in trace.get('events') or [])
                            for trace in traces))
        if execution_settings(c.read(saved_config)) != execution_settings(config):
            return None
        manifest = c.read(manifest_path)
        if (manifest.get('complete') is not True
                or not isinstance((manifest.get('files') or {}).get('report.json'), str)
                or not (manifest.get('files') or {}).get('report.json')
                or _report_judge_projection(source) != c.judge_projection(config)):
            return None
        native = c.read(source)
        rows = native.get('trials') or []
        versions = {row['task_id']: row.get('task_version') for row in tasks}
        expected_version = next((row.get('version') for row in config.get('variants') or []
                                 if row.get('id') == 'candidate'), None)
        metadata = native.get('configuration') or {}
        if (native.get('complete') is not True
                or not isinstance(native.get('run_id'), str) or not native.get('run_id')
                or not isinstance(native.get('dataset_version'), str) or not native.get('dataset_version')
                or not isinstance(expected_version, str) or not expected_version
                or manifest.get('run_id') != native.get('run_id')
                or manifest.get('dataset_version') != native.get('dataset_version')
                or _report_variant_version(native) != expected_version
                or metadata.get('model_enabled') is not True or metadata.get('allow_unreviewed') is not False
                or any(metadata.get(key) != config.get(key) for key in ('repetitions', 'splits'))
                or len(rows) != len(task_ids) or {row.get('task_id') for row in rows} != requested
                or any(row.get('variant_id') != 'candidate'
                       or row.get('task_version') != versions.get(row.get('task_id'))
                       or not closed_capture(row)
                       or replay_reasons(row, review_enabled=True)
                       or not any(check.get('check_id') == 'model' for check in row.get('checks') or [])
                       for row in rows)):
            return None
        captures = sorted(output_root.glob('capture*/report.json'),
                          key=lambda path: path.stat().st_mtime_ns, reverse=True)
        matched_capture = False
        for path in captures:
            capture = original_capture if path == output_root / 'capture/report.json' and original_capture is not None else c.read(path)
            captured = capture.get('trials') or []
            if (capture.get('complete') is True and len(captured) == len(task_ids)
                    and {row.get('task_id') for row in captured} == requested
                    and all(closed_capture(row) for row in captured)):
                captured_manifest = c.read(path.with_name('manifest.json'))
                if (captured_manifest.get('complete') is not True
                        or captured_manifest.get('run_id') != capture.get('run_id')
                        or captured_manifest.get('dataset_version') != capture.get('dataset_version')
                        or not same_capture(capture, native, task_ids)):
                    return None
                matched_capture = True
                break
        if not matched_capture:
            return None
        compact = execution_cost_rows([compact_row(row, 'l0-baseline') for row in rows],
                                      source, output_root, native_report=native)
        if {row['task_id']: row for row in compact} != {row['task_id']: row for row in items}:
            return None
        return native, source, compact
    except (OSError, ValueError, TypeError, AttributeError, KeyError, RuntimeError):
        return None


def _larger_baseline_source(output_root: Path, task_ids: list[str], config: dict,
                            tasks: list[dict]) -> tuple[dict, Path] | None:
    """Reuse healthy larger grades only when existing task execution is identical."""
    from training import same_capture
    larger = _prior_baseline_source(output_root, task_ids, config, tasks, larger_only=True)
    if larger is None or len(larger[0].get('trials') or []) <= len(task_ids):
        return None
    native, path = larger
    requested = set(task_ids)
    # The source remains the real larger report. Compare its actual task rows
    # against the retained smaller execution; never manufacture a native subset.
    captures = sorted(output_root.glob('capture*/report.json'),
                      key=lambda item: item.stat().st_mtime_ns, reverse=True)
    for capture_path in captures:
        capture = c.read(capture_path)
        rows = capture.get('trials') or []
        if ({row.get('task_id') for row in rows} == requested
                and capture.get('complete') is True):
            if not same_capture(native, capture, task_ids):
                return None
            return native, path
    if captures:
        return None
    return native, path


def _expand_baseline(source_report: dict, source: Path, *, dataset: Path,
                     config_path: Path, output_root: Path, task_ids: list[str], env: dict) -> dict:
    """Native scope expansion copies old trials whole and executes only new tasks."""
    report_path, manifest = output_root / 'report.json', output_root / 'manifest.json'
    if report_path.is_file() and manifest.is_file():
        report = c.read(report_path)
    else:
        if (output_root.is_dir() and not report_path.exists() and not manifest.exists()
                and not any((output_root / 'trials').glob('*.json'))
                and not any((output_root / 'artifacts').rglob('*.*'))):
            shutil.rmtree(output_root)
        if output_root.exists():
            raise RuntimeError('l0_baseline_expansion_capture_incomplete')
        output_root.parent.mkdir(parents=True, exist_ok=True)
        command = [str(c.BINARY), 'dataset', 'run', str(dataset), '--config', str(config_path),
                   '--output-root', str(output_root), '--model-config', str(c.RUNTIME / 'runtime.json'),
                   '--resume-report', str(source)]
        # Scope expansion changes the full-batch config, not the frozen task,
        # Agent or judging semantics. Native still maps original trial IDs.
        local_env = dict(env, TRAJECTORY_UNCONDITIONAL_RESUME='1')
        with (output_root.parent / 'capture-recovery.log').open('a', encoding='utf-8') as log:
            completed = subprocess.run(command, cwd=ROOT, env=local_env, stdout=log,
                                       stderr=subprocess.STDOUT, check=False)
        if (completed.returncode not in (0, 1, 3) or not report_path.is_file()
                or not manifest.is_file()):
            raise RuntimeError('l0_baseline_expansion_unavailable')
        report = c.read(report_path)
    if (report.get('complete') is not True or c.read(manifest).get('complete') is not True
            or len(report.get('trials') or []) != len(task_ids)
            or {row.get('task_id') for row in report['trials']} != set(task_ids)):
        raise RuntimeError('l0_baseline_expansion_scope_incomplete')
    by_id = {row['task_id']: row for row in report['trials']}
    if any(by_id.get(row['task_id']) != row for row in source_report['trials']):
        raise RuntimeError('l0_baseline_expansion_changed_existing_trial')
    # These are existing native metadata fields, not new fingerprints. They
    # ensure reused grades belong to the same frozen scoring contract.
    for key in ('dataset_version',):
        if source_report.get(key) != report.get(key):
            raise RuntimeError('l0_baseline_expansion_scoring_changed')
    for key in ('variants',):
        if (source_report.get('configuration') or {}).get(key) != (report.get('configuration') or {}).get(key):
            raise RuntimeError('l0_baseline_expansion_scoring_changed')
    return report


def baseline_main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--count", type=int, required=True, choices=(32, 100, 500))
    parser.add_argument("--workers", type=int, default=16)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output = args.output.resolve()
    dataset = DATASET
    # Baseline and candidate captures use the same configured worker budget so
    # timing and provider-load comparisons remain on one execution contract.
    capture_workers = max(1, args.workers)
    rows = [json.loads(line) for line in (dataset / "holdout.jsonl").read_text().splitlines()
            if line.strip()]
    task_ids = sorted(row["task_id"] for row in rows)[:args.count]
    artifact = c.read(c.PUBLISHED / "L0.json")
    config = c.evaluation_config(task_ids, artifact, capture_workers, service_path=l0_evaluation_service())
    config.update(splits=["holdout"], task_ids=task_ids, parallelism=capture_workers,
                  max_trials=len(task_ids), timeout_ms=1_800_000)
    args.output.mkdir(parents=True, exist_ok=True)
    config_path = args.output / "capture.config.json"
    # Bind the native execution capture to the Python host revision as a
    # sidecar.  Native reports only carry the compiled Agent artifact version;
    # without this marker a failed judge retry could be mistaken for a fresh
    # capture after the host changes and trigger a full Agent rerun.
    host_marker = args.output / 'capture.host-version'
    current_host = c.host_version()
    if host_marker.is_file():
        recorded_host = host_marker.read_text(encoding='utf-8').strip()
        if recorded_host != current_host:
            _archive_incompatible_baseline(args.output, recorded_host or 'unknown')
            host_marker = args.output / 'capture.host-version'
    elif capture_path := (args.output / 'capture' / 'report.json'):
        # An unmarked completed capture is ambiguous after a host revision.
        # Archive it rather than silently mixing execution with a new host.
        if capture_path.is_file():
            _archive_incompatible_baseline(args.output, 'unmarked-host')
    host_marker.write_text(current_host + '\n', encoding='utf-8')
    had_capture_config = config_path.is_file()
    previous_projection = c.judge_projection(c.read(config_path)) if had_capture_config else None
    old_result_path = args.output / 'result.json'
    if old_result_path.is_file():
        old_result = c.read(old_result_path)
        old_host = old_result.get('host_version')
        if old_host != c.host_version():
            _archive_incompatible_baseline(args.output, old_result.get('host_version'))
            had_capture_config = False
            previous_projection = None
        else:
            previous_projection = old_result.get('judge_projection')
    capture_path = args.output / 'capture' / 'report.json'
    capture_report = None
    if capture_path.is_file():
        # Native report manifests bind the compiled artifact identity
        # (``agent-version-v1:<digest>``).  Resolve it lazily: a fresh capture
        # has no prior report to validate, and tests/recovery fixtures may use
        # a lightweight fake artifact that deliberately cannot be hashed by
        # the native CLI.  Once a report exists, a missing identity is a hard
        # provenance error rather than a reason to reuse it.
        # The native baseline intentionally stores the raw L0 host revision
        # (see evaluation_config's policy-free L0 branch), whereas learned
        # candidates store the content digest.  Compare against the version
        # actually emitted in the current capture config, not c.version(),
        # which would incorrectly archive every valid L0 v22 capture.
        expected_agent_version = next(
            (variant.get('version') for variant in config.get('variants', [])
             if variant.get('id') == 'candidate'), None)
        if not isinstance(expected_agent_version, str) or not expected_agent_version:
            raise RuntimeError('l0_artifact_version_missing')
        capture_report = c.read(capture_path)
        capture_host = _report_variant_version(capture_report)
        # Native reports bind the agent artifact revision in
        # configuration.  That is distinct from the Python host revision
        # recorded in the compact result; compare the former to L0's exact
        # artifact revision and the latter to host_version above.
        if capture_host != expected_agent_version:
            _archive_incompatible_baseline(args.output, capture_host)
            had_capture_config = False
            previous_projection = None
    # Prefer this scope's actual execution over a subsequently completed,
    # independent smaller baseline. Interrupted 500 and 100 runs can share
    # task IDs while their trial IDs/trajectories differ.
    own_source = _own_baseline_source(args.output, task_ids, config, rows, capture_report)
    larger_source = None if own_source else _larger_baseline_source(args.output, task_ids, config, rows)
    existing_source = None
    for path in ([] if own_source or larger_source else sorted(args.output.glob('capture*/report.json'),
                       key=lambda path: path.stat().st_mtime_ns, reverse=True)):
        existing = c.read(path)
        if (existing.get('complete') is True
                and path.with_name('manifest.json').is_file()
                and c.read(path.with_name('manifest.json')).get('complete') is True
                and len(existing.get('trials') or []) == len(task_ids)
                and {row.get('task_id') for row in existing['trials']} == set(task_ids)
                and _report_variant_version(existing) == config['variants'][0]['version']):
            existing_source = (existing, path)
            break
    partial = (None if own_source or larger_source or existing_source else
               _partial_baseline_checkpoint(args.output, task_ids, config, rows, dataset))
    prior = (None if own_source or larger_source or existing_source or partial else
             _prior_baseline_source(args.output, task_ids, config, rows))
    c.save(config_path, config)
    env = c.environment()
    env["TRAJECTORY_EVALUATION_PARALLELISM"] = str(capture_workers)
    env["APPWORLD_EVIDENCE_ROOT"] = str(args.output / "official-evidence")
    report_path = args.output / "capture" / "report.json"
    if own_source:
        report, report_path, validated_items = own_source
        source_projection = _report_judge_projection(report_path)
    elif larger_source:
        report, report_path = larger_source
        source_projection = _report_judge_projection(report_path, c.judge_projection(config))
    elif existing_source:
        report, report_path = existing_source
        source_projection = previous_projection if had_capture_config else c.judge_projection(config)
    elif partial:
        report, report_path = _resume_partial_baseline(partial, stage=args.output,
            dataset=dataset, config_path=config_path, config=config, env=env)
        source_projection = previous_projection if had_capture_config else c.judge_projection(config)
    elif prior:
        previous, previous_path = prior
        if {row['task_id'] for row in previous['trials']} == set(task_ids):
            report, report_path = previous, previous_path
        else:
            report = _expand_baseline(previous, previous_path, dataset=dataset,
                config_path=config_path, output_root=args.output / 'capture',
                task_ids=task_ids, env=env)
        source_projection = _report_judge_projection(previous_path, previous_projection)
    else:
        # A complete native capture with the current host marker is valid
        # execution evidence even when its model judge needs another retry.
        # Reuse it and let _review_rows retry only the missing judge rows.
        existing_capture = args.output / 'capture' / 'report.json'
        existing_manifest = args.output / 'capture' / 'manifest.json'
        existing = c.read(existing_capture) if existing_capture.is_file() else {}
        existing_rows = existing.get('trials') or []
        if (existing.get('complete') is True
                and existing_manifest.is_file()
                and c.read(existing_manifest).get('complete') is True
                and len(existing_rows) == len(task_ids)
                and {row.get('task_id') for row in existing_rows} == set(task_ids)
                and all(c.framework_capture_complete(row)
                        and not c._execution_error_code(row) for row in existing_rows)):
            report = existing
            source_projection = previous_projection if had_capture_config else c.judge_projection(config)
        else:
            report = c.shared.capture(dataset, config_path, args.output / "capture", env)
            source_projection = previous_projection if had_capture_config else c.judge_projection(config)
    if own_source:
        # The canonical source already passed current profile, closed capture,
        # native review and full cost-lineage checks above. Preserve that exact
        # source and its recomputed compact rows within this invocation.
        rows, reviewed_source = report['trials'], report_path
    else:
        report, report_path = recover_baseline_capture(report, report_path, dataset=dataset,
            config_path=config_path, output_root=args.output, env=env)
        rows = report.get("trials", [])
        if larger_source:
            selected_ids = set(task_ids)
            rows = [row for row in rows if row.get("task_id") in selected_ids]
        rows, reviewed_source = _review_rows(rows, report_path=report_path, dataset=dataset,
                            config_path=config_path, output_root=args.output / "regrade", env=env,
                            return_source=True, source_projection=source_projection)
    _assert_review_complete(rows)
    items = validated_items if own_source else execution_cost_rows(
        [compact_row(row, "l0-baseline") for row in rows], reviewed_source, args.output)
    if len(items) != len(task_ids) or {item.get("task_id") for item in items} != set(task_ids):
        raise RuntimeError("l0_baseline_task_coverage_invalid")
    incomplete = [item for item in items if item.get("invalid_loop_basis") != "model_review"
                  or item.get("quality_status") != "complete"]
    if incomplete:
        raise RuntimeError(f"l0_baseline_review_incomplete:{len(incomplete)}")
    result = {"scope": f"same_task_l0_baseline_{args.count}", "status": "complete",
              "host_version": c.host_version(),
              "seed": str(c.PUBLISHED / "L0.json"), "source_report": str(reviewed_source),
              "judge_projection": c.judge_projection(config),
              "metrics": summarize(items, len(items)), "items": items,
              "model_calls": sum(item.get("model_calls") or 0 for item in items)}
    c.save(args.output / "result.json", result)
    print(json.dumps({"status": result["status"], "completed": len(items),
                      "metrics": result["metrics"]}, ensure_ascii=False))
    return 0


def main() -> int:
    import sys
    if '--summarize-only' in sys.argv:
        parser = argparse.ArgumentParser(description='Recompute existing derived costs without API calls')
        parser.add_argument('--summarize-only', type=Path, required=True)
        args = parser.parse_args()
        result = summarize_existing(args.summarize_only)
        print(json.dumps({'status': result.get('status'), 'metrics': result['metrics']}, ensure_ascii=False))
        return 0
    if '--baseline' in sys.argv:
        sys.argv.remove('--baseline')
        return baseline_main()
    return comparison_main()


if __name__ == '__main__':
    raise SystemExit(main())
