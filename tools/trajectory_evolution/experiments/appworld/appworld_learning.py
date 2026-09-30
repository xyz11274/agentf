"""Native AppWorld training import and explicit, evidence-backed Seed0 release.

Original reports and official scores remain untouched. This helper never runs
the Agent or uses development trials as training evidence.
"""
from __future__ import annotations

from copy import deepcopy
import fcntl
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time


def learning_usable(controller, result, returncode=0):
    """The disabled shared-memory manager legitimately stays idle."""
    if not isinstance(result, dict) or not result or returncode not in (0, 3):
        return False
    if (result.get('appworld_model_analysis_failures')
            and not result.get('appworld_successful_model_analysis_runs')):
        return False
    service = controller.read(controller.RUNTIME / 'service.json')
    if service.get('sharing', {}).get('enabled', False):
        return controller.shared.learning_result_usable(result, returncode)
    if result.get('shared_status') not in ('idle', 'completed', 'completed_with_unresolved'):
        return False
    if result.get('agent_evaluation_error'):
        return False
    outcomes = result.get('run_outcomes')
    if not isinstance(outcomes, dict) or not outcomes:
        return False
    if any(isinstance(value, bool) or not isinstance(value, (int, float)) or value < 0
           for value in outcomes.values()):
        return False
    if not sum(outcomes.values()):
        return False
    for name in ('pending_runs', 'unpublished_runs', 'training_pending_users',
                 'memory_pending_reviews', 'agent_evaluation_pending_jobs',
                 'shared_pending_users', 'training_failed_users',
                 'memory_management_failed_users', 'shared_quarantined_users'):
        value = result.get(name, 0)
        if isinstance(value, bool) or not isinstance(value, (int, float)) or value != 0:
            return False
    managers = result.get('memory_management', {})
    if not isinstance(managers, dict):
        return False
    for manager in managers.values():
        if not isinstance(manager, dict) or manager.get('status') in (
                'pending', 'running', 'failed', 'waiting_for_model') or manager.get('pending', 0):
            return False
    return True


def _redact(text, env):
    for key, value in env.items():
        if value and any(word in key.upper() for word in ('KEY', 'TOKEN', 'PASSWORD')):
            text = text.replace(value, '[redacted]')
    return text


def _readiness(controller):
    """Fail before a model call when a runtime prompt is unavailable."""
    service_path = controller.RUNTIME / 'service.json'
    service = controller.read(service_path)
    if service.get('application_id') != controller.AGENT:
        raise RuntimeError('appworld_learning_agent_mismatch')
    if not any(value.get('user_id') == controller.OWNER for value in service.get('principals', [])):
        raise RuntimeError('appworld_learning_owner_mismatch')
    expected = controller.TRAINING / 'evolution'
    data = Path(service['data_dir'])
    if not data.is_absolute():
        data = service_path.parent / data
    if data.resolve() != expected.resolve() or service.get('memory_source_dir'):
        raise RuntimeError('appworld_learning_requires_fresh_private_store')
    runtime_path = Path(service['runtime_config'])
    if not runtime_path.is_absolute():
        runtime_path = service_path.parent / runtime_path
    runtime = controller.read(runtime_path)
    for prompt in runtime.get('prompts', {}).values():
        path = Path(prompt)
        if not path.is_absolute():
            path = runtime_path.parent / path
        if not path.is_file():
            raise RuntimeError('appworld_learning_prompt_missing')
    return expected / 'by-user' / controller.OWNER / 'memory/evolution-memory.json'


def learn(controller, report, path, env):
    """Serialize all writers; a busy native store is not a failed experiment."""
    started = time.monotonic()
    with (controller.TRAINING / 'learning-writer.lock').open('a') as lock:
        while True:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                controller.state(phase='waiting_for_learning_writer',
                                 learning_wait_seconds=round(time.monotonic() - started))
                time.sleep(5)
        while True:
            result = _learn_unlocked(controller, report, path, env)
            if result.get('reason') != 'data_directory_already_in_use':
                return result
            # Also tolerate a writer started before this shared lock existed.
            # This exit happened before import or any model analysis.
            controller.state(phase='waiting_for_native_learning_writer',
                             learning_wait_seconds=round(time.monotonic() - started))
            time.sleep(5)


def _learn_unlocked(controller, report, path, env):
    _readiness(controller)
    train_ids = set(controller.ids('train'))
    if any(row['task_id'] not in train_ids for row in report.get('trials', [])):
        raise RuntimeError('non_training_evidence_cannot_enter_memory')
    eligible = deepcopy(report)
    admitted = []
    baseline = controller.read(controller.PUBLISHED / 'L0.json')
    if baseline.get('level') != 'L0' or baseline.get('policy') is not None:
        raise RuntimeError('appworld_training_requires_original_l0')
    loaded_version = controller.version(baseline)
    host_revision = baseline.get('agent', {}).get('revision')
    variant_revisions = {value['id']: value['version']
                         for value in report.get('configuration', {}).get('variants', [])}
    for row in eligible.get('trials', []):
        result = controller.output(row)
        if (row.get('execution_status') != 'completed'
                or result.get('appworld_reward') not in (0, 1)
                or result.get('error_code') or row.get('verdict') not in ('passed', 'failed')):
            row['verdict'] = 'error'
            continue
        evidence = row.get('evidence') or {}
        if evidence.get('loaded_version') != loaded_version:
            raise RuntimeError('appworld_training_loaded_baseline_mismatch')
        if variant_revisions.get(row.get('variant_id')) not in (loaded_version, host_revision):
            raise RuntimeError('appworld_training_declared_baseline_mismatch')
        traces = evidence.get('traces') or [evidence.get('trace')]
        if not traces or any(not isinstance(trace, dict) for trace in traces):
            raise RuntimeError('scored_training_trace_missing')
        for trace in traces:
            identity = trace.get('identity', {})
            if identity.get('user_id') != controller.OWNER or identity.get('agent_id') != controller.AGENT:
                raise RuntimeError('appworld_training_trace_owner_or_agent_mismatch')
            if not any(event.get('kind') == 'run.start'
                       and event.get('attributes', {}).get('agent_version_id') == loaded_version
                       and event.get('attributes', {}).get('evaluation_trial_id') == row['trial_id']
                       for event in trace.get('events', [])):
                raise RuntimeError('appworld_training_trace_baseline_binding_mismatch')
        admitted.append(row['task_id'])
    if not admitted:
        return {'usable': False, 'admissible_trials': 0, 'reason': 'no_scored_training_trace'}
    # Native cold-start import interprets configuration.version as the host
    # revision and constructs its L0 artifact itself. Explicit-artifact capture
    # stores a canonical hash there instead. Adapt only this derived import
    # metadata after proving the actual original artifact and run bindings.
    # All task scores, events, execution versions and canonical files stay intact.
    metadata_bridge = False
    for value in eligible.get('configuration', {}).get('variants', []):
        if value.get('version') == loaded_version:
            value['version'] = host_revision
            metadata_bridge = True
    eligible['trials'] = [row for row in eligible['trials'] if row.get('verdict') != 'error']
    temporary = path.parent / '.learning-input'
    if temporary.exists():
        shutil.rmtree(temporary)
    temporary.mkdir()
    controller.save(temporary / 'report.json', eligible)
    manifest = deepcopy(controller.read(path.parent / 'manifest.json'))
    manifest.setdefault('files', {})['report.json'] = hashlib.sha256(
        (temporary / 'report.json').read_bytes()).hexdigest()
    controller.save(temporary / 'manifest.json', manifest)
    learning_input = temporary
    provenance = {'original_report': str(path), 'original_loaded_version': loaded_version,
                  'native_import_host_revision': host_revision,
                  'configuration_version_bridge': metadata_bridge,
                  'admitted_training_tasks': admitted,
                  'original_scores_and_events_preserved': True}
    local_env = dict(env, NO_PROXY='*', no_proxy='*')
    for name in ('HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY', 'http_proxy', 'https_proxy', 'all_proxy'):
        local_env.pop(name, None)
    command = [str(controller.BINARY), 'learn', '--config', str(controller.RUNTIME / 'service.json'),
               '--training-evaluation', str(learning_input), '--dataset', str(controller.DATASET)]
    started = time.monotonic()
    # A broken scheduler must not block the controller forever. Pending work
    # remains in its native queue and can be resumed without re-running tasks.
    limit = int(env.get('APPWORLD_LEARNING_TIMEOUT_SECONDS', '900'))
    proc = subprocess.Popen(command, cwd=controller.ROOT, env=local_env, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        while True:
            try:
                stdout, stderr = proc.communicate(timeout=5)
                break
            except subprocess.TimeoutExpired:
                controller.state(phase='learning', learning_elapsed_seconds=round(time.monotonic() - started),
                                 learning_tasks=len(admitted))
                if time.monotonic() - started >= limit:
                    proc.terminate()
                    try:
                        stdout, stderr = proc.communicate(timeout=10)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        stdout, stderr = proc.communicate()
                    controller.save(path.parent / 'learning-status.json', {
                        'status': 'learning_timeout', 'elapsed_seconds': round(time.monotonic() - started),
                        'admissible_trials': len(admitted), 'resume_native_queue': True,
                        'appworld_input': provenance})
                    with (path.parent / 'learning.log').open('a') as stream:
                        stream.write(_redact(stderr[-16384:], local_env))
                    return {'usable': False, 'admissible_trials': len(admitted), 'reason': 'learning_timeout'}
        try:
            result = json.loads(stdout)
        except ValueError:
            result = {'status': 'native_learning_status_missing', 'exit_code': proc.returncode}
            if 'data_directory_already_in_use' in stderr:
                result['error_code'] = 'data_directory_already_in_use'
        result['appworld_input'] = provenance
        failed_analysis, successful_analysis = [], []
        identities = set()
        for row in eligible['trials']:
            evidence = row.get('evidence') or {}
            for trace in evidence.get('traces') or [evidence.get('trace')]:
                identity = trace['identity']
                identities.add((identity['conversation_id'], identity['run_id']))
        for conversation_id, run_id in sorted(identities) if proc.returncode in (0, 3) else []:
            evidence_path = (controller.TRAINING / 'evolution/by-user' / controller.OWNER /
                             'evaluation' / conversation_id / (run_id + '.evaluation.json'))
            if not evidence_path.is_file():
                continue  # Native import/queue status handles unfinished work.
            analysis = controller.read(evidence_path)
            requested, failed = (int(analysis.get(key, 0)) for key in
                                 ('model_requested_count', 'model_failed_count'))
            if requested > 0 and failed >= requested:
                failed_analysis.append({'run_id': run_id, 'conversation_id': conversation_id,
                    'model_requested_count': requested, 'model_failed_count': failed,
                    'evidence': str(evidence_path)})
            elif requested > failed:
                successful_analysis.append(run_id)
        if failed_analysis:
            result['appworld_model_analysis_failures'] = failed_analysis
        result['appworld_successful_model_analysis_runs'] = successful_analysis
        controller.save(path.parent / 'learning-status.json', result)
        with (path.parent / 'learning.log').open('a') as stream:
            stream.write(_redact(stderr[-16384:], local_env))
        return {'usable': learning_usable(controller, result, proc.returncode),
                'admissible_trials': len(admitted), 'elapsed_seconds': round(time.monotonic() - started),
                'reason': ('partial_model_analysis_failure' if failed_analysis and successful_analysis else
                           'learning_model_failed' if failed_analysis else
                           result.get('error_code', result.get('status')))}
    finally:
        if proc.poll() is None:
            proc.terminate()
        if temporary and temporary.exists():
            shutil.rmtree(temporary)


def freeze_seed(controller, *, label='seed0', memory_path=None):
    if label not in ('seed0', 'seed-pilot'):
        raise ValueError('appworld_seed_label_invalid')
    canonical_memory = _readiness(controller)
    memory = canonical_memory
    if memory_path is not None:
        memory = Path(memory_path).resolve()
        if not memory.is_relative_to(controller.TRAINING.resolve()):
            raise RuntimeError('appworld_evidence_memory_outside_training')
    if not memory.is_file():
        raise RuntimeError('appworld_learned_memory_missing')
    identity = {'user_id': controller.OWNER, 'agent_id': controller.AGENT,
                'conversation_id': 'evaluation-version', 'run_id': 'evaluation-version'}
    shadow = {'target_scopes': [controller.SCOPE], 'max_policy_entries': 64, 'max_advice_items': 2,
              'max_advice_bytes': 2048, 'max_query_bytes': 32768, 'max_memory_bytes': 16777216,
              'max_snapshot_bytes': 262144, 'max_match_terms': 128, 'min_matched_terms': 2,
              'require_validated_evidence': True}
    snapshot = json.loads(controller.native(controller.BINARY, controller.ROOT, ['shadow', 'compile'],
        {'memory_path': str(memory), 'identity': identity, 'options': shadow}))
    experiences = snapshot.get('experiences', [])
    # Control-only evidence cannot produce a useful L1 prompt strategy.
    prompt_experiences = [item for item in experiences
                          if item.get('control_target_type') in (None, '', 'NONE', 0)]
    diagnostic = {'seed_label': label, 'memory_source': str(memory), 'experience_count': len(experiences),
                  'canonical_memory_source': str(canonical_memory),
                  'prompt_experience_count': len(prompt_experiences),
                  'excluded_counts': snapshot.get('excluded_counts', {}),
                  'scope': controller.SCOPE, 'validated_evidence_required': True,
                  'published': False}
    controller.save(controller.TRAINING / 'seed-compile-status.json', diagnostic)
    if not prompt_experiences:
        raise RuntimeError('no_validated_learned_prompt_experience_for_seed0')
    if any(item.get('target_scope') != controller.SCOPE for item in prompt_experiences):
        raise RuntimeError('appworld_compiled_experience_scope_mismatch')
    options = {'requested_level': 'L1', 'host_capabilities': ['context.request', 'action.filter', 'loop.decide'],
               'fallback': 'strict', 'allowed_users': [controller.OWNER],
               'approved_experiences': [{key: item[key] for key in ('memory_id', 'content_revision')}
                                        for item in prompt_experiences],
               'experience_template': (controller.ROOT / 'tools/trajectory_evolution/config/prompts/runtime-experience.txt').read_text(),
               'max_context_bytes': 2048, 'request_context_pointer': '/system', 'request_context_mode': 'text'}
    policy = json.loads(controller.native(controller.BINARY, controller.ROOT, ['runtime', 'compile'],
        {'memory_path': str(memory), 'identity': identity, 'shadow_options': shadow, 'options': options}))
    if not policy.get('snapshot', {}).get('experiences'):
        raise RuntimeError('appworld_runtime_policy_empty')
    artifact = controller.read(controller.PUBLISHED / 'L0.json')
    artifact.update(level='L1', policy=policy)
    target = controller.PUBLISHED / label / 'strategy.json'
    if target.exists() and controller.read(target) != artifact:
        raise RuntimeError('frozen_seed0_cannot_be_overwritten')
    controller.save(target, artifact)
    admitted_tasks = set()
    status_paths = list((controller.TRAINING / 'captures').glob('train*/learning-status.json'))
    status_paths += list((controller.TRAINING / 'preflight').glob('train/learning-status.json'))
    for status_path in status_paths:
        status = controller.read(status_path)
        if learning_usable(controller, status, 0):
            admitted_tasks.update(status.get('appworld_input', {}).get('admitted_training_tasks', []))
    controller.save(controller.PUBLISHED / label / 'manifest.json', {
        'version': controller.version(artifact), 'experience_count': len(prompt_experiences),
        'memory_source': str(memory), 'training_input_tasks': sorted(admitted_tasks),
        'canonical_memory_source': str(canonical_memory),
        'planned_training_count': len(controller.ids('train')),
        'dev_tasks': controller.ids('dev'), 'scope': 'fresh_appworld_training_to_fixed_dev',
        'independent_holdout': False, 'validated_evidence_required': True})
    controller.save(controller.RUNTIME / ('dev-Seed0.json' if label == 'seed0' else 'dev-seed-pilot.json'),
                    controller.evaluation_config(controller.ids('dev'), artifact, 2))
    diagnostic['published'] = True
    controller.save(controller.TRAINING / 'seed-compile-status.json', diagnostic)
    return artifact
