#!/usr/bin/env python3
"""Small fixed AppWorld train/dev loop using real native capture and learning."""
from __future__ import annotations
import argparse
import ast
from concurrent.futures import ThreadPoolExecutor
from copy import deepcopy
import fcntl
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
OUTPUT = ROOT / 'trace_output/appworld'
TRAINING, GEPA, PUBLISHED = [OUTPUT / stage for stage in ('training', 'gepa', 'published')]
RUNTIME = HERE / 'runtime/training'
DATASET = HERE / 'datasets/native'
PYTHON = HERE / 'runtime/venv/bin/python'
HOST = HERE / 'appworld_host.py'
BINARY = ROOT / 'trace_output/business-simulator/published/seed004/runtime/trajectory_evolution'
AGENT, OWNER = 'appworld-agentflow', 'appworld-training'


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
from published_policy_transfer import native
read, save = shared.read, shared.save


def ids(label):
    return ['appworld-' + value.removeprefix('appworld-')
            for value in read(HERE / 'datasets/selection.json')[label + '_ids']]


def environment():
    env = shared._environment()
    env.update(AGENTFLOW_PYTHON=str(PYTHON), APPWORLD_ROOT=str(HERE / 'runtime/world'),
        APPWORLD_MAX_STEPS='30', TRAJECTORY_UNCONDITIONAL_RESUME='1',
        TRAJECTORY_EVALUATION_PARALLELISM='4', NO_PROXY='*', no_proxy='*')
    env['PATH'] = str(PYTHON.parent) + os.pathsep + env.get('PATH', '')
    env.setdefault('EVOLUTION_APPWORLD_TOKEN', secrets.token_urlsafe(32))
    for key in ('HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY', 'http_proxy', 'https_proxy', 'all_proxy'):
        env.pop(key, None)
    return env


def version(artifact):
    return native(BINARY, ROOT, ['artifact-version'], artifact)


def evaluation_config(task_ids, artifact, workers=4):
    config = shared._evaluation_config([], workers)
    config.update(splits=sorted({'train' if task in set(ids('train')) else 'val' for task in task_ids}),
        task_ids=task_ids, parallelism=workers, max_trials=len(task_ids), timeout_ms=1800000)
    variant = config['variants'][0]
    variant.update(id='candidate', version=version(artifact), artifact=artifact,
        command=[str(PYTHON), '-B', str(HOST), '--run-entry'],
        instrumentation_config=str(RUNTIME / 'service.json'))
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
    config['judge']['runtime_config'] = str(RUNTIME / 'runtime.json')
    config['judge']['enabled'] = False
    config['artifact_retention'].update(retain_trial_files=True, retain_report_pb=False)
    return config


def prepare():
    global HOST_VERSION
    HOST_VERSION = host_version()
    env = environment()
    for directory in (TRAINING, GEPA, PUBLISHED, RUNTIME):
        directory.mkdir(parents=True, exist_ok=True)
    selection = read(HERE / 'datasets/selection.json')
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
    runtime['model'].update(endpoint=env['AF_LLM_BASE_URL'].rstrip('/') + '/chat/completions',
        model_id=env['AF_LLM_MODEL'], api_key_env='OPENAI_API_KEY', thinking_mode='disabled',
        timeout_ms=120000, max_retries=4, max_output_tokens=16384, sessionlog_max_output_tokens=16384)
    runtime['evaluation']['require_chinese_reason'] = False
    runtime['evolution'].update(require_chinese_reason=False, memory_review_parallelism=4)
    save(RUNTIME / 'runtime.json', runtime)
    service = resolve_prompts(read(ROOT / 'tools/trajectory_evolution/config/boundary-service.json'))
    service.update(application_id=AGENT, data_dir=str(TRAINING / 'evolution'), port=0, workers=8,
        max_level='L0', auto_evaluate=True, auto_approve_experiences=False,
        principals=[{'user_id': OWNER, 'token_env': 'EVOLUTION_APPWORLD_TOKEN'}],
        runtime_config=str(RUNTIME / 'runtime.json'),
        experience_template_file=str(ROOT / 'tools/trajectory_evolution/config/prompts/runtime-experience.txt'))
    service['instrumentation'] = {'level': 'L0', 'capabilities': ['model', 'actions', 'loop']}
    service['learning'].update(min_new_runs=1, min_interval_ms=0, max_batch_runs=12,
        max_batch_bytes=33554432, analysis_parallelism=4, max_analysis_retries=2, retry_backoff_ms=15000)
    service['sharing']['enabled'] = False
    service['runtime_options'].update(action_rules=[], loop_rules=[], schedule_rules=[])
    save(RUNTIME / 'service.json', service)
    baseline = {'schema_version': 1, 'agent': {'name': AGENT, 'revision': HOST_VERSION},
                'user_id': OWNER, 'level': 'L0', 'policy': None}
    save(PUBLISHED / 'L0.json', baseline)
    plan = {'scope': 'fixed_official_train_to_small_dev', 'independent_holdout': False,
        'train_ids': ids('train'), 'dev_ids': ids('dev'), 'parallelism': 4,
        'host_version': HOST_VERSION, 'baseline_version': version(baseline),
        'official_strict': 'world.evaluate().success', 'quality_auxiliary': 'official passed_tests/total_tests; not TGC',
        'loop_basis': 'native rule_lower_bound', 'intent_labels': None, 'api_calls_in_prepare': 0}
    save(RUNTIME / 'plan.json', plan)
    for label, selected, workers in [('train-preflight', ids('train')[:1], 1),
                                      ('dev-preflight', ids('dev')[:1], 1),
                                      ('dev-L0', ids('dev'), 2)]:
        save(RUNTIME / (label + '.json'), evaluation_config(selected, baseline, workers))
    for number, start in enumerate(range(0, len(ids('train')), 12), 1):
        save(RUNTIME / f'train-{number:02d}.json', evaluation_config(ids('train')[start:start + 12], baseline))
    return plan


def output(trial):
    return shared._terminal_output(trial)


def metrics(trials):
    values = [(row, output(row)) for row in trials]
    scored = [(row, value) for row, value in values if value.get('appworld_reward') in (0, 1) and not value.get('error_code')]
    average = lambda values: sum(values) / len(values) if values else None
    full_costs = all(value.get('tokens_complete') is True and isinstance(value.get('tokens'), (int, float)) for _, value in values)
    from collections import defaultdict
    groups = defaultdict(list)
    for row, value in scored:
        groups[row.get('group_id') or row['task_id'].rsplit('_', 1)[0]].append(value['appworld_reward'])
    complete_groups = [successes for successes in groups.values() if len(successes) == 3]
    loops = [row.get('evidence', {}).get('trajectory', {}).get('summary', {}).get('invalid_loop_lower_bound')
             for row, _ in scored]
    known_loops = [value for value in loops if isinstance(value, (int, float))]
    calls = [value.get('model_calls') for _, value in values]
    return {'completed': len(values), 'scored': len(scored), 'errored': len(values) - len(scored),
        'strict_passed': sum(value['appworld_reward'] == 1 for _, value in scored),
        'task_success_rate': average([value['appworld_reward'] for _, value in scored]),
        'scenario_success_rate_complete_groups': average([float(all(successes)) for successes in complete_groups]),
        'complete_scenario_groups': len(complete_groups),
        'auxiliary_test_fraction': average([value['quality_score'] for _, value in scored]),
        'tokens': sum(value['tokens'] for _, value in values) if values and full_costs else None,
        'known_tokens': sum(value.get('known_agent_tokens', 0) or 0 for _, value in values),
        'token_covered_trials': sum(value.get('tokens_complete') is True for _, value in values),
        'model_calls': sum(calls) if calls and all(isinstance(value, (int, float)) for value in calls) else None,
        'latency_ms': average([row['duration_ms'] for row, _ in values if isinstance(row.get('duration_ms'), (int, float))]),
        'avg_invalid_loop': average(known_loops), 'invalid_loop_coverage': len(known_loops),
        'loop_basis': 'native rule_lower_bound', 'intent_f1': None,
        'levels': {level: sum(value[level.lower() + '_applied'] for _, value in values)
            if values and all(isinstance(value.get(level.lower() + '_applied'), (int, float)) for _, value in values)
            else None for level in ('L1', 'L2', 'L3')}}


def capture(label, config, env, resume=None):
    destination = TRAINING / 'captures' / label if label.startswith('train') else GEPA / 'captures' / label
    existing = destination / 'report.json'
    if 'preflight' in label and existing.is_file() and metrics(read(existing).get('trials', []))['scored'] == 0:
        number = 1
        archive = destination.with_name(destination.name + '-infra-attempt-' + str(number))
        while archive.exists():
            number += 1
            archive = destination.with_name(destination.name + '-infra-attempt-' + str(number))
        destination.rename(archive)
    local_env = dict(env, TRAJECTORY_EVALUATION_PARALLELISM=str(read(config)['parallelism']),
        APPWORLD_EVIDENCE_ROOT=str(destination.parent / 'official-evidence'))
    report = shared.capture(DATASET, config, destination, local_env, resume_report=resume)
    save(destination / 'metrics.json', metrics(report.get('trials', [])))
    return report, destination / 'report.json'


def learn(report, path, env):
    import appworld_learning
    return appworld_learning.learn(sys.modules[__name__], report, path, env)


def freeze_seed():
    import appworld_learning
    return appworld_learning.freeze_seed(sys.modules[__name__])


def state(**fields):
    path = TRAINING / 'status.json'
    value = read(path) if path.exists() else {}
    value.update(fields, updated_at=time.time(), pid=os.getpid())
    save(path, value)


def run(preflight_train=None, preflight_dev=None):
    import appworld_schedule
    return appworld_schedule.run(sys.modules[__name__], preflight_train, preflight_dev)


def evaluate_seed0(baseline_report=None):
    """Evaluate the first real frozen Seed0 while further learning is separate."""
    baseline_path = baseline_report or GEPA / 'captures/dev-L0/report.json'
    with (TRAINING / 'controller.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        artifact_path = PUBLISHED / 'seed0/strategy.json'
        while not artifact_path.is_file():
            state(status='running', phase='waiting_for_learned_seed0', artifact=str(artifact_path))
            time.sleep(5)
        artifact = read(artifact_path)
        baseline = read(PUBLISHED / 'L0.json')
        if (artifact.get('agent') != baseline.get('agent')
                or artifact.get('user_id') != baseline.get('user_id')
                or artifact.get('level') != 'L1'
                or not artifact.get('policy', {}).get('snapshot', {}).get('experiences')):
            raise RuntimeError('learned_seed0_identity_or_evidence_invalid')
        plan = read(RUNTIME / 'plan.json')
        if plan['host_version'] != host_version():
            raise RuntimeError('frozen_appworld_host_changed')
        baseline_result = read(baseline_path)
        config = RUNTIME / 'dev-Seed0.json'
        save(config, evaluation_config(ids('dev'), artifact, 12))
        state(status='running', phase='seed0_dev_comparison', version=version(artifact),
              parallelism={'Seed0': 12, 'L0': 0})
        result, result_path = capture('dev-Seed0', config, environment())
        reports = {'L0': (baseline_result, baseline_path), 'Seed0': (result, result_path)}
        results = {name: {'report': str(path), 'metrics': metrics(report.get('trials', []))}
                   for name, (report, path) in reports.items()}
        valid = {name: {row['task_id']: row for row in report.get('trials', [])
                       if row.get('execution_status') == 'completed'
                       and output(row).get('appworld_reward') in (0, 1)
                       and not output(row).get('error_code')}
                 for name, (report, _) in reports.items()}
        paired = sorted(set(valid['L0']) & set(valid['Seed0']))
        comparison = {'scope': plan['scope'], 'independent_holdout': False,
                      'results': results, 'paired_tasks': len(paired),
                      'paired_metrics': {name: metrics([valid[name][task] for task in paired])
                                         for name in ('L0', 'Seed0')}}
        save(GEPA / 'seed0-comparison.json', comparison)
        state(status='seed0_evaluated', phase='gap_review', results=results,
              paired_tasks=len(paired))
        return comparison


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepare', action='store_true')
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--status', action='store_true')
    parser.add_argument('--evaluate-seed0', action='store_true')
    parser.add_argument('--baseline-report', type=Path)
    parser.add_argument('--train-preflight-report', type=Path)
    parser.add_argument('--dev-preflight-report', type=Path)
    args = parser.parse_args()
    if args.status:
        print(json.dumps(read(TRAINING / 'status.json') if (TRAINING / 'status.json').exists() else {'status': 'not_started'}))
    elif args.evaluate_seed0:
        print(json.dumps(evaluate_seed0(args.baseline_report), ensure_ascii=False))
    elif args.run:
        run(args.train_preflight_report, args.dev_preflight_report)
    elif args.prepare:
        print(json.dumps(prepare()))
    else:
        parser.error('choose --prepare, --run, or --status')


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        state(status='failed', error_type=type(error).__name__, error=str(error))
        raise
