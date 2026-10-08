#!/usr/bin/env python3
"""Small fixed AppWorld train/dev loop using real native capture and learning."""
from __future__ import annotations
import argparse
import ast
import errno
from concurrent.futures import ThreadPoolExecutor
from copy import deepcopy
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import time
from typing import Any

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
OUTPUT = ROOT / 'trace_output/appworld'
TRAINING, GEPA, PUBLISHED = [OUTPUT / stage for stage in ('training', 'gepa', 'published')]
ADAPTER = ROOT / 'examples/agent/business-simulator/appworld'
WORLD = ADAPTER / 'runtime/world'
RUNTIME = TRAINING / 'runtime'
TRAINING_SERVICE = RUNTIME / 'training-service.json'
DATASET = TRAINING / 'datasets/native'
PYTHON = ADAPTER / 'runtime/venv/bin/python'
HOST = ADAPTER / 'appworld_host.py'
# AppWorld runs use the freshly rebuilt runtime when available.  Keep the
# historical binary as a fallback so unrelated experiments remain untouched.
_REBUILT_BINARY = ROOT / 'tools/trajectory_evolution/build-v4/trajectory_evolution'
BINARY = _REBUILT_BINARY if _REBUILT_BINARY.is_file() else ROOT / 'tools/trajectory_evolution/build/trajectory_evolution'
AGENT, OWNER = 'appworld-agentflow', 'appworld-training'
# Native run state includes both request boundaries and the final trace.  This
# ceiling admits long public tasks without expanding their execution budget.
MAX_TRACE_BYTES = 64 * 1024 * 1024
# Full AppWorld reports retain every real execution trace. This is a storage
# ceiling only; per-task execution budgets and judge settings are unchanged.
FULL_SCOPE_REPORT_BYTES = 2 * 1024 * 1024 * 1024
# Whole-store semantic curation is optional; retain local semantic matching
# while avoiding a global sweep after every small training batch.
# Match the business training contract so AppWorld does not postpone full
# memory curation until the store is already too large.  These are runtime
# scheduling bounds; they do not alter admission or evaluation gates.
CURATION_NEW_ENTRY_THRESHOLD = 64
CURATION_REVIEW_MAX_ATTEMPTS = 2

# Share the business evaluator's readers without importing its runner or
# changing any business process. AppWorld adds capture-completeness checks.
_business_spec = importlib.util.spec_from_file_location(
    "_appworld_business_metrics", HERE.parent / "business-coverage/progress_report.py")
_business_metrics = importlib.util.module_from_spec(_business_spec)
_business_spec.loader.exec_module(_business_metrics)


def host_version():
    # Read the host's declared revision without importing its AppWorld runtime.
    for node in ast.parse(HOST.read_text()).body:
        if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id == 'HOST_VERSION'
                for target in node.targets):
            value = ast.literal_eval(node.value)
            if isinstance(value, str) and value:
                return value
    raise RuntimeError('appworld_host_revision_missing')


HOST_VERSION = host_version()
SCOPE = 'agent:appworld-agentflow/actor:default'
sys.path.insert(0, str(HERE.parent / 'tau2-retail-verified'))
import tau2_experiment as shared
sys.path.insert(0, str(HERE.parent))
from published_policy_transfer import native as _policy_native
read, save = shared.read, shared.save
# shared.capture is reused from the tau2 harness; bind it to this AppWorld CLI
# so deleting business published runtime artifacts cannot break AppWorld.
shared.BINARY = BINARY


def baseline_result(count: int, *, output_root: Path | None = None) -> Path:
    """Current execution-contract L0 family; keep historical results in place."""
    root = OUTPUT if output_root is None else Path(output_root)
    return root / 'gepa' / f'l0-current-stage{count}' / 'result.json'


def ids(label):
    from dataset import rows
    return [row['task_id'] for row in rows('val' if label == 'dev' else label)]


def environment():
    env = shared._environment()
    env.update(AGENTFLOW_PYTHON=str(PYTHON), APPWORLD_ROOT=str(WORLD),
        APPWORLD_MAX_STEPS='30', TRAJECTORY_UNCONDITIONAL_RESUME='1',
        TRAJECTORY_EVALUATION_PARALLELISM='16', NO_PROXY='*', no_proxy='*')
    env['PATH'] = str(PYTHON.parent) + os.pathsep + env.get('PATH', '')
    env.setdefault('EVOLUTION_APPWORLD_TOKEN', secrets.token_urlsafe(32))
    for key in ('HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY', 'http_proxy', 'https_proxy', 'all_proxy'):
        env.pop(key, None)
    return env



def native(binary, root, operation, value):
    """Retry only a CLI executable that failed before any process started."""
    for attempt in range(9):
        try:
            return _native_once(binary, root, operation, value)
        except OSError as exc:
            if (exc.errno not in (errno.EACCES, errno.ETXTBSY)
                    or exc.filename is None or Path(exc.filename) != Path(binary)
                    or attempt == 8):
                raise
            time.sleep(1)


def _native_once(binary, root, operation, value):
    """Private learn commands need the same principal environment as training."""
    control_operation = len(operation) == 2 and operation[0] == 'runtime' and operation[1] in ('control-input', 'control-compile')
    if not operation or (operation[0] != 'learn' and not control_operation):
        return _policy_native(binary, root, operation, value)
    proc = subprocess.run([str(binary), *operation], input=json.dumps(value, ensure_ascii=False),
        text=True, capture_output=True, cwd=root, env=environment() if operation[0] == 'learn' else None,
        timeout=120 if operation[0] == 'learn' else 30)
    if proc.returncode:
        reason = None
        try:
            reason = json.loads(proc.stdout).get('reason')
        except (ValueError, AttributeError):
            pass
        if not isinstance(reason, str):
            reason = proc.stderr.strip().removeprefix('ERROR: ').strip()
        # Only fixed native error codes may enter diagnostics; raw output and
        # credentials never become part of the persisted exception.
        import re
        if re.fullmatch(r'[a-z][a-z0-9_]{0,99}', reason or '') is None:
            reason = 'native_learning_failed'
        error = RuntimeError('native_learn_failed' if operation[0] == 'learn' else
                             'native_' + '_'.join(operation) + '_failed')
        error.native_reason = reason
        error.native_returncode = proc.returncode
        raise error
    return proc.stdout.strip()


def version(artifact):
    return native(BINARY, ROOT, ['artifact-version'], artifact)


def judge_projection(config: dict[str, Any] | None = None) -> dict[str, Any]:
    """Actual judge evidence settings shared by capture and retained regrading."""
    settings = {
        'evidence_field_bytes': 32768,
        'keep_causal_anchors': True,
        'keep_dispatch_bindings': True,
        'keep_outer_request_inputs': True,
        'require_dispatch_dependency': True,
        'require_state_dependency': True,
        'keep_dispatch_bindings_before_preview': True,
        'answer_field_bytes': 32768,
        'include_model_requests': False,
        'share_repeated_inputs': True,
        'share_repeated_text': True,
        'evidence_projection_command': [str(PYTHON), str(HOST), '--project-official-code'],
    }
    if config is None:
        return settings
    judge = config.get('judge') or {}
    return {key: judge.get(key) for key in settings}


def evaluation_config(task_ids, artifact, workers=4, *, service_path=None):
    config = shared._evaluation_config([], workers)
    config.update(splits=sorted({'train' if task in set(ids('train')) else 'val' for task in task_ids}),
        task_ids=task_ids, parallelism=workers, max_trials=len(task_ids), timeout_ms=1800000,
        max_trace_bytes=MAX_TRACE_BYTES)
    if len(task_ids) > 100:
        config['max_report_bytes'] = FULL_SCOPE_REPORT_BYTES
    variant = config['variants'][0]
    variant.update(id='candidate', version=version(artifact), artifact=artifact,
        command=[str(PYTHON), '-B', str(HOST), '--run-entry'],
        instrumentation_config=str(service_path or (RUNTIME / 'service.json')))
    if artifact.get('level') == 'L0' and artifact.get('policy') is None:
        # Native capture constructs this same baseline and records its canonical
        # loaded version. Learning imports expect the raw host revision here.
        variant['version'] = artifact['agent']['revision']
        variant.pop('artifact')
    variant['pass_env'] += ['APPWORLD_ROOT', 'APPWORLD_MAX_STEPS', 'APPWORLD_EVIDENCE_ROOT', 'EVOLUTION_APPWORLD_TOKEN']
    config['checks'] = {'*': [{'id': 'strict', 'path': '/output/appworld_reward', 'op': 'equals', 'value': 1.0}]}
    for key, filename in [('prompt', 'task-judge-system.txt'), ('loop_review_prompt', 'task-loop-review.txt'),
                          ('consistency_prompt', 'task-consistency.txt')]:
        config['judge'][key] = str(ROOT / 'tools/trajectory_evolution/config/prompts' / filename)
    # Keep AppWorld on the same judge contract as the business experiment.
    # The native task score remains the deterministic strict signal; the
    # enabled objectives judge supplies the same assessment, turn evidence,
    # and model-reviewed loop_review fields used by business training/GEPA.
    config['judge'].update(
        enabled=True,
        assessment_mode='objectives',
        max_batch_turns=4,
        max_evidence_lookups=2,
        # AppWorld traces contain many API/tool events.  Business training
        # uses a 2 MiB judge batch bound for the same objective/loop packet;
        # the old 256 KiB AppWorld value caused evaluation_judge_input_limit
        # and turned valid executions into unscored rows.
        batch_input_bytes=2097152,
        max_input_bytes=2097152,
        max_repair_attempts=2,
        goal_prompt=str(ROOT / 'tools/trajectory_evolution/config/prompts/task-goals.txt'),
        runtime_config=str(RUNTIME / 'runtime.json'),
    )
    config['judge'].update(judge_projection())
    # Objectives, loop review and both consistency checks can need bounded
    # format repairs. Reserve their full budget without changing Agent cost.
    config['judge']['max_model_calls'] = max(8 * len(task_ids), 8)
    config['artifact_retention'].update(retain_trial_files=True, retain_report_pb=False)
    return config


def prepare():
    global HOST_VERSION
    HOST_VERSION = host_version()
    env = environment()
    for directory in (TRAINING, GEPA, PUBLISHED, RUNTIME):
        directory.mkdir(parents=True, exist_ok=True)
    selection = read(HERE / 'datasets/manifest.json')
    if selection.get('selection_uses_scores') is not False or set(ids('train')) & set(ids('dev')):
        raise RuntimeError('selection_not_fixed_or_training_dev_overlap')
    runtime = read(ROOT / 'tools/trajectory_evolution/config/standard-runtime.json')
    def resolve_prompts(value):
        if isinstance(value, dict):
            return {key: resolve_prompts(item) for key, item in value.items()}
        if isinstance(value, list):
            return [resolve_prompts(item) for item in value]
        if isinstance(value, str) and value.startswith('prompts/'):
            return str(ROOT / 'tools/trajectory_evolution/config' / value)
        return value
    # Runtime parser requires relative prompt paths. Reuse the official files
    # through one directory link; never copy the prompt tree per experiment.
    prompt_directory = RUNTIME / 'prompts'
    if not prompt_directory.exists():
        prompt_directory.symlink_to(ROOT / 'tools/trajectory_evolution/config/prompts', target_is_directory=True)
    # AppWorld judge and learning packets can contain long multi-step traces.
    # Keep this bound scoped to the AppWorld runtime; business keeps its own
    # runtime profile.  The native adapter accepts up to 300 seconds.
    learning_timeout_ms = min(300000, max(1000, int(
        os.environ.get('APPWORLD_LEARNING_MODEL_TIMEOUT_MS', '300000'))))
    runtime['model'].update(endpoint=env['AF_LLM_BASE_URL'].rstrip('/') + '/chat/completions',
        model_id=env['AF_LLM_MODEL'], api_key_env='OPENAI_API_KEY', thinking_mode='disabled',
        timeout_ms=learning_timeout_ms, max_retries=4, max_output_tokens=16384,
        sessionlog_max_output_tokens=32768, json_object_output=True)
    runtime['evaluation']['require_chinese_reason'] = False
    # Keep semantic review parallelism aligned with the business cold-start
    # runner.  This is a runtime tuning value only; it does not change the
    # evidence or admission rules.
    # AppWorld training has one principal and one episode per official task.
    # Keep the same evidence/quality gates as business, but do not require a
    # second user/dialogue that this public benchmark does not provide.
    runtime['evolution'].update(require_chinese_reason=False, memory_review_parallelism=16,
                                min_supporting_dialogues=1, min_supporting_episodes=1,
                                full_curation_new_entry_threshold=CURATION_NEW_ENTRY_THRESHOLD,
                                memory_review_max_attempts=CURATION_REVIEW_MAX_ATTEMPTS)
    runtime_path = RUNTIME / 'runtime.json'
    preserve_grading_runtime = False
    if runtime_path.is_file():
        existing = read(runtime_path)
        existing_model = existing.get('model') or {}
        preserve_grading_runtime = (
            existing_model.get('endpoint') == runtime['model']['endpoint']
            and existing_model.get('model_id') == runtime['model']['model_id']
            and isinstance(existing.get('prompts'), dict)
            and bool(existing['prompts'])
            and all(isinstance(value, str) and not Path(value).is_absolute()
                    and '..' not in Path(value).parts and (RUNTIME / value).is_file()
                    for value in existing['prompts'].values()))
    if not preserve_grading_runtime:
        save(runtime_path, runtime)
    service = resolve_prompts(read(ROOT / 'tools/trajectory_evolution/config/boundary-service.json'))
    # ``service.json`` is the evaluation/GEPA boundary.  Training gets a
    # separate L0 service below, otherwise a shared max_level=L3 service can
    # accidentally apply a learned policy while collecting its own evidence.
    service.update(application_id=AGENT, data_dir=str(TRAINING / 'evolution'), port=0, workers=16,
        preserve_runtime_policy_state=True,
        max_run_bytes=MAX_TRACE_BYTES,
        max_level='L3', auto_evaluate=True, auto_approve_experiences=False,
        principals=[{'user_id': OWNER, 'token_env': 'EVOLUTION_APPWORLD_TOKEN'}],
        runtime_config=str(RUNTIME / 'runtime.json'),
        experience_template_file=str(ROOT / 'tools/trajectory_evolution/config/prompts/runtime-experience.txt'))
    service['instrumentation'] = {'level': 'L3', 'capabilities': ['model', 'actions', 'loop']}
    service['learning'].update(min_new_runs=1, min_interval_ms=0, max_batch_runs=32,
        max_batch_bytes=MAX_TRACE_BYTES, analysis_parallelism=16, max_analysis_retries=2, retry_backoff_ms=15000)
    # Evaluation consumes the frozen training store; it must not write new
    # cross-task experiences while scoring.  Training below enables the same
    # shared-memory stage used by business, so this is isolation of the
    # evaluation run rather than a second evolution flow.
    service['sharing']['enabled'] = False
    # This is an executable host contract, not a claim that a learned control
    # has already applied.  L0 capture never uses it.  Native GEPA can tighten
    # bounded recovery after repeated, structurally observed execution errors.
    # Successful empty observations may be mutating API calls and cannot prove
    # stagnation, so the base rule requires a real action error.
    service['runtime_options'].update(
        action_rules=[], schedule_rules=[],
        loop_rules=[{
            'rule_id': 'appworld-bounded-recovery', 'target_scope': SCOPE,
            'max_iterations': 30, 'max_no_progress': 3,
            'no_progress_decision': 'replan', 'max_replans': 1,
            'replan_context': (
                'The recent AppWorld execution repeated a documented failure. '
                'Inspect the original observation and API requirements, then '
                'change the code or arguments. Avoid repeating the failed block. '
                'Continue the remaining task within the original 30-step budget.'),
            'error_codes': ['appworld_code_error', 'appworld_type_error_positional',
                            'appworld_auth_401', 'appworld_validation_422',
                            'policy_action_rejected'],
            'reason': 'Bounded recovery from repeated observed AppWorld action failure',
        }])
    # Generation is enabled by offline_gepa only after its evaluation file is
    # configured.  Plain training/capture cannot start an evaluation manager.
    service['control_learning'] = {
        'enabled': False,
        'prompt_file': str(ROOT / 'tools/trajectory_evolution/config/prompts/control-candidates.txt'),
        'actions': {'appworld.execute': ['code']}, 'max_rules': 8,
        'min_iterations': 2, 'min_no_progress': 1,
        'max_attempts': 2, 'retry_backoff_ms': 15000,
    }
    save(RUNTIME / 'service.json', service)
    # Training uses the same L0 capture and 16-way learning contract as the
    # business pipeline. Evaluation/GEPA keeps the separate service so L1-L3
    # policy injection is never accidentally enabled during data collection.
    training_service = deepcopy(service)
    training_service.update(max_level='L0', workers=16)
    training_service['instrumentation'] = {
        'level': 'L0', 'capabilities': ['model', 'actions', 'loop']}
    training_service['learning'].update(min_new_runs=4, min_interval_ms=0,
        max_batch_runs=32, analysis_parallelism=16, max_analysis_retries=2,
        retry_backoff_ms=15000)
    training_service['sharing'].update(enabled=True, min_interval_ms=1000,
                                       min_supporting_users=1,
                                       max_model_calls_per_cycle=32, model_parallelism=16)
    save(TRAINING_SERVICE, training_service)
    baseline = {'schema_version': 1, 'agent': {'name': AGENT, 'revision': HOST_VERSION},
                'user_id': OWNER, 'level': 'L0', 'policy': None}
    save(PUBLISHED / 'L0.json', baseline)
    plan = {'scope': 'fixed_official_train_to_small_dev', 'independent_holdout': False,
        'train_ids': ids('train'), 'dev_ids': ids('dev'), 'parallelism': 16,
        'host_version': HOST_VERSION, 'baseline_version': version(baseline),
        'official_strict': 'world.evaluate().success', 'quality_auxiliary': 'official passed_tests/total_tests; not TGC',
        'loop_basis': 'model_review',
        'semantic_loop_diagnostic': 'semantic_runtime_lower_bound',
        'intent_labels': None, 'api_calls_in_prepare': 0,
        'experience_support_policy': 'single_user_single_episode_appworld_v1'}
    save(RUNTIME / 'plan.json', plan)
    for label, selected, workers in [('train-preflight', ids('train')[:1], 1),
                                      ('dev-preflight', ids('dev')[:1], 1),
                                      ('dev-L0', ids('dev'), 2)]:
        service_path = TRAINING_SERVICE if label.startswith('train-') else None
        save(RUNTIME / (label + '.json'),
             evaluation_config(selected, baseline, workers, service_path=service_path))
    for number, start in enumerate(range(0, len(ids('train')), 12), 1):
        save(RUNTIME / f'train-{number:02d}.json',
             evaluation_config(ids('train')[start:start + 12], baseline,
                              service_path=TRAINING_SERVICE))
    return plan


def output(trial):
    return shared._terminal_output(trial)


def framework_capture_complete(trial: dict[str, Any]) -> bool:
    """New observer failures are unmeasurable; historical rows stay unchanged."""
    terminal = output(trial)
    if 'framework_capture' not in terminal:
        return True
    observed = terminal['framework_capture']
    return isinstance(observed, dict) and str(observed.get('status') or '').lower() == 'complete'


def assessment(trial: dict[str, Any]) -> dict[str, Any] | None:
    value = (trial.get('evidence') or {}).get('assessment')
    return value if isinstance(value, dict) else None


def assessed_quality(trial: dict[str, Any]) -> float | None:
    """Read the same effective objectives quality as the business evaluator."""
    if _trial_error_code(trial) is not None:
        return None
    value = assessment(trial)
    if not value:
        return None
    score = _number(value.get('quality'))
    if score is None or not 0 <= score <= 1:
        return None
    # Historical assessments may omit these fields; an explicit incomplete
    # or contradictory new assessment is never a formal score.
    if 'coverage' in value:
        coverage = _number(value['coverage'])
        if coverage is None or not 1 - 1e-12 <= coverage <= 1:
            return None
    if 'quality_upper' in value:
        upper = _number(value['quality_upper'])
        if upper is None or not score <= upper <= 1:
            return None
    return _business_metrics._quality(trial)


def _number(value: Any) -> float | None:
    return _business_metrics._number(value)


_INVALID_LOOP_STATUSES = _business_metrics._INVALID_LOOP_STATUSES
_VALID_LOOP_STATUSES = _business_metrics._VALID_LOOP_STATUSES
_EXECUTION_ERROR_STATUSES = _business_metrics._EXECUTION_ERROR_STATUSES


def _trial_error_code(trial: dict[str, Any]) -> str | None:
    if not framework_capture_complete(trial):
        return 'appworld_framework_capture_incomplete'
    return _business_metrics._trial_error_code(trial)


def _judge_error_code(trial: dict[str, Any]) -> str | None:
    raw = trial.get('error_code')
    if isinstance(raw, str) and raw.strip().startswith('evaluation_judge_'):
        return raw.strip()
    for check in trial.get('checks') or []:
        if not isinstance(check, dict) or check.get('check_id') != 'model':
            continue
        if str(check.get('status') or '').lower() == 'error':
            return str(check.get('reason') or 'evaluation_check_error').strip()[:256]
    return None


def _execution_error_code(trial: dict[str, Any]) -> str | None:
    """Separate infrastructure failures from the auxiliary official reward."""
    if not framework_capture_complete(trial):
        return 'appworld_framework_capture_incomplete'
    raw = trial.get('error_code')
    if isinstance(raw, str) and raw.strip() and not raw.strip().startswith('evaluation_judge_'):
        return raw.strip()
    status = str(trial.get('execution_status') or '').strip().lower()
    if status in _EXECUTION_ERROR_STATUSES:
        return f'execution_{status}'
    if str(trial.get('verdict') or '').lower() == 'error' and _judge_error_code(trial) is None:
        return 'trial_error'
    return None


def _deterministic_scored(trial: dict[str, Any], value: dict[str, Any]) -> bool:
    return (value.get('appworld_reward') in (0, 1)
            and _execution_error_code(trial) is None)


def review_measurable(review: Any) -> bool:
    """Accept a complete native review, retaining explicit zero and unknown."""
    if not isinstance(review, dict) or str(review.get('status') or '').lower() not in _VALID_LOOP_STATUSES:
        return False
    count = _number(review.get('count'))
    wasted = review.get('wasted_iterations')
    if count is None:
        count = float(len(wasted)) if isinstance(wasted, list) else _number(wasted)
    if count is None or count < 0 or int(count) != count:
        return False
    if isinstance(wasted, list) and len(wasted) != count:
        return False
    if 'observed_iterations' in review:
        observed = _number(review['observed_iterations'])
        if observed is None or observed < 1 or int(observed) != observed or count > observed:
            return False
        rows = review.get('iteration_reviews')
        if rows is not None:
            if not isinstance(rows, list) or len(rows) != observed:
                return False
            if any(not isinstance(row, dict) or row.get('decision') not in ('keep', 'waste') for row in rows):
                return False
            if sum(row['decision'] == 'waste' for row in rows) != count:
                return False
            identifiers = [row.get('iteration_id') for row in rows]
            if any(identifiers) and (not all(identifiers) or len(set(identifiers)) != len(rows)):
                return False
    return True


def _structured_invalid_loops(trial: dict[str, Any]) -> float | None:
    """Read the AppWorld host's factual semantic loop count from run.end."""
    sources = [trial]
    trace = (trial.get('evidence') or {}).get('trace')
    if isinstance(trace, dict):
        sources.extend(event.get('output') for event in trace.get('events', [])
                       if isinstance(event, dict) and event.get('kind') == 'run.end')
    for source in sources:
        if not isinstance(source, dict) or source.get('invalid_loop_basis') != 'semantic_runtime':
            continue
        value = _number(source.get('invalid_loop_count'))
        if value is not None and value >= 0 and int(value) == value:
            return value
    return None


def _invalid_loops(trial: dict[str, Any]) -> float | None:
    """Return the formal model-review loop count; host semantics are diagnostic-only.

    The host semantic counter is deterministic telemetry, but it is a lower
    bound and does not inspect the full coordinator/skill round. Business and
    AppWorld comparisons therefore use the same model-review contract.
    """
    if _trial_error_code(trial) is not None:
        return None
    review = (trial.get('evidence') or {}).get('loop_review')
    if isinstance(review, dict):
        if not review_measurable(review):
            return None
        return _business_metrics._invalid_loops(trial)
    if trial.get('invalid_loop_basis') == 'model_review':
        value = _business_metrics._invalid_loops(trial)
        return value if value is not None and value >= 0 and int(value) == value else None
    return None


def _has_complete_loop_review(trial: dict[str, Any]) -> bool:
    review = (trial.get('evidence') or {}).get('loop_review')
    return review_measurable(review) and _invalid_loops(trial) is not None


def _trace_complete(trial: dict[str, Any]) -> bool:
    """Whether the durable row contains a complete validated execution trace."""
    if not framework_capture_complete(trial):
        return False
    evidence = trial.get('evidence') or {}
    trace = evidence.get('trace')
    if not isinstance(trace, dict) or not trace.get('events'):
        return False
    integrity = (evidence.get('trajectory') or {}).get('trace_integrity')
    if isinstance(integrity, dict) and integrity.get('valid') is False:
        return False
    return any(event.get('kind') == 'run.end' for event in trace.get('events', []))


class RegradeError(RuntimeError):
    """A native regrade failure with bounded diagnostics for resumable runs."""

    def __init__(self, returncode: int, stderr: str, stdout: str = ''):
        self.returncode = returncode
        self.stderr = stderr[-4000:]
        self.stdout = stdout[-2000:]
        super().__init__(f'regrade_failed:{returncode}:{self.stderr[-500:]}')


def regrade_report(report: Path, dataset: Path, config: Path | dict[str, Any], output_root: Path,
                   *, env: dict[str, str] | None = None, recheck: bool = False,
                   parallelism: int = 16) -> dict[str, Any]:
    """Regrade retained AppWorld traces without executing the Agent.

    The C++ ``--regrade-report`` path rejects disabled judges and reads the
    saved evidence directly.  A derived config enables only the model judge;
    the caller's native capture config is never modified.
    """
    if not report.is_file():
        raise FileNotFoundError(report)
    source = read(config) if isinstance(config, Path) else deepcopy(config)
    source['parallelism'] = min(max(1, int(source.get('parallelism') or parallelism)),
                                max(1, int(parallelism)))
    judge = source.setdefault('judge', {})
    judge['enabled'] = True
    # Retained AppWorld traces contain substantially more API/tool evidence
    # than the original small development reports.  Regrading with the old
    # 256 KiB default silently turns every row into an unmeasurable judge
    # result; use the same bounded 2 MiB packet as fresh AppWorld capture.
    judge['batch_input_bytes'] = max(int(judge.get('batch_input_bytes') or 0), 2097152)
    judge['max_input_bytes'] = max(int(judge.get('max_input_bytes') or 0), 2097152)
    judge.update(judge_projection())
    judge['max_repair_attempts'] = max(int(judge.get('max_repair_attempts') or 0), 2)
    judge['max_model_calls'] = max(int(judge.get('max_model_calls') or 0),
                                   4 * len(read(report).get('trials', [])))
    # objectives mode loads this prompt even when capture used an absolute
    # judge prompt. The derived config lives beside the output directory, so
    # the native default would otherwise resolve to a missing relative path.
    prompts = ROOT / 'tools/trajectory_evolution/config/prompts'
    judge['goal_prompt'] = str(prompts / 'task-goals.txt')
    if judge.get('prompt'):
        prompt = Path(judge['prompt'])
        judge['prompt'] = str(prompt if prompt.is_absolute() else prompts / prompt.name)
    for key in ('loop_review_prompt', 'consistency_prompt'):
        if judge.get(key):
            prompt = Path(judge[key])
            judge[key] = str(prompt if prompt.is_absolute() else prompts / prompt.name)
    output_root = output_root.resolve()
    if output_root.exists():
        raise FileExistsError(output_root)
    output_root.parent.mkdir(parents=True, exist_ok=True)
    config_path = output_root.with_name(output_root.name + '.regrade.config.json')
    save(config_path, source)
    command = [str(BINARY), 'dataset', 'run', str(dataset), '--config', str(config_path),
               '--output-root', str(output_root), '--regrade-report', str(report)]
    if recheck:
        command.append('--recheck')
    grading_env = dict(env if env is not None else environment())
    grading_env['TRAJECTORY_EVALUATION_PARALLELISM'] = str(source['parallelism'])
    completed = subprocess.run(command, cwd=ROOT, env=grading_env,
                               text=True, capture_output=True, check=False)
    if completed.returncode not in (0, 1, 3):
        raise RegradeError(completed.returncode, completed.stderr, completed.stdout)
    result = output_root / 'report.json'
    if not result.is_file():
        raise RegradeError(completed.returncode,
                           completed.stderr + f'\nregrade_report_missing:{result}',
                           completed.stdout)
    return read(result)


def trace_level_counts(evidence: dict[str, Any]) -> tuple[dict[str, int], bool]:
    """Count actual applied native receipts, deduplicated within their run.

    Like the business report, attribution uses the receipt boundary. Policy
    levels and the task.status aggregate are capabilities, not applications.
    A complete validated trace establishes a measured zero when no receipt
    was applied; partial or malformed telemetry remains unavailable.
    """
    counts = {level: 0 for level in ('L1', 'L2', 'L2_action', 'L2_schedule', 'L3')}
    trace = evidence.get('trace') if isinstance(evidence, dict) else None
    if not isinstance(trace, dict):
        return counts, False
    events = trace.get('events')
    if not isinstance(events, list):
        return counts, False
    complete = _trace_complete({'evidence': evidence})
    if (trace.get('attributes') or {}).get('capture_complete') is False:
        complete = False
    seen = set()
    run_id = (trace.get('identity') or {}).get('run_id')
    for event in events:
        if not isinstance(event, dict) or event.get('kind') != 'advice.applied':
            continue
        receipt = ((event.get('attributes') or {}).get('receipt') or {})
        application = event.get('application') or receipt.get('application') or {}
        status = application.get('status')
        boundary = str(receipt.get('boundary') or event.get('boundary') or '').lower()
        level = {'model': 'L1', 'llm': 'L1', 'actions': 'L2', 'action': 'L2',
                 'schedule': 'L2', 'loop': 'L3'}.get(boundary)
        if not level or not isinstance(status, str):
            complete = False
            continue
        event_id = event.get('event_id')
        if event_id:
            key = (run_id, event_id)
        else:
            reference = (event.get('execution_id'), event.get('sequence'), event.get('step_id'))
            if not any(reference):
                complete = False
                continue
            key = (run_id, *reference, boundary)
        if key in seen:
            continue
        seen.add(key)
        if status != 'applied':
            continue
        counts[level] += 1
        if boundary in ('actions', 'action'):
            counts['L2_action'] += 1
        elif boundary == 'schedule':
            counts['L2_schedule'] += 1
    return counts, complete


def metrics(trials, planned=None):
    """Business metrics over the same task rows; official scores stay auxiliary."""
    trials = list(trials)
    values = [(row, output(row)) for row in trials]
    planned = len(trials) if planned is None else planned
    mean = _business_metrics._mean
    quality = [value for row in trials if (value := assessed_quality(row)) is not None]
    loops = [value for row in trials if (value := _invalid_loops(row)) is not None]
    errors = [_trial_error_code(row) for row in trials]
    quality_complete = bool(trials) and len(trials) >= planned and not any(errors) and len(quality) == len(trials)
    loop_complete = bool(trials) and len(trials) >= planned and not any(errors) and len(loops) == len(trials)
    valid = [row for row in trials if _trial_error_code(row) is None]
    verdicts = [row.get('verdict') for row in valid if row.get('verdict') in ('passed', 'failed', 'rejected')]
    strict = sum(value == 'passed' for value in verdicts) if verdicts else None
    resources = {name: [value for row in trials if (value := _business_metrics._metric(row, key)) is not None]
                 for name, key in [('tokens', 'tokens'), ('model_calls', 'llm_calls'), ('native_loop_count', 'loop_count')]}
    latency = [value for row in trials if (value := _number(row.get('duration_ms'))) is not None]
    official = [(row, value) for row, value in values if _deterministic_scored(row, value)]
    official_quality = [score for _, value in official if (score := _number(value.get('quality_score'))) is not None]
    from collections import defaultdict
    groups = defaultdict(list)
    for row, value in official:
        groups[row.get('group_id') or row['task_id'].rsplit('_', 1)[0]].append(value['appworld_reward'])
    complete_groups = [successes for successes in groups.values() if len(successes) == 3]
    intent = [value for row in valid if (value := _business_metrics._intent_counts(row)) is not None]
    tp, fp, fn = (sum(value[index] for value in intent) for index in range(3)) if intent else (0, 0, 0)
    intent_turn_keys = sorted({f"{row.get('task_id', '')}#{int(item['turn'])}" for row in valid
                               for item in _business_metrics._measured_intent_rows(row)})
    measured_levels = [trace_level_counts(row.get('evidence') or {}) if framework_capture_complete(row)
                       else ({level: 0 for level in ('L1', 'L2', 'L2_action', 'L2_schedule', 'L3')}, False)
                       for row in trials]
    # Applications describe the observed trials, just like resource totals.
    # Unstarted tasks cannot invalidate fully captured, closed L0 receipts.
    # Missing/partial telemetry within an observed trial still stays unknown.
    levels_complete = bool(measured_levels) and all(complete for _, complete in measured_levels)
    observed_iterations = [_number(review.get('observed_iterations')) for row in trials
                           if _has_complete_loop_review(row)
                           and isinstance(review := (row.get('evidence') or {}).get('loop_review'), dict)]
    observed_iterations = [value for value in observed_iterations if value is not None]
    tokens_complete = bool(trials) and len(trials) >= planned and len(resources['tokens']) == len(trials) \
        and all(framework_capture_complete(row) and value.get('tokens_complete') is not False for row, value in values)
    return {
        'planned': planned, 'completed': len(trials), 'scored': len(quality),
        'errored': sum(error is not None for error in errors), 'unscored': len(trials) - len(quality),
        'missing': max(0, planned - len(trials)), 'judged': len(verdicts),
        'strict_passed': strict, 'strict_pass_rate': strict / len(verdicts) if verdicts else None,
        'quality': mean(quality), 'quality_coverage': len(quality), 'quality_complete': quality_complete,
        'quality_status': 'complete' if quality_complete else ('partial' if quality else 'unavailable'),
        'official_scored': len(official), 'official_strict_passed': sum(value['appworld_reward'] == 1 for _, value in official),
        'task_success_rate': mean([value['appworld_reward'] for _, value in official]),
        'scenario_success_rate_complete_groups': mean([float(all(successes)) for successes in complete_groups]),
        'complete_scenario_groups': len(complete_groups), 'official_quality': mean(official_quality),
        'auxiliary_test_fraction': mean(official_quality),
        'judge_errors': sum(_judge_error_code(row) is not None for row in trials),
        'execution_errors': sum(_execution_error_code(row) is not None for row in trials),
        'tokens': mean(resources['tokens']), 'model_calls': mean(resources['model_calls']),
        'llm_calls': mean(resources['model_calls']), 'latency_ms': mean(latency),
        'total_tokens': sum(resources['tokens']) if resources['tokens'] else None,
        'total_model_calls': sum(resources['model_calls']) if resources['model_calls'] else None,
        'known_tokens': sum(_number(value.get('known_agent_tokens')) or 0 for _, value in values),
        'tokens_complete': tokens_complete, 'token_covered_trials': len(resources['tokens']),
        'model_call_covered_trials': len(resources['model_calls']), 'latency_covered_trials': len(latency),
        'invalid_loop': mean(loops), 'avg_invalid_loop': mean(loops),
        'invalid_loop_coverage': len(loops),
        'invalid_loop_status': 'complete' if loop_complete else ('partial' if loops else 'unavailable'),
        'loop_basis': ('model_review' if loops else 'model_review_unavailable'),
        'loop_observed_iterations': sum(observed_iterations) if loops and len(observed_iterations) == len(loops) else None,
        'loop_observed_coverage': len(observed_iterations),
        'native_loop_count': mean(resources['native_loop_count']), 'native_loop_coverage': len(resources['native_loop_count']),
        'intent_f1': 2 * tp / (2 * tp + fp + fn) if intent and (2 * tp + fp + fn) else None,
        'intent_counts': [tp, fp, fn] if intent else None,
        'intent_measured': len(intent_turn_keys), 'intent_task_measured': len(intent), 'intent_turn_keys': intent_turn_keys,
        'level_count_basis': 'trace advice.applied by boundary' if levels_complete else 'unavailable',
        'level_covered_trials': sum(complete for _, complete in measured_levels),
        'levels': {level: sum(counts[level] for counts, _ in measured_levels) if levels_complete else None
                   for level in ('L1', 'L2', 'L2_action', 'L2_schedule', 'L3')},
    }


def compact_metrics(items, planned=None):
    """Compatibility adapter to the shared business aggregation contract."""
    return _business_metrics.compact_metrics(items, planned)


def capture(label, config, env, resume=None):
    destination = TRAINING / 'captures' / label if label.startswith('train') else GEPA / 'captures' / label
    existing = destination / 'report.json'
    if existing.is_file():
        report = read(existing)
        expected = set(read(config).get('task_ids') or [])
        rows = report.get('trials') or []
        if (report.get('complete') is True and len(rows) == len(expected)
                and {row.get('task_id') for row in rows} == expected
                and all(assessed_quality(row) is not None and _has_complete_loop_review(row) for row in rows)):
            return report, existing
    local_env = dict(env, TRAJECTORY_EVALUATION_PARALLELISM=str(read(config)['parallelism']),
        APPWORLD_EVIDENCE_ROOT=str(destination.parent / 'official-evidence'))
    report = shared.capture(DATASET, config, destination, local_env, resume_report=resume)
    save(destination / 'metrics.json', metrics(report.get('trials', [])))
    return report, destination / 'report.json'


def learn(report, path, env):
    import learning
    return learning.learn(sys.modules[__name__], report, path, env)


def freeze_seed():
    import learning
    return learning.freeze_seed(sys.modules[__name__])


def state(**fields):
    path = TRAINING / 'status.json'
    value = read(path) if path.exists() else {}
    value.update(fields, updated_at=time.time(), pid=os.getpid())
    save(path, value)



def main():
    parser = argparse.ArgumentParser(description='Prepare AppWorld native runtime configuration.')
    parser.add_argument('--prepare', action='store_true', required=True)
    parser.parse_args()
    print(json.dumps(prepare(), ensure_ascii=False))


if __name__ == '__main__':
    main()
