"""Changed approved cards reach real office requests before the next action."""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[4]
ADAPTER = ROOT / 'examples/agent/business-simulator/appworld'
sys.path.insert(0, str(ROOT / 'packages/agentflow-runtime/src'))
sys.path.insert(0, str(ADAPTER))
from appworld_skill import (NativeFrameworkObserver, appworld_prevention_sources,
                           office_prevention_override_state, prepare_skill, set_request)

META = {'source': 'skill_executor', 'skill_name': 'appworld-office',
        'exec_skill_key': 'office', 'node_id': 'skill_executor', 'round': 1, 'task_id': 'fixture'}
REF = {'memory_id': 'approved-changed-source', 'content_revision': 'exact-source-revision'}
SCOPE = 'agent:fixture/actor:default'


def artifact():
    return {'level': 'L3', 'agent': {}, 'policy': {'options': {
        'approved_experiences': [REF], 'l1_prevention_sources': [REF['memory_id']],
        'experience_overrides': [{'source_experience': REF,
            'trigger_pattern': 'playlist_id song_id',
            'content': 'Use add_song_to_playlist and remove_song_from_playlist, then verify.'}]},
        'snapshot': {'options': {'target_scopes': [SCOPE]}, 'experiences': [{**REF,
            'target_scope': SCOPE, 'trigger_pattern': 'old playlist condition',
            'content': 'Original approved card.'}]}}}


def history(code='print(apis.gmail.show_email(email_id=3, access_token=token))',
            observation='{"playlist_id": 17, "song_id": 23}'):
    return [{'role': 'assistant', 'content': code}, {'role': 'user', 'content': observation}]


def qualify(value=None, instruction='Update my Spotify playlist from the recommendation message.', rows=None):
    return office_prevention_override_state('', history() if rows is None else rows, instruction,
                                            lambda x: x,
                                            sources=appworld_prevention_sources(value or artifact()))


def test_profile_is_private_exact_approved_changed_and_l0_is_ignored(tmp_path, monkeypatch):
    value = artifact()
    profile = appworld_prevention_sources(value)
    assert profile[0]['ref'] == REF
    assert profile[0]['anchors'] == ['playlist_id', 'song_id']
    assert qualify(value)[1] == [REF]
    config = tmp_path / 'trial.json'
    config.write_text('{}')
    (tmp_path / 'version.json').write_text(json.dumps({'artifact': value}))
    monkeypatch.setenv('TRAJECTORY_EVOLUTION_TRIAL_CONFIG', str(config))
    meta = prepare_skill(tmp_path / 'private', tmp_path / 'skill')
    set_request(meta, 'Original public task.', [], history(), 1)
    state = json.loads(Path(meta['state_path']).read_text())
    assert state['appworld_prevention_sources'] == profile
    assert REF['memory_id'] not in Path(meta['skill_doc_path']).read_text()
    assert value['policy']['snapshot']['experiences'][0]['content'] not in Path(meta['skill_doc_path']).read_text()
    state['appworld_l1_count'] = 3
    Path(meta['state_path']).write_text(json.dumps(state))
    set_request(meta, 'Original public task.', [], history(), 2)
    assert json.loads(Path(meta['state_path']).read_text())['appworld_l1_count'] == 3
    value['level'] = 'L0'
    (tmp_path / 'version.json').write_text(json.dumps({'artifact': value}))
    set_request(meta, 'Original public task.', [], history(), 2)
    assert 'appworld_prevention_sources' not in json.loads(Path(meta['state_path']).read_text())


def test_original_published_parent_has_no_prevention_profile_or_new_private_state(tmp_path, monkeypatch):
    published = ROOT / 'trace_output/appworld/published/strategy/strategy.json'
    if not published.is_file(): pytest.skip('published parent unavailable')
    value = json.loads(published.read_text())
    assert appworld_prevention_sources(value) is None
    config = tmp_path / 'trial.json'
    config.write_text('{}')
    (tmp_path / 'version.json').write_text(json.dumps({'artifact': value}))
    monkeypatch.setenv('TRAJECTORY_EVOLUTION_TRIAL_CONFIG', str(config))
    meta = prepare_skill(tmp_path / 'private', tmp_path / 'skill')
    set_request(meta, 'Original public task.', [], history(), 1,
                l1_window_mode='observed_phase_v3', native_run_id='original-parent-run')
    state = json.loads(Path(meta['state_path']).read_text())
    assert not any(key.startswith('appworld_prevention') for key in state)
    assert state['appworld_l1_count'] == 0 and state['messages'] == history()


@pytest.mark.parametrize('change', ['unapproved', 'revision', 'scope', 'disabled', 'unchanged',
                                    'unchanged_default_trigger', 'duplicate', 'too_many'])
def test_invalid_prevention_sources_fail_closed(change):
    value = artifact()
    options = value['policy']['options']
    if change == 'unapproved': options['approved_experiences'] = []
    elif change == 'revision': options['experience_overrides'][0]['source_experience'] = dict(REF, content_revision='wrong')
    elif change == 'scope': value['policy']['snapshot']['experiences'][0]['target_scope'] = 'other-scope'
    elif change == 'disabled': options['disabled_injections'] = [REF]
    elif change in ('unchanged', 'unchanged_default_trigger'):
        source = value['policy']['snapshot']['experiences'][0]
        options['experience_overrides'][0].update(content=source['content'],
            trigger_pattern='' if change == 'unchanged_default_trigger' else source['trigger_pattern'])
    elif change == 'duplicate': options['l1_prevention_sources'] *= 2
    else: options['l1_prevention_sources'] = ['source-' + str(i) for i in range(17)]
    with pytest.raises(ValueError, match='appworld_prevention'):
        appworld_prevention_sources(value)


@pytest.mark.parametrize('instruction,rows', [
    ('Count my unread Gmail messages.', history()),
    ('Update my Spotify playlist.', []),
    ('Update my Spotify playlist.', history('print(1)', '{"playlist_id":17,"song_id":23}')),
    ('Update my Spotify playlist.', history(observation='Execution failed: KeyError: playlist_id')),
    ('Update my Spotify playlist.', history(observation='<system-reminder>playlist_id song_id</system-reminder>')),
    ('Update my Spotify playlist.', history(observation='{"email_id":17}')),
    ('Update my Spotify playlist.', history('if False:\n print(apis.gmail.show_email(email_id=3))')),
    ('Update my Spotify playlist.', history(
        'apis.gmail.show_email(email_id=3)\nif False:\n apis.spotify.show_playlist(playlist_id=17, song_id=23)',
        'Execution successful.')),
])
def test_unrelated_task_future_prose_failed_or_unmatched_observation_cannot_open_window(instruction, rows):
    assert qualify(instruction=instruction, rows=rows) is None


def test_dedicated_auth_and_saved_result_sources_keep_their_existing_causal_guards():
    value = artifact()
    for key in ('l1_auth_parameter_experiences', 'l1_shape_error_auth_experiences', 'l1_saved_result_experiences'):
        value['agent']['appworld_options'] = {key: [REF]}
        assert appworld_prevention_sources(value) is None


NODE1_TRIGGER = '调用 login 或认证类API时，先查询API文档确认参数要求，并核对凭据来源与标识符格式后再调用。'
NODE1_CONTENT = ('在调用认证类API（如 phone.login、venmo.login 等需要用户名/密码的接口）前，'
    '先通过 apis.api_docs.show_api_doc() 查询该API文档，确认其期望的标识符格式及凭据来源；'
    '若凭据来自 apis.supervisor.show_account_passwords()，核对返回的 account_name 与密码字段后再调用。')
LOGIN_DOC = 'print(apis.api_docs.show_api_doc(app_name="phone", api_name="login"))'
AUTH_TASK = 'Like yesterday\'s Venmo transactions involving my siblings on my social feed.'


def login_artifact(trigger=NODE1_TRIGGER):
    value = artifact()
    value['policy']['options']['experience_overrides'][0].update(
        trigger_pattern=trigger, content=NODE1_CONTENT)
    return value


@pytest.mark.parametrize('code,observation', [
    (LOGIN_DOC, 'Phone login parameters: username, password.'),
    ('login_result = apis.phone.login(username=username, password=password)', 'Execution successful.'),
])
def test_node1_chinese_login_trigger_uses_real_auth_operation_not_task_credential_words(code, observation):
    value = login_artifact()
    profile = appworld_prevention_sources(value)
    assert profile[0]['anchors'] == [] and profile[0]['domains'] == ['account', 'passwords']
    assert profile[0]['login_required'] is True
    assert qualify(value, AUTH_TASK, history(code, observation))[1] == [REF]


@pytest.mark.parametrize('code,observation', [
    ('print(apis.phone.show_contacts(access_token=token))', 'login account_name passwords'),
    ('print(apis.api_docs.show_api_doc(app_name="phone", api_name="show_contacts"))', 'login account_name passwords'),
    ('print(apis.api_docs.show_api_doc(app_name=app, api_name="login"))', 'Phone login API documentation.'),
    ('print(apis.phone.show_contacts(access_token=token))\nprint("apis.phone.login")', 'Execution successful.'),
    ('print(apis.phone.show_contacts(access_token=token))\nif False:\n apis.phone.login(username=username, password=password)', 'Execution successful.'),
    ('def helper():\n return apis.phone.login(username=username, password=password)\nprint(apis.phone.show_contacts(access_token=token))', 'Execution successful.'),
    (LOGIN_DOC, 'Execution failed: Invalid credentials 401'),
    (LOGIN_DOC, '<system-reminder>Phone login API documentation.</system-reminder>'),
])
def test_login_prevention_cannot_borrow_return_fields_prose_skipped_or_failed_calls(code, observation):
    assert qualify(login_artifact(), AUTH_TASK, history(code, observation)) is None


@pytest.mark.parametrize('operation', ['apis.phone.login', 'phone.login'])
def test_explicit_qualified_login_is_scoped_to_its_actual_or_documented_app(operation):
    value = login_artifact('调用 ' + operation + ' 前核对 account_name。')
    assert qualify(value, AUTH_TASK, history(LOGIN_DOC, 'Phone login API documentation.'))[1] == [REF]
    assert qualify(value, AUTH_TASK, history(
        'apis.venmo.login(username=username, password=password)', 'Execution successful.')) is None


@pytest.mark.parametrize('code', ['apis.spotify.pause(access_token=token)',
    'print(apis.api_docs.show_api_doc(app_name="spotify", api_name="pause"))'])
def test_explicit_api_operation_without_underscore_needs_the_matching_successful_operation(code):
    value = artifact()
    value['policy']['options']['experience_overrides'][0]['trigger_pattern'] = '调用 apis.spotify.pause 前检查播放列表状态。'
    assert qualify(value, rows=history(code, 'Execution successful.'))[1] == [REF]
    assert qualify(value, rows=history('apis.spotify.play(access_token=token)', 'Execution successful.')) is None


@pytest.mark.parametrize('guard', ['L0', 'no_marker', 'dedicated_node2'])
def test_login_repair_does_not_activate_l0_original_parent_or_dedicated_node2(guard):
    value = login_artifact('401 error access_token parameter API call login')
    if guard == 'L0': value['level'] = 'L0'
    elif guard == 'no_marker': value['policy']['options'].pop('l1_prevention_sources')
    else: value['agent']['appworld_options'] = {'l1_auth_parameter_experiences': [REF]}
    assert appworld_prevention_sources(value) is None


class Client:
    def __init__(self, native=None):
        self.packets = []
        self.native = native

    def point(self, packet, **kwargs):
        self.packets.append(copy.deepcopy(packet))
        response = {'context': {'run_id': 'fixture-run', 'activity_id': 'fixture-model'},
                    'input': copy.deepcopy(packet.get('input', {}))}
        if self.native and packet.get('kind') == 'model' and packet.get('apply_policy'):
            context = self.native(packet)
            if context:
                response['input']['messages'].insert(0, {'role': 'system', 'content': context})
        return response


@pytest.mark.parametrize('level,role,count,marked', [('L0', 'skill', 0, True),
    ('L3', 'coordinator', 0, True), ('L3', 'skill', 3, True), ('L3', 'skill', 0, False)])
def test_original_parent_l0_other_actor_and_exhausted_budget_have_no_new_injection(monkeypatch, level, role, count, marked):
    monkeypatch.setenv('APPWORLD_POLICY_LEVEL', level)
    monkeypatch.delenv('APPWORLD_RUNTIME_STATE', raising=False)
    value = artifact()
    if not marked: value['policy']['options'].pop('l1_prevention_sources')
    state = {'native_context': {'run_id': 'fixture-run'}, 'messages': history(),
             'public_instruction': 'Update my Spotify playlist.', 'appworld_l1_count': count,
             'native_advice_cache': [{'scope': {'run_id': 'fixture-run', 'skill': 'appworld-office',
                 'task_id': 'fixture', 'actor': 'skill_executor'}, 'content': 'Previously verified card.'}]}
    profile = appworld_prevention_sources(value)
    if profile is not None: state['appworld_prevention_sources'] = profile
    client = Client(lambda packet: 'must not be called')
    meta = dict(META)
    if role == 'coordinator': meta.update(source='coordinator', node_id='coordinator_decide', skill_name='')
    messages = [{'role': 'system', 'content': 'Original public request.'}]
    before = copy.deepcopy(messages)
    observer = NativeFrameworkObserver(client, state_reader=lambda: state, project=lambda x: x)
    cache = copy.deepcopy(observer.native_advice_cache)
    observer.before_model(messages, meta)
    assert client.packets[-1]['apply_policy'] is False
    assert messages == before and observer.native_advice_cache == cache


def test_no_match_never_replays_an_unrelated_cached_card_in_new_prevention_window(monkeypatch):
    monkeypatch.setenv('APPWORLD_POLICY_LEVEL', 'L3')
    state = {'native_context': {'run_id': 'fixture-run'}, 'messages': history(),
        'public_instruction': 'Update my Spotify playlist.', 'appworld_prevention_sources': appworld_prevention_sources(artifact())}
    messages = [{'role': 'system', 'content': 'Original public request.'}]
    observer = NativeFrameworkObserver(Client(), state_reader=lambda: state, project=lambda x: x)
    observer.native_advice_cache = [{'scope': observer._cache_scope(state, META), 'content': 'Unrelated previous card.'}]
    observer.before_model(messages, dict(META))
    assert messages == [{'role': 'system', 'content': 'Original public request.'}]


@pytest.mark.parametrize('kind', ['keywords', 'playlist', 'login', 'login_chinese'])
def test_actual_approved_sources_change_real_messages_or_preserve_native_rejection_under_budget(monkeypatch, kind):
    """Use authority source revisions and native compile/query, with zero API calls."""
    binary = Path(os.environ.get('APPWORLD_PREVENTION_TEST_BINARY', ROOT / 'tools/trajectory_evolution/build-v4/trajectory_evolution'))
    published = ROOT / 'trace_output/appworld/published/strategy/strategy.json'
    if not binary.is_file() or not published.is_file(): pytest.skip('published native source fixture unavailable')
    value = json.loads(published.read_text())
    original = copy.deepcopy(value)
    source = next(item for item in value['policy']['snapshot']['experiences']
                  if ({'keywords': '1f8ff550', 'playlist': '40d243550',
                       'login': '0c16299', 'login_chinese': '0c16299'}[kind]) in item['memory_id'])
    ref = {key: source[key] for key in ('memory_id', 'content_revision')}
    override = {'source_experience': ref,
        'trigger_pattern': ('调用apis.api_docs.show_api_descriptions等AppWorld API时使用位置参数报错appworld_type_error_positional；或执行批量状态变更操作前需要确认目标集合精确性。'
                            if kind == 'keywords' else '建议消息中已包含playlist_id和song_id，需要增删播放列表歌曲'),
        'content': ('AppWorld的apis.*方法要求以关键字参数调用。执行状态变更操作前，需确认目标对象集合与任务要求精确匹配。'
                    if kind == 'keywords' else '调用add_song_to_playlist和remove_song_from_playlist，然后调用show_playlist验证结果；create_playlist使用title参数。')}
    if kind.startswith('login'):
        override.update(trigger_pattern=NODE1_TRIGGER if kind == 'login_chinese' else 'apis.api_docs.show_api_doc login',
                        content=NODE1_CONTENT)
    options = value['policy']['options']
    options['experience_overrides'] = [row for row in options['experience_overrides']
                                     if row['source_experience']['memory_id'] != source['memory_id']] + [override]
    options['l1_prevention_sources'] = [source['memory_id']]

    def native(operation, packet):
        process = subprocess.run([str(binary), 'runtime', operation], input=json.dumps(packet, ensure_ascii=False),
                                 capture_output=True, text=True, check=True)
        return json.loads(process.stdout)

    value['policy'] = native('recompile', {'policy': original['policy'], 'options': options})
    instruction = ('Archive matching Gmail threads while preserving other threads.' if kind == 'keywords'
                   else 'Update my Spotify playlist from the recommendation message.')
    rows = (history('print(apis.api_docs.show_api_descriptions(app_name="gmail"))', 'Gmail API descriptions.')
            if kind == 'keywords' else history(observation=
                '{"body":"For playlist_id 17, add song_id 23 to that playlist."}'))
    if kind.startswith('login'):
        instruction = AUTH_TASK
        rows = history(LOGIN_DOC, 'Phone login parameters: username, password.')
    state = {'native_context': {'run_id': 'fixture-run'}, 'messages': rows, 'public_instruction': instruction,
             'appworld_prevention_sources': appworld_prevention_sources(value)}
    monkeypatch.setenv('APPWORLD_POLICY_LEVEL', 'L3')
    monkeypatch.delenv('APPWORLD_RUNTIME_STATE', raising=False)
    policy = value['policy']

    def query(packet):
        envelope = {'retrieval_state': json.dumps(packet['state'], ensure_ascii=False),
                    'runtime_experience_allowlist': packet['extensions']['runtime_experience_allowlist']}
        result = native('query', {'policy': policy, 'query': {'advice_query': {
            'identity': policy['snapshot']['identity'], 'target_scope': source['target_scope'],
            'query_id': 'prevention-query', 'decision_id': 'prevention-decision',
            'task': json.dumps({'instruction': instruction}), 'state': json.dumps(envelope)},
            'execution_id': 'prevention-model', 'boundary': 'llm', 'actor_id': 'skill_executor'}})
        if kind == 'login_chinese':
            # The host qualifies actual auth evidence, but cannot translate
            # or bypass a native trigger that lacks machine matching anchors.
            assert result['status'] == 'no_change' and not result.get('experiences')
            return None
        assert result['status'] == 'ready' and result['experiences'] == [ref]
        assert len(result['context'].encode()) <= policy['options']['max_context_bytes']
        return result['context']

    client = Client(query)
    messages = [{'role': 'system', 'content': 'Original public request.'}, {'role': 'user', 'content': instruction}]
    observer = NativeFrameworkObserver(client, state_reader=lambda: state, project=lambda x: x)
    observer.before_model(messages, dict(META))
    if kind == 'login_chinese':
        assert client.packets[-1]['apply_policy'] is True
        assert messages == [{'role': 'system', 'content': 'Original public request.'},
                            {'role': 'user', 'content': instruction}]
    else:
        assert override['content'] in messages[0]['content']
        assert messages[1]['content'] == 'Original public request.' and messages[2]['content'] == instruction
    state['public_instruction'] = 'Count my Phone contacts.'
    # A different task/API cannot acquire this changed source merely from a
    # generic success phase, and no native context is manufactured for it.
    state['messages'] = history('print(apis.phone.show_contacts(access_token=token))', '[]')
    assert office_prevention_override_state('', state['messages'], state['public_instruction'], lambda x: x,
                                           sources=state['appworld_prevention_sources']) is None
