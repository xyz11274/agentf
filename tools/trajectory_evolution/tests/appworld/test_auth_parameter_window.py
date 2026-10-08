"""Use executed failures and returned API documents, never task answers."""
import json
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'examples/agent/business-simulator/appworld'))
from appworld_skill import auth_parameter_failure_state


def history(call='apis.phone.show_alarms()', *, status=401, required=True,
            doc_app='phone', doc_call=None):
    doc_call = doc_call or 'print(apis.api_docs.show_api_doc(app_name="phone", api_name="show_alarms"))'
    doc = {'app_name': doc_app, 'api_name': 'show_alarms',
           'parameters': [{'name': 'access_token', 'required': required}]}
    return [{'role': 'assistant', 'content': doc_call},
            {'role': 'user', 'content': json.dumps(doc)},
            {'role': 'assistant', 'content': call},
            {'role': 'user', 'content': f'Execution failed\nTraceback:\n{call}\nException: {status} Unauthorized'}]


def check(rows):
    return auth_parameter_failure_state('', rows, 'Update my alarms.', lambda value: value)


def test_executed_missing_required_token():
    state = check(history())
    assert state is not None
    assert '401' in state['causal_auth_parameter_failure']
    assert 'access_token' in state['causal_auth_parameter_failure']
    assert 'phone.show_alarms' in state['causal_auth_parameter_failure']


def test_wrong_token_alias_uses_returned_contract():
    assert check(history('apis.phone.show_alarms(token=phone_token)')) is not None


@pytest.mark.parametrize('call', [
    'apis.phone.show_alarms(access_token=phone_token)',
    'apis.phone.show_alarms(phone_token)',
    'apis.phone.show_alarms(**params)',
    'apis.gmail.show_alarms()',
    'apis.phone.login()',
])
def test_correct_unknown_or_unrelated_argument_never_qualifies(call):
    assert check(history(call)) is None


@pytest.mark.parametrize('change', [
    {'status': 422}, {'required': False}, {'doc_app': 'gmail'},
    {'doc_call': 'print("made-up document")'},
    {'doc_call': 'if False:\n    apis.api_docs.show_api_doc(app_name="phone", api_name="show_alarms")'},
])
def test_missing_contract_or_failure_does_not_qualify(change):
    assert check(history(**change)) is None


def test_success_and_stale_failure_do_not_qualify():
    rows = history()
    rows[-1]['content'] = 'Success: 401 is merely text.'
    assert check(rows) is None
    rows += [{'role': 'assistant', 'content': 'print("done")'},
             {'role': 'user', 'content': 'done'}]
    assert check(rows) is None


def test_login_failed_before_a_later_protected_call_does_not_qualify():
    rows = history('apis.phone.login()\napis.phone.show_alarms()')
    rows[-1]['content'] = 'Execution failed\nTraceback:\napis.phone.login()\nException: 401 Unauthorized'
    assert check(rows) is None


TOKEN_REF = {'memory_id': 'token-source', 'content_revision': 'r-token', 'phase': 'parameters'}
LOGIN_REF = {'memory_id': 'login-source', 'content_revision': 'r-login', 'phase': 'login'}


def phase_artifact():
    return {'level': 'L3', 'agent': {'appworld_options': {'l1_auth_parameter_experiences':
            [dict(TOKEN_REF), dict(LOGIN_REF)]}}, 'policy': {
        'options': {'approved_experiences': [{k: r[k] for k in ('memory_id','content_revision')}
                                            for r in (TOKEN_REF, LOGIN_REF)]},
        'snapshot': {'experiences': [
            {'memory_id': TOKEN_REF['memory_id'], 'content_revision': TOKEN_REF['content_revision'],
             'content': 'Pass access_token to subsequent API calls.'},
            {'memory_id': LOGIN_REF['memory_id'], 'content_revision': LOGIN_REF['content_revision'],
             'content': 'Read supervisor show_account_passwords; call the app login; retain access_token.'}]}}}


def successful_login(app='phone'):
    return [{'role':'assistant','content':f'app_token = apis.{app}.login(username=identifier, password=password)["access_token"]'},
            {'role':'user','content':'Execution successful.'}]


def test_phased_sources_bind_approved_text_and_reject_forged_roles():
    import copy
    from appworld_skill import appworld_auth_parameter_experiences
    a=phase_artifact()
    assert appworld_auth_parameter_experiences(a)==[TOKEN_REF,LOGIN_REF]
    invalid=copy.deepcopy(a);invalid['agent']['appworld_options']['l1_auth_parameter_experiences'][0]['phase']='login'
    with pytest.raises(ValueError,match='phase_source_invalid'):appworld_auth_parameter_experiences(invalid)
    invalid=copy.deepcopy(a);invalid['agent']['appworld_options']['l1_auth_parameter_experiences'][0]['content_revision']='wrong'
    with pytest.raises(ValueError,match='source_invalid'):appworld_auth_parameter_experiences(invalid)
    invalid=copy.deepcopy(a);invalid['policy']['snapshot']['experiences'][1]['content']='Call apis.spotify.login and reuse access_token.'
    with pytest.raises(ValueError,match='phase_source_invalid'):appworld_auth_parameter_experiences(invalid)
    invalid=copy.deepcopy(a);invalid['policy']['snapshot']['experiences'][1]['content']='先通过apis.supervisor.show_account_passwords()获取amazon账号密码，再调用apis.amazon.login获取access_token。'
    with pytest.raises(ValueError,match='phase_source_invalid'):appworld_auth_parameter_experiences(invalid)


@pytest.mark.parametrize('prior,expected',[
    ([],LOGIN_REF), (successful_login(),TOKEN_REF), (successful_login('gmail'),LOGIN_REF),
    ([{'role':'assistant','content':'if False:\n    token=apis.phone.login()["access_token"]'},
      {'role':'user','content':'Execution successful.'}],LOGIN_REF),
    (successful_login()+[{'role':'assistant','content':'apis.phone.logout(access_token=app_token)'},
                        {'role':'user','content':'Execution successful.'}],LOGIN_REF),
    (successful_login()+[{'role':'assistant','content':'apis.phone.show_alarms(access_token=app_token)'},
                        {'role':'user','content':'Execution failed\nTraceback:\napis.phone.show_alarms(access_token=app_token)\n401 Unauthorized'}],LOGIN_REF),
])
def test_phased_same_app_prior_login_and_logout(prior,expected):
    from appworld_skill import auth_parameter_phase_experiences
    rows=prior+history('apis.phone.show_alarms(token=app_token)')
    causal=check(rows)
    assert causal is not None
    actual=auth_parameter_phase_experiences([TOKEN_REF,LOGIN_REF],rows,causal)
    assert actual==[{k:expected[k] for k in ('memory_id','content_revision')}]
    assert auth_parameter_phase_experiences([{k:TOKEN_REF[k] for k in ('memory_id','content_revision')}],rows,causal)==[
        {k:TOKEN_REF[k] for k in ('memory_id','content_revision')}]


def test_phased_login_recovery_is_only_real_unique_contract_bound_401():
    from appworld_skill import auth_login_failure_state
    contracts=[['phone','show_alarms']]
    assert auth_login_failure_state('',history('apis.phone.show_alarms(access_token=app_token)'),'',lambda x:x,public_contracts=contracts)
    assert auth_login_failure_state('',history('apis.phone.login(username=identifier,password=password)'),'',lambda x:x,public_contracts=contracts)
    assert auth_login_failure_state('',history(status=422),'',lambda x:x,public_contracts=contracts) is None
    assert auth_login_failure_state('',history(doc_app='gmail'),'',lambda x:x,public_contracts=[]) is None
    rows=history();rows[-1]['content']='Execution successful.'
    assert auth_login_failure_state('',rows,'',lambda x:x,public_contracts=contracts) is None
    rows=history('apis.phone.show_alarms()\napis.phone.show_notes()')
    rows[-1]['content']='Execution failed\nTraceback:\napis.phone.show_alarms()\napis.phone.show_notes()\n401 Unauthorized'
    assert auth_login_failure_state('',rows,'',lambda x:x,public_contracts=contracts+[['phone','show_notes']]) is None


@pytest.mark.parametrize('prior,expected',[(successful_login(),TOKEN_REF),([],LOGIN_REF),(successful_login('gmail'),LOGIN_REF)])
def test_whole_before_model_phase_selects_only_canonical_sources(monkeypatch,prior,expected):
    import copy
    from appworld_skill import NativeFrameworkObserver
    monkeypatch.setenv('APPWORLD_POLICY_LEVEL','L3');monkeypatch.delenv('APPWORLD_RUNTIME_STATE',raising=False)
    state={'native_context':{'run_id':'source-run'},'policy':'Public policy',
           'messages':prior+history('apis.phone.show_alarms(token=app_token)'),
           'public_instruction':'Read alarms.','appworld_auth_parameter_experiences':[TOKEN_REF,LOGIN_REF],
           'appworld_auth_parameter_contracts':[['phone','show_alarms']],
           'appworld_auth_fallback_experiences':[]}
    class Client:
        def __init__(self):self.packets=[]
        def point(self,packet,**kw):
            self.packets.append(copy.deepcopy(packet))
            return {'context':{'run_id':'source-run','activity_id':'source-model'},'input':copy.deepcopy(packet.get('input',{}))}
    client=Client();observer=NativeFrameworkObserver(client,state_reader=lambda:state,project=lambda x:x)
    request=[{'role':'system','content':'Original request.'}]
    observer.before_model(copy.deepcopy(request),{'source':'skill_executor','skill_name':'appworld-office',
                         'exec_skill_key':'office','node_id':'skill_executor','round':1})
    packet=next(p for p in reversed(client.packets) if p.get('kind')=='model')
    assert packet['apply_policy'] is True
    assert packet['extensions']['runtime_experience_allowlist']==[{k:expected[k] for k in ('memory_id','content_revision')}]
    assert packet['input']['messages']==request


def prefix_history(code, failed_line=None, observed_api='phone.show_alarms'):
    rows = history(code)
    if failed_line is None:
        failed_line = next(i for i, line in enumerate(code.splitlines(), 1)
                           if 'apis.phone.show_alarms(' in line)
    rows[-1]['content'] = (f'Execution failed\nTraceback:\nline {failed_line}\n'
                          f'apis.{observed_api}(token=token)\nException: 401 Unauthorized')
    return rows


def prefix_refs():
    return [dict(ref, login_evidence='completed_prefix') for ref in (TOKEN_REF, LOGIN_REF)]


@pytest.mark.parametrize('code', [
    'auth=apis.phone.login(username=identifier,password=password)\ntoken=auth["access_token"]\nresult=apis.phone.show_alarms(token=token)',
    'token=apis.phone.login(username=identifier,password=password)["access_token"]\nresult=apis.phone.show_alarms(token=token)',
    'for item in accounts:\n    identifier=item\nauth=apis.phone.login(username=identifier,password=password)\ntoken=auth["access_token"]\nprint(token)\nresult=apis.phone.show_alarms(token=token)',
])
def test_completed_prefix_only_new_mode_proves_key_presence(code):
    from appworld_skill import auth_completed_login_prefix, auth_parameter_phase_experiences
    rows=prefix_history(code);causal=check(rows)
    assert causal is not None
    proof=auth_completed_login_prefix(rows,causal)
    assert proof['app']=='phone' and proof['method']=='show_alarms'
    assert proof['login_line']<=proof['token_line']<proof['failure_line']
    assert auth_parameter_phase_experiences([TOKEN_REF,LOGIN_REF],rows,dict(causal))==[
        {k:LOGIN_REF[k] for k in ('memory_id','content_revision')}]
    assert auth_parameter_phase_experiences(prefix_refs(),rows,causal)==[
        {k:TOKEN_REF[k] for k in ('memory_id','content_revision')}]
    assert 'Only key presence' in causal['completed_authentication_state']
    assert 'password' not in causal['completed_authentication_state']


@pytest.mark.parametrize('code', [
    'auth=apis.gmail.login(username=identifier,password=password)\ntoken=auth["access_token"]\nresult=apis.phone.show_alarms(token=token)',
    'auth=apis.phone.login(username=identifier,password=password)\nresult=apis.phone.show_alarms(token=token)\ntoken=auth["access_token"]',
    'if True:\n    auth=apis.phone.login(username=identifier,password=password)\n    token=auth["access_token"]\nresult=apis.phone.show_alarms(token=token)',
    'try:\n    auth=apis.phone.login(username=identifier,password=password)\n    token=auth["access_token"]\nexcept Exception:\n    pass\nresult=apis.phone.show_alarms(token=token)',
    'auth=apis.phone.login(username=identifier,password=password)\ntoken=auth["access_token"]\napis.phone.logout(access_token=token)\nresult=apis.phone.show_alarms(token=token)',
    'auth=apis.phone.login(username=identifier,password=password)\ntoken=auth["access_token"]\nhelper()\nresult=apis.phone.show_alarms(token=token)',
    'print=helper\nauth=apis.phone.login(username=identifier,password=password)\ntoken=auth["access_token"]\nprint(token)\nresult=apis.phone.show_alarms(token=token)',
    'auth=apis.phone.login(username=identifier,password=password)\nauth["access_token"]="invented"\ntoken=auth["access_token"]\nresult=apis.phone.show_alarms(token=token)',
    'auth=apis.phone.login(username=identifier,password=password)\nauth=other_response\ntoken=auth["access_token"]\nresult=apis.phone.show_alarms(token=token)',
    'auth=apis.phone.login(username=identifier,password=password)\ntoken=auth["access_token"]\nif True:\n    result=apis.phone.show_alarms(token=token)',
    'auth=apis.phone.login(username=identifier,password=password)\ntoken=auth["access_token"]\napis.phone.show_alarms(token=token)\nresult=apis.phone.show_alarms(token=token)',
])
def test_completed_prefix_rejects_unproved_or_ambiguous_execution(code):
    from appworld_skill import auth_completed_login_prefix
    rows=prefix_history(code);causal=check(rows)
    assert causal is not None
    assert auth_completed_login_prefix(rows,causal) is None


def test_completed_prefix_requires_exact_terminal_failure_and_optin_schema():
    import copy
    from appworld_skill import appworld_auth_parameter_experiences, auth_completed_login_prefix
    code='auth=apis.phone.login(username=identifier,password=password)\ntoken=auth["access_token"]\nresult=apis.phone.show_alarms(token=token)'
    rows=prefix_history(code);causal=check(rows)
    for text in ['Execution successful.', rows[-1]['content'].replace('401','422'),
                 rows[-1]['content'].replace('line 3','line 2'),
                 rows[-1]['content']+'\napis.phone.login()',
                 'Execution failed\n401 Unauthorized']:
        altered=copy.deepcopy(rows);altered[-1]['content']=text
        assert auth_completed_login_prefix(altered,causal) is None
    assert auth_completed_login_prefix(rows,dict(causal,causal_auth_login_failure='phone.login returned 401;')) is None
    artifact=phase_artifact();artifact['agent']['appworld_options']['l1_auth_parameter_experiences']=prefix_refs()
    assert appworld_auth_parameter_experiences(artifact)==prefix_refs()
    for invalid in [dict(prefix_refs()[0],login_evidence='unverified'), dict(prefix_refs()[0],other='unsafe')]:
        artifact['agent']['appworld_options']['l1_auth_parameter_experiences']=[invalid,prefix_refs()[1]]
        with pytest.raises(ValueError,match='experiences_invalid'):appworld_auth_parameter_experiences(artifact)
    artifact['agent']['appworld_options']['l1_auth_parameter_experiences']=[prefix_refs()[0],LOGIN_REF]
    with pytest.raises(ValueError,match='experiences_invalid'):appworld_auth_parameter_experiences(artifact)


def schema_artifact():
    artifact=phase_artifact()
    schema={'memory_id':'schema-source','content_revision':'r-schema','phase':'parameters',
            'login_evidence':'completed_prefix','parameter_evidence':'source_schema'}
    artifact['agent']['appworld_options']['l1_auth_parameter_experiences']=prefix_refs()+[schema]
    artifact['policy']['options']['approved_experiences'].append(
        {key:schema[key] for key in ('memory_id','content_revision')})
    artifact['policy']['snapshot']['experiences'].append({
        'memory_id':schema['memory_id'],'content_revision':schema['content_revision'],
        'content':'Use apis.simple_note.search_notes or apis.phone.search_text_messages with query and page_limit.'})
    return artifact


def schema_history(call='apis.simple_note.search_notes(q=keyword,page_limit=20)',app='simple_note'):
    code=f'auth=apis.{app}.login(username=identifier,password=password)\ntoken=auth["access_token"]\nresult={call}'
    rows=prefix_history(code,failed_line=3,observed_api=call.split('(',1)[0].removeprefix('apis.'))
    rows[0]['content']='print(apis.api_docs.show_api_doc(app_name="simple_note",api_name="search_notes"))'
    rows[1]['content']=json.dumps({'app_name':'simple_note','api_name':'search_notes','parameters':[
        {'name':'access_token','required':True},{'name':'query','required':False}]})
    return rows


def test_source_schema_ref_uses_exact_approved_body_and_public_optional_parameter():
    import copy
    from appworld_skill import appworld_auth_parameter_experiences, auth_parameter_source_contracts
    artifact=schema_artifact();refs=appworld_auth_parameter_experiences(artifact)
    contracts=refs[-1]['schema_contracts']
    assert {(row['app'],row['method']) for row in contracts}=={
        ('simple_note','search_notes'),('phone','search_text_messages')}
    assert all(row['source_arguments']==['page_limit','query'] for row in contracts)
    assert all('q' not in row['supported_arguments'] for row in contracts)
    assert auth_parameter_source_contracts({'content':'Use query without a named API.'}) is None
    for mutate in ['unapproved','body','mode']:
        invalid=copy.deepcopy(artifact)
        if mutate=='unapproved':invalid['policy']['options']['approved_experiences'].pop()
        elif mutate=='body':invalid['policy']['snapshot']['experiences'][-1]['content']='Use unknown_api query.'
        else:invalid['agent']['appworld_options']['l1_auth_parameter_experiences'][-1]['parameter_evidence']='guessed'
        with pytest.raises(ValueError):appworld_auth_parameter_experiences(invalid)


def test_source_schema_qualification_is_complementary_and_keeps_existing_budget():
    from appworld_skill import appworld_auth_parameter_experiences, auth_parameter_phase_experiences, auth_parameter_failure_state
    refs=appworld_auth_parameter_experiences(schema_artifact())
    rows=schema_history()
    causal=auth_parameter_failure_state('',rows,'Search notes.',lambda x:x,
                                       public_contracts=[['simple_note','search_notes']])
    assert causal is not None
    selected=auth_parameter_phase_experiences(refs,rows,causal)
    assert selected==[{'memory_id':'schema-source','content_revision':'r-schema'}]
    mismatch=json.loads(causal['source_parameter_schema_mismatch'])[0]
    assert mismatch['unknown_arguments']==['q'] and mismatch['omitted_source_arguments']==['query']
    assert 'completed_authentication_state' in causal
    repaired=schema_history('apis.simple_note.search_notes(query=keyword,page_limit=20)')
    causal=auth_parameter_failure_state('',repaired,'Search notes.',lambda x:x,
                                       public_contracts=[['simple_note','search_notes']])
    assert auth_parameter_phase_experiences(refs,repaired,causal)==[
        {key:TOKEN_REF[key] for key in ('memory_id','content_revision')}]


@pytest.mark.parametrize('call,app',[
    ('apis.simple_note.search_notes(page_limit=20)','simple_note'),
    ('apis.simple_note.search_notes(token=token,page_limit=20)','simple_note'),
    ('apis.simple_note.search_notes(query=keyword,page_limit=20)','simple_note'),
    ('apis.gmail.search_notes(q=keyword,page_limit=20)','gmail'),
])
def test_source_schema_does_not_displace_token_source_for_ordinary_or_other_app(call,app):
    from appworld_skill import appworld_auth_parameter_experiences, auth_parameter_phase_experiences, auth_parameter_failure_state
    refs=appworld_auth_parameter_experiences(schema_artifact());rows=schema_history(call,app)
    method=call.split('(',1)[0].rsplit('.',1)[1]
    causal=auth_parameter_failure_state('',rows,'Search.',lambda x:x,public_contracts=[[app,method]])
    assert causal is not None
    selected=auth_parameter_phase_experiences(refs,rows,causal)
    assert selected==[{key:TOKEN_REF[key] for key in ('memory_id','content_revision')}]


def _reject_new_auth_optins(monkeypatch):
    import appworld_skill as skill
    def unexpected(*args, **kwargs):
        raise AssertionError('Legacy auth references entered a new opt-in path.')
    for name in ('auth_recovery_source_phase', 'auth_parameter_source_contracts',
                 'auth_completed_login_prefix', 'auth_parameter_schema_mismatch',
                 'auth_login_failure_state'):
        monkeypatch.setattr(skill, name, unexpected)


def test_legacy_two_field_refs_require_approval_without_new_source_or_schema_reads(monkeypatch):
    import copy
    import appworld_skill as skill
    _reject_new_auth_optins(monkeypatch)
    artifact = phase_artifact()
    legacy = {key: TOKEN_REF[key] for key in ('memory_id', 'content_revision')}
    artifact['agent']['appworld_options']['l1_auth_parameter_experiences'] = [legacy]
    artifact['policy']['snapshot']['experiences'] = []
    assert skill.appworld_auth_parameter_experiences(artifact) == [legacy]
    assert skill.appworld_auth_parameter_experiences(artifact)[0] is not legacy
    unapproved = copy.deepcopy(artifact)
    unapproved['policy']['options']['approved_experiences'] = []
    with pytest.raises(ValueError, match='source_invalid'):
        skill.appworld_auth_parameter_experiences(unapproved)


@pytest.mark.parametrize('rows', [
    history('apis.phone.show_alarms(token=app_token)'),
    prefix_history('auth=apis.phone.login(username=identifier,password=password)\n'
                   'token=auth["access_token"]\nresult=apis.phone.show_alarms(token=token)'),
])
def test_legacy_refs_ignore_new_completed_prefix_and_schema_phase(monkeypatch, rows):
    import copy
    import appworld_skill as skill
    _reject_new_auth_optins(monkeypatch)
    legacy = [{key: TOKEN_REF[key] for key in ('memory_id', 'content_revision')}]
    causal = check(rows)
    assert causal is not None
    original = copy.deepcopy(causal)
    assert skill.auth_parameter_phase_experiences(legacy, rows, causal) == legacy
    assert causal == original
    assert 'completed_authentication_state' not in causal
    assert 'source_parameter_schema_mismatch' not in causal


@pytest.mark.parametrize('rows,qualified', [
    (history(), True),
    (history('apis.phone.show_alarms(token=app_token)'), True),
    (prefix_history('auth=apis.phone.login(username=identifier,password=password)\n'
                    'token=auth["access_token"]\nresult=apis.phone.show_alarms(token=token)'), True),
    (history('apis.phone.login(username=identifier,password=password)'), False),
    (history(status=422), False),
    (history('apis.phone.show_alarms(access_token=app_token)'), False),
])
def test_whole_before_model_legacy_refs_preserve_only_original_parameter_qualification(
        monkeypatch, rows, qualified):
    import copy
    import appworld_skill as skill
    _reject_new_auth_optins(monkeypatch)
    monkeypatch.setenv('APPWORLD_POLICY_LEVEL', 'L3')
    monkeypatch.delenv('APPWORLD_RUNTIME_STATE', raising=False)
    legacy = [{key: TOKEN_REF[key] for key in ('memory_id', 'content_revision')}]
    state = {'native_context': {'run_id': 'legacy-run'}, 'policy': 'Public policy',
             'messages': rows, 'public_instruction': 'Read alarms.',
             'appworld_auth_parameter_experiences': legacy,
             'appworld_auth_parameter_contracts': [['phone', 'show_alarms']],
             'appworld_auth_fallback_experiences': []}
    class Client:
        def __init__(self):
            self.packets = []
        def point(self, packet, **kw):
            self.packets.append(copy.deepcopy(packet))
            return {'context': {'run_id': 'legacy-run', 'activity_id': 'legacy-model'},
                    'input': copy.deepcopy(packet.get('input', {}))}
    client = Client()
    observer = skill.NativeFrameworkObserver(client, state_reader=lambda: state, project=lambda x: x)
    request = [{'role': 'system', 'content': 'Original request.'}]
    observer.before_model(copy.deepcopy(request), {'source': 'skill_executor',
        'skill_name': 'appworld-office', 'exec_skill_key': 'office',
        'node_id': 'skill_executor', 'round': 1})
    packet = next(p for p in reversed(client.packets) if p.get('kind') == 'model')
    assert packet['apply_policy'] is True
    assert packet['extensions']['runtime_experience_allowlist'] == (legacy if qualified else [])
    assert packet['input']['messages'] == request
    assert 'completed_authentication_state' not in packet['state']
    assert 'source_parameter_schema_mismatch' not in packet['state']
    assert 'causal_auth_login_failure' not in packet['state']
    assert state['appworld_auth_parameter_experiences'] == legacy
