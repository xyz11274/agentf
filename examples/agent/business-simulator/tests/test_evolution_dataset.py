"""业务数据进入原生自进化协议的离线回归；不使用真实 API。"""
from copy import deepcopy
import json
from pathlib import Path
import sys

import pytest

PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
sys.path[:0] = [str(PACKAGE/'evals'), str(PACKAGE/'harness'), str(REPO/'benchmark')]
from prepare_dataset import prepare_product_splits, evaluation_task, prepare_configuration
from converters import convert_product, convert_daily_qa, merge_evalsets, load_domain_map, parse_intents
from evalset_schema import normalize_case, normalize_evalset
from business_sim.evaluation_world import load_profile
from business_sim.state import Store, fresh_state, scope_key, database_path


def test_generated_configuration_marks_snapshot_miss_non_retryable(tmp_path):
    """只为新配置声明确定的能力缺失，不把所有空结果或业务失败混为一类。"""
    configuration = prepare_configuration(tmp_path/'config', tmp_path/'dataset')
    service = json.loads((configuration/'service.json').read_text())
    assert service['instrumentation']['action_outcomes'] == [{
        'pointer': '/error', 'equals': 'snapshot_miss', 'status': 'failed',
        'error': {'code': 'snapshot_miss', 'retryability': 'non_retryable'},
    }]
    # 模型失败仍由单独映射处理，不能误套工具的不可重试结论。
    assert service['instrumentation']['model_outcomes'] == [{
        'pointer': '/_llm_error', 'equals': True, 'status': 'failed',
        'error': {'code': 'model_call_failed', 'retryability': 'unknown'},
    }]


def corpus():
    mapping = load_domain_map(REPO/'benchmark/domain_skill_map.json')
    return normalize_evalset(merge_evalsets([convert_product(p, mapping, [])
        for p in sorted((REPO/'test_cases/product').glob('*.xlsx'))]))


@pytest.mark.parametrize('fields,expected', [
    ({'level': 'L2'}, 'L2'),
    ({'case_level': 'L3'}, 'L3'),
    ({'case_level': 'L1', 'level': 'L3'}, 'L1'),
    ({'case_level': '', 'level': 'L3'}, ''),
])
def test_case_level_rename_preserves_value_and_legacy_input(tmp_path, fields, expected):
    case = {'id': 'case-level', 'turns': [{'user': 'test'}], 'difficulty': 'easy', **fields}
    path = tmp_path/'input.json'
    path.write_text(json.dumps({'cases': [case]}))
    for normalized in (normalize_case(case), convert_daily_qa(path)['cases'][0]):
        assert normalized['case_level'] == expected and 'level' not in normalized
        assert normalized['difficulty'] == 'easy'
        assert normalize_case(normalized) == normalize_case(normalize_case(normalized))


def test_product_case_level_is_metadata_not_evolution_level():
    case = deepcopy(corpus()['cases'][0])
    assert 'level' not in case and case['case_level']
    case['split'] = 'train'
    case['provenance']['group_id'] = 'case-level'
    first = evaluation_task(case, 'profile')
    case['case_level'] = '业务自定义标签'
    second = evaluation_task(case, 'profile')
    assert first['attributes']['product_source'].pop('case_level')
    assert second['attributes']['product_source'].pop('case_level') == '业务自定义标签'
    assert 'level' not in first['attributes']['product_source']
    assert first == second  # 元数据不控制输入、预算、评分标准或启用能力。


@pytest.mark.parametrize('text,expected', [
    ('1.QA\n2.\n3.观影', {1: ['QA'], 2: [], 3: ['观影']}),
    ('1.300001,100051\n2.\n3.', {1: ['300001', '100051'], 2: [], 3: []}),
    ('1. QA，GUI\r\n2. \t\r\n3. 居家', {1: ['QA', 'GUI'], 2: [], 3: ['居家']}),
])
def test_empty_numbered_labels_do_not_consume_the_next_turn(text, expected):
    assert parse_intents(text) == expected


def test_exact_deterministic_whole_dialogue_splits():
    dataset = corpus()
    sizes = {'train': 400, 'val': 200, 'holdout': 200}
    splits, report = prepare_product_splits(dataset, sizes, 20260921)
    assert (splits, report) == prepare_product_splits(dataset, sizes, 20260921)
    assert len(report['excluded_cases']) == 113
    assert report['eligible_cases'] == 961 and len(report['unselected_case_ids']) == 161
    ids, groups, inputs = set(), {}, {}
    for split, cases in splits.items():
        assert len(cases) == sizes[split]
        for case in cases:
            assert case['id'] not in ids
            ids.add(case['id'])
            group = case['provenance']['group_id']
            assert groups.setdefault(group, split) == split
            key = tuple(' '.join(t['user'].split()) for t in case['turns'])
            assert inputs.setdefault(key, split) == split
            assert all(t['user'] and t['expected_behavior'] for t in case['turns'])
    assert any(t['expected_skills'] is None for cs in splits.values() for c in cs for t in c['turns'])


def test_multiturn_preserved_and_answers_not_in_execution_input():
    splits, _ = prepare_product_splits(corpus(), {'train': 400, 'val': 200, 'holdout': 200}, 20260921)
    case = max((c for cs in splits.values() for c in cs), key=lambda c: len(c['turns']))
    task = evaluation_task(case, 'abc')
    assert [task['task_input'], *task['followup_inputs']] == [t['user'] for t in case['turns']]
    assert len(task['attributes']['turn_expectations']) == len(case['turns'])
    assert task['review_status'] == 'pending'
    assert not {'expected_behavior', 'expected_skills', 'turn_expectations'} & task['initial_state'].keys()
    assert task['split'] == case['split']


def test_missing_middle_turn_rejects_whole_case_and_short_pool_fails():
    data = corpus()
    case = next(c for c in data['cases'] if len(c['turns']) >= 3)
    case['turns'][1]['expected_behavior'] = ''
    splits, report = prepare_product_splits(data, {'train': 2, 'val': 2, 'holdout': 2}, 1)
    assert case['id'] in {c['case_id'] for c in report['excluded_cases']}
    with pytest.raises(ValueError):
        prepare_product_splits(data, {'train': 2000}, 1)


def trial(tmp_path, monkeypatch, preference='科幻'):
    monkeypatch.setenv('BUSINESS_EXECUTION_MODE', 'real')
    profile = load_profile(PACKAGE/'data/evaluation-profile.json')
    folder = tmp_path/preference
    folder.mkdir()
    initial = {'schema': 'business-simulator.initial-state.v1', 'profile_sha256': profile['sha256'],
               'precondition': '已注入声纹 B，用户喜好' + preference, 'device_type': '3', 'voiceprint_id': preference}
    path = folder/'initial-state.json'
    path.write_text(json.dumps(initial))
    monkeypatch.setenv('TRAJECTORY_EVOLUTION_TRIAL_INITIAL_STATE', str(path))
    monkeypatch.setenv('TRAJECTORY_EVOLUTION_TRIAL_CONFIG', str(folder/'instrumentation.json'))
    return path


def test_native_trial_initializes_once_per_state_and_isolates_candidates(tmp_path, monkeypatch):
    first = trial(tmp_path, monkeypatch)
    store = Store(); scope = scope_key('same-learning-user', 'same-conversation')
    state = fresh_state()
    assert state['preferences']['电影偏好'] == '科幻'
    assert state['preferences']['target_device'] == 'virtual-tv-1'
    state['preferences']['电影偏好'] = '喜剧'
    store.reset(scope, state)
    assert store.read(scope)['preferences']['电影偏好'] == '喜剧'
    trial(tmp_path, monkeypatch, '动画')
    assert database_path() != first.parent/'business.sqlite3'
    other = Store()
    assert other.read(scope)['preferences']['电影偏好'] == '动画'
    assert store.read(scope)['preferences']['电影偏好'] == '喜剧'


def test_changed_profile_and_untrusted_extra_initial_fields_fail_closed(tmp_path, monkeypatch):
    path = trial(tmp_path, monkeypatch)
    original = json.loads(path.read_text())
    for changes in ({'profile_sha256': 'changed'}, {'expected_behavior': 'pretend success'}):
        path.write_text(json.dumps({**original, **changes}))
        with pytest.raises(ValueError):
            fresh_state()
    monkeypatch.delenv('TRAJECTORY_EVOLUTION_TRIAL_CONFIG')
    assert 'evaluation_world' not in fresh_state()


@pytest.mark.parametrize('parallel_skills', [False, True])
def test_native_entry_real_daemon_local_model_only(tmp_path, monkeypatch, parallel_skills):
    """运行真实 v1，但用本地固定回复检验运输/采集；不是业务能力或 GEPA 收益实验。"""
    import http.server
    import os
    import subprocess
    import threading
    binary = REPO/'build/trajectory-evolution/trajectory_evolution'
    if not binary.exists() or not (REPO/'build/agentflowd').exists():
        pytest.skip('需要编译原生评测器和 daemon')
    requests = []

    class Model(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            requests.append(request)
            message = {'role': 'assistant', 'content': '离线协议测试完成。'}
            names = {t['function']['name'] for t in request.get('tools', [])}
            messages = request['messages']
            function, arguments = '', {}
            if parallel_skills and not any(m['role'] == 'tool' for m in messages):
                if 'run_skills' in names and sum(m['role'] == 'user' for m in messages) == 1:
                    function, arguments = 'run_skills', {'reason': '并发采集回归', 'skills': [
                        {'name': 'web-search', 'prompt': '查询北京今天的天气。'},
                        {'name': 'smart-home', 'prompt': '查询全屋设备，不修改状态。'}]}
                elif 'sim_web_search' in names:
                    function, arguments = 'sim_web_search', {'action': 'weather', 'location': '北京', 'date': '今天'}
                elif 'sim_smart_home' in names:
                    function, arguments = 'sim_smart_home', {'action': 'query'}
            if function:
                message.update(content='', tool_calls=[{'id': 'call-'+str(len(requests)), 'type': 'function',
                    'function': {'name': function, 'arguments': json.dumps(arguments, ensure_ascii=False)}}])
            body = json.dumps({'choices': [{'finish_reason': 'tool_calls' if function else 'stop', 'message': message}],
                'usage': {'prompt_tokens': 20, 'completion_tokens': 5, 'total_tokens': 25}}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers(); self.wfile.write(body)

    profile = load_profile(PACKAGE/'data/evaluation-profile.json')
    task = {'task_id': 'protocol', 'group_id': 'protocol', 'task_input': '请回复收到。', 'followup_inputs': ['再回复一次。'],
            'expected_behavior': '验收标记不可发送给 Agent', 'category': 'protocol', 'difficulty': 'easy',
            'initial_state': {'schema': 'business-simulator.initial-state.v1', 'profile_sha256': profile['sha256'],
                              'precondition': '', 'device_type': '', 'voiceprint_id': ''},
            'split': 'train', 'review_status': 'reviewed'}
    (tmp_path/'tasks.jsonl').write_text(json.dumps(task, ensure_ascii=False)+'\n')
    (tmp_path/'dataset.json').write_text(json.dumps({'schema_version': 1, 'name': 'business-protocol',
        'split_seed': '1', 'split_percent': [50, 25, 25], 'max_tasks': 1, 'max_model_calls': 0,
        'budget': {'max_steps': 128, 'max_tool_calls': 10, 'timeout_ms': 60000},
        'sources': [{'kind': 'golden', 'path': 'tasks.jsonl'}]}))
    subprocess.run([str(binary), 'dataset', 'build', '--config', str(tmp_path/'dataset.json'),
                    '--output-root', str(tmp_path/'dataset')], check=True, capture_output=True)
    configuration = prepare_configuration(tmp_path/'config', tmp_path/'dataset')
    settings = json.loads((configuration/'evaluation.json').read_text())
    assert settings['judge']['share_repeated_inputs'] is True
    # pytest 可通过解释器绝对路径启动，PATH 中的 python3 却属于另一套环境。
    # 本地协议测试必须使用同一解释器；否则缺依赖会被误读为真实 Agent 超时。
    settings['variants'][0]['command'][0] = sys.executable
    settings.update(splits=['train'], parallelism=1, timeout_ms=60000, judge={'enabled': False})
    expected_calls = 7 if parallel_skills else 2
    settings['checks'] = {'*': [{'id': 'real-calls', 'path': '/metrics/llm_calls', 'op': 'equals', 'value': expected_calls}]}
    (configuration/'evaluation.json').write_text(json.dumps(settings))
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    worker = threading.Thread(target=server.serve_forever, daemon=True); worker.start()
    env = {**os.environ, 'AF_LLM_API_KEY': 'protocol-test-opaque-credential-0123456789',
           'AF_LLM_BASE_URL': f'http://127.0.0.1:{server.server_port}/v1', 'AF_LLM_MODEL': 'local-protocol',
           'AF_LLM_THINKING_MODE': 'disabled', 'AF_LLM_TIMEOUT': '10'}
    try:
        result = subprocess.run([str(binary), 'dataset', 'run', str(tmp_path/'dataset'), '--config',
            str(configuration/'evaluation.json'), '--output-root', str(tmp_path/'result')],
            env=env, capture_output=True, text=True, timeout=75)
    finally:
        server.shutdown(); server.server_close(); worker.join(timeout=3)
    assert result.returncode == 0, result.stdout + result.stderr
    report = json.loads((tmp_path/'result/report.json').read_text())
    trial = report['trials'][0]
    assert trial['execution_status'] == 'completed' and len(trial['evidence']['traces']) == 2
    assert len(requests) == expected_calls
    text = json.dumps(requests, ensure_ascii=False)
    assert '验收标记不可发送' not in text and profile['clock'] in text
    assert '离线协议测试完成' in json.dumps(requests[-1]['messages'], ensure_ascii=False)
    assert trial['metrics']['llm_calls'] == expected_calls
    if parallel_skills:
        assert trial['metrics']['tool_calls'] == 2
        assert all(t['attributes']['capture_complete'] for t in trial['evidence']['traces'])
        assert env['AF_LLM_API_KEY'] not in (tmp_path/'result/report.json').read_text()
    assert not (tmp_path/'result/.work').exists()


@pytest.mark.parametrize('name', ['AF_EXEC_SKILL_KEY', 'AF_COMPLETED_BATCH_KEY'])
def test_public_databus_keys_do_not_disable_secret_filter(monkeypatch, name):
    monkeypatch.syspath_prepend(str(REPO/'tools/trajectory_evolution/clients/python'))
    from evolution_configured import _private_payload
    monkeypatch.setenv(name, 'dispatch:protocol-reference:batch:1')
    monkeypatch.setenv('BUSINESS_PRIVATE_CREDENTIAL', 'opaque-test-private-value-123456')
    fields = {'ref': {'run': 'protocol-run', 'key': 'dispatch:protocol-reference:batch:1'}}
    assert _private_payload(fields)  # 未声明公开标识时复现原来的误报。
    assert not _private_payload(fields, (name,))
    assert _private_payload({'output': 'opaque-test-private-value-123456'}, (name,))
    assert _private_payload({'output': 'Bearer fake-credential-format-1234567890'}, (name,))
