"""Native-validated policy transfer without changing experience content."""
from __future__ import annotations
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def native(binary, root, operation, value):
    proc = subprocess.run([str(binary), *operation], input=json.dumps(value, ensure_ascii=False),
        text=True, capture_output=True, cwd=root, timeout=30)
    if proc.returncode:
        raise RuntimeError('native_' + '_'.join(operation) + '_failed')
    return proc.stdout.strip()


def transfer_policy(source, *, root, binary, protoc, temporary_root, agent, scope):
    """Scope affects revision: derive real hashes, then validate with native compile."""
    from google.protobuf import json_format
    proto = root / 'tools/trajectory_evolution/proto'
    with tempfile.TemporaryDirectory(prefix='.proto-', dir=temporary_root) as temp:
        subprocess.run([str(protoc), '-I' + str(proto),
            '-I' + str(protoc.parent.parent / 'include'), '--python_out=' + temp,
            str(proto / 'trajectory_trace.proto'), str(proto / 'shadow_policy.proto'),
            str(proto / 'runtime_policy.proto')], check=True, capture_output=True)
        sys.path.insert(0, temp)
        try:
            import runtime_policy_pb2
            policy = json_format.ParseDict(source, runtime_policy_pb2.RuntimePolicy())
        finally:
            sys.path.remove(temp)
    digest = lambda message: hashlib.sha256(message.SerializeToString(deterministic=True)).hexdigest()
    original_id = policy.policy_id
    policy.ClearField('policy_id')
    if 'runtime-v2:' + digest(policy) != original_id:
        raise RuntimeError('source_native_policy_digest_mismatch')
    policy.policy_id = original_id
    revisions = {}
    before = {x.memory_id: (x.content, x.trigger_pattern, list(x.match_terms))
              for x in policy.snapshot.experiences}
    for item in policy.snapshot.experiences:
        old = item.content_revision
        item.target_scope = scope
        item.ClearField('content_revision')
        item.content_revision = digest(item)
        revisions[item.memory_id] = {'source_revision': old, 'target_revision': item.content_revision}
    snapshot = policy.snapshot
    snapshot.identity.agent_id = agent
    del snapshot.options.target_scopes[:]
    snapshot.options.target_scopes.append(scope)
    snapshot.ClearField('snapshot_id')
    snapshot.snapshot_id = 'shadow-v1:' + digest(snapshot)
    for approval in policy.options.approved_experiences:
        approval.content_revision = revisions[approval.memory_id]['target_revision']
    for override in policy.options.experience_overrides:
        ref = override.source_experience
        ref.content_revision = revisions[ref.memory_id]['target_revision']
    del policy.options.host_capabilities[:]
    policy.options.host_capabilities.extend(['context.request', 'action.filter', 'loop.decide'])
    if policy.options.action_rules or policy.options.loop_rules or policy.options.schedule_rules or snapshot.cross_relations:
        raise RuntimeError('transfer_requires_explicit_control_relation_remap')
    policy.ClearField('policy_id')
    policy.policy_id = 'runtime-v2:' + digest(policy)
    document = json_format.MessageToDict(policy, preserving_proto_field_name=True)
    compiled = json.loads(native(binary, root, ['runtime', 'recompile'],
        {'policy': document, 'options': document['options']}))
    after = {x['memory_id']: (x['content'], x['trigger_pattern'], x.get('match_terms', []))
             for x in compiled['snapshot']['experiences']}
    if before != after or len(after) != 11:
        raise RuntimeError('published_experience_content_changed')
    return compiled, revisions
