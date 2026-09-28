"""业务覆盖缺口回归：不调用模型、不访问外网、不依赖历史实验产物。"""
from copy import deepcopy
from datetime import datetime
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

import pytest

PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
sys.path[:0] = [str(PACKAGE/'harness'), str(PACKAGE/'evals'), str(REPO/'benchmark')]
from converters import convert_product, load_domain_map, merge_evalsets, split_steps
from evalset_schema import normalize_evalset, validate_evalset
from business_sim.evaluation_world import load_profile, seed
from business_sim.services import dispatch
from business_sim.state import Store, fresh_state, scope_key
from business_sim.task_context import task_context
from business_observer import BusinessObserver, multi_intent_metrics, observe_intents
from check_business_results import check_case


@pytest.mark.parametrize('text,expected', [
    ('1.第一问...2.第二问3.第三问', ['第一问...', '第二问', '第三问']),
    ('1.查询1.30G流量和2.5元余额2.继续问', ['查询1.30G流量和2.5元余额', '继续问']),
    ('1.甲\n2.\n3.丙', ['甲', '', '丙']),
    ('1.30G流量怎么领取', ['1.30G流量怎么领取']),
    ('解释Python 3.12版本', ['解释Python 3.12版本']),
])
def test_split_numbered_turns_without_decimal_or_empty_shift(text, expected):
    assert split_steps(text) == expected


def test_real_workbooks_preserve_eight_turns_intents_and_operation_codes():
    mapping = load_domain_map(REPO/'benchmark/domain_skill_map.json')
    dataset = normalize_evalset(merge_evalsets([convert_product(p, mapping, [])
        for p in sorted((REPO/'test_cases/product').glob('*.xlsx'))]))
    assert not validate_evalset(dataset)
    assert len(dataset['cases']) == 1074
    cases = {c['id']: c for c in dataset['cases']}
    assert len(cases['DH_YT_CYFF_02_033']['turns']) == 8
    turn = cases['TGYJ_YD2_001']['turns'][0]
    assert set(turn['expected_intents']) == {'QA', '观影', '居家'}
    assert set(turn['expected_operation_types']) == {'200002', '100051'}
    assert max(len(c['turns']) for c in dataset['cases']) >= 20


@pytest.fixture
def world(tmp_path, monkeypatch):
    monkeypatch.setenv('BUSINESS_EXECUTION_MODE', 'real')
    monkeypatch.delenv('BUSINESS_BACKEND_URL', raising=False)
    from business_sim import real_services
    def forbidden(*a, **k):
        raise AssertionError('snapshot unexpectedly called live network')
    for name in ('weather', 'search', 'fetch_json', 'image_generate'):
        monkeypatch.setattr(real_services, name, forbidden)
    store = Store(tmp_path/'business.sqlite3')
    profile = load_profile(PACKAGE/'data/evaluation-profile.json')
    scope = scope_key('user', 'chat')
    state = fresh_state(); seed(state, profile); store.reset(scope, state)
    return store, scope, profile


def call(world, skill, **args):
    return dispatch(skill, args, store=world[0], scope=world[1])


def test_seeded_history_is_computed_and_wall_clock_does_not_advance_world(world, monkeypatch):
    from business_sim.device_world import tick
    # 主机日期跳变不能快进冻结业务世界；物理推进仅取单调时钟的真实差值。
    monkeypatch.setattr('business_sim.device_world.time.monotonic', lambda: 1000.)
    monkeypatch.setattr('business_sim.device_world.time.time', lambda: 9999999999.)
    before = world[0].read(world[1])
    answer = call(world, 'smart-home', action='habits')
    assert answer['status'] == 'succeeded'
    assert answer['UserOutputObject']['enough_history'] is True
    assert len(answer['UserOutputObject']['modeList']) == 2
    tick(world[0])
    after = world[0].read(world[1])
    assert before['world']['last_tick'] == after['world']['last_tick']
    assert before['home_energy'] == after['home_energy']


def test_frozen_device_clock_advances_after_serialization(monkeypatch):
    """每个工具使用新 Python 进程时，设备仍按已持久化的同源时钟渐进移动。"""
    from business_sim import device_world
    from business_sim.evaluation_world import clock
    frozen = '2026-09-25T09:00:00+08:00'
    state = {'evaluation_world': {'clock': frozen}}
    monkeypatch.setattr(device_world.time, 'monotonic', lambda: 1000.)
    answer = device_world.execute(state, {'action': 'control', 'commands': [
        {'device_id': 'living-curtain', 'action': 'off'}]})
    curtain = answer['UserOutputObject']['devices'][0]
    assert answer['UserOutputObject']['settled'] is False
    assert curtain['observed']['position'] == 100 and curtain['moving'] is True
    # 显式传输 JSON 到另一个解释器；不能依赖上一个进程的全局变量或时钟锚点。
    program = """import json,sys
sys.path.insert(0, sys.argv[1])
from business_sim import device_world
state=json.load(sys.stdin)
device_world.time.monotonic=lambda:1004.
device_world.execute(state, {'action':'query'})
json.dump(state,sys.stdout)
"""
    child = subprocess.run([sys.executable, '-c', program, str(PACKAGE/'harness')],
                           input=json.dumps(state), capture_output=True, text=True, timeout=10)
    assert child.returncode == 0, child.stderr
    state = json.loads(child.stdout)
    curtain = next(d for d in state['devices'] if d['id'] == 'living-curtain')
    assert curtain['observed']['position'] == 80 and curtain['moving'] is True
    assert state['world']['last_tick'] == datetime.fromisoformat(frozen).timestamp() + 4
    monkeypatch.setattr(device_world.time, 'monotonic', lambda: 1005.)
    device_world.advance(state)
    assert curtain['observed']['position'] == 75 and curtain['moving'] is True
    assert clock(state).isoformat() == frozen


def test_frozen_device_clock_explicit_now_and_rebase(monkeypatch):
    """显式 now 不被业务日期覆盖；时钟回退仅重锚，不倒退设备或伪造完成。"""
    from business_sim import device_world
    frozen = '2026-09-25T09:00:00+08:00'
    start = datetime.fromisoformat(frozen).timestamp()
    state = {'evaluation_world': {'clock': frozen}}
    monkeypatch.setattr(device_world.time, 'monotonic', lambda: 1000.)
    device_world.execute(state, {'action': 'control', 'commands': [
        {'device_id': 'living-curtain', 'action': 'off'}]}, now=start)
    device_world.execute(state, {'action': 'query'}, now=start + 2)
    curtain = next(d for d in state['devices'] if d['id'] == 'living-curtain')
    assert curtain['observed']['position'] == 90 and curtain['moving'] is True
    monkeypatch.setattr(device_world.time, 'monotonic', lambda: 50.)
    device_world.advance(state)
    assert state['world']['last_tick'] == start + 2
    assert curtain['observed']['position'] == 90
    monkeypatch.setattr(device_world.time, 'monotonic', lambda: 51.)
    device_world.advance(state)
    assert curtain['observed']['position'] == 85
    device_world.advance(state, now=start + 20)
    assert curtain['observed']['position'] == 0 and curtain['moving'] is False
    assert state['evaluation_world']['clock'] == frozen


def test_frozen_device_worker_tick_uses_elapsed_clock(world, monkeypatch):
    """后台 tick 与工具调用共用 SQLite 中的锚点，同一时刻不会被重复推进。"""
    from business_sim import device_world
    monkeypatch.setattr(device_world.time, 'monotonic', lambda: 1000.)
    monkeypatch.setattr(device_world.time, 'time', lambda: 9999999999.)
    controlled = call(world, 'smart-home', action='control', commands=[
        {'device': '客厅窗帘', 'action': 'off'}])
    assert controlled['status'] == 'succeeded', controlled
    assert controlled['UserOutputObject']['settled'] is False
    before = world[0].read(world[1])
    monkeypatch.setattr(device_world.time, 'monotonic', lambda: 1005.)
    device_world.tick(world[0])
    after = world[0].read(world[1])
    curtain = next(d for d in after['devices'] if d['id'] == 'living-curtain')
    assert curtain['observed']['position'] == 75 and curtain['moving'] is True
    assert after['world']['last_tick'] == before['world']['last_tick'] + 5
    assert after['evaluation_world']['clock'] == before['evaluation_world']['clock']
    device_world.tick(world[0])
    assert world[0].read(world[1]) == after


def test_movie_followups_and_product_knowledge_use_fixed_snapshot(world):
    movies = call(world, 'movie-query', action='recommend', genre='科幻', count=2)
    assert movies['status'] == 'succeeded'
    assert len(movies['UserOutputObject']['aiMovieData']['list']) == 2
    selected = call(world, 'movie-query', action='play', index=2)
    assert selected['UserOutputObject']['player']['title'] == movies['UserOutputObject']['aiMovieData']['list'][1]['title']
    assert call(world, 'movie-query', action='pause')['UserOutputObject']['player']['status'] == 'paused'
    assert call(world, 'movie-query', action='resume')['UserOutputObject']['player']['status'] == 'playing'
    knowledge = call(world, 'knowledge-base', action='search', query='云手机注册登录')
    assert knowledge['status'] == 'succeeded' and knowledge['UserOutputObject']['results']
    assert knowledge['snapshot_sha256'] == world[2]['sha256']


def test_home_statistics_bound_output_without_losing_totals(world):
    """长历史只影响统计量，不使工具结果随遥测条数无限膨胀。"""
    before = world[0].read(world[1])
    answer = call(world, 'smart-home', action='habits')
    obj = answer['UserOutputObject']
    assert obj['telemetry_records'] > 4000
    assert len(json.dumps(answer, ensure_ascii=False).encode()) < 12000
    assert len(obj['history']) <= 3
    assert all('before' not in row and 'after' not in row for row in obj['history'])
    assert sum(row['powered_hours'] for row in obj['energy']) == pytest.approx(
        sum(sum(row['powered_hours']) for row in obj['hourly_usage']), abs=.01)
    assert sum(row['energy_kwh'] for row in obj['energy']) == pytest.approx(
        sum(row['energy_kwh'] for row in before['home_energy'].values()))
    for row in obj['hourly_usage']:
        assert len(row['powered_hours']) == len(row['observed_days']) == 24
    assert world[0].read(world[1])['home_energy'] == before['home_energy']
    empty = call(world, 'smart-home', action='habits', date='2020-01-01')['UserOutputObject']
    assert empty['hourly_usage'] == [] and empty['telemetry_records'] == 0


def test_weather_repeatable_and_external_mutations_do_not_escape(world):
    args = dict(action='weather', location='北京', date='下周一')
    first = call(world, 'web-search', **args)
    assert first == call(world, 'web-search', **args)
    assert first['UserOutputObject']['start_date'] == '2026-09-28'
    assert first['UserOutputObject']['rain_expected'] is True
    answer = call(world, 'multimodal', action='generate_image', content='测试图片')
    assert answer['status'] == 'unsupported' and answer['error'] == 'snapshot_miss'


def test_inventory_isolated_across_evaluation_users(world):
    ids = []
    for i in range(3):
        scope = scope_key('candidate-'+str(i), 'chat')
        state = fresh_state(); seed(state, world[2]); world[0].reset(scope, state)
        answer = dispatch('health-advice', {'action': 'appointment_create', 'department': '内科', 'date': '明天上午9点'}, store=world[0], scope=scope)
        assert answer['status'] == 'succeeded', answer
        ids.append(answer['UserOutputObject']['appointment']['slot_id'])
    assert len(set(ids)) == 3


def test_branch_request_ids_do_not_replay_another_branch(world):
    for branch, value in [('a', '科幻'), ('b', '动画')]:
        with task_context(branch):
            answer = call(world, 'user-memory', action='write', key='电影偏好', value=value, request_id='same')
            assert answer['status'] == 'succeeded', answer
            assert not answer.get('replayed')
    assert world[0].read(world[1])['preferences']['电影偏好'] == '动画'


def test_observer_keeps_turn_evidence_not_entire_history_and_intents_are_observational(world):
    observer = BusinessObserver(world[0], world[1])
    call(world, 'movie-query', action='recommend', genre='科幻')
    result = {'dispatched_skills': ['movie-query']}
    turn = {'user': '推荐电影', 'expected_skills': ['movie-query', 'web-search'],
            'expected_intents': ['观影', 'QA'], 'expected_operation_types': ['200002']}
    observer(result, turn, 1)
    assert result['business_rule_pass'] is True  # 漏派观察不会改业务结果
    assert result['intent_observation']['missing'] == ['web-search']
    assert result['execution_evidence']['total_events'] == 1
    assert 'home_history' not in json.dumps(result)
    following = {'dispatched_skills': []}
    observer(following, {'user': '谢谢', 'expected_skills': []}, 2)
    assert following['execution_evidence']['total_events'] == 0


def test_multi_intent_metrics_deduplicate_and_separate_conditions():
    turn = {'user': '查天气并关灯', 'expected_skills': ['web-search', 'smart-home'], 'expected_intents': ['QA', '居家']}
    observed = observe_intents(turn, ['web-search', 'web-search', 'smart-home'])
    conditional = observe_intents({**turn, 'user': '如果下雨就关灯'}, ['web-search'])
    metrics = multi_intent_metrics([{'turns': [{'intent_observation': observed}, {'intent_observation': conditional}]}])
    assert metrics['measured_turns'] == 1 and metrics['conditional_turns'] == 1
    assert metrics['precision'] == metrics['recall'] == metrics['complete_rate'] == 1
    assert metrics['observation_only'] is True


def test_non_recharge_gui_not_checked_as_recharge(world):
    answer = call(world, 'device-gui', action='plan', app='爱奇艺', operation='play', title='太平年', episode=2)
    assert answer['status'] == 'succeeded', answer
    case = {'case_id': 'CK_GUI_FHCK_00_007', 'turns': [{'response': '操作步骤', 'checks': {}, 'judge_pass': True}],
            'business_execution': {'state': world[0].read(world[1]), 'events': world[0].events(world[1])}}
    checked = check_case(case)
    assert checked['outcome'] == 'passed', checked
    assert not any('充值' in item['name'] for item in checked['checks'])


def test_no_history_profile_keeps_missing_history_distinct(world):
    profile = deepcopy(world[2]); profile['history_days'] = 0
    state = fresh_state(); seed(state, profile); world[0].reset(world[1], state)
    result = call(world, 'smart-home', action='habits')
    assert result['UserOutputObject']['enough_history'] is False


def test_snapshot_clock_drives_reminders_without_wall_time_delivery(world):
    from business_sim import reminders
    from datetime import datetime
    created = call(world, 'health-advice', action='reminder_create', medicine='测试药', times=['10:00'])
    assert created['status'] == 'succeeded'
    assert created['UserOutputObject']['medicationReminder']['created_at'] == world[2]['clock']
    reminders.tick(world[0], now=datetime.fromisoformat('2030-01-01T00:00:00+00:00'))
    assert not reminders.notifications(world[0], world[1])
    state = world[0].read(world[1]); state['evaluation_world']['clock'] = '2026-09-21T10:01:00+08:00'
    world[0].reset(world[1], state)
    assert reminders.tick(world[0]) == 1
    assert len(call(world, 'health-advice', action='notifications')['UserOutputObject']['notifications']) == 1


def test_snapshot_device_listing_never_probes_physical_devices(world, monkeypatch):
    from business_sim import device_connectors
    monkeypatch.setattr(device_connectors, 'physical_devices', lambda: pytest.fail('physical probe'))
    answer = call(world, 'multi-device', action='list')
    assert answer['status'] == 'succeeded' and answer['UserOutputObject']['physical_count'] == 0


def test_live_player_is_shared_across_dialogue_dispatches(tmp_path, monkeypatch):
    from business_sim import movies
    from business_sim.task_context import current_task
    monkeypatch.setenv('BUSINESS_EXECUTION_MODE', 'real')
    seen = []
    def playback(*args):
        seen.append(current_task())
        return {'status': 'succeeded', 'UserOutputObject': {}, 'UserOutputText': 'ok'}
    monkeypatch.setattr(movies, 'playback', playback)
    store, scope = Store(tmp_path/'live.sqlite3'), scope_key('user', 'chat')
    for task, action in [('first', 'play'), ('second', 'pause')]:
        with task_context(task):
            assert dispatch('movie-query', {'action': action, 'title': '测试'}, store=store, scope=scope)['status'] == 'succeeded'
    assert seen == ['movie-player', 'movie-player']


@pytest.mark.parametrize('business,judge,expected', [(False, True, False), (True, True, True), (True, False, False)])
def test_runner_business_extension_is_opt_in_and_judge_sees_execution(world, monkeypatch, business, judge, expected):
    import run_eval as runner
    class Client:
        def __init__(self, *a, **kw): pass
        def connect(self): pass
        def close(self): pass
    monkeypatch.setattr(runner, 'RpcClient', Client)
    monkeypatch.setattr(runner, 'drain_stale_responses', lambda *a: None)
    monkeypatch.setattr(runner, 'send_turn', lambda *a: call(world, 'movie-query', action='recommend', genre='科幻'))
    monkeypatch.setattr(runner, 'wait_turn_response', lambda *a: {'final_text': '推荐片单', 'texts': [], 'timed_out': False, 'error': ''})
    monkeypatch.setattr(runner, 'find_sessionlog', lambda *a: None)
    monkeypatch.setattr(runner, 'rule_check', lambda *a: {'response_nonempty': True, 'missing_include': [], 'hit_not_include': [], 'skills_exact': False})
    def judge_score(cfg, messages):
        if business:
            assert 'movie-query' in messages[-1]['content'] and '200002' in messages[-1]['content']
        return 5, 'test judge'
    monkeypatch.setattr(runner, 'judge_score', judge_score)
    cfg = {'host': 'localhost', 'port': 1, 'agent_id': 'business-simulator', 'user_id': 'test',
           'turn_timeout': 30, 'judge': judge, 'judge_cfg': {}, 'judge_threshold': 4}
    if business:
        cfg.update(intent_observation_only=True, turn_observer=BusinessObserver(world[0], world[1]))
    result = runner.run_case({'id': 'unit', 'category': 'movie', 'turns': [
        {'user': '推荐科幻电影', 'expected_skills': ['web-search'], 'expected_operation_types': ['200002']}]}, cfg)
    assert result['case_success'] is expected
    if business and not judge:
        assert result['turns'][0]['business_verdict'] == 'needs_judge'


def test_http_backend_receives_worker_branch_identity(tmp_path, monkeypatch):
    from business_sim import backend_client, real_services
    from business_sim.backend import BusinessServer
    from business_sim.task_context import current_task
    seen = []
    def execute(skill, args, store, scope):
        seen.append(current_task())
        return {'status': 'succeeded', 'UserOutputObject': {}, 'UserOutputText': 'ok'}
    monkeypatch.setattr(real_services, 'execute', execute)
    monkeypatch.setenv('BUSINESS_EXECUTION_MODE', 'real')
    monkeypatch.setenv('AF_USER_ID', 'user'); monkeypatch.setenv('AF_CLIENT_ID', 'chat')
    with_server = BusinessServer(('127.0.0.1', 0), tmp_path/'http.sqlite3').start()
    try:
        monkeypatch.setenv('BUSINESS_BACKEND_URL', with_server.url)
        for key in ('dispatch:一', 'dispatch:二'):
            monkeypatch.setenv('AF_EXEC_SKILL_KEY', key)
            assert backend_client.call('web-search', {'action': 'query', 'query': 'test'})['status'] == 'succeeded'
        assert seen == ['dispatch:一', 'dispatch:二']
    finally:
        with_server.close()
