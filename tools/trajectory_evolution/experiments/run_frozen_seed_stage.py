#!/usr/bin/env python3
"""Run a frozen seed stage while seed screening continues in another tmux session."""
import argparse
import json
import os
from pathlib import Path
import sys

import deadline_finish as d


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--selection-file', type=Path, required=True)
    parser.add_argument('--stage', choices=('val', 'val-remainder', 'holdout'), required=True)
    parser.add_argument('--parallelism', type=int, choices=range(1, 65), default=16)
    parser.add_argument('--native-binary', type=Path)
    args = parser.parse_args()
    sys.path.insert(0, str(d.HERE / 'business-coverage'))
    import best_strategy as best
    import offline_gepa

    selection_file = args.selection_file.resolve()
    directory = selection_file.parent
    freeze = d.read(selection_file)
    environment = offline_gepa.environment()
    for key in ('HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY', 'http_proxy',
                'https_proxy', 'all_proxy', 'BUSINESS_BROWSER_PROXY'):
        environment.pop(key, None)
    environment['NO_PROXY'] = environment['no_proxy'] = '*'
    environment['TRAJECTORY_EVALUATION_PARALLELISM'] = str(args.parallelism)
    os.environ.clear()
    os.environ.update(environment)
    view = d.snapshot('business', freeze['job_id'])
    chosen, _ = d.business_selection(best, view, freeze_path=selection_file,
        required_judge=freeze['judge_fingerprint'], prefer_seeds=True)
    if args.native_binary:
        chosen['native_binary'] = str(args.native_binary.resolve())
    if best._candidate_artifact_version(chosen) != chosen['version']:
        raise RuntimeError('candidate_runtime_version_mismatch')

    original_config = best._comparison_config
    def config(*positional, **keywords):
        path = original_config(*positional, **keywords)
        value = d.read(path)
        value['parallelism'] = args.parallelism
        d.write(path, value)
        return path
    best._comparison_config = config
    original_comparison = best.run_comparison
    def comparison(*positional, **keywords):
        keywords.setdefault('retries', 0)
        return original_comparison(*positional, **keywords)
    best.run_comparison = comparison
    best.BEST_FILE = directory / 'best.json'
    best.COMPARISON_FILE = directory / 'comparison.json'
    best.REQUEST_FILE = directory / 'request.json'
    receipt = directory / f'parallel-{args.stage}-worker.json'
    with d.lock(directory / f'parallel-{args.stage}.lock'):
        d.write(receipt, {'status': 'running', 'pid': os.getpid(),
            'version': chosen['version'], 'parallelism': args.parallelism,
            'selection_file': str(selection_file), 'holdout_used_for_ranking': False,
            'native_binary': chosen.get('native_binary')})
        if args.stage == 'val':
            result = best._final_validation(chosen)
        elif args.stage == 'val-remainder':
            screen = set(freeze['selection_task_ids'])
            _, splits = best._source_rows()
            val = {task for task, split in splits.items() if split == 'val'}
            if len(screen) != 50 or not screen <= val or len(val) != 200:
                raise RuntimeError('frozen_selection_panel_invalid')
            result = best.run_comparison(chosen, splits=('val',),
                task_ids=sorted(val - screen), purpose='final_val_with_cached_l0',
                include_l0=False, persist_result=False, retries=0)
        else:
            result = best.run_comparison(chosen, splits=('holdout',), purpose='deadline_holdout_best',
                include_l0=False, persist_result=False, retries=0)
        result_file = directory / f'parallel-{args.stage}-result.json'
        d.write(result_file, result)
        d.write(receipt, {'status': result['status'], 'pid': os.getpid(),
            'version': chosen['version'], 'parallelism': args.parallelism,
            'result_file': str(result_file)})
        print(json.dumps({'status': result['status'], 'version': chosen['version'],
            'complete_trials': result.get('complete_trials')}, ensure_ascii=False), flush=True)
    return 0 if result['status'] == 'complete' else 1


if __name__ == '__main__':
    raise SystemExit(main())
