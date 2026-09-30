#!/usr/bin/env python3
"""Frozen final AppWorld L0: eight small reports, concurrency 32 then 64."""
from __future__ import annotations

import argparse
from collections import defaultdict
import fcntl
import json
from pathlib import Path
import subprocess
import time

import appworld_experiment as c

SELECTION = c.HERE / 'datasets/final500-selection.json'
DATASET = c.HERE / 'datasets/final500-native'
RUNTIME = c.HERE / 'runtime/final500'
PUBLISHED = c.PUBLISHED / 'final-500'
OUTPUT = c.GEPA / 'final-500-L0'
HEALTH = c.TRAINING / 'rpc-health-v3/evaluation/report.json'


def state(**fields):
    OUTPUT.mkdir(parents=True, exist_ok=True)
    path = OUTPUT / 'status.json'
    value = c.read(path) if path.exists() else {}
    value.update(fields, updated_at=time.time())
    c.save(path, value)


def prepare():
    selection = c.read(SELECTION)
    raw_ids = selection['final_ids']
    task_ids = ['appworld-' + value.removeprefix('appworld-') for value in raw_ids]
    excluded = {'appworld-' + value.removeprefix('appworld-') for value in c.read(c.HERE / 'datasets/selection.json')['dev_ids']}
    if (len(task_ids) != 500 or len(set(task_ids)) != 500 or set(task_ids) & excluded
            or selection.get('selection_uses_scores') is not False):
        raise RuntimeError('final500_frozen_scope_invalid')
    if c.host_version() != 'appworld-agentflow-v3':
        raise RuntimeError('final500_requires_frozen_v3_host')
    RUNTIME.mkdir(parents=True, exist_ok=True)
    PUBLISHED.mkdir(parents=True, exist_ok=True)
    baseline = {'schema_version': 1, 'agent': {'name': c.AGENT, 'revision': c.host_version()},
                'user_id': c.OWNER, 'level': 'L0', 'policy': None}
    c.save(PUBLISHED / 'L0.json', baseline)
    version = c.version(baseline)
    shards, start = [], 0
    for index in range(8):
        count = 63 if index < 4 else 62
        selected = task_ids[start:start + count]
        start += count
        config = c.evaluation_config(selected, baseline, 8)
        config.update(splits=['holdout'], max_trials=count)
        path = RUNTIME / ('shard-' + str(index) + '.json')
        c.save(path, config)
        shards.append({'index': index, 'tasks': count, 'task_ids': selected, 'config': str(path)})
    plan = {'scope': 'frozen_final500', 'independent_holdout': True,
            'selection': str(SELECTION), 'dataset': str(DATASET), 'tasks': 500,
            'host_version': c.host_version(), 'baseline_version': version,
            'initial_concurrency': 32, 'maximum_concurrency': 64,
            'per_task_agent_decisions': 30, 'judge_enabled': False,
            'official_score': 'unmodified world.evaluate()', 'shards': shards}
    c.save(RUNTIME / 'plan.json', plan)
    return plan


def healthy(plan):
    if not HEALTH.is_file():
        return False
    report = c.read(HEALTH)
    rows = report.get('trials', [])
    if not report.get('complete') or len(rows) != 1:
        return False
    row, value = rows[0], c.output(rows[0])
    return (row.get('execution_status') == 'completed'
            and row.get('evidence', {}).get('loaded_version') == plan['baseline_version']
            and value.get('appworld_reward') in (0, 1)
            and not value.get('error_code')
            and value.get('degraded_boundary_points', 0) == 0)


def summarize(records):
    values = list(records.values())
    metrics = [value['metrics'] for value in values]
    scored = [value for value in values if value['metrics']['scored']]
    avg = lambda values: sum(values) / len(values) if values else None
    groups = defaultdict(list)
    for value in scored:
        groups[value['group_id']].append(value['reward'])
    complete_groups = [rewards for rewards in groups.values() if len(rewards) == 3]
    return {'completed': len(values), 'planned': 500, 'scored': len(scored),
            'errored': len(values) - len(scored),
            'strict_passed': sum(value['metrics']['strict_passed'] for value in values),
            'scored_only_success_rate': avg([value['reward'] for value in scored]),
            'full_terminal_success_rate_lower_bound': sum(value['metrics']['strict_passed'] for value in values) / len(values) if values else None,
            'native_score_coverage': len(scored) / len(values) if values else None,
            'scored_only_auxiliary_test_fraction': avg([value['metrics']['auxiliary_test_fraction'] for value in scored]),
            'scenario_success_rate_complete_groups': avg([float(all(rewards)) for rewards in complete_groups]),
            'complete_scenario_groups': len(complete_groups),
            'latency_ms': avg([value['latency_ms'] for value in metrics if value['latency_ms'] is not None]),
            'tokens': sum(value['tokens'] for value in metrics) if metrics and all(value['tokens'] is not None for value in metrics) else None,
            'known_tokens': sum(value['known_tokens'] for value in metrics),
            'token_covered_trials': sum(value['token_covered_trials'] for value in metrics),
            'model_calls': sum(value['model_calls'] for value in metrics) if metrics and all(value['model_calls'] is not None for value in metrics) else None,
            'avg_invalid_loop': avg([value['metrics']['avg_invalid_loop'] for value in scored if value['metrics']['avg_invalid_loop'] is not None]),
            'loop_basis': 'native rule_lower_bound', 'intent_f1': None,
            'levels': {level: sum(value['levels'][level] for value in metrics)
                       if metrics and all(value['levels'][level] is not None for value in metrics) else None
                       for level in ('L1', 'L2', 'L3')}}


def run():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    with (OUTPUT / 'runner.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if (OUTPUT / 'DISPATCH_PAUSED').exists():
            state(status='paused', phase='dispatch_paused', pause_marker=str(OUTPUT / 'DISPATCH_PAUSED'))
            return
        while not SELECTION.is_file() or not (DATASET / 'manifest.json').is_file():
            state(status='running', phase='waiting_for_frozen_dataset', completed=0)
            time.sleep(2)
        plan = prepare()
        while not healthy(plan):
            state(status='running', phase='waiting_for_v3_health', baseline_version=plan['baseline_version'])
            time.sleep(5)
        env = c.environment()
        env.update(TRAJECTORY_EVALUATION_PARALLELISM='8',
                   APPWORLD_EVIDENCE_ROOT=str(OUTPUT / 'official-evidence'))
        processes, records, checkpoints, completed, pending = {}, {}, {}, set(), list(plan['shards'])
        maximum_shards, ramped = 4, False

        def collect(row, source, stamp):
            task_id = row['task_id']
            expected = {task for shard in plan['shards'] for task in shard['task_ids']}
            if task_id not in expected or row.get('variant_id') != 'candidate':
                raise RuntimeError('final500_trial_outside_frozen_scope')
            loaded = row.get('evidence', {}).get('loaded_version')
            if loaded is not None and loaded != plan['baseline_version']:
                raise RuntimeError('final500_loaded_version_mismatch')
            value = c.output(row)
            records[task_id] = {'task_id': task_id, 'group_id': row.get('group_id') or task_id.rsplit('_', 1)[0],
                'trial_id': row['trial_id'], 'source': str(source), 'mtime': stamp,
                'reward': value.get('appworld_reward'), 'error_code': value.get('error_code') or row.get('error_code'),
                'degraded_boundary_points': value.get('degraded_boundary_points', 0), 'metrics': c.metrics([row])}

        def start(shard):
            index = shard['index']
            destination = OUTPUT / ('shard-' + str(index))
            report_path = destination / 'report.json'
            if report_path.is_file() and c.read(report_path).get('complete'):
                report = c.read(report_path)
                for row in report['trials']:
                    collect(row, report_path, report_path.stat().st_mtime_ns)
                completed.add(index)
                return
            if destination.exists():
                raise RuntimeError('incomplete_shard_retained_resume_requires_native_report:' + str(index))
            command = [str(c.BINARY), 'dataset', 'run', str(DATASET), '--config', shard['config'],
                       '--output-root', str(destination), '--allow-unreviewed']
            log = (OUTPUT / ('shard-' + str(index) + '.log')).open('a')
            proc = subprocess.Popen(command, cwd=c.ROOT, env=env, stdout=log,
                                    stderr=subprocess.STDOUT, start_new_session=True)
            processes[index] = {'process': proc, 'log': log, 'destination': destination, 'tasks': shard['tasks']}
            print(json.dumps({'event': 'native_shard_started', 'index': index, 'pid': proc.pid,
                              'tasks': shard['tasks'], 'parallelism': 8}), flush=True)

        while pending or processes:
            while pending and len(processes) < maximum_shards and not (OUTPUT / 'DISPATCH_PAUSED').exists():
                start(pending.pop(0))
            for index, entry in list(processes.items()):
                for path in (entry['destination'] / 'trials').glob('*.json'):
                    stamp = path.stat().st_mtime_ns
                    if checkpoints.get(path) == stamp:
                        continue
                    try:
                        collect(c.read(path), path, stamp)
                    except (OSError, ValueError):
                        continue
                    checkpoints[path] = stamp
                code = entry['process'].poll()
                if code is not None:
                    entry['log'].close()
                    report_path = entry['destination'] / 'report.json'
                    if report_path.is_file():
                        report = c.read(report_path)
                        for row in report.get('trials', []):
                            collect(row, report_path, report_path.stat().st_mtime_ns)
                    else:
                        state(native_report_errors={str(index): {'exit_code': code, 'evidence_preserved': True}})
                    completed.add(index)
                    del processes[index]
            recent = sorted(records.values(), key=lambda value: value['mtime'])[-16:]
            if (not ramped and len(recent) == 16
                    and all(value['metrics']['errored'] == 0 and not value['error_code']
                            and value['degraded_boundary_points'] == 0 for value in recent)):
                maximum_shards, ramped = 8, True
                print(json.dumps({'event': 'concurrency_ramp', 'from': 32, 'to': 64,
                                  'healthy_completed_window': 16}), flush=True)
            metrics = summarize(records)
            state(status='running', phase='final500_baseline', metrics=metrics,
                  active_workers=len(processes) * 8, concurrency_limit=maximum_shards * 8,
                  native_pids={str(index): entry['process'].pid for index, entry in processes.items()},
                  finished_shards=sorted(completed), baseline_version=plan['baseline_version'])
            if processes:
                time.sleep(10)
            elif pending and (OUTPUT / 'DISPATCH_PAUSED').exists():
                state(status='paused', phase='dispatch_paused', metrics=metrics, active_workers=0)
                return
        metrics = summarize(records)
        result = {'scope': plan['scope'], 'independent_holdout': True,
                  'version': plan['baseline_version'], 'host_version': plan['host_version'],
                  'selection': str(SELECTION), 'metrics': metrics, 'items': list(records.values()),
                  'status': 'complete' if len(records) == 500 and metrics['scored'] == 500 else 'incomplete',
                  'execution_complete': len(records) == 500,
                  'scores_used_for_training_or_selection': False}
        c.save(OUTPUT / 'result.json', result)
        state(status=result['status'], phase='final500_baseline_finished', metrics=metrics,
              active_workers=0, native_pids={})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--prepare', action='store_true')
    parser.add_argument('--status', action='store_true')
    args = parser.parse_args()
    if args.run:
        run()
    elif args.status:
        print(json.dumps(c.read(OUTPUT / 'status.json') if (OUTPUT / 'status.json').exists() else {'status': 'not_started'}))
    else:
        print(json.dumps(prepare()))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        state(status='failed', error_type=type(error).__name__, error=str(error))
        raise
