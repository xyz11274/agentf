"""Focused tests for AppWorld code-shaped native-view redaction."""

from __future__ import annotations

import ast
import json
import os
import sys
import pytest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = Path(__file__).resolve().parents[4]
ADAPTER = ROOT / "examples/agent/business-simulator/appworld"
sys.path.insert(0, str(ROOT / "packages/agentflow-runtime/src"))
sys.path.insert(0, str(ADAPTER))

from appworld_host import REDACTION, Trial, appworld_failure_code, native_contains_secret, native_view, real_clock  # noqa: E402


def test_appworld_advice_cache_limit_reads_snapshot_and_preserves_l0_default(tmp_path, monkeypatch):
    from appworld_skill import _policy_advice_cache_limit

    assert _policy_advice_cache_limit({"policy": {"snapshot": {
        "options": {"max_advice_items": 1}}}}) == 1
    assert _policy_advice_cache_limit({"policy": {"snapshot": {
        "options": {"max_advice_items": 2}}}}) == 2
    assert _policy_advice_cache_limit({}) == 2

    config = tmp_path / "trial.json"
    config.write_text("{}")
    (tmp_path / "version.json").write_text(json.dumps({
        "artifact": {"policy": {"snapshot": {
            "options": {"max_advice_items": 1}}}}}))
    monkeypatch.setenv("TRAJECTORY_EVOLUTION_TRIAL_CONFIG", str(config))
    assert _policy_advice_cache_limit({}) == 1


def test_appworld_advice_cache_observer_uses_policy_cap_and_l0_fallback():
    from appworld_skill import NativeFrameworkObserver

    outer = {"run_id": "fixture-run"}
    cards = [{"scope": {"run_id": "fixture-run", "skill": "appworld-office",
                          "task_id": "fixture", "actor": "skill_executor"},
              "content": "card-1"},
             {"scope": {"run_id": "fixture-run", "skill": "appworld-office",
                          "task_id": "fixture", "actor": "skill_executor"},
              "content": "card-2"}]
    state = {"native_context": outer, "native_advice_cache_limit": 1,
             "native_advice_cache": cards}
    observer = NativeFrameworkObserver(client=object(), state_reader=lambda: state,
                                       project=lambda value: value)
    scope = {"run_id": "fixture-run", "skill": "appworld-office",
             "task_id": "fixture", "actor": "skill_executor"}
    assert observer.native_advice_cache_limit == 1
    assert observer._cached_cards(scope) == ["card-1"]

    state = {"native_context": outer, "native_advice_cache": cards}
    observer = NativeFrameworkObserver(client=object(), state_reader=lambda: state,
                                       project=lambda value: value)
    assert observer.native_advice_cache_limit == 2
    assert observer._cached_cards(scope) == ["card-1", "card-2"]


def test_real_main_replan_is_verified_by_its_actual_framework_model(tmp_path, monkeypatch):
    """Native receipts must follow real model evidence, without double counting turns."""
    import re
    import selectors
    import subprocess
    import time
    from types import SimpleNamespace
    import pytest
    from appworld_skill import NativeFrameworkObserver
    from appworld_host import loop_model_policy
    sys.path.insert(0, str(ROOT / 'tools/trajectory_evolution/clients/python'))
    from evolution import Evolution
    build = ROOT / 'tools/trajectory_evolution/build-v4'
    cli = Path(os.environ.get('APPWORLD_TEST_CLI', str(build / 'trajectory_evolution')))
    if not cli.is_file() or not (build / 'test_integration_service').is_file():
        pytest.skip('native fixture binaries unavailable')
    folder = tmp_path / 'native'
    subprocess.run([str(build / 'test_integration_service'), '--fixture', str(folder)],
                   check=True, capture_output=True)
    config_path = folder / 'service.json'
    config = json.loads(config_path.read_text())
    config.update(auto_evaluate=False, auto_approve_experiences=False,
                  actors=['default', 'assistant', 'worker'], port=0,
                  instrumentation={'level': 'L3', 'capabilities': ['model', 'actions', 'loop']})
    config['runtime_options'].update(action_rules=[], approved_experiences=[], loop_rules=[{
        'rule_id': 'fixture-recovery', 'target_scope': 'agent:my-agent/actor:default',
        'max_iterations': 30, 'max_no_progress': 1, 'max_replans': 1,
        'no_progress_decision': 'replan', 'replan_context': 'Inspect the actual failed request and change it.',
        'reason': 'Fixture bounded recovery'}])
    config_path.write_text(json.dumps(config))
    env = dict(os.environ, INTEGRATION_TEST_A='fixture-principal-alpha-00000001',
               INTEGRATION_TEST_B='fixture-principal-bravo-00000002')
    server = subprocess.Popen([str(cli), 'serve', '--config', str(config_path)],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    monkeypatch.delenv('APPWORLD_RUNTIME_STATE', raising=False)
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(server.stdout, selectors.EVENT_READ)
            assert selector.select(15), 'native fixture startup timeout'
            line = server.stdout.readline()
        port = re.search(r'listening=127.0.0.1:(\d+)', line)
        assert port, line
        client = Evolution('http://127.0.0.1:' + port.group(1), env['INTEGRATION_TEST_A'], retries=0)
        def point(**packet):
            result = client.invoke('point', packet)
            assert not result.get('degraded'), result
            return result
        results = {}
        for mode in ('consumed', 'not-consumed', 'L0'):
            level = 'L0' if mode == 'L0' else 'L3'
            run = point(phase='before', kind='run', level=level,
                        input={'instruction': 'public fixture task'}, conversation_id=mode,
                        capabilities=['model', 'actions', 'loop'])
            first = point(phase='before', kind='loop', context=run['context'], state={'progress': 0})
            point(phase='after', kind='loop', context=first['context'], decision=first['decision'])
            loop = point(phase='before', kind='loop', context=run['context'], state={'progress': 0})
            decision, policy = loop_model_policy('Original public task policy.', loop)
            assert decision == ('continue' if level == 'L0' else 'replan')
            point(phase='after', kind='loop', context=loop['context'], decision=decision)
            wrapper = point(phase='before', kind='agent-turn', name='appworld-agentflow',
                            context=loop['context'], input={'system': policy})
            state = {'native_context': wrapper['context'], 'turn': 1}
            if decision == 'replan':
                state['native_replan_context'] = loop['replan_context']
            observer = NativeFrameworkObserver(client, state_reader=lambda: state, project=lambda value: value)
            messages = [{'role': 'system', 'content': 'Original skill instructions.'},
                        {'role': 'user', 'content': policy}]
            original = json.loads(json.dumps(messages))
            meta = {'node_id': 'skill_executor', 'exec_skill_key': 'fixture-skill', 'round': 1,
                    'source': 'skill_executor', 'skill_name': 'appworld-office'}
            pending = observer.before_model(messages, meta)
            assert pending is not None
            if level == 'L0':
                assert messages == original
                assert pending['context']['loop_id'].startswith('framework:')
            else:
                assert pending['context']['loop_id'] == 'main'
                assert pending['context']['control_id'] == loop['context']['control_id']
                assert loop['replan_context'] in messages[0]['content']
            actual = {'messages': messages if mode != 'not-consumed' else original}
            observer.handle(SimpleNamespace(name='llm.request.prepared', payload={
                'call_id': 'fixture-call', 'request': actual, 'meta': meta}), None)
            recorder = SimpleNamespace(_client=SimpleNamespace(llm=lambda: SimpleNamespace(
                get=lambda key: {'transport_attempts': 1})))
            response = {'_agentflow_call_id': 'fixture-call',
                        'choices': [{'message': {'role': 'assistant', 'content': 'public reply'}}],
                        'usage': {'prompt_tokens': 10, 'completion_tokens': 2, 'total_tokens': 12}}
            observer.finish_model(recorder, response)
            observer.finish_model(recorder, response)  # repeated observer callback cannot add an execution
            point(phase='after', kind='agent-turn', context=wrapper['context'], executed=True,
                  actual_input={'system': policy}, output={'code': 'print(1)'})
            done = point(phase='after', kind='run', context=run['context'], output={'finished': True})
            assert done['capture_complete'] and done['closed']
            assert done['metrics']['llm_calls'] == 1 and done['metrics']['tokens'] == 12
            trace_path = folder / 'data/by-user/user-a/traces' / mode / (run['context']['run_id'] + '.trace.json')
            deadline = time.monotonic() + 5
            while not trace_path.is_file() and time.monotonic() < deadline:
                time.sleep(.02)
            trace = json.loads(trace_path.read_text())
            receipts = [e['attributes']['receipt']['application']['status'] for e in trace['events']
                        if e['kind'] == 'advice.applied'
                        and e.get('attributes', {}).get('receipt', {}).get('boundary') == 'loop']
            results[mode] = receipts
        assert 'applied' in results['consumed'], results
        assert 'applied' not in results['not-consumed'], results
        assert not results['L0'], results
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait(timeout=3)


def test_code_literals_are_redacted_without_collapsing_structure():
    code = (
        'headers = {"Authorization": "Bearer abcdefghijklmnop1234"}\n'
        'result = apis.login(password="actual-secret-value-123", '
        'access_token="ark-12345678901234567890")\n'
        'return result\n'
    )
    safe = native_view(code)
    assert REDACTION in safe
    assert "apis.login" in safe and "return result" in safe
    assert "actual-secret-value-123" not in safe
    assert "ark-12345678901234567890" not in safe
    ast.parse(safe)


def test_identifier_and_attribute_references_are_not_misclassified_as_literals():
    from types import SimpleNamespace
    class PublicApi:
        def login(self, **arguments):
            return arguments
        def get_access_token(self):
            return "synthetic public result"
    cases = [
        "result = apis.login(password=password, access_token=session.access_token)\n",
        "result = apis.login(password=account_password_reference)\n",
        'access_token = login_response["access_token"]\n',
        "access_token = apis.get_access_token()\n",
    ]
    assert native_view("result = apis.login(password=password)\n") == "result = apis.login(password=password)\n"
    for code in cases:
        safe = native_view(code)
        assert REDACTION not in safe
        assert not native_contains_secret(safe)
        assert ast.dump(ast.parse(safe)) == ast.dump(ast.parse(code))
        assert native_view(safe) == safe
        outcomes = []
        for executable in (code, safe):
            context = {"apis": PublicApi(), "password": "public fixture password",
                       "account_password_reference": "public fixture password",
                       "session": SimpleNamespace(access_token="public fixture result"),
                       "login_response": {"access_token": "public fixture result"}}
            exec(executable, context)
            outcomes.append(context.get("result", context.get("access_token")))
        assert outcomes[0] == outcomes[1]
        serialized = json.dumps({"arguments": {"code": code},
                                 "history": [{"role": "assistant", "content": code}]})
        projected = json.loads(native_view(serialized))
        assert not native_contains_secret(json.dumps(projected))
        assert REDACTION not in projected["arguments"]["code"]
        assert ast.dump(ast.parse(projected["arguments"]["code"])) == ast.dump(ast.parse(code))
        assert ast.dump(ast.parse(projected["history"][0]["content"])) == ast.dump(ast.parse(code))


def test_unquoted_credential_like_value_is_quoted_in_persisted_code():
    code = "result = apis.login(password=actual-secret-value-123)\n"
    safe = native_view(code)
    assert REDACTION in safe
    assert "actual-secret-value-123" not in safe
    ast.parse(safe)


def test_compound_credential_assignment_preserves_api_call_structure():
    sources = [
        'phone_token = apis.phone.login(username=username, password=password)\n',
        'spotify_token = apis.spotify.login(username=username, password=password)["access_token"]\n',
        'phone_token: str = apis.phone.login(username=username, password=password)\n',
        'phone_password = accounts["phone"]["password"]\n',
        'result = apis.phone.show_contacts(access_token=phone_token)\n',
    ]
    for source in sources:
        safe = native_view(source, preserve_credential_call_structure=True)
        assert ast.dump(ast.parse(safe)) == ast.dump(ast.parse(source))
        assert not native_contains_secret(safe)
        assert native_view(safe, preserve_credential_call_structure=True) == safe
        nested = json.loads(native_view(json.dumps({'code':source}),
                           preserve_credential_call_structure=True))['code']
        assert ast.dump(ast.parse(nested)) == ast.dump(ast.parse(source))


def test_compound_credential_login_preserves_call_and_masks_literal_values():
    source = ('phone_token = apis.phone.login(username=username, '
              'password="synthetic-fixture-password-123456")\n')
    safe = native_view(source, preserve_credential_call_structure=True)
    assert 'synthetic-fixture-password-123456' not in safe
    original, projected = ast.parse(source), ast.parse(safe)
    call = projected.body[0].value
    assert isinstance(call, ast.Call)
    assert ast.dump(call.func) == ast.dump(original.body[0].value.func)
    assert [x.arg for x in call.keywords] == ['username', 'password']
    assert call.keywords[1].value.value == REDACTION
    assert projected.body[0].targets[0].id == 'phone_token'
    assert not native_contains_secret(safe)
    assert native_view(safe, preserve_credential_call_structure=True) == safe


def test_credential_call_projection_fix_is_explicit_and_legacy_default_exact():
    source = 'phone_token=apis.phone.login(username=username,password=password)'
    legacy = "phone_token='<redacted:credential_field>'(username=username,password=password)"
    assert native_view(source) == legacy
    assert native_view(source, preserve_credential_call_structure=False) == legacy
    assert ast.dump(ast.parse(native_view(source, preserve_credential_call_structure=True))) == ast.dump(ast.parse(source))
    nested = {'code':source,'history':[{'role':'assistant','content':source}]}
    assert native_view(nested) == {'code':legacy,'history':[{'role':'assistant','content':legacy}]}


def _local_projection_fixture(tmp_path, code=None, observation=None):
    root = tmp_path / 'public-evidence'
    report = root / 'capture/report.json'
    report.parent.mkdir(parents=True)
    report.write_text('{}')
    trial_id, run_id, task_id = 'a' * 32, 'b' * 40, 'appworld-fixture_1'
    official = root / 'official-evidence' / ('agentflow-' + 'c' * 32)
    log = official / 'tasks/fixture_1/logs/environment_io.md'
    log.parent.mkdir(parents=True)
    code = code or ('phone_token=apis.phone.login(username=username, '
                    'password="synthetic-fixture-password-123456")')
    observation = 'Execution successful.' if observation is None else observation
    log.write_text(f'```python\n{code.strip()}\n```\n\n```\n{observation.rstrip()}\n```\n')
    old = native_view(code)
    row = {'trial_id':trial_id, 'task_id':task_id, 'evidence': {
        'execution_run_id':run_id,
        'output':{'official_evidence_dir':str(official), 'official_experiment_name':official.name},
        'trace':{'identity':{'run_id':run_id, 'conversation_id':trial_id}, 'events':[
            {'event_id':'event-1','sequence':'1','kind':'run.start','input':{'task_id':task_id}},
            {'event_id':'event-2','sequence':'1','kind':'action.start','action_id':'execution-1','input':{'code':old}},
            {'event_id':'event-3','sequence':'2','kind':'action.end','action_id':'execution-1',
             'attributes':{'executed':True}, 'input':{'code':old}, 'output':{'observation':observation}}]}}}
    atomic = report.parent / 'trials' / (trial_id + '.json')
    atomic.parent.mkdir()
    atomic.write_text(json.dumps(row))
    request = {'schema_version':1,'source_report':str(report),'trial_id':trial_id,
               'task_id':task_id,'execution_run_id':run_id}
    return root, request, row, atomic, log


def test_projection_command_only_metadata_and_exact_bound_sanitized_code(tmp_path):
    from appworld_host import project_official_code_request
    root, request, row, atomic, log = _local_projection_fixture(tmp_path)
    before = atomic.read_bytes()
    result = project_official_code_request(request, evidence_root=root)
    assert result['status']=='complete'
    assert {p['event_id'] for p in result['patches']}=={'event-2','event-3'}
    for patch in result['patches']:
        assert patch['run_id']==request['execution_run_id']
        assert patch['old_code']==row['evidence']['trace']['events'][1]['input']['code']
        assert patch['source']=={'path':str(log),'input_index':0}
        assert 'apis.phone.login' in patch['code']
        assert 'synthetic-fixture-password-123456' not in patch['code']
    assert not native_contains_secret(json.dumps(result))
    assert atomic.read_bytes()==before
    assert set(request)=={'schema_version','source_report','trial_id','task_id','execution_run_id'}


def _completed_projection_fixture(tmp_path):
    root, historical, row, atomic, log = _local_projection_fixture(tmp_path)
    row['execution_status'] = 'completed'
    row['evidence']['execution_status'] = 'completed'
    row['evidence']['output']['evaluation_status'] = 'completed'
    snapshot = atomic.parent.parent / '.work' / row['trial_id'] / 'completed-execution.json'
    snapshot.parent.mkdir(parents=True)
    snapshot.write_text(json.dumps(row))
    request = {key: value for key, value in historical.items() if key != 'source_report'}
    request.update(source_kind='completed_capture', source_trial=str(snapshot))
    return root, request, row, snapshot, log


def test_projection_command_completed_capture_has_no_report_or_manifest(tmp_path):
    from appworld_host import project_official_code_request
    root, request, row, snapshot, log = _completed_projection_fixture(tmp_path)
    before = snapshot.read_bytes()
    (snapshot.parent.parent.parent / 'report.json').unlink()
    result = project_official_code_request(request, evidence_root=root)
    assert result['status'] == 'complete'
    assert {patch['event_id'] for patch in result['patches']} == {'event-2', 'event-3'}
    assert all(patch['source'] == {'path': str(log), 'input_index': 0} for patch in result['patches'])
    assert snapshot.read_bytes() == before
    assert not native_contains_secret(json.dumps(result))
    assert set(request) == {'schema_version', 'source_kind', 'source_trial', 'trial_id', 'task_id', 'execution_run_id'}


@pytest.mark.parametrize('mode', ['kind', 'path', 'trial', 'run', 'incomplete', 'mixed_protocol'])
def test_projection_command_completed_capture_rejects_unverified_or_incomplete_source(tmp_path, mode):
    from appworld_host import project_official_code_request
    root, request, row, snapshot, _ = _completed_projection_fixture(tmp_path)
    if mode == 'kind': request['source_kind'] = 'report'
    elif mode == 'path':
        invalid = snapshot.parent / 'other.json'
        invalid.write_text(snapshot.read_text())
        request['source_trial'] = str(invalid)
    elif mode == 'trial': row['trial_id'] = 'd' * 32
    elif mode == 'run': row['evidence']['execution_run_id'] = 'd' * 40
    elif mode == 'incomplete': row['execution_status'] = 'running'
    else: request['source_report'] = str(snapshot)
    snapshot.write_text(json.dumps(row))
    result = project_official_code_request(request, evidence_root=root)
    assert result['status'] == 'unknown' and result['patches'] == []


@pytest.mark.parametrize('mode', ['trial','task','run','old_code','observation','duplicate_event','mixed_run'])
def test_projection_command_rejects_mismatched_or_ambiguous_source(tmp_path, mode):
    from appworld_host import project_official_code_request
    root, request, row, atomic, _ = _local_projection_fixture(tmp_path)
    if mode=='trial': row['trial_id']='d'*32
    elif mode=='task': row['task_id']='appworld-other_1'
    elif mode=='run': row['evidence']['execution_run_id']='d'*40
    elif mode=='old_code': row['evidence']['trace']['events'][-1]['input']['code']='different()'
    elif mode=='observation': row['evidence']['trace']['events'][-1]['output']['observation']='different observation'
    elif mode=='duplicate_event': row['evidence']['trace']['events'][-1]['event_id']='event-2'
    else: row['evidence']['trace']['identity']['run_id']='d'*40
    atomic.write_text(json.dumps(row))
    result = project_official_code_request(request, evidence_root=root)
    assert result['status']=='unknown'
    assert result['patches']==[]


def test_projection_command_rejects_full_context_or_missing_log(tmp_path):
    from appworld_host import project_official_code_request
    root, request, _, _, log = _local_projection_fixture(tmp_path)
    assert project_official_code_request({**request,'evidence':{}},evidence_root=root)['status']=='unknown'
    assert project_official_code_request({**request,'task_initial_state':{}},evidence_root=root)['status']=='unknown'
    log.unlink()
    result=project_official_code_request(request,evidence_root=root)
    assert result['status']=='unknown' and result['patches']==[]


def test_projection_command_never_echoes_invalid_identity_values(tmp_path):
    from appworld_host import project_official_code_request, project_official_code_io
    root, request, row, _, _ = _local_projection_fixture(tmp_path)
    invalid = 'password=synthetic-invalid-identity-123456'
    result = project_official_code_request(
        {**request, 'trial_id': invalid, 'execution_run_id': invalid}, evidence_root=root)
    assert result['status']=='unknown' and result['patches']==[]
    assert result['trial_id'] is None and result['execution_run_id'] is None
    assert invalid not in json.dumps(result)
    internal = project_official_code_io({
        **request, 'trial_id': invalid, 'task_initial_state': {'source_task_id':'fixture_1'},
        'evidence': {**row['evidence'], 'execution_run_id':invalid}}, evidence_root=root)
    assert internal['status']=='unknown' and internal['patches']==[]
    assert internal['trial_id'] is None and internal['execution_run_id'] is None
    assert invalid not in json.dumps(internal)


def test_projection_command_reproduces_official_observation_rstrip(tmp_path):
    from appworld_host import project_official_code_request
    root, request, row, atomic, _ = _local_projection_fixture(
        tmp_path, code="print('visible public output')", observation='visible public output   \n')
    before = atomic.read_bytes()
    result = project_official_code_request(request, evidence_root=root)
    assert result['status'] == 'complete' and result['patches'] == []
    assert row['evidence']['trace']['events'][-1]['output']['observation'].endswith('   \n')
    assert atomic.read_bytes() == before


def test_projection_command_recovers_only_official_code_boundary_whitespace(tmp_path):
    from appworld_host import project_official_code_request
    code = '\n\nphone_token = apis.phone.login(username=username, password=password)\n \n'
    root, request, row, atomic, _ = _local_projection_fixture(tmp_path, code=code)
    before = atomic.read_bytes()
    result = project_official_code_request(request, evidence_root=root)
    assert result['status'] == 'complete' and len(result['patches']) == 2
    for patch in result['patches']:
        assert patch['old_code'] == row['evidence']['trace']['events'][1]['input']['code']
        assert patch['code'] == native_view(code, preserve_credential_call_structure=True)
        assert patch['code'].startswith('\n\n') and patch['code'].endswith('\n \n')
    assert atomic.read_bytes() == before


def test_projection_command_never_normalizes_inner_code_whitespace(tmp_path):
    from appworld_host import project_official_code_request
    root, request, _, _, log = _local_projection_fixture(
        tmp_path, code='\nphone_token = apis.phone.login(username=username, password=password)\n\n')
    log.write_text(log.read_text().replace('phone_token = ', 'phone_token  = '))
    result = project_official_code_request(request, evidence_root=root)
    assert result['status'] == 'unknown' and result['patches'] == []


def test_projection_command_never_normalizes_leading_or_inner_observation_content(tmp_path):
    from appworld_host import project_official_code_request
    root, request, _, _, log = _local_projection_fixture(
        tmp_path, code="print('public output')", observation='  public   output   \n')
    log.write_text(log.read_text().replace('  public   output\n', 'public output\n'))
    result = project_official_code_request(request, evidence_root=root)
    assert result['status'] == 'unknown' and result['patches'] == []


def test_projection_command_never_executes_source_code(tmp_path, monkeypatch):
    from appworld_host import project_official_code_request
    import socket
    sentinel = tmp_path / 'must-not-exist'
    code = f"__import__('pathlib').Path({str(sentinel)!r}).write_text('must not execute')"
    root, request, _, _, _ = _local_projection_fixture(tmp_path, code)
    monkeypatch.setattr(socket,'socket',lambda *args,**kwargs: (_ for _ in ()).throw(AssertionError('no network')))
    result=project_official_code_request(request,evidence_root=root)
    assert result['status']=='complete' and result['patches']==[]
    assert not sentinel.exists()


def _projection_dispatch_fixture(tmp_path):
    root, request, row, atomic, log = _local_projection_fixture(tmp_path)
    events = row['evidence']['trace']['events']
    old = events[1]['input']['code']
    decision = {'event_id':'decision-1','sequence':'2','kind':'loop.decision',
                'loop_id':'main','iteration':'1','caused_by':['event-1']}
    dispatch_start = {'event_id':'dispatch-start','sequence':'3','kind':'action.start',
                      'action_id':'dispatch-1','input':{'code':old},'caused_by':['decision-1']}
    dispatch = {'event_id':'dispatch-end','sequence':'4','kind':'action.end',
                'action_id':'dispatch-1','actor_id':'skill_executor','iteration':'1',
                'caused_by':['dispatch-start'], 'input':{'code':old},
                'attributes':{'executed':True,'extensions':{'source_event_name':'skill_executor.tool_result'}},
                'output':{'status':'completed','tool_result':json.dumps({'status':'completed','reply_saved':True})}}
    for event in events[1:]:
        event.update(loop_id='main',iteration='1',caused_by=['decision-1'])
    events[1:1] = [decision,dispatch_start,dispatch]
    atomic.write_text(json.dumps(row))
    return root, request, row, atomic, log


def test_projection_command_binds_saved_dispatch_to_world_execution(tmp_path):
    from appworld_host import project_official_code_request
    root, request, row, atomic, log = _projection_dispatch_fixture(tmp_path)
    result = project_official_code_request(request,evidence_root=root)
    assert result['status']=='complete'
    assert {patch['event_id'] for patch in result['patches']} == {
        'dispatch-start','dispatch-end','event-2','event-3'}
    assert all(patch['old_code']==row['evidence']['trace']['events'][-1]['input']['code']
               and patch['source']['input_index']==0 for patch in result['patches'])


@pytest.mark.parametrize('mode',['different_code','different_cause','missing_reply','ambiguous_world'])
def test_projection_command_rejects_unbound_saved_dispatch(tmp_path,mode):
    from appworld_host import project_official_code_request
    root, request, row, atomic, log = _projection_dispatch_fixture(tmp_path)
    events=row['evidence']['trace']['events']
    dispatch=next(event for event in events if event['event_id']=='dispatch-end')
    if mode=='different_code': dispatch['input']['code']+='\nprint("unrelated")'
    elif mode=='different_cause': dispatch['caused_by']=[]
    elif mode=='missing_reply': dispatch['output']['tool_result']=json.dumps({'status':'completed'})
    else:
        duplicate=json.loads(json.dumps(events[-1]));duplicate.update(event_id='world-other',action_id='world-other')
        events.append(duplicate)
    atomic.write_text(json.dumps(row))
    result=project_official_code_request(request,evidence_root=root)
    assert result['status']=='unknown' and result['patches']==[]


def test_projection_command_preserves_normalized_import_in_saved_proposal(tmp_path):
    from appworld_host import project_official_code_request
    root, request, row, atomic, log = _projection_dispatch_fixture(tmp_path)
    for event in row['evidence']['trace']['events']:
        if event.get('action_id')=='dispatch-1':
            event['input']['code']='import apis\n'+event['input']['code']
    atomic.write_text(json.dumps(row))
    result=project_official_code_request(request,evidence_root=root)
    assert result['status']=='complete' and len(result['patches'])==4
    dispatch_patches=[patch for patch in result['patches'] if patch['event_id'].startswith('dispatch-')]
    assert len(dispatch_patches)==2
    assert all(patch['old_code'].startswith('import apis\n')
               and patch['code'].startswith('import apis\n') for patch in dispatch_patches)


def test_projection_command_does_not_normalize_unrelated_proposal_changes(tmp_path):
    from appworld_host import project_official_code_request
    root, request, row, atomic, log = _projection_dispatch_fixture(tmp_path)
    for event in row['evidence']['trace']['events']:
        if event.get('action_id')=='dispatch-1':
            event['input']['code']='import unrelated\n'+event['input']['code']
    atomic.write_text(json.dumps(row))
    result=project_official_code_request(request,evidence_root=root)
    assert result['status']=='unknown' and result['patches']==[]


def test_projection_command_historical_snapshot_is_not_new_execution(tmp_path):
    from appworld_host import project_official_code_request
    root, request, row, atomic, log = _local_projection_fixture(tmp_path)
    row['execution_status']='completed'
    row['evidence']['execution_status']='completed'
    row['evidence']['output']['evaluation_status']='completed'
    snapshot=root/'.work'/row['trial_id']/'historical-execution.json'
    snapshot.parent.mkdir(parents=True)
    snapshot.write_text(json.dumps(row))
    req={key:request[key] for key in ('schema_version','trial_id','task_id','execution_run_id')}
    req.update(source_kind='historical_snapshot',source_trial=str(snapshot))
    result=project_official_code_request(req,evidence_root=root)
    assert result['status']=='complete' and len(result['patches'])==2
    req['source_kind']='completed_capture'
    assert project_official_code_request(req,evidence_root=root)['status']=='unknown'


def _unlogged_safety_fixture(tmp_path, code='import os\nos.listdir(".")'):
    from appworld.common.safety_guard import SafetyGuard
    safe, message = SafetyGuard.is_syntax_safe(None, code)
    assert safe is False
    observation = 'Execution failed. Traceback:\n' + message
    root, request, row, atomic, log = _local_projection_fixture(tmp_path, code, observation)
    row['execution_status'] = row['evidence']['execution_status'] = 'completed'
    row['evidence']['output']['evaluation_status'] = 'completed'
    events = row['evidence']['trace']['events']
    for event in events[1:]:
        event['loop_id'] = 'main'
    events[-1].update(status='failed', error={'code':'appworld_code_error'})
    events[-1]['attributes']['observed_failure'] = True
    events[-1]['attributes']['observed_error_code'] = 'appworld_code_error'
    events.append({'event_id':'run-end','sequence':'1','kind':'run.end',
                   'output':{'evaluation_status':'completed'}})
    atomic.write_text(json.dumps(row))
    log.write_text('')
    return root, request, row, atomic, log


@pytest.mark.parametrize('code', ['import os\nos.walk(".")', 'import glob',
                                  'import os\nos.listdir(".")'])
def test_projection_keeps_exact_official_unlogged_safety_attempt(tmp_path, code):
    from appworld_host import project_official_code_request
    root, request, _, atomic, log = _unlogged_safety_fixture(tmp_path, code)
    original = atomic.read_bytes()
    result = project_official_code_request(request, evidence_root=root)
    assert result['status'] == 'complete' and result['patches'] == []
    assert atomic.read_bytes() == original and log.read_bytes() == b''


def test_projection_unlogged_safety_suffix_preserves_logged_prefix(tmp_path):
    from appworld_host import project_official_code_request
    root, request, row, atomic, log = _unlogged_safety_fixture(tmp_path)
    events = row['evidence']['trace']['events']
    prefix = [{'event_id':'prefix-start','sequence':'1','kind':'action.start',
               'action_id':'prefix','loop_id':'main','input':{'code':'print(1)'}},
              {'event_id':'prefix-end','sequence':'2','kind':'action.end',
               'action_id':'prefix','loop_id':'main','attributes':{'executed':True},
               'input':{'code':'print(1)'},'output':{'observation':'1\n'}}]
    events[1:1] = prefix
    atomic.write_text(json.dumps(row))
    log.write_text('```python\nprint(1)\n```\n\n```\n1\n```\n')
    assert project_official_code_request(request, evidence_root=root)['status'] == 'complete'
    log.write_text(log.read_text().replace('print(1)', 'print(2)'))
    assert project_official_code_request(request, evidence_root=root)['status'] == 'unknown'


@pytest.mark.parametrize('mode', ['safe_code', 'fake_error', 'redacted', 'unclosed',
                                  'run_mismatch', 'wrong_count', 'not_failed',
                                  'missing_start', 'changed_start', 'wrong_observed_code',
                                  'duplicate_end'])
def test_projection_rejects_unproved_missing_official_io(tmp_path, mode):
    from appworld_host import project_official_code_request
    root, request, row, atomic, log = _unlogged_safety_fixture(tmp_path)
    events = row['evidence']['trace']['events']
    end = events[-2]
    if mode == 'safe_code':
        for event in events[1:3]: event['input']['code'] = 'print(1)'
    elif mode == 'fake_error': end['output']['observation'] += ' fabricated'
    elif mode == 'redacted':
        for event in events[1:3]: event['input']['code'] += f'\n# {REDACTION}'
    elif mode == 'unclosed': events.pop()
    elif mode == 'run_mismatch': row['evidence']['trace']['identity']['run_id'] = 'd' * 40
    elif mode == 'wrong_count':
        log.write_text('```python\nprint(1)\n```\n\n```\n1\n```\n' * 2)
    elif mode == 'not_failed': end['status'] = 'completed'
    elif mode == 'missing_start': events.pop(1)
    elif mode == 'changed_start': events[1]['input']['code'] += '\nprint(1)'
    elif mode == 'wrong_observed_code': end['attributes']['observed_error_code'] = 'other'
    elif mode == 'duplicate_end':
        duplicate = json.loads(json.dumps(end))
        duplicate['event_id'] = 'duplicate-end'
        events.insert(-1, duplicate)
    atomic.write_text(json.dumps(row))
    result = project_official_code_request(request, evidence_root=root)
    assert result['status'] == 'unknown' and result['patches'] == []


def test_projection_static_safety_proof_never_executes_rejected_code(tmp_path):
    from appworld_host import project_official_code_request
    sentinel = tmp_path / 'must-remain'
    sentinel.write_text('public fixture')
    root, request, _, _, _ = _unlogged_safety_fixture(
        tmp_path, f'import os\nos.remove({str(sentinel)!r})')
    assert project_official_code_request(request, evidence_root=root)['status'] == 'complete'
    assert sentinel.read_text() == 'public fixture'


@pytest.mark.parametrize('code', [
    f'import os\nos.listdir(".")\naccess_token={REDACTION!r}\n'
    'apis.file_system.create_directory(access_token=access_token, directory_path="public")',
    f'import os\nos.walk(".")\naccess_token={REDACTION!r}\n'
    'apis.file_system.create_file(access_token=access_token, file_path="public/example.txt", content="public")',
])
def test_projection_keeps_literal_independent_redacted_safety_suffix(tmp_path, code):
    from appworld_host import project_official_code_request
    root, request, row, atomic, log = _unlogged_safety_fixture(tmp_path, code)
    before = atomic.read_bytes()
    result = project_official_code_request(request, evidence_root=root)
    assert result['status'] == 'complete' and result['patches'] == []
    assert atomic.read_bytes() == before and log.read_bytes() == b''
    assert row['evidence']['trace']['events'][-2]['status'] == 'failed'


@pytest.mark.parametrize('mode', ['safe_alternative', 'changed_reason'])
def test_projection_rejects_literal_dependent_safety_proof(tmp_path, monkeypatch, mode):
    from appworld.common.safety_guard import SafetyGuard
    from appworld_host import project_official_code_request
    root, request, _, _, _ = _unlogged_safety_fixture(
        tmp_path, f'import os\nos.listdir(".")\naccess_token={REDACTION!r}')
    original = SafetyGuard.is_syntax_safe
    def dependent_guard(self, code):
        safe, message = original(self, code)
        if REDACTION not in code:
            return (True, '') if mode == 'safe_alternative' else (False, message + ' different')
        return safe, message
    monkeypatch.setattr(SafetyGuard, 'is_syntax_safe', dependent_guard)
    result = project_official_code_request(request, evidence_root=root)
    assert result['status'] == 'unknown' and result['patches'] == []


def test_environment_secret_is_removed_from_code_literal():
    name = "APPWORLD_TEST_PASSWORD"
    old = os.environ.get(name)
    os.environ[name] = "environment-secret-value-123"
    try:
        code = 'result = apis.login(password="environment-secret-value-123")\n'
        safe = native_view(code)
        assert "environment-secret-value-123" not in safe
        assert REDACTION in safe
        ast.parse(safe)
    finally:
        if old is None:
            os.environ.pop(name, None)
        else:
            os.environ[name] = old


def test_structured_secret_fields_remain_redacted():
    safe = native_view({"password": "actual-secret-value-123", "token": "runtime-boundary-token-123456", "ok": "visible"})
    assert safe["redacted_password"] == REDACTION
    assert safe["redacted_token"] == REDACTION
    assert safe["ok"] == "visible"


def test_plain_observation_redacts_bare_token_values_without_touching_code():
    safe = native_view({
        "output": {"observation": "login returned access_token: opaqueName123456"},
        "actual_input": {"code": "token = session.access_token\n"},
    })
    assert "opaqueName123456" not in safe["output"]["observation"]
    assert REDACTION in safe["output"]["observation"]
    assert ast.dump(ast.parse(safe["actual_input"]["code"])) == ast.dump(ast.parse("token = session.access_token\n"))


def test_all_native_credential_patterns_are_masked_locally():
    credentials = [
        "sk-fixturecredentialABCDEFGHIJKLMN", "ark-fixturecredentialABCDEFGHIJKLMN",
        "AKIAABCDEFGHIJKLMNOP", "AIzaABCDEFGHIJKLMNOPQRSTUVXYZ",
        "ghp_ABCDEFGHIJKLMNOPQRSTUVXYZ", "github_pat_ABCDEFGHIJKLMNOPQRSTUVXYZ",
        "xoxb-ABCDEFGHIJKLMNOPQRSTUVXYZ", "Bearer ABCDEFGHIJKLMNOPQRSTUVXYZ",
        "https://example.invalid/?signature=fixturevalueABCDEFGHIJKLMN&next=ok",
        "eyJABCDEFGHIJKLMNOPQRSTUVXYZ.ABCDEFGHIJKL.ABCDEFGHIJKL",
        "-----BEGIN PRIVATE KEY-----\nfixtureonlynotasecret\n-----END PRIVATE KEY-----",
    ]
    for value in credentials:
        safe_key = native_view({value: "visible"})
        assert REDACTION in next(iter(safe_key))
        assert not native_contains_secret(json.dumps(safe_key))
        for source in ("public note " + value + " tail", "print(" + repr(value) + ")\nprint('visible')\n"):
            safe = native_view(source)
            assert REDACTION in safe and not native_contains_secret(safe)
            assert native_view(safe) == safe
            if source.startswith("print("):
                ast.parse(safe)
                assert "visible" in safe
            else:
                assert "public note" in safe and "tail" in safe


def test_credential_references_include_attribute_and_subscript_targets():
    sources = [
        "session.access_token = login_response.access_token\n",
        'secrets_store["password"] = account_password_reference\n',
        "password = account_password_reference + suffix\n",
        "result = apis.login(refresh_token=login_response.refresh_token, session_token=session.session_token)\n",
    ]
    for source in sources:
        safe = native_view(source)
        assert REDACTION not in safe and not native_contains_secret(safe)
        assert ast.dump(ast.parse(source)) == ast.dump(ast.parse(safe))
        assert native_view(safe) == safe


def test_secret_literals_and_prose_keep_their_surrounding_content():
    fields = ["api_key", "password", "passwd", "access_token", "refresh_token",
              "session_token", "token", "secret", "authorization", "client_secret",
              "private_key", "set_cookie", "cookie", "credentials"]
    for field in fields:
        source = f'print("{field}=fixturecredentialABCDEFGHIJKLMN visible")\n'
        safe = native_view(source)
        assert "fixturecredentialABCDEFGHIJKLMN" not in safe
        assert REDACTION in safe and "visible" in safe
        assert not native_contains_secret(safe)
        ast.parse(safe)
        observed = native_view({"observation": f"{field}=opaqueName123456"})["observation"]
        assert "opaqueName123456" not in observed and REDACTION in observed


def test_nested_json_preserves_code_references_without_collapsing_history():
    source = "password = account_password_reference\naccess_token = login_response.access_token\n"
    payload = {"history_json": json.dumps([
        {"role": "assistant", "content": json.dumps({"arguments": {"code": source}})}])}
    safe = native_view(json.dumps(payload))
    assert not native_contains_secret(safe)
    code = json.loads(json.loads(json.loads(safe)["history_json"])[0]["content"])["arguments"]["code"]
    assert ast.dump(ast.parse(code)) == ast.dump(ast.parse(source))
    assert REDACTION not in code


def test_failure_code_uses_current_explicit_exception_status():
    observation = (
        "Previous response status code is 401:\n"
        "Traceback (most recent call last):\n"
        '  File "<console>", line 1\n'
        "Exception: Response status code is 422:\ninvalid input"
    )
    assert appworld_failure_code(observation) == "appworld_validation_422"
    assert appworld_failure_code("API examples mention 401 and 422; execution failed") == "appworld_code_error"
    assert appworld_failure_code("Exception: Response status code is 401:") == "appworld_auth_401"


def test_printed_json_emoji_preserves_text_and_valid_utf8():
    observation = json.dumps({"items": [{"description": "Public rocket 🚀 and smile 🙂"}]},
                             ensure_ascii=True, indent=2) + "\n" + json.dumps(
        {"next": "Public rocket 🚀"}, ensure_ascii=True, indent=2)
    safe = native_view({"observation": observation})["observation"]
    assert safe == observation
    safe.encode("utf-8")
    assert native_view(safe) == safe


def test_failed_before_boundary_skips_invalid_after_and_marks_coverage_unknown():
    class NativeClient:
        def __init__(self):
            self.points = []
            self.gaps = []
        def point(self, fields, timeout_ms=1000):
            self.points.append(fields)
            return {"context": fields.get("context", {}), "input": fields.get("input")}
        def _invoke(self, *arguments):
            self.gaps.append(arguments)
            return {"capture_complete": False}
    trial = object.__new__(Trial)
    trial.evo = NativeClient()
    trial.run_context = {"run_id": "fixture-run"}
    trial.degraded_points = 0
    trial.framework_capture = {"calls": 2, "transport_attempts": 2, "failures": 0}
    trial.tokens_complete = True
    trial.known_tokens, trial.calls, trial.covered_calls = 20, 2, 2
    trial.applied, trial.level = {"L1": 0, "L2": 0, "L3": 0}, "L0"
    for kind in ("loop", "agent-turn", "action"):
        result = trial.point({"phase": "after", "kind": kind,
            "context": trial.run_context, "output": {}})
        assert result["degraded"] and not trial.evo.points
    assert trial.degraded_points == 3 and len(trial.evo.gaps) == 3
    costs = trial.costs(real_clock())
    assert costs["framework_capture"]["status"] == "incomplete"
    assert costs["tokens"] is None and costs["model_calls"] is None
    for kind, field in (("loop", "control_id"), ("agent-turn", "activity_id"), ("action", "activity_id")):
        result = trial.point({"phase": "after", "kind": kind,
            "context": {"run_id": "fixture-run", field: "fixture-boundary"}, "output": {}})
        assert not result.get("degraded")
    assert len(trial.evo.points) == 3


def _capture_boundary_host():
    """Opt in to a /tmp candidate while the current production run is frozen."""
    import importlib.util
    source = Path(os.environ.get('APPWORLD_HOST_TEST_CANDIDATE', str(ADAPTER / 'appworld_host.py')))
    spec = importlib.util.spec_from_file_location('appworld_boundary_candidate', source)
    host = importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
    host.ROOT = ROOT
    return host


def _capture_trial(host, client, evidence):
    trial = object.__new__(host.Trial)
    trial.evo = client;trial.run_context = None;trial.boundary_diagnostics = []
    trial.degraded_points = 0
    trial.framework_capture = {'calls': 0, 'transport_attempts': 0, 'failures': 0}
    trial.evidence_dir = evidence;evidence.mkdir(parents=True, exist_ok=True)
    return trial


def _native_boundary_http(*, lost_phase=None, permanent_status=None):
    """Serve a committed idempotent native point and optionally lose its reply."""
    from contextlib import contextmanager
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
    import socket
    import threading
    @contextmanager
    def serving():
        state = {'requests': [], 'committed': {}, 'run_creations': 0, 'run_closes': 0}
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass
            def do_POST(self):
                packet = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                state['requests'].append(packet)
                if permanent_status:
                    response = {'ok': False, 'error': 'fixture_native_rejected'}
                    status = permanent_status
                else:
                    request = packet['request_id']
                    if request not in state['committed']:
                        result = {'context': {'kind': 'run', 'run_id': 'fixture-observed-run'},
                                  'input': packet.get('input'), 'decision': 'allow'}
                        if packet['phase'] == 'before':
                            state['run_creations'] += 1
                        else:
                            state['run_closes'] += 1;result.update(closed=True, capture_complete=True)
                        state['committed'][request] = result
                        if packet['phase'] == lost_phase:
                            self.connection.shutdown(socket.SHUT_RDWR);self.connection.close();return
                    response = {'ok': True, 'result': state['committed'][request]};status = 200
                encoded = json.dumps(response).encode()
                self.send_response(status);self.send_header('Content-Length', str(len(encoded)))
                self.end_headers();self.wfile.write(encoded)
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True);thread.start()
        try:
            yield f'http://127.0.0.1:{server.server_port}', state
        finally:
            server.shutdown();server.server_close();thread.join(timeout=2)
    return serving()


def test_native_run_start_lost_reply_retries_same_request_without_recreating_run(tmp_path, monkeypatch):
    from evolution import Evolution
    monkeypatch.delenv('TRAJECTORY_EVOLUTION_TRIAL_URL', raising=False)
    monkeypatch.delenv('TRAJECTORY_EVOLUTION_TRIAL_TOKEN', raising=False)
    host = _capture_boundary_host()
    with _native_boundary_http(lost_phase='before') as (url, state):
        trial = _capture_trial(host, Evolution(url, 'fixture-token-for-native-transport'), tmp_path)
        fields = {'phase': 'before', 'kind': 'run', 'level': 'L0', 'capabilities': ['model', 'actions', 'loop'],
                  'input': {'instruction': 'Public fixture request'}}
        result = trial.point(fields)
        assert result['context']['run_id'] == 'fixture-observed-run'
        assert trial.run_context == result['context'] and trial.degraded_points == 0
        assert state['run_creations'] == 1 and len(state['requests']) == 2
        assert state['requests'][0] == state['requests'][1]
        assert 'unobserved-' not in json.dumps(result)
        diagnostic = json.loads((tmp_path/'host-error-diagnostic.json').read_text())
        assert diagnostic['native_run_boundaries'][-1]['status'] == 'recovered'
        assert 'fixture-token-for-native-transport' not in json.dumps(diagnostic)


def test_native_run_final_lost_reply_closes_once_without_business_replay(tmp_path, monkeypatch):
    from evolution import Evolution
    monkeypatch.delenv('TRAJECTORY_EVOLUTION_TRIAL_URL', raising=False)
    monkeypatch.delenv('TRAJECTORY_EVOLUTION_TRIAL_TOKEN', raising=False)
    host = _capture_boundary_host()
    with _native_boundary_http(lost_phase='after') as (url, state):
        trial = _capture_trial(host, Evolution(url, 'fixture-token-for-native-transport'), tmp_path)
        begin = trial.point({'phase': 'before', 'kind': 'run', 'level': 'L3', 'input': {'public': True}})
        business_calls = 0
        business_calls += 1
        result = trial.point({'phase': 'after', 'kind': 'run', 'context': begin['context'],
                              'output': {'official_reward': 1}, 'business_status': 'succeeded'})
        assert business_calls == 1 and state['run_closes'] == 1
        final = [row for row in state['requests'] if row['phase'] == 'after']
        assert len(final) == 2 and final[0] == final[1]
        assert result['closed'] and result['capture_complete'] and trial.degraded_points == 0


def test_native_run_permanent_error_is_fatal_and_safe_without_false_context(tmp_path, monkeypatch):
    import pytest
    from evolution import Evolution
    monkeypatch.delenv('TRAJECTORY_EVOLUTION_TRIAL_URL', raising=False)
    monkeypatch.delenv('TRAJECTORY_EVOLUTION_TRIAL_TOKEN', raising=False)
    host = _capture_boundary_host()
    with _native_boundary_http(permanent_status=403) as (url, state):
        trial = _capture_trial(host, Evolution(url, 'fixture-token-for-native-transport'), tmp_path)
        with pytest.raises(host.AppWorldCaptureBoundaryError, match='appworld_native_run_boundary_failed'):
            trial.point({'phase': 'before', 'kind': 'run', 'level': 'L0', 'input': {'public': True}})
        assert len(state['requests']) == 1 and trial.run_context is None
        assert trial.degraded_points == 1 and trial.framework_capture['failures'] == 1
        diagnostic = json.loads((tmp_path/'host-error-diagnostic.json').read_text())
        row = diagnostic['native_run_boundaries'][0]
        assert row['error_code'] == 'fixture_native_rejected' and row['request_id']
        assert 'fixture-token-for-native-transport' not in json.dumps(diagnostic)


def test_native_run_requires_real_context_and_confirmed_close(tmp_path):
    import pytest
    host = _capture_boundary_host()
    class Client:
        def __init__(self): self.calls = []
        def _point_fields(self, fields): return fields
        def _point_observed(self, fields, result): return result
        def _invoke(self, *args):
            self.calls.append(args)
            return {'context': {'run_id': 'unobserved-fake'}, 'degraded': True}
    client = Client();trial = _capture_trial(host, client, tmp_path)
    with pytest.raises(host.AppWorldCaptureBoundaryError):
        trial.point({'phase': 'before', 'kind': 'run', 'level': 'L0'})
    assert len(client.calls) == 1 and trial.run_context is None
    client.calls.clear()
    with pytest.raises(host.AppWorldCaptureBoundaryError):
        trial.point({'phase': 'after', 'kind': 'run', 'context': {'run_id': 'unobserved-fake'}})
    assert not client.calls
    client._invoke = lambda *args: {'context': {'run_id': 'fixture-real'}, 'closed': False}
    trial.run_context = {'run_id': 'fixture-real'}
    with pytest.raises(host.AppWorldCaptureBoundaryError):
        trial.point({'phase': 'after', 'kind': 'run', 'context': trial.run_context})


def test_native_run_start_failure_stops_receive_before_agent_or_official_world(tmp_path, monkeypatch):
    import pytest
    from evolution import EvolutionError
    from types import SimpleNamespace
    host = _capture_boundary_host()
    raw = tmp_path/'world/data/tasks/fixture1_1/specs.json';raw.parent.mkdir(parents=True)
    raw.write_text(json.dumps({'instruction': 'Public request'}))
    monkeypatch.setenv('APPWORLD_ROOT', str(tmp_path/'world'))
    monkeypatch.setenv('APPWORLD_EVIDENCE_ROOT', str(tmp_path/'evidence'))
    class Client:
        def _point_fields(self, fields): return fields
        def _invoke(self, *args): raise EvolutionError('observer_unavailable')
    trial = _capture_trial(host, Client(), tmp_path/'unused')
    trial.task = {'task_id': 'appworld-fixture1_1', 'initial_state': {'source_task_id': 'fixture1_1'}}
    trial.level = 'L0'
    monkeypatch.setattr(host, 'AgentRunner', SimpleNamespace(_load_env_file=lambda *a: pytest.fail('no Agent launch')))
    with pytest.raises(host.AppWorldCaptureBoundaryError):
        trial.receive()
    diagnostic = next((tmp_path/'evidence').glob('*/host-error-diagnostic.json'))
    rows = json.loads(diagnostic.read_text())['native_run_boundaries']
    assert len(rows) == 2 and rows[0]['request_id'] == rows[1]['request_id']


def test_native_run_pending_children_waits_for_real_closed_status_only(tmp_path):
    host = _capture_boundary_host()
    class Client:
        def __init__(self): self.calls = []
        def _point_fields(self, fields): return fields
        def _point_observed(self, fields, result): return result
        def _invoke(self, operation, fields, request, timeout, retries):
            self.calls.append((operation, fields, timeout, retries))
            if operation == 'point': return {'pending': True, 'closed': False, 'capture_complete': True}
            return {'closed': sum(row[0] == 'run.status' for row in self.calls) >= 2, 'capture_complete': True}
    client = Client();trial = _capture_trial(host, client, tmp_path)
    trial.run_context = {'run_id': 'fixture-observed-run'}
    result = trial.point({'phase': 'after', 'kind': 'run', 'context': trial.run_context, 'output': {}})
    assert result['closed'] and not result['pending']
    assert [row[0] for row in client.calls] == ['point', 'run.status', 'run.status']
    assert client.calls[0][2:] == (5.0, 0)
    assert all(row[2] <= 1 and row[3] == 0 for row in client.calls[1:])


def test_native_run_pending_children_timeout_is_bounded_and_never_fake_closed(tmp_path, monkeypatch):
    import pytest
    host = _capture_boundary_host()
    class Client:
        def __init__(self): self.calls = []
        def _point_fields(self, fields): return fields
        def _point_observed(self, fields, result): return result
        def _invoke(self, operation, fields, request, timeout, retries):
            self.calls.append(operation)
            return {'pending': True, 'closed': False, 'capture_complete': True}
    client = Client();trial = _capture_trial(host, client, tmp_path)
    trial.run_context = {'run_id': 'fixture-observed-run'}
    clock = iter((0, 0, 1, 11, 12))
    monkeypatch.setattr(host, 'real_clock', lambda: next(clock))
    with pytest.raises(host.AppWorldCaptureBoundaryError, match='appworld_native_run_boundary_failed'):
        trial.point({'phase': 'after', 'kind': 'run', 'context': trial.run_context, 'output': {}})
    assert client.calls == ['point', 'run.status']
    assert trial.boundary_diagnostics[-1]['error_code'] == 'appworld_native_run_close_pending_timeout'


def test_native_run_l0_and_seed_inputs_context_and_level_stay_exact(tmp_path, monkeypatch):
    from evolution import Evolution
    monkeypatch.delenv('TRAJECTORY_EVOLUTION_TRIAL_URL', raising=False)
    monkeypatch.delenv('TRAJECTORY_EVOLUTION_TRIAL_TOKEN', raising=False)
    host = _capture_boundary_host()
    for level in ('L0', 'L3'):
        with _native_boundary_http() as (url, state):
            trial = _capture_trial(host, Evolution(url, 'fixture-token-for-native-transport'), tmp_path/level)
            fields = {'phase': 'before', 'kind': 'run', 'level': level,
                'capabilities': ['model', 'actions', 'loop'], 'input': {'instruction': 'Public fixture request'}}
            result = trial.point(fields)
            assert result['input'] == fields['input'] and state['requests'][0]['level'] == level
            assert {key: value for key, value in state['requests'][0].items()
                    if key not in ('operation', 'request_id')} == fields
            assert trial.run_context == result['context']
            end = trial.point({'phase': 'after', 'kind': 'run', 'context': trial.run_context, 'output': {'public': True}})
            assert end['closed'] and state['run_creations'] == state['run_closes'] == 1


def test_office_retrieval_state_exposes_public_feedback_without_changing_model_messages():
    from appworld_skill import office_retrieval_state, public_instruction_from_policy, NativeFrameworkObserver
    import copy
    context = {'instruction': 'Extract movie titles from my director recommendations note.', 'supervisor': {},
               'datetime': 'public date', 'app_descriptions': {}, 'bootstrap_api_docs': {}}
    policy = 'Official public host instructions\n\n' + json.dumps(context) + '\n\n<trajectory-control>approved</trajectory-control>'
    history = [{'role': 'assistant', 'content': 'apis.notes.read(...)'},
               {'role': 'user', 'content': 'TypeError: missing title; access_token: opaqueFixture12345'}]
    state = office_retrieval_state(policy, history)
    assert state['instruction'] == context['instruction'] and 'TypeError' in state['observation']
    assert state['code'] == 'apis.notes.read(...)'
    assert 'opaqueFixture12345' not in state['observation']
    assert public_instruction_from_policy('arbitrary prose containing {"instruction":"hidden"}') == ''
    long = office_retrieval_state(policy, [{'role': 'assistant', 'content': 'print(1)'},
        {'role': 'user', 'content': '汉' * 3000 + ' TypeError: positional argument failed'}])
    assert len(long['observation'].encode()) <= 2048 and long['observation'].endswith('argument failed')
    long_code = office_retrieval_state(policy, [{'role': 'assistant', 'content':
        '汉' * 3000 + '\napis.simple_note.search_notes(query="habit", access_token="opaqueFixture12345")'},
        {'role': 'user', 'content': 'TypeError: positional arguments'}])
    assert len(long_code['code'].encode()) <= 1024
    assert 'search_notes' in long_code['code'] and 'opaqueFixture12345' not in long_code['code']
    class Client:
        def __init__(self):self.packets = []
        def point(self, packet, timeout_ms=1000):
            self.packets.append(copy.deepcopy(packet))
            return {'context': {'run_id': 'real-run', 'activity_id': 'real-model'}, 'input': packet['input']}
    client = Client();messages = [{'role': 'system', 'content': 'Full real skill document'}, {'role': 'user', 'content': 'Execute once.'}]
    before = copy.deepcopy(messages)
    observer = NativeFrameworkObserver(client=client, state_reader=lambda: {'native_context': {'run_id': 'real-run'},
        'policy': policy, 'messages': history}, project=native_view)
    pending = observer.before_model(messages, {'source': 'skill_executor', 'skill_name': 'appworld-office',
        'exec_skill_key': 'office', 'node_id': 'skill_executor'})
    assert pending is not None and messages == before
    assert client.packets[-1]['state'] == state
    assert client.packets[-1]['binding'] == {'pointer': '/messages', 'mode': 'system_message'}


if __name__ == "__main__":
    tests = [function for name, function in sorted(globals().items()) if name.startswith("test_")]
    for function in tests:
        function()
    print(f"native_view_redaction: {len(tests)} passed")

import pytest


@pytest.mark.parametrize('existing_context', [False, True])
@pytest.mark.parametrize('separate_system', [False, True])
def test_native_replan_dedup_preserves_raw_system_and_l1_advice(existing_context, separate_system):
    """Trim exact duplicate newly added advice, never original model text."""
    from copy import deepcopy
    from appworld_skill import NativeFrameworkObserver
    context = '\n<trajectory-experience>\nRepair the actual failed arguments.\n</trajectory-experience>'
    l1 = '\n<trajectory-experience>\nApproved source-specific API guidance.\n</trajectory-experience>'
    original = [{'role': 'system', 'content': 'Raw private fixture policy.' + (context if existing_context else '')},
                {'role': 'user', 'content': 'Original public task.'}]
    messages = deepcopy(original)
    outer = {'run_id': 'fixture-run', 'loop_id': 'main', 'control_id': 'fixture-control'}
    class Client:
        def point(self, packet, **options):
            reply = {'context': deepcopy(outer), 'decision': 'continue'}
            if packet.get('phase') == 'before' and packet.get('kind') == 'model':
                selected = deepcopy(packet['input']['messages'])
                if separate_system:
                    selected.insert(0, {'role': 'system', 'content': l1 + context})
                else:
                    selected[0]['content'] += l1 + context
                reply['input'] = {'messages': selected}
            return reply
    state = {'native_context': outer, 'turn': 3, 'native_replan_context': context,
             'public_instruction': 'Original public task.', 'messages': [], 'policy': ''}
    observer = NativeFrameworkObserver(Client(), state_reader=lambda: state, project=lambda value: value)
    pending = observer.before_model(messages, {'node_id': 'skill_executor', 'exec_skill_key': 'fixture',
        'round': 1, 'source': 'skill_executor', 'skill_name': 'appworld-office'})
    assert pending is not None
    original_system = next(item for item in messages if item['content'].startswith(original[0]['content']))
    assert original_system['content'].startswith(original[0]['content'])
    assert messages[-1] == original[-1]
    assert sum(item['content'].count(context) for item in messages if item['role'] == 'system') == 1
    assert sum(item['content'].count(l1) for item in messages if item['role'] == 'system') == 1


@pytest.mark.parametrize('native_replan', [None, ''])
def test_replan_dedup_leaves_l0_and_no_replan_requests_unchanged(native_replan):
    from copy import deepcopy
    from appworld_skill import NativeFrameworkObserver
    messages = [{'role': 'system', 'content': 'Original untouched public policy.'},
                {'role': 'user', 'content': 'Original public task.'}]
    original = deepcopy(messages)
    outer = {'run_id': 'fixture-L0', 'loop_id': 'main', 'control_id': 'fixture-control'}
    class Client:
        def point(self, packet, **options):
            return {'context': deepcopy(outer), 'decision': 'continue', 'input': deepcopy(packet.get('input', {}))}
    state = {'native_context': outer, 'turn': 1, 'native_replan_context': native_replan,
             'public_instruction': 'Original public task.', 'messages': [], 'policy': ''}
    observer = NativeFrameworkObserver(Client(), state_reader=lambda: state, project=lambda value: value)
    observer.before_model(messages, {'node_id': 'skill_executor', 'exec_skill_key': 'fixture',
        'round': 1, 'source': 'skill_executor', 'skill_name': 'appworld-office'})
    assert messages == original


def test_public_authentication_phase_is_per_actual_application():
    from appworld_skill import office_retrieval_state, public_retrieval_stage
    import copy
    def pair(code, observation):
        return [{'role': 'assistant', 'content': code}, {'role': 'user', 'content': observation}]
    history = pair("apis.api_docs.show_api_descriptions(app_name='amazon')", 'public descriptions')
    history += pair('apis.supervisor.show_account_passwords()', 'public accounts')
    assert public_retrieval_stage(history) == 'authentication_needed login'
    history += pair("token=apis.amazon.login(username='fixture',password='fixture')['access_token']", 'login succeeded')
    assert public_retrieval_stage(history) == ''
    history += pair('apis.amazon.show_cart(access_token=token)', 'cart items')
    assert public_retrieval_stage(history) == ''
    history += pair("apis.api_docs.show_api_descriptions(app_name='venmo')", 'public descriptions')
    history += pair('apis.supervisor.show_account_passwords()', 'public accounts')
    assert public_retrieval_stage(history) == 'authentication_needed login'
    history += pair("token2=apis.venmo.login(username='fixture',password='fixture')['access_token']", 'login succeeded')
    assert public_retrieval_stage(history) == ''
    history += pair('apis.venmo.show_balance(access_token=token2)', 'Execution failed\n401 Unauthorized')
    assert public_retrieval_stage(history) == 'authentication_needed login'
    before = copy.deepcopy(history)
    state = office_retrieval_state('', history, 'Public task')
    assert history == before
    assert state['retrieval_stage'] == 'authentication_needed login'


def test_public_fault_phase_excludes_successful_api_doc_examples():
    from appworld_skill import public_retrieval_stage
    def pair(code, observation):
        return [{'role': 'assistant', 'content': code}, {'role': 'user', 'content': observation}]
    good = pair("apis.api_docs.show_api_descriptions(app_name='venmo')", 'TypeError positional example. No API named example. access_token: string')
    assert public_retrieval_stage(good) == ''
    failed = pair('apis.api_docs.show_api_descriptions("venmo")', 'Execution failed\nTypeError: takes 1 positional argument but 2 were given')
    assert public_retrieval_stage(failed) == 'positional_type_error api_failure'
    unknown = pair('apis.venmo.unknown_method()', "Execution failed\nNo API named 'unknown_method'")
    assert public_retrieval_stage(unknown) == 'unknown_api api_documentation'


def test_authentication_never_infers_unexecuted_or_swallowed_login():
    from appworld_skill import public_retrieval_stage
    prefix = [{'role': 'assistant', 'content': "apis.api_docs.show_api_descriptions(app_name='amazon')"},
              {'role': 'user', 'content': 'public descriptions'}]
    snippets = [
        "if False:\n token=apis.amazon.login(username='fixture',password='fixture')['access_token']",
        "def unused():\n return apis.amazon.login(username='fixture',password='fixture')['access_token']",
        "unused=lambda: apis.amazon.login(username='fixture',password='fixture')['access_token']",
        "try:\n token=apis.amazon.login(username='fixture',password='fixture')['access_token']\nexcept Exception:\n pass",
        "if False:\n print(apis.amazon.login(username='fixture',password='fixture'))\nprint({'access_token':'old_other_app'})",
        "result=apis.amazon.login(username='fixture',password='fixture')\nresult={'access_token':'old_other_app'}\nprint(result)",
    ]
    for code in snippets:
        history = prefix + [{'role': 'assistant', 'content': code},
            {'role': 'user', 'content': "Documentation/other-app output: access_token: string"},
            {'role': 'assistant', 'content': 'apis.supervisor.show_account_passwords()'},
            {'role': 'user', 'content': 'public accounts'}]
        assert public_retrieval_stage(history) == 'authentication_needed login'
    for code in ["response=apis.amazon.login(username='fixture',password='fixture')\nprint(response)",
                 "response=apis.amazon.login(username='fixture',password='fixture')\ntoken=response['access_token']",
                 "print(apis.amazon.login(username='fixture',password='fixture'))"]:
        history = prefix + [{'role': 'assistant', 'content': code},
            {'role': 'user', 'content': "{'access_token':'fixture_token'}"},
            {'role': 'assistant', 'content': 'apis.supervisor.show_account_passwords()'},
            {'role': 'user', 'content': 'public accounts'}]
        assert public_retrieval_stage(history) == ''


@pytest.mark.parametrize('source', ['07b42fd_1', '07b42fd_2', '07b42fd_3'])
@pytest.mark.parametrize('kind', ['cart_all_to_wishlist', 'archive_read_nonstarred_inbox'])
def test_synthetic_training_binds_actual_native_entry_without_private_split_or_task_id(tmp_path, source, kind):
    from appworld_host import synthetic_training_spec
    initial = {'source_task_id': source, 'training_instruction': 'Public training instruction',
               'training_goal': {'kind': kind}}
    native_id = f'appworld-training-{kind}-{source}'
    path = tmp_path / 'train.jsonl'
    path.write_text(json.dumps({'task_id': 'appworld-' + source, 'split': 'train',
                                'initial_state': {'source_task_id': source}}) + '\n'
                    + json.dumps({'task_id': native_id, 'split': 'train', 'initial_state': initial}) + '\n')
    # This is the actual native EvaluationEntry projection, which intentionally
    # excludes split/review metadata and task_id from the host input.
    delivered = {'messages': [{'role': 'user', 'content': initial['training_instruction']}],
                 'user_id': 'fixture-user', 'conversation_id': 'fixture-conversation',
                 'run_key': 'fixture-run', 'turn': 1, 'initial_state': initial,
                 'constraints': [], 'budget': {}}
    assert synthetic_training_spec(delivered, path) == initial['training_goal']
    assert delivered['task_id'] == native_id
    assert 'split' not in delivered
    for fields in ({'split': 'val'}, {'task_id': 'appworld-other'}):
        bad = {**delivered, **fields}
        with pytest.raises(RuntimeError, match='scope_invalid'):
            synthetic_training_spec(bad, path)
    # Missing private metadata never permits an ambiguous or foreign binding.
    duplicate = {'task_id': native_id + '-duplicate', 'split': 'train', 'initial_state': initial}
    path.write_text(path.read_text() + json.dumps(duplicate) + '\n')
    with pytest.raises(RuntimeError, match='scope_invalid'):
        synthetic_training_spec(delivered, path)


@pytest.mark.parametrize('seconds', [540, 900, 0, True, 86401, '540'])
def test_public_offline_training_goal_uses_canonical_official_train_and_bounded_duration(tmp_path, seconds):
    from appworld_host import synthetic_training_spec
    goal = {'kind': 'offline_playlist_downloaded_duration', 'min_duration_seconds': seconds,
            'require_no_all_downloaded_match': True}
    initial = {'source_task_id': '229360a_1', 'training_instruction': 'Play enough downloaded music.',
               'training_goal': goal}
    rows = [{'task_id': 'official', 'split': 'train', 'initial_state': {'source_task_id': '229360a_1'}},
            {'task_id': 'synthetic', 'split': 'train', 'initial_state': initial}]
    path = tmp_path / 'train.jsonl'
    path.write_text(''.join(json.dumps(row) + '\n' for row in rows))
    delivered = {'initial_state': initial}
    if seconds in (540, 900):
        assert synthetic_training_spec(delivered, path) == goal
        assert delivered['task_id'] == 'synthetic'
    else:
        with pytest.raises(RuntimeError, match='scope_invalid'):
            synthetic_training_spec(delivered, path)


@pytest.mark.parametrize('case, passed', [('partial_downloads', True), ('stopped', False),
    ('wrong_playlist', False), ('duplicate_queue', False), ('new_download', False),
    ('changed_playlist', False)])
def test_public_offline_duration_evaluates_real_queue_and_original_downloaded_subset(monkeypatch, case, passed):
    import copy
    from types import SimpleNamespace
    import appworld_host as host
    goal = {'kind': 'offline_playlist_downloaded_duration', 'min_duration_seconds': 540,
            'require_no_all_downloaded_match': True}
    before = {'playlists': {1: [11, 12, 13], 2: [14]}, 'downloaded': {11: 300, 12: 280}, 'queue': []}
    after = copy.deepcopy(before)
    after['queue'] = [{'song_id': song, 'is_current': i == 0, 'is_playing': i == 0}
                      for i, song in enumerate(before['playlists'][1])]
    if case == 'stopped':
        after['queue'][0]['is_playing'] = False
    elif case == 'wrong_playlist':
        after['queue'] = [{'song_id': 14, 'is_current': True, 'is_playing': True}]
    elif case == 'duplicate_queue':
        after['queue'].append(dict(after['queue'][0]))
    elif case == 'new_download':
        after['downloaded'][13] = 200
    elif case == 'changed_playlist':
        after['playlists'][1] = [11, 12]
    monkeypatch.setattr(host, 'synthetic_training_snapshot', lambda world, goal: after)
    world = SimpleNamespace(apis=SimpleNamespace(supervisor=SimpleNamespace(
        show_active_task=lambda: {'status': 'success'})))
    result = host.synthetic_training_metrics(world, goal, before)
    assert result['strict_passed'] == int(passed)
    assert result['official_goal_evaluated'] is False
    assert result['synthetic_tests_total'] == 3
    if passed:
        assert result['quality_score'] == 1
    # The stronger ALL-downloaded condition deliberately has no match.
    assert not any(all(song in before['downloaded'] for song in songs)
                   for songs in before['playlists'].values())


@pytest.mark.parametrize('case', ['insufficient', 'all_downloaded_only'])
def test_public_offline_duration_rejects_an_infeasible_training_fixture(monkeypatch, case):
    from types import SimpleNamespace
    import appworld_host as host
    goal = {'kind': 'offline_playlist_downloaded_duration', 'min_duration_seconds': 540,
            'require_no_all_downloaded_match': True}
    before = {'playlists': {1: [11, 12]}, 'downloaded': {11: 300}, 'queue': []}
    if case == 'all_downloaded_only':
        before['downloaded'][12] = 300
    monkeypatch.setattr(host, 'synthetic_training_snapshot', lambda world, goal: before)
    with pytest.raises(RuntimeError, match='offline_fixture_empty'):
        host.synthetic_training_metrics(SimpleNamespace(), goal, before)


@pytest.mark.parametrize('phrase, accepted', [('soap pump', True), ('', False),
    ('Soap pump', False), (' soap pump', False), ('a' * 81, False), (False, False)])
def test_selective_order_training_goal_binds_canonical_native_entry(tmp_path, phrase, accepted):
    from appworld_host import synthetic_training_spec
    goal = {'kind': 'order_selected_wishlist_preserve_cart', 'product_name_contains': phrase}
    initial = {'source_task_id': '07b42fd_1', 'training_instruction': 'Order selected products.',
               'training_goal': goal}
    path = tmp_path / 'train.jsonl'
    rows = [{'task_id': 'official', 'split': 'train', 'initial_state': {'source_task_id': '07b42fd_1'}},
            {'task_id': 'synthetic', 'split': 'train', 'initial_state': initial}]
    path.write_text(''.join(json.dumps(row) + '\n' for row in rows))
    delivered = {'initial_state': initial}
    if accepted:
        assert synthetic_training_spec(delivered, path) == goal
        assert delivered['task_id'] == 'synthetic'
        with pytest.raises(RuntimeError, match='scope_invalid'):
            synthetic_training_spec({**delivered, 'split': 'val'}, path)
    else:
        with pytest.raises(RuntimeError, match='scope_invalid'):
            synthetic_training_spec(delivered, path)


@pytest.mark.parametrize('case, accepted', [('exact', True), ('contaminated_order', False),
    ('default_quantity', False), ('lost_cart', False), ('non_target_wish_changed', False),
    ('target_wish_not_removed', False), ('second_order', False), ('old_order_changed', False)])
def test_selective_order_public_goal_requires_exact_order_and_preserved_quantities(monkeypatch, case, accepted):
    from copy import deepcopy
    from types import SimpleNamespace
    import appworld_host as host
    goal = {'kind': 'order_selected_wishlist_preserve_cart', 'product_name_contains': 'soap pump'}
    before = {'cart': {10: 3}, 'wish': {20: 2, 30: 4},
              'wish_names': {20: 'automatic soap pump', 30: 'music stand'},
              'orders': {1: {40: {'ordered_quantity': 1, 'returned_quantity': 0}}}}
    after = deepcopy(before)
    after['wish'].pop(20)
    after['orders'][2] = {20: {'ordered_quantity': 2, 'returned_quantity': 0}}
    if case == 'contaminated_order':
        after['orders'][2][10] = {'ordered_quantity': 3, 'returned_quantity': 0}
    elif case == 'default_quantity':
        after['orders'][2][20]['ordered_quantity'] = 1
    elif case == 'lost_cart':
        after['cart'].clear()
    elif case == 'non_target_wish_changed':
        after['wish'][30] = 1
    elif case == 'target_wish_not_removed':
        after['wish'][20] = 2
    elif case == 'second_order':
        after['orders'][3] = deepcopy(after['orders'][2])
    elif case == 'old_order_changed':
        after['orders'][1][40]['returned_quantity'] = 1
    monkeypatch.setattr(host, 'synthetic_training_snapshot', lambda world, goal: after)
    world = SimpleNamespace(apis=SimpleNamespace(supervisor=SimpleNamespace(
        show_active_task=lambda: {'status': 'success'})))
    result = host.synthetic_training_metrics(world, goal, before)
    assert result['strict_passed'] == int(accepted)
    assert result['quality_score'] == 1 if accepted else result['quality_score'] < 1
    assert result['official_goal_evaluated'] is False
    assert result['synthetic_tests_total'] == 5


def test_real_public_train_selective_order_requires_cart_restore(tmp_path, monkeypatch):
    """The independent TRAIN goal is feasible through public APIs, without GT."""
    import uuid
    source = ADAPTER / 'runtime/world/data'
    if not (source / 'tasks/07b42fd_1/specs.json').is_file():
        pytest.skip('public AppWorld TRAIN resources unavailable')
    root = tmp_path / 'world'
    root.mkdir()
    (root / 'data').symlink_to(source, target_is_directory=True)
    monkeypatch.setenv('APPWORLD_ROOT', str(root))
    from appworld import AppWorld
    from appworld_host import synthetic_training_snapshot, synthetic_training_metrics
    world = AppWorld(task_id='07b42fd_1', experiment_name='public-order-' + uuid.uuid4().hex,
                     load_ground_truth=False)
    try:
        assert world.task.ground_truth is None
        goal = {'kind': 'order_selected_wishlist_preserve_cart', 'product_name_contains': 'soap pump'}
        before = synthetic_training_snapshot(world, goal)
        targets = {key: quantity for key, quantity in before['wish'].items()
                   if 'soap pump' in before['wish_names'][key]}
        assert before['cart'] and targets and any(q > 1 for q in targets.values())
        assert len(targets) < len(before['wish'])
        passwords = {row['account_name']: row['password']
                     for row in world.apis.supervisor.show_account_passwords()}
        amazon = world.apis.amazon
        token = amazon.login(username=world.task.supervisor.email, password=passwords['amazon'])['access_token']
        cards = amazon.show_payment_cards(access_token=token)
        current = world.task.datetime
        valid = [card for card in cards if (card['expiry_year'], card['expiry_month']) > (current.year, current.month)]
        assert valid
        card = max(valid, key=lambda row: (row['expiry_year'], row['expiry_month']))
        address = amazon.show_addresses(access_token=token)[0]
        amazon.clear_cart(access_token=token)
        for key, quantity in targets.items():
            amazon.move_product_from_wish_list_to_cart(product_id=key, quantity=quantity, access_token=token)
        amazon.place_order(payment_card_id=card['payment_card_id'], address_id=address['address_id'], access_token=token)
        world.apis.supervisor.complete_task(status='success')
        missing = synthetic_training_metrics(world, goal, before)
        assert missing['strict_passed'] == 0
        assert any(row['requirement'] == 'original_cart_products_and_quantities_preserved'
                   for row in missing['training_feedback']['failures'])
        for key, quantity in before['cart'].items():
            amazon.add_product_to_cart(product_id=key, quantity=quantity, access_token=token)
        result = synthetic_training_metrics(world, goal, before)
        assert result['strict_passed'] == 1
        assert result['quality_score'] == 1
        assert result['synthetic_tests_total'] == 5
        assert result['official_goal_evaluated'] is False
    finally:
        world.close()


def test_public_offline_snapshot_exhausts_public_pages_and_rejects_duplicate_ids():
    from types import SimpleNamespace
    from appworld_host import synthetic_training_snapshot
    calls = []
    duplicate = False
    def playlists(**kwargs):
        calls.append(('library', kwargs['page_index']))
        return [{'playlist_id': 1, 'song_ids': [11, 12, 13]}] if kwargs['page_index'] == 0 else []
    def downloaded(**kwargs):
        calls.append(('downloads', kwargs['page_index']))
        if kwargs['page_index'] == 0:
            return [{'song_id': 11, 'duration': 300}]
        if kwargs['page_index'] == 1:
            return [{'song_id': 11 if duplicate else 12, 'duration': 280}]
        return []
    spotify = SimpleNamespace(login=lambda **kwargs: {'access_token': 'fixture-token'},
        show_playlist_library=playlists, show_downloaded_songs=downloaded,
        show_song_queue=lambda **kwargs: [])
    supervisor = SimpleNamespace(show_account_passwords=lambda: [{'account_name': 'spotify', 'password': 'fixture'}])
    world = SimpleNamespace(apis=SimpleNamespace(spotify=spotify, supervisor=supervisor),
        task=SimpleNamespace(supervisor=SimpleNamespace(email='public-fixture@example.com')))
    goal = {'kind': 'offline_playlist_downloaded_duration'}
    result = synthetic_training_snapshot(world, goal)
    assert result == {'playlists': {1: [11, 12, 13]}, 'downloaded': {11: 300, 12: 280}, 'queue': []}
    assert ('library', 1) in calls and ('downloads', 2) in calls
    duplicate = True
    with pytest.raises(RuntimeError, match='pagination_duplicate'):
        synthetic_training_snapshot(world, goal)


@pytest.mark.parametrize('instruction, marker', [
    ('Play an existing playlist for my walk.', '[task_object_playlist]'),
    ('Play an album without network.', '[task_object_album]'),
    ('Count songs across playlists.', '[task_object_playlist]'),
    ('Compare albums and playlists.', None),
    ('Find my latest email.', None)])
def test_private_object_marker_comes_only_from_public_request_and_preserves_messages(instruction, marker):
    from appworld_skill import office_retrieval_state
    messages = [{'role': 'assistant', 'content': 'apis.spotify.show_playlist_library()'},
                {'role': 'user', 'content': 'album and playlist API documentation'}]
    before = json.loads(json.dumps(messages))
    state = office_retrieval_state('', messages, instruction, project=lambda value: value)
    assert state.get('task_object') == marker
    assert state['instruction'] == instruction and messages == before


@pytest.mark.parametrize('case, expected', [
    ('known_protected_alias', 'authentication_parameter_failure access_token'),
    ('serialized_document', 'authentication_parameter_failure access_token'),
    ('login_failure', 'authentication_needed login'),
    ('correct_token_invalid', 'authentication_needed login'),
    ('missing_document', 'authentication_needed login'),
    ('wrong_application_document', 'authentication_needed login'),
    ('failed_document', 'authentication_needed login'),
    ('required_false', 'authentication_needed login'),
    ('unknown_traceback_target', 'authentication_needed login'),
    ('missing_traceback', 'authentication_needed login'),
    ('unexecuted_document', 'authentication_needed login'),
    ('swallowed_document', 'authentication_needed login'),
])
def test_protected_token_parameter_failure_requires_actual_public_contract(case, expected):
    from appworld_skill import public_retrieval_stage
    def pair(code, observation):
        return [{'role': 'assistant', 'content': code}, {'role': 'user', 'content': observation}]
    contract = {'app_name': 'phone', 'api_name': 'show_contact_relationships',
                'parameters': [{'name': 'access_token', 'required': case != 'required_false'}]}
    if case == 'wrong_application_document':
        contract['app_name'] = 'gmail'
    doc_code = "print(apis.api_docs.show_api_doc(app_name='phone',api_name='show_contact_relationships'))"
    if case == 'serialized_document':
        doc_code = "print(json.dumps(apis.api_docs.show_api_doc(app_name='phone',api_name='show_contact_relationships')))"
        doc_observation = 'phone documentation: ' + json.dumps(contract)
    if case == 'unexecuted_document':
        doc_code = 'if False:\n ' + doc_code
    elif case == 'swallowed_document':
        doc_code = 'try:\n ' + doc_code + '\nexcept Exception:\n pass'
    doc_observation = 'phone documentation: ' + (json.dumps(contract) if case == 'serialized_document' else repr(contract))
    if case == 'failed_document':
        doc_observation = 'Execution failed\n' + doc_observation
    history = [] if case == 'missing_document' else pair(doc_code, doc_observation)
    history += pair("phone_token=apis.phone.login(username='public_fixture',password='fixture')['access_token']", 'login succeeded')
    code = 'apis.phone.show_contact_relationships(token=phone_token)'
    trace = 'apis.phone.show_contact_relationships(token=<redacted:credential_field>)'
    if case == 'login_failure':
        code = "apis.phone.login(username='public_fixture',password='fixture')"
        trace = "apis.phone.login(username='public_fixture',password=<redacted:credential_field>)"
    elif case == 'correct_token_invalid':
        code = 'apis.phone.show_contact_relationships(access_token=phone_token)'
        trace = 'apis.phone.show_contact_relationships(access_token=<redacted:credential_field>)'
    elif case == 'unknown_traceback_target':
        trace = 'unrelated_wrapper()'
    error = 'Execution failed. Traceback:\n' + trace + '\nException: Response status code is 401: Unauthorized'
    if case == 'missing_traceback':
        error = error.replace('Traceback:', '')
    history += pair(code, error)
    before = json.loads(json.dumps(history))
    assert public_retrieval_stage(history) == expected
    assert history == before


def test_public_token_parameter_contract_supports_json_and_repr_without_credentials():
    from appworld_skill import _public_access_token_contracts
    value = {'app_name': 'phone', 'api_name': 'search_contacts',
             'parameters': [{'name': 'access_token', 'required': True}],
             'response_schemas': {'success': [{'relationship': "friend's group"}]}}
    assert _public_access_token_contracts('doc: ' + repr(value)) == {('phone', 'search_contacts')}
    assert _public_access_token_contracts(json.dumps(value)) == {('phone', 'search_contacts')}
    assert _public_access_token_contracts("{'access_token':'unrelated credential'}") == set()
    assert _public_access_token_contracts('doc: {broken') == set()
    value['parameters'] = None
    assert _public_access_token_contracts(repr(value)) == set()


@pytest.mark.parametrize('status, calls, recovers', [(0, 2, True), (503, 2, True), (400, 1, False)])
def test_private_capture_point_recovers_only_transport_errors_with_one_identity(status, calls, recovers):
    from appworld_skill import native_capture_point
    from evolution import EvolutionError
    class Client:
        def __init__(self):
            self.requests = []
        def _point_fields(self, packet):
            return dict(packet)
        def _point_observed(self, packet, result):
            return result
        def _invoke(self, operation, fields, request_id, timeout, retries):
            self.requests.append((operation, fields, request_id, timeout, retries))
            if len(self.requests) == 1:
                raise EvolutionError('observer_unavailable', status)
            return {'context': {'run_id': 'real-run', 'activity_id': 'real-activity'}}
    client = Client()
    packet = {'phase': 'before', 'kind': 'model', 'context': {'run_id': 'real-run'}}
    unchanged = json.loads(json.dumps(packet))
    if recovers:
        assert native_capture_point(client, packet)['context']['activity_id'] == 'real-activity'
    else:
        with pytest.raises(EvolutionError):
            native_capture_point(client, packet)
    assert len(client.requests) == calls
    assert len({request[2] for request in client.requests}) == 1
    assert all(request[3:] == (5.0, 0) for request in client.requests)
    assert packet == unchanged


def test_private_capture_point_permanent_unavailable_is_bounded():
    from appworld_skill import native_capture_point
    from evolution import EvolutionError
    class Client:
        def __init__(self):
            self.ids = []
        def _point_fields(self, packet):
            return packet
        def _point_observed(self, packet, result):
            return result
        def _invoke(self, operation, fields, request_id, timeout, retries):
            self.ids.append(request_id)
            raise EvolutionError('observer_unavailable')
    client = Client()
    with pytest.raises(EvolutionError):
        native_capture_point(client, {'phase': 'after', 'kind': 'action'})
    assert len(client.ids) == 2 and len(set(client.ids)) == 1


@pytest.mark.parametrize('case, accepted', [('correct', True), ('default_one', False),
    ('moves_non_target', False), ('loses_existing_wish', False), ('changes_non_target_quantity', False)])
def test_selective_cart_training_preserves_requested_scope_and_full_quantities(monkeypatch, case, accepted):
    from types import SimpleNamespace
    import appworld_host as host
    before = {'cart': {11: 3, 12: 3}, 'wish': {13: 2}, 'cart_ratings': {11: 4.5, 12: 5.0}}
    after = {'cart': {12: 3}, 'wish': {11: 3, 13: 2}, 'cart_ratings': {12: 5.0}}
    if case == 'default_one':
        after['cart'][11] = 2
        after['wish'][11] = 1
    elif case == 'moves_non_target':
        after['cart'] = {}
        after['wish'][12] = 3
    elif case == 'loses_existing_wish':
        after['wish'].pop(13)
    elif case == 'changes_non_target_quantity':
        after['cart'][12] = 2
    monkeypatch.setattr(host, 'synthetic_training_snapshot', lambda world, goal: after)
    world = SimpleNamespace(apis=SimpleNamespace(supervisor=SimpleNamespace(
        show_active_task=lambda: {'status': 'success'})))
    result = host.synthetic_training_metrics(world,
        {'kind': 'cart_selected_rating_to_wishlist', 'rating_below': 4.8}, before)
    assert result['strict_passed'] == int(accepted)
    assert result['quality_score'] == 1 if accepted else result['quality_score'] < 1
    assert result['official_goal_evaluated'] is False


@pytest.mark.parametrize('threshold, accepted', [(4.8, True), (True, False), (0, False), (6, False),
    ('4.8', False), (float('nan'), False)])
def test_selective_cart_training_goal_uses_canonical_public_scope(tmp_path, threshold, accepted):
    from appworld_host import synthetic_training_spec
    goal = {'kind': 'cart_selected_rating_to_wishlist', 'rating_below': threshold}
    initial = {'source_task_id': '07b42fd_1', 'training_instruction': 'Move selected cart units.', 'training_goal': goal}
    path = tmp_path / 'train.jsonl'
    rows = [{'task_id': 'official', 'split': 'train', 'initial_state': {'source_task_id': '07b42fd_1'}},
            {'task_id': 'synthetic', 'split': 'train', 'initial_state': initial}]
    path.write_text(''.join(json.dumps(row) + '\n' for row in rows))
    delivered = {'initial_state': initial}
    if accepted:
        assert synthetic_training_spec(delivered, path) == goal
        with pytest.raises(RuntimeError, match='scope_invalid'):
            synthetic_training_spec({**delivered, 'split': 'val'}, path)
    else:
        with pytest.raises(RuntimeError, match='scope_invalid'):
            synthetic_training_spec(delivered, path)


@pytest.mark.parametrize('source_task, threshold, expected_targets', [('07b42fd_1', 4.8, 1), ('07b42fd_3', 3.5, 2), ('07b42fd_2', 4.2, 4)])
def test_real_public_selective_cart_requires_full_quantity_and_preserves_non_targets(tmp_path, monkeypatch, source_task, threshold, expected_targets):
    import uuid
    source = ADAPTER / 'runtime/world/data'
    if not (source / ('tasks/' + source_task + '/specs.json')).is_file():
        pytest.skip('public AppWorld TRAIN resources unavailable')
    root = tmp_path / 'world'
    root.mkdir()
    (root / 'data').symlink_to(source, target_is_directory=True)
    monkeypatch.setenv('APPWORLD_ROOT', str(root))
    from appworld import AppWorld
    from appworld_host import synthetic_training_snapshot, synthetic_training_metrics
    world = AppWorld(task_id=source_task, experiment_name='public-selected-cart-' + uuid.uuid4().hex,
                     load_ground_truth=False)
    try:
        assert world.task.ground_truth is None
        goal = {'kind': 'cart_selected_rating_to_wishlist', 'rating_below': threshold}
        before = synthetic_training_snapshot(world, goal)
        targets = {key: quantity for key, quantity in before['cart'].items()
                   if before['cart_ratings'][key] < goal['rating_below']}
        assert len(targets) == expected_targets and len(targets) < len(before['cart'])
        key, quantity = next(iter(targets.items()))
        assert quantity > 1
        passwords = {row['account_name']: row['password']
                     for row in world.apis.supervisor.show_account_passwords()}
        token = world.apis.amazon.login(username=world.task.supervisor.email,
                                         password=passwords['amazon'])['access_token']
        world.apis.amazon.move_product_from_cart_to_wish_list(product_id=key, access_token=token)
        world.apis.supervisor.complete_task(status='success')
        partial = synthetic_training_metrics(world, goal, before)
        assert partial['strict_passed'] == 0
        assert any(row['requirement'] == 'all_target_units_removed_and_other_cart_quantities_preserved'
                   for row in partial['training_feedback']['failures'])
        world.apis.amazon.move_product_from_cart_to_wish_list(product_id=key, quantity=quantity - 1, access_token=token)
        for other, other_quantity in targets.items():
            if other != key:
                world.apis.amazon.move_product_from_cart_to_wish_list(product_id=other, quantity=other_quantity, access_token=token)
        result = synthetic_training_metrics(world, goal, before)
        assert result['strict_passed'] == 1 and result['quality_score'] == 1
        assert result['official_goal_evaluated'] is False
    finally:
        world.close()


@pytest.mark.parametrize('instruction, expected', [
    ('Archive all read Gmail inbox conversations.', True),
    ('In Gmail, find the complete set of read inbox threads and archive exactly those.', True),
    ('Delete all my read Gmail threads from inbox/outbox, except priority starred ones.', True),
    ('Delete all my read Phone messages.', False),
    ('Delete all my read Gmail outbox threads.', False),
    ('Send an email to every recipient in my Gmail inbox.', False),
    ('Do not delete all my read Gmail inbox threads.', False),
    ('Archive the Gmail inbox thread titled example.', False),
    ('Star all my gmail threads with email/s from or to my coworkers and unstar the rest. Ignore the archived threads.', True),
    ('Star all my gmail threads with email/s from or to my coworkers.', False),
    ('Star all my Gmail archived threads.', False)])
def test_private_gmail_complete_inbox_purpose_is_current_request_scoped(instruction, expected):
    from appworld_skill import public_gmail_inbox_complete_enumeration
    assert public_gmail_inbox_complete_enumeration(instruction) is expected


@pytest.mark.parametrize('case, ready', [('successful', True), ('docs', False),
    ('failed', False), ('unexecuted', False), ('phone_after_gmail', False), ('logout', False)])
def test_private_gmail_enumeration_phase_requires_real_current_gmail_auth(case, ready):
    from appworld_skill import office_retrieval_state, public_retrieval_stage
    instruction = 'Delete all my read Gmail threads from inbox/outbox, except priority starred ones.'
    code = 'gmail_token = apis.gmail.login(username=user, password=password)["access_token"]'
    observation = 'Authenticated.'
    if case == 'docs':
        code = "print(apis.api_docs.show_api_doc(app_name='gmail', api_name='show_inbox_threads'))"
        observation = '{"api_name": "show_inbox_threads", "parameters": ["page_index", "page_limit"]}'
    elif case == 'failed':
        observation = 'Execution failed: appworld_auth_401 Unauthorized'
    elif case == 'unexecuted':
        code = 'def later():\n    return apis.gmail.login(username=user, password=password)["access_token"]'
    messages = [{'role': 'assistant', 'content': code}, {'role': 'user', 'content': observation}]
    if case == 'phone_after_gmail':
        messages += [{'role': 'assistant', 'content': 'print(apis.phone.search_contacts(access_token=phone_token))'},
                     {'role': 'user', 'content': '[{"contact_id": 1}]'}]
    elif case == 'logout':
        messages += [{'role': 'assistant', 'content': 'apis.gmail.logout(access_token=gmail_token)'},
                     {'role': 'user', 'content': 'Logged out.'}]
    before = json.loads(json.dumps(messages))
    details = {}
    stage = public_retrieval_stage(messages)
    assert public_retrieval_stage(messages, details) == stage
    state = office_retrieval_state('', messages, instruction, project=lambda value: value)
    assert state['task_collection'] == '[task_gmail_inbox_complete_enumeration]'
    assert bool(state.get('collection_phase')) is ready and messages == before


@pytest.mark.parametrize('instruction, expected', [
    ('Start playing an existing Spotify library playlist for my nine-minute offline walk.', True),
    ('Choose and start playing a playlist for a fifteen-minute walk without network access.', True),
    ('Give me artists with the least songs in my playlists.', False),
    ('Which downloaded playlist has the longest duration?', False),
    ('Play an album for a fifteen-minute offline walk.', False),
    ('Do not play a playlist; count downloaded songs lasting fifteen minutes.', False),
    ('Create a playlist of downloaded songs.', False),
    ('Play an existing playlist without network.', False)])
def test_private_downloaded_duration_purpose_comes_only_from_current_request(instruction, expected):
    from appworld_skill import office_retrieval_state
    messages = [{'role': 'assistant', 'content': 'apis.spotify.show_playlist_library()'},
                {'role': 'user', 'content': 'Download durations: play offline playlists for fifteen minutes.'}]
    before = json.loads(json.dumps(messages))
    state = office_retrieval_state('', messages, instruction, project=lambda value: value)
    assert bool(state.get('task_purpose')) is expected
    assert state['instruction'] == instruction and messages == before


@pytest.mark.parametrize('instruction, expected', [
    ('Move every unit of every product currently in my Amazon cart to my wish list. Collect all target IDs first.', True),
    ('Move all products from my Amazon cart to wish list.', True),
    ('Move all products with under 4.2 rating from my amazon cart to wish list.', False),
    ('Move only selected products from my Amazon cart to wish list.', False),
    ('Move all products from my Amazon wish list to cart.', False)])
def test_private_whole_cart_advice_excludes_filtered_and_reverse_requests(instruction, expected):
    from appworld_skill import public_whole_cart_to_wishlist
    assert public_whole_cart_to_wishlist(instruction) is expected


@pytest.mark.parametrize('instruction, expected', [
    ('Move all products from my Amazon cart to wish list.', True),
    ('Move all products with under 4.2 rating from my Amazon cart to wish list.', True),
    ('Move only selected products from my cart to my wishlist.', True),
    ('Transfer selected products from my cart to my wish list.', True),
    ('Move all products from my wish list to my cart.', False),
    ('Do not move products from my cart to my wish list.', False),
    ("Don't move selected products from my cart to my wishlist.", False),
    ('Never move products from my cart to my wish list.', False),
    ('List the quantities in my cart and my wish list.', False)])
def test_private_cart_quantity_purpose_includes_subsets_but_not_reverse_or_negation(instruction, expected):
    from appworld_skill import public_cart_to_wishlist, office_retrieval_state
    messages = [{'role': 'assistant', 'content': 'cart = apis.amazon.show_cart(access_token=cart_token); print(cart)'},
                {'role': 'user', 'content': '{"cart_items": [{"product_id": 7, "quantity": 3}]}'}]
    before = json.loads(json.dumps(messages))
    state = office_retrieval_state('', messages, instruction, project=lambda value: value)
    assert public_cart_to_wishlist(instruction) is expected
    assert bool(state.get('task_quantity_transfer')) is expected
    assert bool(state.get('cart_phase')) is expected
    assert messages == before
    if 'selected' in instruction or '4.2' in instruction or not expected:
        assert 'task_transfer' not in state


@pytest.mark.parametrize('case, ready', [('documentation', False), ('actual', True),
    ('python_repr', True), ('python_schema', False), ('later_rating', True)])
def test_private_subset_quantity_phase_waits_for_actual_cart_state(case, ready):
    from appworld_skill import office_retrieval_state
    instruction = 'Move all products with under 4.2 rating from my Amazon cart to wish list.'
    code = 'cart = apis.amazon.show_cart(access_token=cart_token); print(cart)'
    observation = '{"cart_items": [{"product_id": 7, "quantity": 3}]}'
    if case == 'documentation':
        code = "print(apis.api_docs.show_api_doc(app_name='amazon', api_name='show_cart'))"
        observation = '{"response_schemas": {"cart_items": {"type": "array"}}, "api_name": "move_product_from_cart_to_wish_list", "parameter": "quantity"}'
    elif case == 'python_repr':
        observation = "Cart: {'promo_code': None, 'promo_valid': False, 'cart_items': [{'product_id': 7, 'quantity': 3}]}"
    elif case == 'python_schema':
        code = "print(apis.api_docs.show_api_doc(app_name='amazon', api_name='show_cart'))"
        observation = "{'response_schemas': {'cart_items': {'type': 'array'}}}"
    messages = [{'role': 'assistant', 'content': code}, {'role': 'user', 'content': observation}]
    if case == 'later_rating':
        messages += [{'role': 'assistant', 'content': 'print(apis.amazon.show_product(product_id=7, access_token=cart_token))'},
                     {'role': 'user', 'content': '{"rating": 3.5}'}]
    before = json.loads(json.dumps(messages))
    state = office_retrieval_state('', messages, instruction, project=lambda value: value)
    assert state['task_quantity_transfer'] == '[task_cart_to_wishlist_quantity]'
    assert bool(state.get('cart_phase')) is ready
    assert 'task_transfer' not in state and messages == before


@pytest.mark.parametrize('case, ready', [
    ('actual', True), ('later_rating', True), ('empty_verification', True),
    ('documentation', False), ('unexecuted_function', False),
    ('failed', False), ('schema', False), ('incomplete_items', False)])
def test_private_cart_ready_requires_real_read_and_actual_quantity_observation(case, ready):
    from appworld_skill import office_retrieval_state
    instruction = 'Move all products from my Amazon cart to wish list.'
    code = 'cart = apis.amazon.show_cart(access_token=cart_token); print(cart)'
    observation = json.dumps({'cart_items': [{'product_id': 7, 'quantity': 3}]})
    if case == 'documentation':
        code = "print(apis.api_docs.show_api_doc(app_name='amazon', api_name='show_cart'))"
    elif case == 'unexecuted_function':
        code = 'def unused():\n    return apis.amazon.show_cart(access_token=cart_token)'
    elif case == 'empty_verification':
        observation = json.dumps({'cart_items': []})
    elif case == 'failed':
        observation += '\nTraceback: appworld_auth_401'
    elif case == 'schema':
        observation = json.dumps({'response_schemas': {'cart_items': {'type': 'array'}}})
    elif case == 'incomplete_items':
        observation = json.dumps({'cart_items': [{'product_id': 7}]})
    messages = [{'role': 'assistant', 'content': code}, {'role': 'user', 'content': observation}]
    if case == 'later_rating':
        messages += [{'role': 'assistant', 'content': 'print(apis.amazon.show_product(product_id=7, access_token=cart_token))'},
                     {'role': 'user', 'content': '{"rating": 3.5}'}]
    before = json.loads(json.dumps(messages))
    state = office_retrieval_state('', messages, instruction, project=lambda value: value)
    assert bool(state.get('cart_phase')) is ready and messages == before
    assert state['task_transfer'] == '[task_cart_to_wishlist_full_quantity]'


@pytest.mark.parametrize('instruction, expected', [
    ('Give me artist names with the least songs in my Spotify playlists.', True),
    ('Which of my Spotify playlists has the most songs in my entire library?', True),
    ('Count all the songs across my Spotify playlists.', True),
    ('Choose one of my existing Spotify library playlists for an offline walk. Inspect every page of both collections.', True),
    ('Play the Spotify playlist titled Morning Mix.', False),
    ('Count songs in my Spotify playlist Morning Mix.', False),
    ('Count the least common artists in my Spotify albums.', False),
    ('List all contacts from my Phone playlists.', False),
    ('Do not list all my Spotify playlists.', False)])
def test_private_spotify_complete_library_purpose_matches_only_public_full_collection(instruction, expected):
    from appworld_skill import public_spotify_playlist_library_complete_enumeration
    assert public_spotify_playlist_library_complete_enumeration(instruction) is expected


@pytest.mark.parametrize('case, ready', [('successful', True), ('docs', False),
    ('failed', False), ('unexecuted', False), ('phone_after_spotify', False), ('logout', False)])
def test_private_spotify_library_phase_requires_actual_current_spotify_login(case, ready):
    from appworld_skill import office_retrieval_state
    instruction = 'Give me artist names with the least songs in my Spotify playlists.'
    code = 'spotify_token = apis.spotify.login(username=user, password=password)["access_token"]'
    observation = 'Authenticated.'
    if case == 'docs':
        code = "print(apis.api_docs.show_api_doc(app_name='spotify', api_name='show_playlist_library'))"
        observation = '{"api_name":"show_playlist_library","parameters":["page_index","page_limit"]}'
    elif case == 'failed':
        observation = 'Execution failed: appworld_auth_401 Unauthorized'
    elif case == 'unexecuted':
        code = 'def later():\n    return apis.spotify.login(username=user, password=password)["access_token"]'
    messages = [{'role':'assistant','content':code}, {'role':'user','content':observation}]
    if case == 'phone_after_spotify':
        messages += [{'role':'assistant','content':'print(apis.phone.search_contacts(access_token=phone_token))'},
                     {'role':'user','content':'[{"contact_id":1}]'}]
    elif case == 'logout':
        messages += [{'role':'assistant','content':'apis.spotify.logout(access_token=spotify_token)'},
                     {'role':'user','content':'Logged out.'}]
    before = json.loads(json.dumps(messages))
    state = office_retrieval_state('', messages, instruction, project=lambda value:value)
    assert state['playlist_collection'] == '[task_spotify_playlist_library_complete_enumeration]'
    assert bool(state.get('playlist_collection_phase')) is ready
    assert messages == before


@pytest.mark.parametrize('case, ready', [('dumps', True), ('repr', True),
    ('docs_only', False), ('failed', False), ('unexecuted', False)])
def test_private_login_readiness_accepts_real_serialized_login_result_only(case, ready):
    from appworld_skill import public_retrieval_stage
    code = 'login_response = apis.spotify.login(username=user, password=password)\nprint(json.dumps(login_response))'
    observation = 'Login response: {"access_token":"<redacted:credential_field>"}'
    if case == 'repr':
        code = code.replace('json.dumps(login_response)', 'repr(login_response)')
    elif case == 'docs_only':
        code = "print(json.dumps(apis.api_docs.show_api_doc(app_name='spotify', api_name='login')))"
    elif case == 'failed':
        observation = 'Execution failed: appworld_auth_401 Unauthorized'
    elif case == 'unexecuted':
        code = 'def later():\n    login_response = apis.spotify.login(username=user, password=password)\n    print(json.dumps(login_response))'
    messages = [{'role':'assistant','content':code}, {'role':'user','content':observation}]
    before = json.loads(json.dumps(messages))
    details = {}
    public_retrieval_stage(messages, details)
    assert ('spotify' in details['authenticated_apps']) is ready
    assert messages == before


@pytest.mark.parametrize('instruction, expected', [
    ('Place an order for all wrench sets in my amazon wish list.', True),
    ('Place one Amazon order for every unit of every soap pump currently in my wish list.', True),
    ('From my Amazon wishlist, buy every unit of the selected products.', True),
    ('Order the selected products from my Amazon wish list.', True),
    ('Move all products from my Amazon wish list to my cart.', False),
    ('Move all products from my Amazon cart to my wish list.', False),
    ('Show my last Amazon order and all products in my wish list.', False),
    ('Do not buy any products from my Amazon wish list.', False),
    ('Place an order for products in my Amazon cart.', False),
    ('Buy a new book on Amazon and leave my wish list unchanged.', False),
    ('Buy products on Amazon but not from my wish list.', False),
    ('Buy the products in my Phone wish list.', False)])
def test_private_wishlist_purchase_purpose_is_only_current_public_order_goal(instruction, expected):
    from appworld_skill import public_wishlist_target_purchase
    assert public_wishlist_target_purchase(instruction) is expected


@pytest.mark.parametrize('case, ready', [('successful', True), ('empty', True),
    ('docs', False), ('failed_cart', False), ('no_auth', False),
    ('phone_after_cart', False), ('unexecuted', False)])
def test_private_purchase_phase_requires_actual_authenticated_cart_state(case, ready):
    from appworld_skill import office_retrieval_state
    instruction = 'Place an order for all wrench sets in my amazon wish list.'
    login = 'amazon_token = apis.amazon.login(username=user, password=password)["access_token"]'
    messages = [{'role':'assistant','content':login}, {'role':'user','content':'Authenticated.'}]
    code = 'print(apis.amazon.show_cart(access_token=amazon_token))'
    observation = '{"cart_items":[{"product_id":1,"quantity":3}]}'
    if case == 'empty':
        observation = '{"cart_items":[]}'
    elif case == 'docs':
        code = "print(apis.api_docs.show_api_doc(app_name='amazon', api_name='show_cart'))"
        observation = '{"response_schemas":{"cart_items":[{"product_id":1,"quantity":3}]}}'
    elif case == 'failed_cart':
        observation = 'Execution failed: TypeError: unavailable cart'
    elif case == 'no_auth':
        messages = []
    elif case == 'unexecuted':
        code = 'def later():\n    return apis.amazon.show_cart(access_token=amazon_token)'
    messages += [{'role':'assistant','content':code}, {'role':'user','content':observation}]
    if case == 'phone_after_cart':
        messages += [{'role':'assistant','content':'print(apis.phone.search_contacts(access_token=phone_token))'},
                     {'role':'user','content':'[{"contact_id":1}]'}]
    before = json.loads(json.dumps(messages))
    state = office_retrieval_state('', messages, instruction, project=lambda value:value)
    assert state['purchase_purpose'] == '[task_wishlist_target_purchase]'
    assert bool(state.get('purchase_phase')) is ready
    assert messages == before


def _failed_projection_dispatch_fixture(tmp_path):
    root, request, row, atomic, log = _projection_dispatch_fixture(tmp_path)
    events = row['evidence']['trace']['events']
    start = {'event_id':'failed-start','sequence':'5','kind':'action.start',
             'action_id':'failed-dispatch','actor_id':'skill_executor',
             'caused_by':['decision-1'],'input':{'code':'print("failed proposal")'}}
    end = {'event_id':'failed-end','sequence':'6','kind':'action.end',
           'action_id':'failed-dispatch','actor_id':'skill_executor',
           'caused_by':['failed-start'],'input':dict(start['input']),
           'attributes':{'executed':True,'extensions':{'source_event_name':'skill_executor.tool_result'}},
           'output':{'status':'error','tool_result':json.dumps({'error':'fixture dispatch failed'})}}
    events[2:2] = [start,end]
    atomic.write_text(json.dumps(row))
    return root, request, row, atomic, log


def test_projection_preserves_failed_framework_dispatch_and_projects_only_real_world(tmp_path):
    from appworld_host import project_official_code_request
    root,request,row,atomic,log = _failed_projection_dispatch_fixture(tmp_path)
    before = {path:path.read_bytes() for path in (atomic,log)}
    result = project_official_code_request(request,evidence_root=root)
    assert result['status']=='complete'
    assert {patch['event_id'] for patch in result['patches']} == {'dispatch-start','dispatch-end','event-2','event-3'}
    assert all(path.read_bytes()==data for path,data in before.items())
    assert next(event for event in row['evidence']['trace']['events'] if event['event_id']=='failed-end')['output']['status']=='error'


@pytest.mark.parametrize('mode',['world_binding','successful_reply','missing_start','different_start_code','observation','fake_success_no_world','wrong_actor','duplicate_end'])
def test_projection_rejects_unproven_failed_or_fake_successful_dispatch(tmp_path,mode):
    from appworld_host import project_official_code_request
    root,request,row,atomic,log = _failed_projection_dispatch_fixture(tmp_path)
    events = row['evidence']['trace']['events']
    start = next(event for event in events if event['event_id']=='failed-start')
    end = next(event for event in events if event['event_id']=='failed-end')
    if mode=='world_binding':
        code=next(event for event in events if event['event_id']=='event-3')['input']['code']
        start['input']['code']=code;end['input']['code']=code
    elif mode=='successful_reply':end['output']['tool_result']=json.dumps({'error':'failure','reply_saved':True})
    elif mode=='missing_start':events.remove(start)
    elif mode=='different_start_code':start['input']['code']+='\nprint("unrelated")'
    elif mode=='observation':end['output']['observation']='Must remain unknown.'
    elif mode=='fake_success_no_world':end['output'].update(status='completed',tool_result=json.dumps({'status':'completed','reply_saved':True}))
    elif mode=='wrong_actor':start['actor_id']='other_actor'
    elif mode=='duplicate_end':
        duplicate=json.loads(json.dumps(end));duplicate['event_id']='other-failed-end';events.append(duplicate)
    atomic.write_text(json.dumps(row))
    before=atomic.read_bytes()
    result=project_official_code_request(request,evidence_root=root)
    assert result['status']=='unknown' and result['patches']==[]
    assert atomic.read_bytes()==before


def _pending_close_fixture(tmp_path, monkeypatch, *, close_at=None, progress_every=4,
                           error=None, task_deadline=None):
    host = _capture_boundary_host()
    class Clock:
        now = 0.0
        def sleep(self, delay): self.now += delay
    clock = Clock()
    monkeypatch.setattr(host, 'real_clock', lambda: clock.now)
    monkeypatch.setattr(host.time, 'sleep', clock.sleep)
    class Client:
        def __init__(self): self.calls = []
        def _point_fields(self, fields): return fields
        def _point_observed(self, fields, result): return result
        def _invoke(self, operation, fields, request, timeout, retries):
            self.calls.append((operation, fields, timeout, retries))
            if operation == 'point':
                return {'pending': True, 'closed': False, 'capture_complete': True}
            if error is not None:
                failure = error(clock.now) if callable(error) else error
                if failure is not None: raise failure
            return {'run_id': 'fixture-observed-run',
                    'event_count': int(clock.now / progress_every) if progress_every else 251,
                    'closed': close_at is not None and clock.now >= close_at,
                    'capture_complete': True}
    client = Client();trial = _capture_trial(host, client, tmp_path)
    trial.run_context = {'run_id': 'fixture-observed-run'}
    if task_deadline is not None: trial._task_deadline = task_deadline
    return host, trial, client, clock


def test_native_run_close_retains_reporting_children_past_ten_seconds(tmp_path, monkeypatch):
    host, trial, client, clock = _pending_close_fixture(tmp_path, monkeypatch, close_at=13)
    result = trial.point({'phase': 'after', 'kind': 'run', 'context': trial.run_context,
                          'output': {'official_reward': 1}})
    assert 13 <= clock.now < 14 and result['closed'] is True and result['pending'] is False
    assert sum(row[0] == 'point' for row in client.calls) == 1
    assert all(row[2] <= 1 and row[3] == 0 for row in client.calls[1:])
    assert trial.degraded_points == 0 and trial.framework_capture['failures'] == 0
    assert trial.boundary_diagnostics[-1]['status'] == 'pending_children_closed'


@pytest.mark.parametrize('progress_every, task_deadline, expected', [(4, None, 60), (None, None, 10), (1, 6, 6)])
def test_native_run_close_progress_is_capped_without_fake_completion(
        tmp_path, monkeypatch, progress_every, task_deadline, expected):
    host, trial, client, clock = _pending_close_fixture(
        tmp_path, monkeypatch, progress_every=progress_every, task_deadline=task_deadline)
    with pytest.raises(host.AppWorldCaptureBoundaryError, match='appworld_native_run_boundary_failed'):
        trial.point({'phase': 'after', 'kind': 'run', 'context': trial.run_context, 'output': {}})
    assert expected - .001 <= clock.now <= expected + .001
    assert sum(row[0] == 'point' for row in client.calls) == 1
    assert trial.boundary_diagnostics[-1]['error_code'] == 'appworld_native_run_close_pending_timeout'
    assert trial.framework_capture['failures'] == 1
    assert all(row['status'] != 'pending_children_closed' for row in trial.boundary_diagnostics)
    diagnostic = json.loads((tmp_path/'host-error-diagnostic.json').read_text())
    assert diagnostic['native_run_boundaries'] == trial.boundary_diagnostics


@pytest.mark.parametrize('code,status', [('source_not_authorized', 403), ('evaluation_cancelled', 409),
                                         ('evaluation_cancelled', 0)])
def test_native_run_close_does_not_swallow_authority_or_cancel(tmp_path, monkeypatch, code, status):
    from evolution import EvolutionError
    host, trial, client, clock = _pending_close_fixture(
        tmp_path, monkeypatch, error=EvolutionError(code, status))
    with pytest.raises(host.AppWorldCaptureBoundaryError, match='appworld_native_run_boundary_failed'):
        trial.point({'phase': 'after', 'kind': 'run', 'context': trial.run_context, 'output': {}})
    assert clock.now == 0 and [row[0] for row in client.calls] == ['point', 'run.status']
    assert trial.boundary_diagnostics[-1]['error_code'] == code
    assert trial.framework_capture['failures'] == 1


@pytest.mark.parametrize('status', [408, 429])
def test_native_run_close_transient_status_waits_without_replaying_agent(tmp_path, monkeypatch, status):
    from evolution import EvolutionError
    host, trial, client, clock = _pending_close_fixture(
        tmp_path, monkeypatch, close_at=13,
        error=lambda elapsed: EvolutionError('fixture_transient_close_status', status) if elapsed < .25 else None)
    result = trial.point({'phase': 'after', 'kind': 'run', 'context': trial.run_context, 'output': {}})
    assert result['closed'] is True and 13 <= clock.now < 14
    assert sum(row[0] == 'point' for row in client.calls) == 1
    assert trial.degraded_points == 0 and trial.framework_capture['failures'] == 0
    assert trial.boundary_diagnostics[-1]['status'] == 'pending_children_closed'


def test_point_gap_schema_diagnostic_is_bounded_and_contains_types_only(tmp_path):
    host = _capture_boundary_host()
    class Client:
        def __init__(self): self.gaps = []
        def _invoke(self, *args): self.gaps.append(args); return {}
    client = Client();trial = _capture_trial(host, client, tmp_path)
    trial.run_context = {'run_id': 'bound-run'}
    secret = 'PRIVATE_PASSWORD_AND_TOKEN_0123456789'
    path = tmp_path / 'host-error-diagnostic.json'
    path.write_text(json.dumps({'other_safe_metadata': {'known': True}}))
    for ordinal in range(10):
        fields = {'phase': 'after', 'kind': 'action', 'context': {'activity_id': 'actual-id'},
            'input': {'code': secret}, 'actual_input': {'password': secret}, 'state': {'access_token': secret},
            'output': {'observation': secret}, 'metrics': {'sequence': ordinal},
            'error': {'code': 'appworld_auth_401', 'retryability': 'retryable', 'message': secret,
                      'invalid-key': secret}}
        unchanged = json.loads(json.dumps(fields))
        result = trial._point_gap(fields, error='contract_invalid')
        assert result == {'context': fields['context'], 'input': fields['input'], 'decision': 'allow',
                          'degraded': True, 'capture_complete': False}
        assert fields == unchanged
    saved = json.loads(path.read_text());points = saved['native_points']
    assert len(points) == 8 and saved['other_safe_metadata'] == {'known': True}
    assert trial.degraded_points == trial.framework_capture['failures'] == 10 and len(client.gaps) == 10
    for row in points:
        assert set(row) == {'phase', 'kind', 'error_code', 'field_types', 'error_field_types',
                           'projected_error_field_types', 'projected_error_code_known', 'projected_retryability_known'}
        assert row['phase'] == 'after' and row['kind'] == 'action' and row['error_code'] == 'contract_invalid'
        assert set(row['field_types'].values()) == {'dict'}
        assert row['error_field_types'] == {'code': 'str', 'retryability': 'str', 'message': 'str'}
        assert set(row['projected_error_field_types'].values()) == {'str'}
        assert row['projected_error_code_known'] is True and row['projected_retryability_known'] is True
    encoded = path.read_text()
    assert secret not in encoded and 'actual-id' not in encoded and 'bound-run' not in encoded
    assert 'code_block' not in encoded and 'observation' not in encoded


def test_point_gap_schema_diagnostic_never_persists_exception_or_unknown_error_values(tmp_path):
    host = _capture_boundary_host();trial = _capture_trial(host, None, tmp_path)
    secret = 'PRIVATE_NATIVE_EXCEPTION_PASSWORD_0123456789'
    trial._point_gap({'phase': 'after', 'kind': 'loop', 'context': {},
                     'error': {'code': secret, 'retryability': secret}}, error=RuntimeError(secret))
    row = json.loads((tmp_path / 'host-error-diagnostic.json').read_text())['native_points'][0]
    assert row['error_code'] == 'observer_unavailable'
    assert row['projected_error_code_known'] is False and row['projected_retryability_known'] is False
    assert secret not in json.dumps(row)
    assert trial.degraded_points == trial.framework_capture['failures'] == 1


def test_healthy_point_and_unobserved_after_do_not_create_error_diagnostic(tmp_path, monkeypatch):
    host = _capture_boundary_host();trial = _capture_trial(host, object(), tmp_path)
    healthy = {'context': {'activity_id': 'actual'}, 'degraded': False}
    monkeypatch.setattr(host, 'native_capture_point', lambda client, packet: healthy)
    assert trial.point({'phase': 'before', 'kind': 'action', 'input': {'public': True}}) is healthy
    assert not (tmp_path / 'host-error-diagnostic.json').exists()
    assert trial.degraded_points == trial.framework_capture['failures'] == 0
    result = trial.point({'phase': 'after', 'kind': 'action', 'context': {}})
    assert result['degraded'] is True and result['capture_complete'] is False
    assert trial.degraded_points == trial.framework_capture['failures'] == 1
    assert not (tmp_path / 'host-error-diagnostic.json').exists()


def test_failed_point_preserves_gap_or_degraded_return_and_records_only_schema(tmp_path, monkeypatch):
    host = _capture_boundary_host();trial = _capture_trial(host, object(), tmp_path)
    fields = {'phase': 'after', 'kind': 'action', 'context': {'activity_id': 'actual'}}
    def failure(*args): raise RuntimeError('contract_invalid')
    monkeypatch.setattr(host, 'native_capture_point', failure)
    result = trial.point(fields)
    assert result == {'context': fields['context'], 'input': None, 'decision': 'allow',
                      'degraded': True, 'capture_complete': False}
    degraded = {'context': {'activity_id': 'same'}, 'degraded': True, 'reason': 'contract_invalid'}
    monkeypatch.setattr(host, 'native_capture_point', lambda *args: degraded)
    assert trial.point(fields) is degraded
    saved = json.loads((tmp_path / 'host-error-diagnostic.json').read_text())
    assert len(saved['native_points']) == trial.degraded_points == trial.framework_capture['failures'] == 2


@pytest.mark.parametrize('failure', ['projection', 'malformed_points'])
def test_point_gap_diagnostic_failure_never_changes_gap_or_capture_notification(tmp_path, monkeypatch, failure):
    host = _capture_boundary_host()
    class Client:
        def __init__(self): self.gaps = []
        def _invoke(self, *args): self.gaps.append(args); return {}
    client = Client();trial = _capture_trial(host, client, tmp_path)
    trial.run_context = {'run_id': 'verified-run'}
    path = tmp_path / 'host-error-diagnostic.json'
    if failure == 'projection':
        def broken_projection(value): raise RuntimeError('projection_failed')
        monkeypatch.setattr(host, 'native_view', broken_projection)
    else:
        path.write_text(json.dumps({'native_points': {'not_a_list': True}}))
    before = path.read_bytes() if path.is_file() else None
    fields = {'phase': 'after', 'kind': 'loop', 'context': {'control_id': 'real-control'}}
    result = trial._point_gap(fields, error='contract_invalid')
    assert result == {'context': fields['context'], 'input': None, 'decision': 'continue',
                      'degraded': True, 'capture_complete': False}
    assert trial.degraded_points == trial.framework_capture['failures'] == len(client.gaps) == 1
    assert client.gaps[0][0] == 'capture.gap' and client.gaps[0][1]['run_id'] == 'verified-run'
    assert (path.read_bytes() if path.is_file() else None) == before
