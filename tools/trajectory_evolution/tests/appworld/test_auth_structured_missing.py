import copy
import json
from pathlib import Path
import sys
import pytest

ROOT=Path(__file__).resolve().parents[4]
sys.path.insert(0,str(ROOT/'examples/agent/business-simulator/appworld'))
from appworld_skill import auth_parameter_failure_state, NativeFrameworkObserver
from appworld_skill import (appworld_shape_error_auth_experiences,
                            non_authentication_type_error, pure_authentication_experience)

MESSAGE=('The host blocked a protected call without access_token. '
         'Reuse the token variable returned by the successful login; do not login again.')
REF={'memory_id':'approved-token-card','content_revision':'fixed'}

def history(code='apis.phone.show_alarms()',error='appworld_missing_access_token'):
    return [{'role':'assistant','content':code},{'role':'user','content':json.dumps({
        'status':'rejected','error_code':error,'message':MESSAGE})}]

@pytest.mark.parametrize('enabled',[False,True])
def test_structured_qualification_requires_new_flag_and_honest_metadata(enabled):
    state=auth_parameter_failure_state('',history(),'Update alarms.',lambda x:x,
        public_contracts=[['phone','show_alarms']],allow_structured_missing=enabled)
    assert (state is not None)==enabled
    if state is not None:
        assert 'host_missing_access_token' in state['causal_auth_parameter_failure']
        assert '401' not in state['causal_auth_parameter_failure']
        assert 'submitted call' in state['causal_auth_parameter_failure']

@pytest.mark.parametrize('code,error,contracts',[
    ('apis.phone.show_alarms(access_token=t)','appworld_missing_access_token',[['phone','show_alarms']]),
    ('apis.phone.show_alarms()','appworld_missing_access_token',[]),
    ('apis.phone.show_alarms()','other_policy_rejection',[['phone','show_alarms']]),
    ('apis.phone.show_alarms(**params)','appworld_missing_access_token',[['phone','show_alarms']]),
])
def test_correct_unknown_missing_contract_or_other_rejection_do_not_qualify(code,error,contracts):
    assert auth_parameter_failure_state('',history(code,error),'Update alarms.',lambda x:x,
        public_contracts=contracts,allow_structured_missing=True) is None

@pytest.mark.parametrize('enabled',[False,True])
def test_full_before_model_preserves_legacy_and_restricts_new_guard(monkeypatch,enabled):
    monkeypatch.setenv('APPWORLD_POLICY_LEVEL','L3');monkeypatch.delenv('APPWORLD_RUNTIME_STATE',raising=False)
    state={'native_context':{'run_id':'fixture'},'policy':'Public policy.','messages':history(),
        'public_instruction':'Update alarms.','appworld_auth_parameter_experiences':[REF],
        'appworld_auth_fallback_experiences':[],'appworld_auth_parameter_contracts':[['phone','show_alarms']]}
    if enabled:state['appworld_auth_structured_missing']=True
    class Client:
        def __init__(self):self.packets=[]
        def point(self,packet,**kwargs):
            self.packets.append(copy.deepcopy(packet))
            return {'context':{'run_id':'fixture','activity_id':'fixture-model'},'input':copy.deepcopy(packet.get('input',{}))}
    client=Client();observer=NativeFrameworkObserver(client,state_reader=lambda:state,project=lambda x:x)
    request=[{'role':'system','content':'Original public request.'}]
    observer.before_model(copy.deepcopy(request),{'source':'skill_executor','skill_name':'appworld-office',
        'exec_skill_key':'office','node_id':'skill_executor','round':1,'task_id':'fixture'})
    packet=next(p for p in reversed(client.packets) if p.get('kind')=='model')
    assert packet['apply_policy'] is enabled
    assert packet['input']['messages']==request
    if enabled:assert packet['extensions']['runtime_experience_allowlist']==[REF]


AUTH_REF = {'memory_id': 'credential-only-source', 'content_revision': 'current'}
OTHER_REF = {'memory_id': 'independent-task-source', 'content_revision': 'current'}
AUTH_CONTENT = '登录后保存access_token，在所有需要认证的API调用中传入该参数。'


def shape_artifact(enabled=True):
    artifact = {'level': 'L3', 'agent': {'appworld_options': {}}, 'policy': {
        'options': {'approved_experiences': [AUTH_REF, OTHER_REF]}, 'snapshot': {'experiences': [
            {**AUTH_REF, 'content': AUTH_CONTENT},
            {**OTHER_REF, 'content': '按page_index分页，直到返回列表长度小于page_limit。'}]}}}
    if enabled:artifact['agent']['appworld_options']['l1_shape_error_auth_experiences'] = [AUTH_REF]
    return artifact


def test_shape_filter_source_is_approved_pure_auth_exact_revision():
    assert pure_authentication_experience(AUTH_CONTENT)
    assert not pure_authentication_experience('登录后按page_index分页收集全部商品。')
    assert not pure_authentication_experience('查询api_docs确认精确参数名，再传入access_token。')
    assert appworld_shape_error_auth_experiences(shape_artifact(False)) is None
    artifact = shape_artifact()
    result = appworld_shape_error_auth_experiences(artifact)
    assert result['excluded_refs'] == [AUTH_REF]
    artifact['agent']['appworld_options']['l1_shape_error_auth_experiences'][0] = {**AUTH_REF, 'content_revision': 'stale'}
    with pytest.raises(ValueError):appworld_shape_error_auth_experiences(artifact)
    artifact = shape_artifact();artifact['agent']['appworld_options']['l1_shape_error_auth_experiences'] = [OTHER_REF]
    with pytest.raises(ValueError):appworld_shape_error_auth_experiences(artifact)
    artifact = shape_artifact();artifact['level'] = 'L0'
    assert appworld_shape_error_auth_experiences(artifact) is None


@pytest.mark.parametrize('observation,eligible',[
    ("TypeError: list indices must be integers or slices, not str", True),
    ("TypeError: string indices must be integers, not 'str'", True),
    ("TypeError: takes 0 positional arguments", True),
    ("TypeError: unexpected keyword argument 'access_token'", False),
    ("TypeError: invalid value\n401 Unauthorized", False),
    ("TypeError: invalid value\nappworld_missing_access_token", False),
    ("Traceback (most recent call last):\n  cart = apis.amazon.show_cart(access_token=token)\n"
     "TypeError: string indices must be integers, not 'str'", True),
    ("Traceback (most recent call last):\n  apis.amazon.show_cart(access_token=token)\n"
     "TypeError: unexpected keyword argument 'access_token'", False),
    ("Successfully returned cart list.", False),
])
def test_shape_filter_current_error_preserves_real_authentication(observation, eligible):
    messages = [{'role': 'user', 'content': 'Earlier request failed with 401.'},
                {'role': 'assistant', 'content': 'cart = apis.amazon.show_cart(access_token=t)'},
                {'role': 'user', 'content': observation}]
    assert non_authentication_type_error(messages) is eligible


@pytest.mark.parametrize('enabled', [False, True])
def test_full_shape_filter_preserves_native_query_and_filters_cached_auth(monkeypatch, tmp_path, enabled):
    monkeypatch.setenv('APPWORLD_POLICY_LEVEL', 'L3')
    monkeypatch.delenv('APPWORLD_RUNTIME_STATE', raising=False)
    config = tmp_path / 'instrumentation.json'; config.write_text('{}')
    (tmp_path / 'version.json').write_text(json.dumps({'artifact': shape_artifact(enabled)}))
    monkeypatch.setenv('TRAJECTORY_EVOLUTION_TRIAL_CONFIG', str(config))
    state = {'native_context': {'run_id': 'fixture'}, 'policy': 'Public policy.',
             'messages': [{'role': 'assistant', 'content': 'cart=apis.amazon.show_cart(access_token=t)'},
                          {'role': 'user', 'content': "TypeError: list indices must be integers, not 'str'"}],
             'public_instruction': 'Move the selected products.'}
    class Client:
        def __init__(self):self.packets=[]
        def point(self, packet, **kwargs):
            self.packets.append(copy.deepcopy(packet))
            return {'context': {'run_id': 'fixture', 'activity_id': 'fixture-model'},
                    'input': copy.deepcopy(packet.get('input', {}))}
    client=Client();observer=NativeFrameworkObserver(client,state_reader=lambda:state,project=lambda x:x)
    observer._cached_cards=lambda scope: [AUTH_CONTENT, 'Relevant existing pagination advice.']
    observer._mark_l1_applied=lambda: None
    request=[{'role':'system','content':'Original request.'}]
    observer.before_model(request, {'source':'skill_executor','skill_name':'appworld-office',
        'exec_skill_key':'office','node_id':'skill_executor','round':1,'task_id':'fixture'})
    packet=next(p for p in reversed(client.packets) if p.get('kind')=='model')
    assert packet['apply_policy'] is True
    if enabled:
        assert packet['extensions']['runtime_experience_allowlist']==[OTHER_REF]
        assert not any(item.get('content')==AUTH_CONTENT for item in request)
    else:
        assert 'runtime_experience_allowlist' not in packet['extensions']
        assert any(item.get('content')==AUTH_CONTENT for item in request)
    assert any(item.get('content')=='Relevant existing pagination advice.' for item in request)


def test_shape_filter_survives_nested_runtime_removal_of_trial_environment(monkeypatch):
    monkeypatch.setenv('APPWORLD_POLICY_LEVEL', 'L3')
    monkeypatch.delenv('TRAJECTORY_EVOLUTION_TRIAL_CONFIG', raising=False)
    monkeypatch.delenv('APPWORLD_RUNTIME_STATE', raising=False)
    validated = appworld_shape_error_auth_experiences(shape_artifact())
    state = {'native_context': {'run_id': 'fixture'}, 'policy': 'Public policy.',
             'messages': [{'role': 'assistant', 'content': 'cart=apis.amazon.show_cart(access_token=t)'},
                          {'role': 'user', 'content': "TypeError: string indices must be integers, not 'str'"}],
             'public_instruction': 'Move the selected products.',
             'appworld_shape_error_auth_experiences': validated}
    class Client:
        def __init__(self): self.packets=[]
        def point(self, packet, **kwargs):
            self.packets.append(copy.deepcopy(packet))
            return {'context': {'run_id': 'fixture', 'activity_id': 'fixture-model'},
                    'input': copy.deepcopy(packet.get('input', {}))}
    client = Client()
    observer = NativeFrameworkObserver(client, state_reader=lambda:state, project=lambda x:x)
    observer.before_model([{'role':'system','content':'Original request.'}],
        {'source':'skill_executor','skill_name':'appworld-office','exec_skill_key':'office',
         'node_id':'skill_executor','round':1,'task_id':'fixture'})
    packet = next(p for p in reversed(client.packets) if p.get('kind')=='model')
    assert packet['apply_policy'] is True
    assert packet['extensions']['runtime_experience_allowlist'] == [OTHER_REF]
    assert observer.shape_error_auth_experiences == validated
    assert observer.shape_error_auth_experiences is not validated


def test_shape_filter_reads_configured_state_after_observer_started(monkeypatch):
    monkeypatch.setenv('APPWORLD_POLICY_LEVEL', 'L3')
    monkeypatch.delenv('TRAJECTORY_EVOLUTION_TRIAL_CONFIG', raising=False)
    monkeypatch.delenv('APPWORLD_RUNTIME_STATE', raising=False)
    state = {'native_context': {'run_id': 'fixture'}}
    class Client:
        def __init__(self): self.packets=[]
        def point(self, packet, **kwargs):
            self.packets.append(copy.deepcopy(packet))
            return {'context': {'run_id': 'fixture', 'activity_id': 'fixture-model'},
                    'input': copy.deepcopy(packet.get('input', {}))}
    client = Client()
    observer = NativeFrameworkObserver(client, state_reader=lambda:state, project=lambda x:x)
    assert observer.shape_error_auth_experiences is None
    state.update(policy='Public policy.', public_instruction='Move selected products.',
        messages=[{'role':'assistant','content':'cart=apis.amazon.show_cart(access_token=t)'},
                  {'role':'user','content':"TypeError: string indices must be integers, not 'str'"}],
        appworld_shape_error_auth_experiences=appworld_shape_error_auth_experiences(shape_artifact()))
    observer.before_model([{'role':'system','content':'Original request.'}],
        {'source':'skill_executor','skill_name':'appworld-office','exec_skill_key':'office',
         'node_id':'skill_executor','round':1,'task_id':'fixture'})
    packet=next(p for p in reversed(client.packets) if p.get('kind')=='model')
    assert packet['extensions']['runtime_experience_allowlist']==[OTHER_REF]
