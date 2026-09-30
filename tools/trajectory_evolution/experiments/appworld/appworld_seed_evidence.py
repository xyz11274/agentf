"""Bind reflection references to existing, successful train assessments.

Canonical memory, statuses and original reflection evidence remain unchanged.
The small compile view adds the actual verdicts of the exact cited episodes.
It never invents observations, reads dev answers or promotes candidate status.
"""
from __future__ import annotations

from copy import deepcopy


def prepare(controller):
    store = controller.TRAINING / 'evolution/by-user' / controller.OWNER
    source = store / 'memory/evolution-memory.json'
    original = controller.read(source)
    memory = deepcopy(original)
    allowed = set()
    train_ids = set(controller.ids('train'))
    report_paths = list((controller.TRAINING / 'captures').glob('train*/report.json'))
    report_paths += list((controller.TRAINING / 'preflight').glob('train/report.json'))
    for path in report_paths:
        for row in controller.read(path).get('trials', []):
            if (row.get('task_id') not in train_ids or row.get('execution_status') != 'completed'
                    or row.get('verdict') not in ('passed', 'failed')
                    or controller.output(row).get('appworld_reward') not in (0, 1)):
                continue
            evidence = row.get('evidence', {})
            for trace in evidence.get('traces') or [evidence.get('trace')]:
                identity = (trace or {}).get('identity', {})
                if identity.get('user_id') == controller.OWNER and identity.get('agent_id') == controller.AGENT:
                    allowed.add((identity['user_id'], identity['conversation_id'], identity['run_id']))
    bindings = []
    cache = {}
    for entry in memory.get('entries', []):
        candidate = entry.get('candidate', {})
        if (candidate.get('status') != 'EVOLUTION_CANDIDATE_STATUS_PROPOSED'
                or candidate.get('target_scope') != controller.SCOPE):
            continue
        appended = []
        for evidence in candidate.get('support_evidence', []):
            identity = evidence.get('source', {})
            key = (identity.get('user_id'), identity.get('conversation_id'), identity.get('dialogue_id'))
            if key not in allowed:
                continue
            path = store / 'evaluation' / key[1] / (key[2] + '.evaluation.json')
            if not path.is_file():
                continue
            if key not in cache:
                report = controller.read(path)
                cache[key] = {item.get('evidence', {}).get('episode_id'): item
                              for item in report.get('assessments', [])}
            for episode in evidence.get('episode_ids', []):
                assessment = cache[key].get(episode)
                if not assessment or assessment.get('model', {}).get('status') != 'MODEL_CALL_STATUS_SUCCEEDED':
                    continue
                actual = assessment.get('evidence', {})
                if not set(evidence.get('evidence_event_ids', [])) & set(actual.get('source_event_ids', [])):
                    continue
                bound = deepcopy(evidence)
                bound.update(evidence_id=evidence['evidence_id'] + ':' + episode,
                             episode_ids=[episode], evidence_event_ids=actual['source_event_ids'],
                             verdict=assessment['final_verdict'])
                appended.append(bound)
                bindings.append({'memory_id': entry['memory_id'], 'evaluation': str(path),
                                 'episode_id': episode, 'verdict': assessment['final_verdict']})
        candidate.setdefault('support_evidence', []).extend(appended)
    target = controller.TRAINING / 'seed-evidence-memory.json'
    controller.save(target, memory)
    controller.save(controller.TRAINING / 'seed-evidence-status.json', {
        'canonical_memory': str(source), 'compile_view': str(target), 'bindings': bindings,
        'canonical_memory_unchanged': controller.read(source) == original,
        'candidate_statuses_unchanged': True, 'model_calls': 0,
    })
    return target
