"""Freeze 500 final comparison tasks using IDs alone, without model calls."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from prepare_dataset import DATASETS, ROOT, WORLD, read, task_ref, write

SEED = 'appworld-final500-fixed-scenarios-v1'


def freeze():
    path = DATASETS / 'final500-selection.json'
    if path.exists():
        return read(path)
    official = {split: (WORLD / 'data/datasets' / (split + '.txt')).read_text().splitlines()
                for split in ('train', 'dev', 'test_normal', 'test_challenge')}
    excluded = read(DATASETS / 'selection.json')['dev_ids']
    mapping = {task: split for split in ('dev', 'test_normal', 'test_challenge')
               for task in official[split] if task.strip()}
    pool = set(mapping) - set(excluded)
    if len(mapping) != 642 or len(pool) != 630 or pool & set(official['train']):
        raise RuntimeError('official_final500_pool_mismatch')
    scenarios = {}
    for task in pool:
        scenarios.setdefault(task.rsplit('_', 1)[0], []).append(task)
    if len(scenarios) != 210 or any(len(tasks) != 3 for tasks in scenarios.values()):
        raise RuntimeError('final500_requires_complete_scenario_pool')
    order = sorted(scenarios, key=lambda value: hashlib.sha256((SEED + ':' + value).encode()).digest())
    final = [task for scenario in order[:166] for task in sorted(scenarios[scenario])]
    last = sorted(scenarios[order[166]])
    final += last[:2]
    remaining = sorted(pool - set(final))
    value = {'dataset': 'appworld-final500', 'final_ids': final, 'remaining_ids': remaining,
             'excluded_dev_ids': excluded, 'quarantined_sibling_ids': last[2:],
             'selector_seed': SEED, 'source_split': {task: mapping[task] for task in sorted(pool)},
             'selection_uses_scores': False, 'independent_holdout': True,
             'final_ids_sha256': hashlib.sha256(('\n'.join(final) + '\n').encode()).hexdigest(),
             'selection_basis': '166 complete scenarios plus two fixed variants of the next scenario, ordered by seeded hash of public scenario IDs.',
             'hidden_answers_read': False, 'final_results_used_for_training': False}
    if len(final) != 500 or len(remaining) != 130 or set(final) & set(remaining):
        raise RuntimeError('final500_partition_mismatch')
    write(path, value)
    return value


def materialize(binary):
    selection = freeze()
    destination = DATASETS / 'final500-native'
    if destination.exists():
        subprocess.run([str(binary), 'dataset', 'check', str(destination), '--require-reviewed'],
                       cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
        return destination
    tasks = []
    for task in selection['final_ids']:
        row = task_ref(task, selection['source_split'][task])
        row['split'] = 'holdout'
        tasks.append(row)
    with tempfile.TemporaryDirectory(prefix='.appworld-final500-', dir=DATASETS) as directory:
        staging = Path(directory)
        (staging / 'tasks.jsonl').write_text(''.join(json.dumps(row, ensure_ascii=False, separators=(',', ':')) + '\n'
                                                  for row in tasks))
        write(staging / 'dataset.json', {'schema_version': 1, 'name': selection['dataset'],
            'split_seed': SEED, 'split_percent': [1, 1, 98], 'max_tasks': 500, 'max_model_calls': 0,
            'budget': {'max_steps': 300, 'max_tool_calls': 300, 'timeout_ms': 1200000},
            'sources': [{'kind': 'golden', 'path': 'tasks.jsonl'}]})
        built = staging / 'built'
        result = subprocess.run([str(binary), 'dataset', 'build', '--config', str(staging / 'dataset.json'),
                                 '--output-root', str(built)], cwd=ROOT, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError((result.stderr or result.stdout)[-2000:])
        subprocess.run([str(binary), 'dataset', 'check', str(built), '--require-reviewed'],
                       cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
        built.rename(destination)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-binary', type=Path,
                        default=ROOT / 'trace_output/business-simulator/published/seed004/runtime/trajectory_evolution')
    args = parser.parse_args()
    selection = freeze()
    destination = materialize(args.native_binary.resolve())
    print(json.dumps({'dataset': str(destination), 'final_tasks': len(selection['final_ids']),
                      'remaining_tasks': len(selection['remaining_ids']),
                      'quarantined_siblings': len(selection['quarantined_sibling_ids']),
                      'selection_uses_scores': False, 'hidden_answers_read': False, 'model_calls': 0}))


if __name__ == '__main__':
    main()
