"""Compile a real PROPOSED memory candidate for research evaluation only."""
from __future__ import annotations

from copy import deepcopy
import argparse
import json
from pathlib import Path

import appworld_experiment as controller
from appworld_learning import _readiness


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--label', choices=('seed0', 'seed1'), default='seed0')
    label = parser.parse_args().label
    target = controller.PUBLISHED / label / 'strategy.json'
    if target.exists():
        manifest = controller.read(target.parent / 'manifest.json')
        print(json.dumps({'strategy': str(target), 'cached': True, **manifest}, ensure_ascii=False))
        return
    canonical_memory = _readiness(controller)
    original = controller.read(canonical_memory)
    corrected = deepcopy(original)
    excluded = []
    retained = []
    for entry in corrected.get('entries', []):
        candidate = entry.get('candidate', {})
        text = candidate.get('trigger_pattern', '') + ' ' + candidate.get('proposed_change', '')
        if any(token in text.lower() for token in ('<redacted', '[redacted]', 'redacted_password', '脱敏占位符')):
            excluded.append({'memory_id': entry['memory_id'],
                             'reason': 'log_safety_projection_is_not_execution_value_evidence'})
        else:
            retained.append(entry)
    corrected['entries'] = retained
    taxonomy = []
    for entry in corrected.get('entries', []):
        candidate = entry.get('candidate', {})
        if (candidate.get('status') == 'EVOLUTION_CANDIDATE_STATUS_PROPOSED'
                and candidate.get('target_scope') == controller.SCOPE
                and candidate.get('target_type') == 'CANDIDATE_TARGET_TYPE_SKILL_ORCHESTRATION'):
            # Ordering login and environment API calls is Python-generation
            # advice, not a change to AgentFlow's actual Skill dispatch graph.
            text = candidate.get('proposed_change', '')
            if ('API' not in text or 'login' not in text
                    or any(word in text for word in ('run_skills', 'appworld-office'))):
                continue
            before = deepcopy(candidate)
            candidate['target_type'] = 'CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION'
            assert {key: value for key, value in before.items() if key != 'target_type'} == {
                key: value for key, value in candidate.items() if key != 'target_type'}
            taxonomy.append({'memory_id': entry['memory_id'], 'source_type': before['target_type'],
                             'runtime_type': candidate['target_type']})
    if not any(entry.get('candidate', {}).get('status') == 'EVOLUTION_CANDIDATE_STATUS_PROPOSED'
               and entry.get('candidate', {}).get('target_type') == 'CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION'
               for entry in corrected.get('entries', [])):
        raise RuntimeError('no_proposed_prompt_candidate_for_research')
    memory = controller.TRAINING / (label + '-research-memory.json')
    controller.save(memory, corrected)
    identity = {'user_id': controller.OWNER, 'agent_id': controller.AGENT,
                'conversation_id': 'evaluation-version', 'run_id': 'evaluation-version'}
    shadow = {'target_scopes': [controller.SCOPE], 'max_policy_entries': 64,
              'max_advice_items': 2, 'max_advice_bytes': 2048, 'max_query_bytes': 32768,
              'max_memory_bytes': 16777216, 'max_snapshot_bytes': 262144,
              'max_match_terms': 128, 'min_matched_terms': 2,
              'require_validated_evidence': False}
    snapshot = json.loads(controller.native(controller.BINARY, controller.ROOT, ['shadow', 'compile'],
        {'memory_path': str(memory), 'identity': identity, 'options': shadow}))
    experiences = [item for item in snapshot.get('experiences', [])
                   if item.get('control_target_type') in (None, '', 'NONE', 0)]
    if not experiences:
        raise RuntimeError('no_proposed_prompt_candidate_for_research: ' + json.dumps({
            'snapshot_count': len(snapshot.get('experiences', [])),
            'excluded_counts': snapshot.get('excluded_counts', {}),
            'control_types': [item.get('control_target_type') for item in snapshot.get('experiences', [])],
        }))
    entries = {item['memory_id']: item for item in corrected.get('entries', [])}
    if any(entries[item['memory_id']]['candidate']['status'] != 'EVOLUTION_CANDIDATE_STATUS_PROPOSED'
           for item in experiences):
        raise RuntimeError('research_candidate_not_proposed')
    options = {'requested_level': 'L1', 'host_capabilities': ['context.request', 'action.filter', 'loop.decide'],
               'fallback': 'strict', 'allowed_users': [controller.OWNER],
               'approved_experiences': [{key: item[key] for key in ('memory_id', 'content_revision')}
                                        for item in experiences],
               'experience_template': (controller.ROOT / 'tools/trajectory_evolution/config/prompts/runtime-experience.txt').read_text(),
               'max_context_bytes': 2048, 'request_context_pointer': '/system', 'request_context_mode': 'text'}
    policy = json.loads(controller.native(controller.BINARY, controller.ROOT, ['runtime', 'compile'],
        {'memory_path': str(memory), 'identity': identity, 'shadow_options': shadow, 'options': options}))
    if not policy.get('snapshot', {}).get('experiences'):
        raise RuntimeError('research_runtime_policy_empty')
    artifact = deepcopy(controller.read(controller.PUBLISHED / 'L0.json'))
    artifact['agent']['revision'] = controller.host_version()
    artifact.update(level='L1', policy=policy)
    if target.exists() and controller.read(target) != artifact:
        raise RuntimeError('frozen_seed0_cannot_be_overwritten')
    evidence = controller.read(controller.TRAINING / 'seed-evidence-status.json')
    selected = {item['memory_id'] for item in experiences}
    bindings = [item for item in evidence['bindings'] if item['memory_id'] in selected]
    positives = {str(Path(item['evaluation']).parent.name) for item in bindings
                 if item['verdict'] in ('SEMANTIC_VERDICT_USEFUL', 'SEMANTIC_VERDICT_NECESSARY_RETRY',
                                       'SEMANTIC_VERDICT_PREMATURE_STOP')}
    manifest = {'version': controller.version(artifact), 'evaluation_only': True,
                'release_status': 'evaluation_only_proposed', 'memory_source': str(memory),
                'canonical_memory_source': str(canonical_memory), 'taxonomy_corrections': taxonomy,
                'evaluation_exclusions': excluded,
                'canonical_memory_unchanged': controller.read(canonical_memory) == original,
                'experience_count': len(experiences), 'candidate_statuses_unchanged': True,
                'validated_evidence_required': False, 'actual_positive_support_dialogues': len(positives),
                'actual_episode_verdicts': [item['verdict'] for item in bindings],
                'host_version': artifact['agent']['revision'], 'excluded_counts': snapshot.get('excluded_counts', {}),
                'independent_holdout': False, 'model_calls': 0}
    controller.save(controller.PUBLISHED / label / 'manifest.json', manifest)
    controller.save(controller.TRAINING / 'seed-compile-status.json', manifest)
    controller.save(controller.RUNTIME / ('dev-' + label.capitalize() + '.json'),
                    controller.evaluation_config(controller.ids('dev'), artifact, 12))
    # The waiting evaluation controller sees only the completed immutable file.
    controller.save(target, artifact)
    print(json.dumps({'strategy': str(target), **manifest}, ensure_ascii=False))


if __name__ == '__main__':
    main()
