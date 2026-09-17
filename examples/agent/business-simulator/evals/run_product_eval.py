#!/usr/bin/env python3
"""Use the unchanged benchmark scorer; seed only explicit raw preconditions."""
from __future__ import annotations

import hashlib
import json
import os
import sys
import uuid
from pathlib import Path

PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
sys.path.insert(0, str(REPO / 'benchmark'))
sys.path.insert(0, str(PACKAGE / 'harness'))
import run_eval as benchmark
from converters import read_xlsx_rows
from business_sim.state import Store, fresh_state, next_id, scope_key


def load_preconditions() -> dict:
    result = {}
    for path in sorted((REPO / 'test_cases/product').glob('*.xlsx')):
        rows = read_xlsx_rows(path)
        for row in rows[1:]:
            fields = dict(zip(rows[0], row))
            result[(path.name, fields.get('用例编号'))] = {
                'text': fields.get('预置条件（可选）', '').strip(),
                'device_type': fields.get('设备类型', ''),
            }
    return result


def seed_case(case: dict, user: str, preconditions: dict, store: Store) -> tuple[str, dict]:
    provenance = case.get('provenance', {})
    source = Path(provenance.get('source', '')).name
    row = preconditions.get((source, case['id']), {})
    text = row.get('text', '')
    state = fresh_state()
    evidence = {'precondition': text, 'seeded': []}
    if '用户喜好科幻' in text:
        state['preferences']['电影偏好'] = '科幻'
        evidence['seeded'].append('电影偏好=科幻')
    elif '用户喜好动画' in text:
        state['preferences']['电影偏好'] = '动画'
        evidence['seeded'].append('电影偏好=动画')
    if '已有健康档案及医嘱用药记录' in text:
        rid = next_id(state, 'record')
        state['records'][rid] = {'id': rid, 'origin': 'synthetic_precondition',
            'content': '模拟医嘱记录：维生素D，每天08:00；剂量未提供，不能自行补全。'}
        evidence['seeded'].append('synthetic medical record; no real patient data')
    supported = ('', '无', '已注入声纹 B，用户喜好科幻', '已注入声纹 B，用户喜好动画', '已有健康档案及医嘱用药记录')
    if text not in supported:
        raise ValueError(f"Unimplemented precondition for {case['id']}: {text}")
    target = {'1': 'phone-1', '2': 'speaker-1', '3': 'tv-1'}.get(row.get('device_type'))
    if target:
        state['preferences']['target_device'] = target
        evidence['seeded'].append(f'target_device={target}')
    client = f"eval-{case['id'].lower()[:32]}"  # benchmark.run_case identity contract
    scope = scope_key(user, client)
    store.reset(scope, state)
    return scope, evidence


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not any(a == '--agent-id' or a.startswith('--agent-id=') for a in argv):
        argv += ['--agent-id', 'business-simulator']
    if not any(a == '--port' or a.startswith('--port=') for a in argv):
        argv += ['--port', '17893']
    if not any(a == '--evalset' or a.startswith('--evalset=') for a in argv):
        argv += ['--evalset', str(REPO / 'benchmark/datasets/product.json')]
    args = benchmark.parse_args(argv)
    if args.concurrency != 1:
        raise ValueError('This package requires serial cases per daemon; use evals/run_isolated.py for parallel isolated daemons')
    if args.agent_id != 'business-simulator':
        raise ValueError('This fixture adapter targets business-simulator only; use benchmark/run_eval.py for other agents')
    if args.env_file:
        benchmark.load_env(Path(args.env_file))
    os.environ.setdefault("AF_TRACE_OUTPUT_DIR", str(REPO / "trace_output/business-simulator"))
    preconditions = load_preconditions()
    run_id = uuid.uuid4().hex[:10]
    original = benchmark.run_case
    original_send, original_wait = benchmark.send_turn, benchmark.wait_turn_response
    from conversation_client import send_turn, wait_turn_response

    def run_with_fixture(case, cfg):
        # Fresh identities also isolate the runtime's dialogue history across repeated runs.
        case_hash = hashlib.sha256(case['id'].encode()).hexdigest()[:12]
        cfg = {**cfg, 'user_id': f"{cfg['user_id'][:50]}-{run_id}-{case_hash}"}
        store = Store()
        scope, evidence = seed_case(case, cfg['user_id'], preconditions, store)
        result = original(case, cfg)
        result['business_execution'] = {**evidence, 'scope': scope, 'mode': os.environ.get('BUSINESS_EXECUTION_MODE','real'),
            'state': store.read(scope), 'events': store.events(scope),
            'note': 'Business state evidence; original benchmark scoring unchanged'}
        return result

    # Local adapter only: do not change the common runner's scoring or ground truth.
    benchmark.run_case = run_with_fixture
    benchmark.send_turn, benchmark.wait_turn_response = send_turn, wait_turn_response
    try:
        return benchmark.main(argv)
    finally:
        benchmark.run_case = original
        benchmark.send_turn, benchmark.wait_turn_response = original_send, original_wait


if __name__ == '__main__':
    raise SystemExit(main())
