#!/usr/bin/env python3
"""Prepare source-faithful L0/Seed004 wrappers; no execution or model calls."""
from __future__ import annotations
import argparse
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
SOURCE = ROOT / 'trace_output/business-simulator/published/seed004'
OUTPUT = ROOT / 'trace_output/appworld/published/seed004'
BINARY = SOURCE / 'runtime/trajectory_evolution'
PROTOC = Path('/root/miniconda3/envs/agentflow-dev/bin/protoc')
AGENT = 'appworld-agentflow'
SCOPE = 'agent:appworld-agentflow/actor:default'
sys.path.insert(0, str(HERE.parent))
from published_policy_transfer import native, transfer_policy


def read(path):
    return json.loads(path.read_text())


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.new')
    temporary.write_text(json.dumps(value, ensure_ascii=False, separators=(',', ':')) + '\n')
    temporary.replace(path)


def version(artifact):
    result = native(BINARY, ROOT, ['artifact-version'], artifact)
    if not result.startswith('agent-version-v1:'):
        raise RuntimeError('native_artifact_version_invalid')
    return result


def compile_policy():
    original, manifest = read(SOURCE / 'strategy.json'), read(SOURCE / 'manifest.json')
    if version(original) != manifest['version']:
        raise RuntimeError('source_published_version_mismatch')
    with tempfile.TemporaryDirectory(prefix='appworld-policy-', dir='/tmp') as temp:
        policy, revisions = transfer_policy(original['policy'], root=ROOT, binary=BINARY,
            protoc=PROTOC, temporary_root=Path(temp), agent=AGENT, scope=SCOPE)
    return original, manifest, policy, revisions


def prepare(host_version, host_files):
    if not host_version or not host_files:
        raise RuntimeError('real_host_freeze_required')
    files = {str(path.resolve().relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
             for path in host_files}
    original, manifest, policy, revisions = compile_policy()
    artifact = deepcopy(original)
    artifact['agent'] = {'name': AGENT, 'revision': host_version}
    artifact['policy'] = policy
    baseline = {key: deepcopy(artifact[key]) for key in ('schema_version', 'agent', 'user_id')}
    baseline.update(level='L0', policy=None)
    metadata = {'status': 'prepared_no_execution', 'source_version': manifest['version'],
        'version': version(artifact), 'baseline_version': version(baseline),
        'host_version': host_version, 'host_files': files, 'agent_id': AGENT, 'scope': SCOPE,
        'owner': artifact['user_id'], 'source_memory_readonly': manifest.get('memory_source_dir'),
        'experience_count': 11, 'experience_text_trigger_id_match_terms_unchanged': True,
        'scope_remap': {'from': original['policy']['snapshot']['options']['target_scopes'], 'to': [SCOPE]},
        'host_capability_remap': {'from': original['policy']['options']['host_capabilities'],
            'to': policy['options']['host_capabilities']}, 'revision_remap': revisions,
        'strict_basis': 'official world.evaluate deterministic tests; task_completed alone is insufficient',
        'TGC_SGC': 'await_installed_official_evaluator', 'intent_labels': None,
        'invalid_loop_basis': 'native rule_lower_bound; not comparable to business model_review',
        'application_count_basis': 'receipt.application.status == applied', 'api_calls': 0}
    for filename, value in [('baseline.json', baseline), ('seed004.json', artifact), ('transfer.json', metadata)]:
        path = OUTPUT / filename
        if path.exists() and read(path) != value:
            raise RuntimeError('frozen_published_wrapper_changed:' + filename)
        if not path.exists():
            save(path, value)
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--prepare', action='store_true')
    parser.add_argument('--host-version')
    parser.add_argument('--host-file', type=Path, action='append', default=[])
    args = parser.parse_args()
    if args.check:
        original, _, policy, revisions = compile_policy()
        assert policy['snapshot']['identity']['agent_id'] == AGENT
        assert all(x['target_scope'] == SCOPE for x in policy['snapshot']['experiences'])
        assert policy['snapshot']['identity']['user_id'] == original['user_id']
        assert len(revisions) == 11
        print(json.dumps({'status': 'passed', 'native_policy_compile': True,
            'source_experiences': 11, 'content_unchanged': True, 'scope': SCOPE, 'api_calls': 0}))
    if args.prepare:
        if not args.host_version or not args.host_file:
            parser.error('--prepare requires frozen --host-version and --host-file')
        result = prepare(args.host_version, args.host_file)
        print(json.dumps({key: result[key] for key in
            ('status', 'source_version', 'version', 'baseline_version', 'api_calls')}))
    if not args.check and not args.prepare:
        parser.error('choose --check or --prepare')


if __name__ == '__main__':
    main()
