#!/usr/bin/env python3
"""Keep seed iteration independent of versioned final comparisons.

Selection uses only complete shared val50 evidence. Each promoted version gets
an immutable selection and separate result. A superseded evaluator checkpoints
its current stage, then yields to the new primary; no seed runner is stopped.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import importlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import deadline_finish as d

STATE = d.ROOT / 'trace_output/business-simulator/audit/continuous-seed-delivery'


def modules():
    sys.path.insert(0, str(d.HERE / 'business-coverage'))
    return importlib.import_module('best_strategy'), importlib.import_module('offline_gepa')


def signature(best, view):
    paths = [best.JOB_ROOT / view['job_id'] / 'search/tree.json',
             best.EXTERNAL_SEED_REGISTRY,
             best.OUTPUT / 'audit/l0-400-20260929/judge-profile.json',
             best.OUTPUT / 'audit/l0-val50-current-judge-20260929/result.json']
    paths.extend(sorted((best.OUTPUT / 'published/best-strategy-runs' / view['job_id']).glob(
        'node-*-*-node*_val50_audit-val-50-*-candidate/result.json')))
    digest = hashlib.sha256(b'same-judge-strict-first-v3')
    for path in paths:
        if path.is_file():
            digest.update(path.read_bytes())
    return digest.hexdigest()[:20]


def promote(best, view, state_dir, generation, required_judge=None, prefer_seeds=False):
    primary_path = state_dir / 'primary.json'
    old = d.read(primary_path)
    previous_version = old.get('version')
    freeze = state_dir / 'screening' / generation / 'selection.json'
    chosen, pool = d.business_selection(best, view, freeze_path=freeze, reuse_prior=False,
                                       required_judge=required_judge, prefer_seeds=prefer_seeds)
    if required_judge and old.get('judge_fingerprint') != required_judge:
        old = {}
    if prefer_seeds and not old.get('candidate_id'):
        old = {}
    old_versions = {item['version'] for item in
                    d.read(Path(old['selection_file'])).get('candidate_ids', [])} if old else set()
    new_versions = {item['version'] for item in pool}
    expand_pool = (old.get('version') == chosen['version'] and len(old_versions) < 3
                   and old_versions < new_versions)
    if old and (chosen['version'] == old.get('version') or
                best._showcase_key(chosen['validation']) <= best._showcase_key(old['validation'])) and not expand_pool:
        return old
    tag = hashlib.sha256((chosen['version'] + '|' + (required_judge or '') + '|' +
                          '|'.join(sorted(new_versions)) + ('|prefer-seeds' if prefer_seeds else '')).encode()).hexdigest()[:20]
    directory = state_dir / 'versions' / tag
    selected = directory / 'selection.json'
    frozen = d.read(freeze)
    if selected.exists() and d.read(selected) != frozen:
        # The first pre-holdout freeze for this version is authoritative.
        frozen = d.read(selected)
    else:
        d.write(selected, frozen)
    primary = {'job_id': view['job_id'], 'version': chosen['version'],
               'candidate_id': chosen.get('external_candidate_id'), 'node': chosen.get('node'),
               'validation': chosen['validation'], 'selection_file': str(selected),
               'judge_fingerprint': required_judge,
               'directory': str(directory), 'supersedes': previous_version,
               'basis': 'same_complete_val50_strict_then_quality', 'holdout_used_for_ranking': False,
               'candidate_preference': 'external_seed' if prefer_seeds else 'all_candidates',
               **{key: chosen[key] for key in ('evaluation_feedback_informed',
                   'training_feedback_sources') if key in chosen},
               'promoted_at': datetime.now(timezone.utc).isoformat()}
    d.write(primary_path, primary)
    d.write(best.OUTPUT / 'published/active-best-candidate.json', primary)
    return primary


def worker(selection_file, state_dir):
    best, gepa = modules()
    frozen = d.read(selection_file)
    directory = selection_file.parent
    receipt = directory / 'worker.json'
    d.write(receipt, {'status': 'running', 'pid': os.getpid(),
                     'version': frozen['best_version'], 'started_at': datetime.now(timezone.utc).isoformat()})
    try:
        with d.lock(directory / 'worker.lock'):
            environment = gepa.environment()
            for key in ('HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY', 'http_proxy',
                        'https_proxy', 'all_proxy', 'BUSINESS_BROWSER_PROXY'):
                environment.pop(key, None)
            environment['NO_PROXY'] = environment['no_proxy'] = '*'
            os.environ.clear()
            os.environ.update(environment)
            # Isolate all convenience pointers. Native run directories already
            # carry the candidate version and preserve completed checkpoints.
            best.BEST_FILE = directory / 'best.json'
            best.COMPARISON_FILE = directory / 'comparison.json'
            best.REQUEST_FILE = directory / 'request.json'
            view = d.snapshot('business', frozen['job_id'])
            selected = d.business_selection(best, view, freeze_path=selection_file, reuse_prior=False)
            superseded = lambda: d.read(state_dir / 'primary.json').get('selection_file') != str(selection_file)
            result = d.business_delivery(best, view, selection=selected,
                                         result_path=directory / 'result.json', superseded=superseded)
            # Promote a completed delivery only if it is still the primary.
            # A newly promoted but incomplete candidate never erases the last
            # completed deliverable or borrows its scores.
            if not superseded() and (result.get('comparison') or {}).get('status') == 'complete':
                d.write(best.OUTPUT / 'published/best-strategy.json', result)
                d.write(best.OUTPUT / 'published/best-strategy-comparison.json', result['comparison'])
            d.write(receipt, {'status': 'finished', 'version': frozen['best_version'],
                             'result_status': result.get('status'),
                             'finished_at': datetime.now(timezone.utc).isoformat()})
        return 0
    except Exception as exc:
        d.write(receipt, {'status': 'error', 'version': frozen.get('best_version'),
                         'error_type': type(exc).__name__,
                         'finished_at': datetime.now(timezone.utc).isoformat()})
        return 1


def supervise(args):
    best, _ = modules()
    begin, deadline = d.utc(args.start_at), d.utc(args.deadline)
    state_dir = args.state_dir.resolve()
    with d.lock(state_dir / 'supervisor.lock'):
        seen = None
        child = None
        while datetime.now(timezone.utc) < deadline:
            now = datetime.now(timezone.utc)
            state = {'updated_at': now.isoformat(), 'seed_iteration_stopped': False,
                     'start_at': begin.isoformat(), 'deadline': deadline.isoformat()}
            try:
                view = d.snapshot('business')
                generation = signature(best, view)
                if args.prefer_seeds:
                    generation += '-prefer-seeds'
                if generation != seen:
                    profile = d.read(best.OUTPUT / 'audit/l0-400-20260929/judge-profile.json')
                    judge = (profile or d.read(best.OUTPUT / 'audit/l0-val50-current-judge-20260929/result.json')).get('judge_fingerprint')
                    if not judge:
                        raise RuntimeError('waiting_current_judge_baseline')
                    primary = promote(best, view, state_dir, generation, required_judge=judge,
                                      prefer_seeds=args.prefer_seeds)
                    seen = generation
                else:
                    primary = d.read(state_dir / 'primary.json')
                if child is not None:
                    child.poll()
                active = []
                for receipt_file in (state_dir / 'versions').glob('*/worker.json'):
                    receipt = d.read(receipt_file)
                    if receipt.get('status') == 'running' and d.process(int(receipt.get('pid', 0))):
                        active.append(receipt['version'])
                state.update(primary=primary.get('version'), active_versions=active)
                receipt = d.read(Path(primary['directory']) / 'worker.json')
                if now < begin:
                    state['status'] = 'iterating_before_comparison'
                elif active:
                    state['status'] = ('evaluating_primary' if primary['version'] in active
                                       else 'primary_promoted_waiting_stage_checkpoint')
                elif d.read(best.OUTPUT / 'audit/l0-400-20260929/judge-profile.json').get('status') != 'complete':
                    state['status'] = 'waiting_baseline_regrade'
                elif receipt.get('status') in ('finished', 'error'):
                    state['status'] = 'monitoring_new_seed' if receipt['status'] == 'finished' else 'needs_review'
                else:
                    with (Path(primary['directory']) / 'worker.log').open('a') as log:
                        child = subprocess.Popen([sys.executable, '-u', str(Path(__file__).resolve()),
                            '--worker-selection', primary['selection_file'], '--state-dir', str(state_dir)],
                            cwd=d.ROOT, stdout=log, stderr=log, start_new_session=True)
                    state.update(status='comparison_started', pid=child.pid)
            except (OSError, ValueError, RuntimeError, KeyError) as exc:
                waiting = isinstance(exc, RuntimeError) and str(exc) in (
                    'current_judge_candidate_missing', 'waiting_current_judge_baseline')
                state.update(status='waiting_current_judge_val50' if waiting else 'needs_review',
                             error_type=type(exc).__name__)
            d.write(state_dir / 'status.json', state)
            print(json.dumps(state, ensure_ascii=False), flush=True)
            if args.once:
                return 0
            time.sleep(15)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--state-dir', type=Path, default=STATE)
    parser.add_argument('--start-at', default=d.CUTOFF)
    parser.add_argument('--deadline', default=d.DEADLINE)
    parser.add_argument('--once', action='store_true')
    parser.add_argument('--prefer-seeds', action='store_true',
                        help='Use the user-requested seed family for the primary strategy.')
    parser.add_argument('--worker-selection', type=Path)
    args = parser.parse_args()
    return worker(args.worker_selection, args.state_dir.resolve()) if args.worker_selection else supervise(args)


if __name__ == '__main__':
    raise SystemExit(main())
