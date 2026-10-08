#!/usr/bin/env python3
"""Run official and public synthetic AppWorld training through L0 and native learning.

This runner is deliberately separate from the small development schedule.  It
reads the canonical public train pack and its explicit synthetic exercises, builds one reviewed native
dataset, captures every task with the original L0 artifact, and imports each
completed batch into the private evolution store.  Reports from the small
development schedule are never silently used as full-training evidence.
"""
from __future__ import annotations

import argparse
import fcntl
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
WORLD = ROOT / "examples/agent/business-simulator/appworld/runtime/world"
DATASETS = HERE / "datasets"
FULL_DATASET = ROOT / "trace_output/appworld/training/datasets/train"
TRAINING = ROOT / "trace_output/appworld/training"
CAPTURES = TRAINING / "full-captures"
STATUS = TRAINING / "full-training-status.json"
_REBUILT_BINARY = ROOT / "tools/trajectory_evolution/build-v4/trajectory_evolution"
BINARY = _REBUILT_BINARY if _REBUILT_BINARY.is_file() else ROOT / "tools/trajectory_evolution/build/trajectory_evolution"

sys.path.insert(0, str(HERE))
import layout as controller
import learning as learning_engine


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".new")
    tmp.write_text(json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n")
    tmp.replace(path)


def official_ids() -> list[str]:
    from dataset import rows
    return [row['task_id'] for row in rows('train')]


def training_batches(task_ids: list[str], batch_size: int) -> list[list[str]]:
    """Keep original official batches intact when synthetic exercises are added."""
    from dataset import rows
    synthetic = {row['task_id'] for row in rows('train')
                 if row.get('initial_state', {}).get('training_goal')}
    official = [task_id for task_id in task_ids if task_id not in synthetic]
    added = [task_id for task_id in task_ids if task_id in synthetic]
    return [scope[start:start + batch_size] for scope in (official, added)
            for start in range(0, len(scope), batch_size)]


def build_dataset(task_ids: list[str]) -> Path:
    from dataset import materialize
    destination = materialize(BINARY, view='train')['path']
    return Path(destination)


def install_learning_runtime() -> Path:
    """Give analysis its own files without changing frozen task-judge inputs."""
    directory = controller.RUNTIME / 'learning'
    directory.mkdir(exist_ok=True)
    prompts = directory / 'prompts'
    if not prompts.exists():
        prompts.symlink_to(controller.ROOT / 'tools/trajectory_evolution/config/prompts',
                          target_is_directory=True)
    path = directory / 'runtime.json'
    runtime = controller.read(controller.RUNTIME / 'runtime.json')
    model = runtime.setdefault('model', {})
    model['timeout_ms'] = max(int(model.get('timeout_ms') or 0),
        int(os.environ.get('APPWORLD_LEARNING_MODEL_TIMEOUT_MS', '300000')))
    model['max_retries'] = 0
    model['max_request_bytes'] = 3 * 1024 * 1024
    write_json(path, runtime)
    learning_engine.install_prompts(controller, runtime_path=path)
    service = controller.read(controller.TRAINING_SERVICE)
    service['runtime_config'] = str(path)
    write_json(controller.TRAINING_SERVICE, service)
    return path


def evaluation_config(task_ids: list[str], artifact: dict, workers: int) -> dict:
    config = controller.shared._evaluation_config([], workers)
    config.update(splits=["train"], task_ids=task_ids, parallelism=workers,
                  max_trials=len(task_ids), timeout_ms=1800000,
                  max_trace_bytes=controller.MAX_TRACE_BYTES)
    variant = config["variants"][0]
    variant.update(id="candidate", version=artifact["agent"]["revision"],
                   command=[str(controller.PYTHON), "-B", str(controller.HOST), "--run-entry"],
                   instrumentation_config=str(controller.TRAINING_SERVICE))
    variant.pop("artifact", None)
    variant["pass_env"] += ["APPWORLD_ROOT", "APPWORLD_MAX_STEPS", "APPWORLD_EVIDENCE_ROOT",
                             "EVOLUTION_APPWORLD_TOKEN"]
    config["checks"] = {"*": [{"id": "strict", "path": "/output/appworld_reward",
                                  "op": "equals", "value": 1.0}]}
    for key, filename in [("prompt", "task-judge-system.txt"),
                          ("loop_review_prompt", "task-loop-review.txt"),
                          ("consistency_prompt", "task-consistency.txt")]:
        config["judge"][key] = str(ROOT / "tools/trajectory_evolution/config/prompts" / filename)
    # Full training uses the same objectives judge as the business pipeline so
    # its learned evidence includes the canonical model-reviewed loop field.
    config["judge"].update(
        enabled=True,
        assessment_mode="objectives",
        max_batch_turns=4,
        max_evidence_lookups=2,
        batch_input_bytes=2097152,
        max_input_bytes=2097152,
        max_repair_attempts=2,
        goal_prompt=str(ROOT / "tools/trajectory_evolution/config/prompts/task-goals.txt"),
        runtime_config=str(controller.RUNTIME / "runtime.json"),
        max_model_calls=max(8 * len(task_ids), 8),
    )
    config['judge'].update(controller.judge_projection())
    config["artifact_retention"].update(retain_trial_files=True, retain_report_pb=False)
    return config


class ReviewIncomplete(RuntimeError):
    """Execution is retained; judge work can resume without re-running Agent."""


def capture_recovery_required(row: dict) -> bool:
    """Only missing execution/capture needs a new Agent run, never task failure."""
    if controller._execution_error_code(row) is not None:
        return True
    terminal = controller.output(row)
    if str(terminal.get('evaluation_status') or '').lower() in ('error', 'errored'):
        return True
    if isinstance(terminal.get('error_code'), str) and terminal['error_code'].strip():
        return True
    return not controller._trace_complete(row)


def capture_scope_complete(report: dict, task_ids: list[str]) -> bool:
    rows = report.get('trials') or []
    return (report.get('complete') is True and len(rows) == len(task_ids)
            and {row.get('task_id') for row in rows} == set(task_ids))


def resume_capture(source: Path, destination: Path, config: Path, env: dict,
                   rerun_trials: list[str]) -> tuple[dict, Path]:
    """Let native resume copy healthy trials and recapture only explicit gaps."""
    number = 1
    target = destination / f'capture-retry-{number:02d}'
    while target.exists():
        number += 1
        target = destination / f'capture-retry-{number:02d}'
    command = [str(BINARY), 'dataset', 'run', str(FULL_DATASET), '--config', str(config),
               '--output-root', str(target), '--resume-report', str(source)]
    for trial_id in rerun_trials:
        command.extend(['--resume-rerun-trial', trial_id])
    try:
        with (destination / 'capture-recovery.log').open('a', encoding='utf-8') as log:
            result = subprocess.run(command, cwd=ROOT, env=env, stdout=log,
                                    stderr=subprocess.STDOUT, check=False)
        report_path = target / 'report.json'
        if result.returncode not in (0, 1, 3) or not report_path.is_file():
            raise ReviewIncomplete(f'native_capture_recovery_unavailable:{result.returncode}')
        return controller.read(report_path), report_path
    except OSError as error:
        raise ReviewIncomplete('native_capture_recovery_unavailable:' + type(error).__name__) from error


def review_complete(row: dict) -> bool:
    return (controller.framework_capture_complete(row)
            and controller.assessed_quality(row) is not None
            and controller._judge_error_code(row) is None
            and controller._has_complete_loop_review(row))


def _transport_aux_output_equal(left, right) -> bool:
    """Preserve output exactly except one adjacent double in host quality_score."""
    for value in (left, right):
        if (isinstance(value, dict) and type(value.get('quality_score')) is float
                and not math.isfinite(value['quality_score'])):
            return False
    if left == right:
        return True
    if not isinstance(left, dict) or not isinstance(right, dict) or left.keys() != right.keys():
        return False
    for key in left:
        a, b = left[key], right[key]
        if a == b:
            continue
        if (key != 'quality_score' or type(a) is not float or type(b) is not float
                or not math.isfinite(a) or not math.isfinite(b)
                or math.nextafter(a, b) != b or math.nextafter(b, a) != a):
            return False
    return True


def _transport_aux_trace_equal(left, right) -> bool:
    for value in (left, right):
        if not isinstance(value, dict) or not isinstance(value.get('events'), list):
            continue
        for event in value['events']:
            if (isinstance(event, dict) and event.get('kind') == 'run.end'
                    and not _transport_aux_output_equal(event.get('output'), event.get('output'))):
                return False
    if left == right:
        return True
    if not isinstance(left, dict) or not isinstance(right, dict) or left.keys() != right.keys():
        return False
    if any(left[key] != right[key] for key in left if key != 'events'):
        return False
    before, after = left.get('events'), right.get('events')
    if not isinstance(before, list) or not isinstance(after, list) or len(before) != len(after):
        return False
    for a, b in zip(before, after):
        if a == b:
            continue
        if (not isinstance(a, dict) or not isinstance(b, dict) or a.keys() != b.keys()
                or a.get('kind') != 'run.end'
                or any(a[key] != b[key] for key in a if key != 'output')
                or not _transport_aux_output_equal(a.get('output'), b.get('output'))):
            return False
    return True


def same_capture(original: dict, reviewed: dict, task_ids: list[str]) -> bool:
    """A regrade may change judging, never the original execution or task scope."""
    left = original.get('trials') or []
    right = reviewed.get('trials') or []
    expected = set(task_ids)
    if (reviewed.get('complete') is not True or len(right) != len(expected)
            or {row.get('task_id') for row in right} != expected):
        return False
    by_id = {(row.get('task_id'), row.get('trial_id')): row for row in left}
    fields = ('metrics', 'execution_status', 'duration_ms', 'variant_id', 'task_version')
    evidence_fields = ('traces', 'loaded_version', 'trajectory')
    for row in right:
        previous = by_id.get((row.get('task_id'), row.get('trial_id')))
        if previous is None or any(previous.get(field) != row.get(field) for field in fields):
            return False
        old_evidence, new_evidence = previous.get('evidence') or {}, row.get('evidence') or {}
        if any(old_evidence.get(field) != new_evidence.get(field) for field in evidence_fields):
            return False
        if (not _transport_aux_output_equal(old_evidence.get('output'), new_evidence.get('output'))
                or not _transport_aux_trace_equal(old_evidence.get('trace'), new_evidence.get('trace'))):
            return False
    return True


def reviewed_source(report: dict, source: Path, task_ids: list[str], config: Path, env: dict):
    """Choose native report+manifest together; never hand-merge or rewrite grades."""
    if any(not controller.framework_capture_complete(row) for row in report.get('trials') or []):
        raise ReviewIncomplete('framework_capture_incomplete_requires_trace_recovery')
    if any(capture_recovery_required(row) for row in report.get('trials') or []):
        raise ReviewIncomplete('execution_capture_incomplete_requires_recovery')
    if all(review_complete(row) for row in report.get('trials') or []):
        return report, source
    best, best_source = report, source
    candidates = [source.parent / 'regrade/report.json', source.parent / 'regrade-retry/report.json']
    for candidate in candidates:
        if not candidate.is_file() or not candidate.with_name('manifest.json').is_file():
            continue
        value = controller.read(candidate)
        if not same_capture(report, value, task_ids):
            continue
        if all(review_complete(row) for row in value['trials']):
            return value, candidate
        if sum(map(review_complete, value['trials'])) >= sum(map(review_complete, best['trials'])):
            best, best_source = value, candidate
    last_error = 'judge_review_incomplete'
    # At most two native judge retries; all raw task executions stay fixed.
    for _ in range(2):
        output = source.parent / ('regrade-retry' if best_source.parent.name == 'regrade' else 'regrade')
        if output.exists():
            # Only discard the inactive incomplete retry, retaining the best
            # native report and original execution. Storage stays bounded.
            shutil.rmtree(output)
        try:
            reviewed = controller.regrade_report(best_source, FULL_DATASET, config, output, env=env)
        except Exception as exc:
            last_error = type(exc).__name__ + ':' + str(exc)[:256]
            continue
        if not same_capture(report, reviewed, task_ids):
            raise RuntimeError('training_regrade_changed_execution_or_scope')
        best, best_source = reviewed, output / 'report.json'
        if all(review_complete(row) for row in best['trials']):
            return best, best_source
    raise ReviewIncomplete('training_batch_model_review_incomplete:' + last_error)



def complete_native_review(report: dict, batch: Path, task_ids: list[str], *,
                           source_compatible=None, prefer_earliest: bool = False):
    """Reuse one complete compatible native pair, irrespective of retry naming.

    Selection uses completion/source order, never a task's score. Callers can
    restrict the grading profile before selecting a pair and preserve the
    first healthy review. The default keeps the existing recency behavior.
    Original execution, costs and scope must match; no rows are hand merged.
    """
    if not capture_scope_complete(report, task_ids):
        return None
    root = batch.resolve()
    candidates = sorted(batch.rglob('report.json'), key=lambda path: path.stat().st_mtime_ns,
                        reverse=not prefer_earliest)
    for candidate in candidates:
        if not candidate.resolve().is_relative_to(root):
            continue
        manifest_path = candidate.with_name('manifest.json')
        if not manifest_path.is_file():
            continue
        try:
            if source_compatible is not None and not source_compatible(candidate):
                continue
            value, manifest = controller.read(candidate), controller.read(manifest_path)
            if (manifest.get('complete') is not True
                    or not isinstance(manifest.get('files'), dict)
                    or not isinstance(manifest['files'].get('report.json'), str)
                    or not manifest['files']['report.json']
                    or not value.get('run_id') or manifest.get('run_id') != value.get('run_id')
                    or manifest.get('dataset_version') != value.get('dataset_version')
                    or not same_capture(report, value, task_ids)
                    or any(not review_complete(row) or capture_recovery_required(row)
                           for row in value.get('trials') or [])):
                continue
        except (OSError, ValueError, TypeError, AttributeError):
            continue
        return value, candidate
    return None


def partial_native_checkpoint(batch: Path, task_ids: list[str], prior: dict | None = None):
    """Read completed native trial checkpoints from an interrupted batch.

    Scores and trajectories come exclusively from native atomic trial files.
    Unclosed or ungraded executions remain absent and must run normally.
    """
    expected = set(task_ids)
    prior_ids = {row['task_id'] for row in (prior or {}).get('trials') or []}
    candidates = [batch, *batch.glob('capture-retry-*')]
    candidates = sorted((path for path in candidates if (path/'started.json').is_file()
                         and not (path/'report.json').is_file()),
                        key=lambda path: (path/'started.json').stat().st_mtime_ns, reverse=True)
    for folder in candidates:
        started = controller.read(folder/'started.json')
        if not started.get('run_id') or not started.get('dataset_version') or started.get('complete') is not False:
            continue
        rows = []
        seen = set()
        for path in sorted((folder/'trials').glob('*.json')):
            row = controller.read(path)
            task = row.get('task_id')
            if task not in expected or task in seen or path.stem != row.get('trial_id'):
                raise ReviewIncomplete('training_partial_checkpoint_identity_mismatch')
            seen.add(task)
            if (row.get('execution_status') == 'completed' and not controller._trial_error_code(row)
                    and row.get('verdict') in ('passed', 'failed')
                    and not capture_recovery_required(row) and review_complete(row)
                    and not (row.get('evidence') or {}).get('artifacts')):
                rows.append(row)
        row_ids = {row['task_id'] for row in rows}
        if not prior_ids <= row_ids or not row_ids - prior_ids:
            continue
        if prior_ids:
            retained = {'complete': True, 'trials': [row for row in rows if row['task_id'] in prior_ids]}
            if not same_capture(prior, retained, list(prior_ids)):
                raise ReviewIncomplete('training_partial_checkpoint_changed_prior_execution')
        return folder, started, rows
    return None


def resume_partial_checkpoint(checkpoint, destination: Path, config: Path, env: dict):
    """Let native validate a temporary partial envelope and reuse exact trials."""
    import hashlib
    folder, started, rows = checkpoint
    settings = controller.read(config)
    metadata = {'splits': settings.get('splits', []), 'repetitions': settings.get('repetitions', 1),
        'parallelism': settings.get('parallelism', 1),
        'model_enabled': settings.get('judge', {}).get('enabled', True),
        'allow_unreviewed': False,
        'variants': [{key: variant[key] for key in ('id', 'version')} for variant in settings['variants']]}
    partial = {'schema_version': 1, 'report_kind': 'task_evaluation.v1',
        'dataset_version': started['dataset_version'], 'run_id': started['run_id'],
        'evaluator_version': ('task-evaluator-v2-goals' if settings.get('judge', {}).get('assessment_mode') == 'objectives'
                              else 'task-evaluator-v1'),
        'configuration': metadata, 'complete': False, 'trials': rows}
    # This is the existing native report/manifest protocol binding, temporary;
    # it introduces no persisted fingerprint or extra compatibility gate.
    with tempfile.TemporaryDirectory(prefix='.checkpoint-', dir=destination) as temporary:
        source = Path(temporary)/'report.json'
        write_json(source, partial)
        write_json(source.with_name('manifest.json'), {'run_id': started['run_id'],
            'dataset_version': started['dataset_version'], 'complete': False,
            'files': {'report.json': hashlib.sha256(source.read_bytes()).hexdigest()}})
        return resume_capture(source, destination, config, env, [])


def capture_batch(label: str, task_ids: list[str], artifact: dict, workers: int, env: dict):
    destination = CAPTURES / label
    source = destination / 'report.json'
    config = evaluation_config(task_ids, artifact, workers)
    config_path = CAPTURES / (label + '.json')
    write_json(config_path, config)
    evidence_root = CAPTURES / 'official-evidence'
    evidence_root.mkdir(parents=True, exist_ok=True)
    local_env = dict(env, TRAJECTORY_EVALUATION_PARALLELISM=str(workers),
                     APPWORLD_EVIDENCE_ROOT=str(evidence_root))
    # A repaired/regraded report is native evidence with its own manifest.
    # Reuse its existing sidecar reference rather than hand-merging old rows.
    metrics_path = destination / 'metrics.json'
    if metrics_path.is_file():
        try:
            previous_source = Path(controller.read(metrics_path).get('source_report', ''))
            if previous_source.is_file() and previous_source.with_name('manifest.json').is_file():
                previous_report = controller.read(previous_source)
                previous_rows = previous_report.get('trials') or []
                previous_ids = {row.get('task_id') for row in previous_rows}
                # A stopped repair may already have retained healthy trials.
                # Continue that native report instead of reverting to the old
                # incomplete source and repeating finished executions.
                if len(previous_ids) == len(previous_rows) and previous_ids <= set(task_ids):
                    source = previous_source
        except (OSError, ValueError, TypeError):
            pass
    report = controller.read(source) if source.is_file() else None
    reused = isinstance(report, dict) and capture_scope_complete(report, task_ids)
    if isinstance(report, dict):
        completed = complete_native_review(report, destination, task_ids)
        if completed is not None:
            report, source = completed
            reused = True
    partial = partial_native_checkpoint(destination, task_ids, report)
    if partial is not None:
        report, source = resume_partial_checkpoint(partial, destination, config_path, local_env)
        reused = False
    if report is None:
        report = controller.shared.capture(FULL_DATASET, config_path, destination, local_env)
    recovery = [row['trial_id'] for row in report.get('trials') or []
                if capture_recovery_required(row)]
    if recovery or not capture_scope_complete(report, task_ids):
        # Fresh output bypasses the shared helper's complete early-return.
        # Native preserves scored task failures and only regrades judge errors.
        report, source = resume_capture(source, destination, config_path, local_env, recovery)
        reused = False
    write_json(metrics_path, dict(controller.metrics(report.get('trials') or []),
                                  source_report=str(source)))
    if (not capture_scope_complete(report, task_ids)
            or any(capture_recovery_required(row) for row in report.get('trials') or [])):
        raise ReviewIncomplete('training_capture_recovery_incomplete')
    graded, graded_source = reviewed_source(report, source, task_ids, config_path, local_env)
    # Preserve native bytes and its manifest binding for cold-start import.
    # Metrics are a tiny sidecar; the authoritative native report is unchanged.
    write_json(destination / 'metrics.json', dict(controller.metrics(graded['trials']),
                                                   source_report=str(graded_source)))
    return graded, graded_source, reused


def learning_source(label: str, report: dict, source: Path, task_ids: list[str]):
    """Resume the original admitted learning source while scores may be regraded."""
    batch = CAPTURES / label
    admitted = set()
    for status_path in sorted(batch.rglob('learning-status.json')):
        try:
            provenance = controller.read(status_path).get('appworld_input') or {}
            original = provenance.get('original_report')
            if original:
                admitted.add(Path(original).resolve())
        except (OSError, ValueError, TypeError, AttributeError):
            continue
    if not admitted:
        return report, source
    if not capture_scope_complete(report, task_ids):
        raise ReviewIncomplete('training_learning_source_execution_mismatch')
    # An appended public-training exercise may extend an already admitted
    # batch. Every earlier native source must be a complete nested subset
    # whose executions, identities and costs remain exactly unchanged.
    expected = set(task_ids)
    scopes, originals = [], []
    for original in sorted(admitted):
        if not original.is_file() or not original.with_name('manifest.json').is_file():
            raise ReviewIncomplete('training_learning_source_native_pair_missing')
        original_report = controller.read(original)
        original_rows = original_report.get('trials') or []
        original_ids = {row.get('task_id') for row in original_rows}
        retained = dict(report, trials=[row for row in report.get('trials') or []
                                       if row.get('task_id') in original_ids])
        if (not original_ids or not original_ids <= expected
                or not capture_scope_complete(original_report, list(original_ids))
                or not same_capture(original_report, retained, list(original_ids))):
            raise ReviewIncomplete('training_learning_source_execution_mismatch')
        if any(not (previous <= original_ids or original_ids <= previous) for previous in scopes):
            raise ReviewIncomplete('training_learning_source_provenance_conflict')
        scopes.append(original_ids)
        originals.append((original_report, original, original_ids))
    # Preserve the original full-scope admission when only judging changed.
    for original_report, original, original_ids in originals:
        if original_ids == expected:
            return original_report, original
    # New tasks must come from the current complete native report+manifest,
    # never from a Python merge of earlier reports or edited trial scores.
    if (not capture_scope_complete(report, task_ids) or not source.is_file()
            or not source.with_name('manifest.json').is_file()
            or controller.read(source) != report):
        raise ReviewIncomplete('training_learning_source_execution_mismatch')
    return report, source


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--batch-size", type=int, default=16)
    parser.add_argument("--workers", type=int, default=16)
    parser.add_argument("--max-batches", type=int, default=0)
    parser.add_argument("--label-prefix", default="train-full",
                        help="Capture directory prefix; use a new prefix to preserve prior runs.")
    args = parser.parse_args()
    if args.batch_size <= 0 or args.workers <= 0:
        raise SystemExit("batch-size and workers must be positive")
    TRAINING.mkdir(parents=True, exist_ok=True)
    with (TRAINING / "full-training.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print(json.dumps({"status": "already_running"}))
            return 2
        # Refresh the runtime and L0 artifact, but do not alter any report or
        # learning evidence.  The official full scope is installed only in
        # this process and cannot leak into the development schedule.
        from dataset import ensure_training_augmentation, rows as dataset_rows, materialize
        ensure_training_augmentation()
        synthetic_ids = [row['task_id'] for row in dataset_rows('train')
                         if row.get('initial_state', {}).get('training_goal')]
        materialize(BINARY, view='native')
        controller.prepare()
        # Native semantic review sends the complete retained trace packet. It
        # is materially larger than the per-task judge packet used by report
        # regrading, so the old 120s shared HTTP budget frequently expired
        # every attempt as ``transport_28``. Keep this timeout scoped to the
        # learning process; Agent capture/evaluation budgets are unchanged.
        # The adapter divides its total deadline between retries. Long
        # complete-trace responses need the full deadline in one attempt;
        # durable native queue recovery retries failed work separately.
        # JSON transport escaping is larger than the bounded 2 MiB semantic
        # input. This private learning allowance does not expand Agent steps.
        # AppWorld-specific projection/taxonomy rules apply only to native
        # learning analysis; the agent's execution prompt remains unchanged.
        install_learning_runtime()
        task_ids = official_ids()
        build_dataset(task_ids)
        controller.DATASET = FULL_DATASET
        controller.FULL_TRAIN_IDS = task_ids
        artifact = controller.read(controller.PUBLISHED / "L0.json")
        env = controller.environment()
        batches = training_batches(task_ids, args.batch_size)
        expected_batches = len(batches)
        if args.max_batches:
            batches = batches[:args.max_batches]
        summary = {"status": "running", "scope": "official_and_public_synthetic_train", "planned_tasks": len(task_ids),
                   "official_planned_tasks": len(task_ids) - len(synthetic_ids),
                   "synthetic_planned_tasks": len(synthetic_ids),
                   "completed_batches": 0, "planned_batches": len(batches),
                   "workers": args.workers, "batch_size": args.batch_size, "imported_batches": 0,
                   "completed_tasks": 0, "deferred_reviews": [],
                   "dataset": str(FULL_DATASET), "started_at": time.time()}
        write_json(STATUS, summary)
        for number, batch in enumerate(batches, 1):
            label = f"{args.label_prefix}-{number:02d}"
            controller.state(phase="full_training_capture", batch=label,
                             full_training_completed_tasks=sum(len(x) for x in batches[:number - 1]),
                             full_training_planned_tasks=len(task_ids))
            try:
                report, path, reused = capture_batch(label, batch, artifact, args.workers, env)
            except ReviewIncomplete as exc:
                summary['deferred_reviews'].append({'batch': label, 'tasks': len(batch), 'reason': str(exc)})
                summary.update(last_batch=label, phase='review_deferred', updated_at=time.time())
                write_json(STATUS, summary)
                controller.state(phase='training_review_deferred', batch=label,
                                 training_deferred_reviews=len(summary['deferred_reviews']))
                continue
            except Exception as exc:
                summary.update(status="failed", last_batch=label,
                               last_error={"type": type(exc).__name__, "message": str(exc)},
                               updated_at=time.time())
                write_json(STATUS, summary)
                controller.state(status="full_training_failed", phase="full_training_capture",
                                  batch=label, full_training_error=type(exc).__name__)
                raise
            metrics = controller.metrics(report.get("trials", []))
            learning_report, learning_path = learning_source(label, report, path, batch)
            previous_learning = learning_path.parent / "learning-status.json"
            if reused and previous_learning.is_file():
                # A complete same-task batch can be reused only after its
                # native semantic review and curation reached a usable state.
                # Earlier runs were marked complete while the model review
                # contained transport failures; treating those as reusable
                # permanently stranded the batch in ``unresolved``.  Resume
                # the original report in that case.  The native importer is
                # idempotent for completed traces and preserves the captured
                # scores/events; only pending review/curation work is retried.
                prior = controller.read(previous_learning)
                if learning_engine.learning_usable(controller, prior, 0):
                    learning_result = {
                        "usable": True,
                        "progressed": learning_engine.learning_progressed(prior, 0),
                        "reused_learning_status": True,
                        "reason": "completed_batch_already_imported",
                    }
                else:
                    try:
                        learning_result = learning_engine.learn(controller, learning_report, learning_path, env)
                        learning_result["reused_capture_learning_retry"] = True
                    except Exception as exc:  # preserve capture and continue later batches
                        learning_result = {"usable": False,
                            "reason": "learning_exception:" + type(exc).__name__,
                            "error": str(exc), "reused_capture_learning_retry": True}
            else:
                try:
                    learning_result = learning_engine.learn(controller, learning_report, learning_path, env)
                except Exception as exc:  # preserve capture and continue later batches
                    learning_result = {"usable": False, "reason": "learning_exception:" + type(exc).__name__,
                                "error": str(exc)}
            if learning_result.get("usable") is True:
                summary["imported_batches"] += 1
            summary.update({"completed_batches": number,
                            "completed_tasks": summary["completed_tasks"] + len(batch),
                            "last_batch": label, "last_batch_metrics": metrics,
                            "last_learning": learning_result, "updated_at": time.time()})
            write_json(STATUS, summary)
        summary.update(status=("complete" if len(batches) == expected_batches and summary["imported_batches"] == expected_batches else "learning_incomplete" if len(batches) == expected_batches else "partial"),
                       finished_at=time.time())
        write_json(STATUS, summary)
        controller.state(status="full_training_complete" if summary["status"] == "complete" else "full_training_learning_incomplete", phase="full_training_import",
                          full_training_tasks=len(task_ids), full_training_batches=len(batches))
        print(json.dumps(summary, ensure_ascii=False))
        if summary["status"] != "complete" and not args.max_batches:
            return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
