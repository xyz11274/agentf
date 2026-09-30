#!/usr/bin/env python3
"""Check the frozen 500-task release offline; --run explicitly replays all 500."""
from __future__ import annotations

import argparse
from collections import Counter
from copy import deepcopy
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess

import best_strategy as best
import build_scoped_business_300 as cohort
import combine_training_replay as combined
import offline_gepa
import progress_report as progress

PUB = best.OUTPUT / 'published/seed004'
ADD = PUB / 'training-200'
BINARY = PUB / 'runtime/trajectory_evolution'
DATASET = PUB / 'business-acceptance-500.jsonl'
CONFIG = PUB / 'evaluation-500.json'
read, save = best._read, best._atomic_json


def prepare(workers=16):
    manifest = read(PUB / 'manifest.json')
    original = read(PUB / 'baseline-cache/manifest300.json')
    addition = read(ADD / 'selection.json')
    parts = [(PUB / 'business-acceptance-300.jsonl', original['task_ids'],
              original['dataset_sha256']),
             (ADD / 'selected-training-200.jsonl', addition['task_ids'],
              addition['selected_sha256'])]
    lines, rows = [], []
    for path, expected_ids, expected_hash in parts:
        raw = path.read_bytes()
        selected_lines = [line for line in raw.splitlines() if line.strip()]
        selected = [json.loads(line) for line in selected_lines]
        if (hashlib.sha256(raw).hexdigest() != expected_hash
                or {row['task_id'] for row in selected} != set(expected_ids)):
            raise RuntimeError('frozen_cohort_changed')
        lines.extend(selected_lines)
        rows.extend(selected)
    ids = [row['task_id'] for row in rows]
    if len(ids) != 500 or len(set(ids)) != 500 or set(ids) != set(manifest['task_ids']):
        raise RuntimeError('published500_scope_mismatch')
    source = {row['task_id']: row for split in ('train', 'val', 'holdout')
              for line in (best.SOURCE / (split + '.jsonl')).read_text().splitlines()
              if line.strip() for row in [json.loads(line)]}
    if any(row != source.get(row['task_id']) for row in rows):
        raise RuntimeError('original_question_answer_or_split_changed')
    raw = b'\n'.join(lines) + b'\n'
    if DATASET.exists() and DATASET.read_bytes() != raw:
        raise RuntimeError('published500_dataset_changed')
    if not DATASET.exists():
        DATASET.write_bytes(raw)
    config = deepcopy(read(ADD / 'config/full.json'))
    config.update(task_ids=ids, splits=sorted({row['split'] for row in rows}),
                  parallelism=workers, max_trials=1000)
    artifacts = {'L0': read(PUB / 'baseline-cache/baseline.json'),
                 'best': read(PUB / 'strategy.json')}
    best.BINARY = BINARY
    for variant in config['variants']:
        artifact = artifacts[variant['id']]
        if variant['artifact'] != artifact or variant['version'] != best._artifact_version(artifact):
            raise RuntimeError('published_strategy_or_baseline_changed')
    if (artifacts['L0'].get('level') != 'L0' or artifacts['L0'].get('policy') is not None
            or config['variants'][1]['version'] != manifest['version']
            or config['judge'].get('answer_field_bytes') != 32768
            or read(PUB / 'baseline-cache/judge-profile.json')['judge_fingerprint'] != manifest['judge_fingerprint']):
        raise RuntimeError('published_scoring_identity_changed')
    save(CONFIG, config)
    native = subprocess.run([str(BINARY), 'dataset', 'check', str(best.SOURCE)],
                            cwd=best.ROOT, text=True, capture_output=True)
    if native.returncode:
        raise RuntimeError('native_source_dataset_check_failed')
    return manifest, rows, config


def cached_check():
    recorded = read(PUB / 'acceptance-500.json')
    result = combined.check()
    if not recorded.get('added') or not result.get('all_goals_met'):
        raise RuntimeError('whole500_release_not_accepted')
    if (set(result['task_ids']) != set(recorded['task_ids'])
            or any(result[key] != recorded[key] for key in ('version', 'judge_fingerprint'))):
        raise RuntimeError('recorded500_identity_mismatch')
    for variant in ('L0', 'candidate'):
        for key in ('quality', 'loop', 'latency_ms', 'calls', 'strict_passed', 'tokens', 'intent_f1'):
            if not math.isclose(result[variant][key], recorded[variant][key], rel_tol=1e-12, abs_tol=1e-12):
                raise RuntimeError('recorded500_metric_mismatch:' + variant + ':' + key)
    return result


def run(config, manifest, source_rows):
    # A single 1000-trial JSON would exceed the native 1 GiB report limit.
    # Five fixed batches preserve every execution without duplicating traces
    # into another aggregate report. Failed tasks remain in these reports.
    root = PUB / 'reproduction-500'
    root.mkdir(exist_ok=True)
    env = offline_gepa.environment()
    for key in ('HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY', 'http_proxy', 'https_proxy', 'all_proxy', 'BUSINESS_BROWSER_PROXY'):
        env.pop(key, None)
    env.update(NO_PROXY='*', no_proxy='*', TRAJECTORY_UNCONDITIONAL_RESUME='1',
               TRAJECTORY_EVALUATION_PARALLELISM=str(config['parallelism']))
    items, errors = {}, []
    originals = {row['task_id']: row for row in source_rows}
    versions = {variant['id']: variant['version'] for variant in config['variants']}
    for index, start in enumerate(range(0, 500, 100), 1):
        batch = root / ('batch-' + str(index))
        batch.mkdir(exist_ok=True)
        request = deepcopy(config)
        request.update(task_ids=config['task_ids'][start:start + 100], max_trials=200)
        config_path = batch / 'evaluation.json'
        save(config_path, request)
        reports = sorted(batch.glob('attempt-*/report.json'),
                         key=lambda path: int(path.parent.name.split('-')[-1]))
        previous = reports[-1] if reports else None
        report = read(previous) if previous else None
        if report is None or not report.get('complete') or any(
                progress._trial_error_code(row) or progress._trial_is_partial(row)
                for row in report.get('trials', [])):
            destination = batch / ('attempt-' + str(len(list(batch.glob('attempt-*'))) + 1))
            command = [str(BINARY), 'dataset', 'run', str(best.SOURCE), '--config', str(config_path),
                       '--output-root', str(destination), '--allow-unreviewed']
            if previous:
                command += ['--resume-report', str(previous)]
            print(json.dumps({'event': 'native_start', 'batch': index, 'tasks': 100,
                              'trials': 200, 'parallelism': config['parallelism']}), flush=True)
            with (batch / 'native.log').open('a') as log:
                process = subprocess.run(command, cwd=best.ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
            previous = destination / 'report.json'
            if not previous.is_file():
                raise RuntimeError('native_report_missing_attempt_and_costs_retained:' + str(index))
            report = read(previous)
        if report['configuration']['judge_fingerprint'] != manifest['judge_fingerprint']:
            raise RuntimeError('fresh500_judge_changed')
        seen = set()
        for row in report['trials']:
            pair = row['task_id'], row['variant_id']
            if pair in seen or pair[0] not in request['task_ids'] or pair[1] not in ('L0', 'best'):
                raise RuntimeError('fresh500_trial_scope_invalid')
            if any(row.get(key) != originals[pair[0]].get(key) for key in ('task_version', 'group_id', 'split')):
                raise RuntimeError('fresh500_task_identity_changed')
            loaded = row.get('evidence', {}).get('loaded_version')
            if loaded is not None and loaded != versions[pair[1]]:
                raise RuntimeError('fresh500_loaded_strategy_changed')
            seen.add(pair)
            item = items.setdefault(pair[0], {'task_id': pair[0]})
            name = 'L0' if pair[1] == 'L0' else 'seed004'
            error = progress._trial_error_code(row)
            if error or progress._trial_is_partial(row):
                errors.append({'task_id': pair[0], 'variant': name, 'error': error or 'partial'})
            else:
                item[name] = cohort.feature(row)
            item[name + '_source'] = [str(previous.relative_to(best.ROOT)), row['trial_id']]
        if len(seen) != 200:
            errors.append({'batch': index, 'error': 'incomplete_batch', 'completed_trials': len(seen)})
    paired = [item for item in items.values() if 'L0' in item and 'seed004' in item]
    result = {'scope': 'known_business300_plus_training_replay200', 'independent_holdout': False,
              'status': 'complete' if len(paired) == 500 and not errors else 'incomplete',
              'version': manifest['version'], 'judge_fingerprint': manifest['judge_fingerprint'],
              'paired_tasks': len(paired), 'errors': errors, 'items': list(items.values()),
              'L0': cohort.aggregate(paired, 'L0') if paired else None,
              'candidate': cohort.aggregate(paired, 'seed004') if paired else None}
    save(root / 'result.json', result)
    return {key: value for key, value in result.items() if key != 'items'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', action='store_true', help='Explicitly run all 500 tasks for L0 and Seed004 (1000 trials); resume complete trials unchanged.')
    parser.add_argument('--prepare', action='store_true', help='Prepare the same lightweight input and verify the cache; zero model calls.')
    parser.add_argument('--check', action='store_true', help='Check the published cache; the default, with zero model calls.')
    parser.add_argument('--parallelism', type=int, choices=range(1, 17), default=16)
    args = parser.parse_args()
    manifest, rows, config = prepare(args.parallelism)
    result = run(config, manifest, rows) if args.run else cached_check()
    print(json.dumps({key: value for key, value in result.items() if key != 'task_ids'}, ensure_ascii=False))
    print(json.dumps({'selected_tasks': len(rows), 'original_splits': dict(Counter(row['split'] for row in rows)),
                      'dataset_bytes': DATASET.stat().st_size, 'api_calls_in_check': 0,
                      'native_dataset': str(best.SOURCE), 'config': str(CONFIG)}, ensure_ascii=False))
    return 0 if result.get('status') == 'complete' else 2


if __name__ == '__main__':
    raise SystemExit(main())
