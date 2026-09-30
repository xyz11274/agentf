#!/usr/bin/env python3
"""Check the entire frozen training200 addition; never select by replay scores."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import layout
from build_scoped_business_300 import aggregate

PUB = layout.PUBLISHED / 'seed004'
ADD = PUB / 'training-200'

def read(path):
    return json.loads(Path(path).read_text())

def save(path, value):
    path = Path(path)
    tmp = path.with_suffix(path.suffix + '.new')
    tmp.write_text(json.dumps(value, ensure_ascii=False, separators=(',', ':')) + '\n')
    tmp.replace(path)

def check():
    if not (ADD / 'features.json').is_file():
        return {'status': 'waiting_for_complete_replay', 'added': False}
    new = read(ADD / 'features.json')
    if new.get('status') != 'complete':
        return {'status': 'waiting_for_complete_replay', 'added': False}
    frozen_manifest = PUB / 'baseline-cache/manifest300.json'
    manifest = read(frozen_manifest if frozen_manifest.is_file() else PUB / 'manifest.json')
    old = read(PUB / 'features400.json')
    old_ids = set(manifest['task_ids'])
    original = [item for item in old['items'] if item['task_id'] in old_ids]
    replay = new['items']
    request = read(ADD / 'selection.json')
    if (len(original) != 300 or len(replay) != 200 or
        {item['task_id'] for item in replay} != set(request['task_ids']) or
        old_ids & set(request['task_ids'])):
        raise RuntimeError('frozen500_scope_incomplete_or_overlapping')
    if new.get('version') != manifest['version'] or new.get('judge_fingerprint') != old['judge_fingerprint']:
        raise RuntimeError('comparison_version_or_judge_mismatch')
    items = original + replay
    for item in items:
        for variant in ('L0', 'seed004'):
            if any(item[variant].get(key) is None for key in ('quality', 'strict', 'loop', 'latency_ms', 'calls')):
                raise RuntimeError('unscored_trial_cannot_join')
    a, b = (aggregate(items, variant) for variant in ('L0', 'seed004'))
    gates = {
        'quality_relative_plus5pct': b['quality'] >= a['quality'] * 1.05,
        'strict_pass_relative_plus5pct': b['strict_passed'] >= a['strict_passed'] * 1.05,
        'invalid_loop_reduction15pct': b['loop'] <= a['loop'] * .85,
        'tokens_not_increased': a['tokens'] is not None and b['tokens'] is not None and b['tokens'] <= a['tokens'],
        'calls_not_increased': b['calls'] <= a['calls'],
        'latency_not_increased': b['latency_ms'] <= a['latency_ms'],
        'common_intent_f1_improved': a['intent_f1'] is not None and b['intent_f1'] > a['intent_f1'],
    }
    result = {'status': 'complete', 'added': False, 'all_goals_met': all(gates.values()),
              'scope': 'known_business300_plus_training_replay200', 'independent_holdout': False,
              'tasks': 500, 'version': manifest['version'], 'judge_fingerprint': old['judge_fingerprint'],
              'selection': 'frozen whole 200, category stratified before evaluation; no post-score subset',
              'gates': gates, 'loop_reduction30pct': b['loop'] <= a['loop'] * .7,
              'L0': a, 'candidate': b, 'task_ids': [item['task_id'] for item in items],
              'evaluated_at': datetime.now(timezone.utc).isoformat()}
    published = PUB / 'acceptance-500.json'
    if published.is_file() and result['all_goals_met']:
        existing = read(published)
        result['added'] = (existing.get('added') is True and
                           existing.get('version') == result['version'] and
                           existing.get('judge_fingerprint') == result['judge_fingerprint'] and
                           existing.get('task_ids') == result['task_ids'])
    save(ADD / 'combined-500.json', result)
    return result

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true', help='Add all200 only when every frozen goal passes')
    args = parser.parse_args()
    result = check()
    if args.apply and result.get('all_goals_met'):
        result['added'] = True
        save(PUB / 'acceptance-500.json', result)
        save(ADD / 'combined-500.json', result)
    print(json.dumps({k: v for k, v in result.items() if k != 'task_ids'}, ensure_ascii=False))
if __name__ == '__main__':
    main()
