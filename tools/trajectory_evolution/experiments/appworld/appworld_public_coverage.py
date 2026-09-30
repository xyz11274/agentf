"""Match frozen public task scenarios to official train instructions only."""
from __future__ import annotations

from collections import Counter
import json
import re

import appworld_experiment as controller


FEATURES = {
    'files': r'\b(file|files|folder|directory|directories|photo|photos|pdf|document|documents)\b',
    'mail': r'\b(email|emails|mail|gmail|inbox|attachment|attachments)\b',
    'calendar_dates': r'\b(calendar|date|dates|month|year|week|february|march|april|meeting|meetings)\b',
    'payments': r'\b(venmo|splitwise|payment|payments|paid|owe|owed|expense|expenses|bill|bills|dollars)\b|\$',
    'shopping': r'\b(amazon|order|orders|buy|purchase|cart|product|products|shopping)\b',
    'music': r'\b(spotify|song|songs|music|playlist|playlists|album|albums)\b',
    'tasks': r'\b(todoist|task|tasks|todo|reminder|reminders|project|projects)\b',
    'contacts_messages': r'\b(contact|contacts|phone|text|texts|sms|message|messages|call|calls)\b',
    'bulk_filter': r'\b(all|each|every|except|excluding|older|newer|before|after|between|only)\b',
    'state_write': r'\b(delete|remove|move|rename|send|create|update|mark|add|request|cancel|archive)\b',
}
STOP = set('a an and are as at be been by can do for from have i in into is it its me my of on or our please that the their them then these this those to was were with you your'.split())


def signatures(instruction):
    text = instruction.lower()
    words = set(re.findall(r'[a-z][a-z_]+', text)) - STOP
    features = {key for key, pattern in FEATURES.items() if re.search(pattern, text)}
    return words, features


def main():
    here = controller.HERE
    selection = controller.read(here / 'datasets/final500-selection.json')
    public = [json.loads(line) for line in (here / 'datasets/final500-native/holdout.jsonl').open()]
    final_ids = [row['task_id'].removeprefix('appworld-') for row in public]
    if set(final_ids) != set(selection['final_ids']) or len(public) != 500:
        raise RuntimeError('public_coverage_frozen_selection_mismatch')
    world = here / 'runtime/world'
    train_ids = (world / 'data/datasets/train.txt').read_text().splitlines()
    # The only training task content read is the public instruction whitelist.
    train = {task: signatures(controller.read(world / 'data/tasks' / task / 'specs.json')['instruction'])
             for task in train_ids if task}
    captured = set(controller.read(here / 'datasets/selection.json')['train_ids'])
    features = Counter()
    hits = Counter()
    scenarios = set()
    examples = {}
    for row in public:
        words, keys = signatures(row['task_input'])
        features.update(keys)
        scenario = row['task_id'].rsplit('_', 1)[0]
        if scenario in scenarios:
            continue
        scenarios.add(scenario)
        ranked = []
        for task, (tw, tk) in train.items():
            union = words | tw
            score = 3 * len(keys & tk) + len(words & tw) / max(1, len(union))
            ranked.append((score, task))
        for score, task in sorted(ranked, key=lambda pair: (-pair[0], pair[1]))[:3]:
            if score > 0:
                hits[task] += 1
                if len(examples.setdefault(task, [])) < 3:
                    examples[task].append(row['task_id'])
    priority = [{'task_id': 'appworld-' + task, 'matching_public_scenarios': count,
                 'already_in_first_train24': task in captured,
                 'public_features': sorted(train[task][1]), 'example_public_task_ids': examples[task]}
                for task, count in sorted(hits.items(), key=lambda pair: (-pair[1], pair[0]))]
    result = {'method': 'deterministic public instruction lexical-feature coverage; not outcome-based selection',
              'final_tasks': 500, 'final_scenarios': len(scenarios), 'official_train_tasks': len(train),
              'final_ids_sha256': selection['final_ids_sha256'],
              'final_results_read': False, 'hidden_answers_read': False, 'model_calls': 0,
              'public_feature_task_counts': dict(features.most_common()),
              'additional_training_priority': [row for row in priority if not row['already_in_first_train24']],
              'existing_training_coverage': [row for row in priority if row['already_in_first_train24']]}
    path = controller.TRAINING / 'public-scenario-training-coverage.json'
    controller.save(path, result)
    print(json.dumps({'path': str(path), 'public_feature_task_counts': result['public_feature_task_counts'],
                      'top_additional_training': result['additional_training_priority'][:8],
                      'model_calls': 0, 'final_results_read': False}, ensure_ascii=False))


if __name__ == '__main__':
    main()
