"""Candidate-only prevention windows use real public execution, never model prose."""
from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'packages/agentflow-runtime/src'))
sys.path.insert(0, str(ROOT / 'examples/agent/business-simulator/appworld'))
from appworld_skill import (NativeFrameworkObserver, appworld_l1_window_mode, appworld_observed_phase_experiences,
                            office_observed_phase_state, prepare_skill, set_request)


def pair(code, observation):
    return [{'role': 'assistant', 'content': code}, {'role': 'user', 'content': observation}]


def authenticated(app):
    return pair(f'{app}_token = apis.{app}.login(username=user, password=password)["access_token"]',
                'Authenticated.')


def public_case(kind):
    if kind == 'spotify':
        return 'Count all songs across my Spotify playlists.', authenticated('spotify')
    if kind == 'gmail':
        return 'Delete all read Gmail threads from my inbox except starred ones.', authenticated('gmail')
    if kind == 'downloaded':
        return ('Choose and play a playlist for a fifteen-minute walk without network access.',
                authenticated('spotify') + pair(
                    'print(apis.spotify.show_downloaded_songs(access_token=spotify_token))',
                    '[{"song_id": 1, "duration": 300}]'))
    if kind == 'cart':
        return ('Move all products from my Amazon cart to wish list.', pair(
            'print(apis.amazon.show_cart(access_token=amazon_token))',
            '{"cart_items":[{"product_id":1,"quantity":3}]}'))
    return ('Place an order for all wrench sets in my Amazon wish list.',
            authenticated('amazon') + pair('print(apis.amazon.show_cart(access_token=amazon_token))',
                '{"cart_items":[{"product_id":1,"quantity":3}]}'))


class Client:
    def __init__(self, inject=False):
        self.packets = []
        self.inject = inject

    def point(self, packet, **options):
        self.packets.append(copy.deepcopy(packet))
        context = {'run_id': 'fixture-run', 'activity_id': f'fixture-{len(self.packets)}'}
        result = {'context': context, 'input': copy.deepcopy(packet.get('input', {}))}
        if self.inject and packet.get('kind') == 'model' and packet.get('apply_policy'):
            result['input']['messages'].insert(0, {'role': 'system', 'content':
                f'<trajectory-experience>Approved fixture card {len(self.packets)}.</trajectory-experience>'})
        return result


META = {'source': 'skill_executor', 'skill_name': 'appworld-office',
        'exec_skill_key': 'office', 'node_id': 'skill_executor', 'round': 1, 'task_id': 'fixture'}
MESSAGES = [{'role': 'system', 'content': 'Original complete public document.'},
            {'role': 'user', 'content': 'Execute once.'}]


def test_candidate_observed_refs_require_exact_native_approvals_and_ignore_l0():
    ref = {'memory_id': 'real-native-card', 'content_revision': 'fixed-revision'}
    artifact = {'level': 'L3', 'agent': {'appworld_options': {'l1_window_mode': 'observed_phase_v3',
                'l1_observed_phase_experiences': [ref]}},
                'policy': {'options': {'approved_experiences': [dict(ref)]}}}
    assert appworld_observed_phase_experiences(artifact) == [ref]
    bad = copy.deepcopy(artifact)
    bad['agent']['appworld_options']['l1_observed_phase_experiences'][0]['content_revision'] = 'wrong'
    with pytest.raises(ValueError, match='source_invalid'):
        appworld_observed_phase_experiences(bad)
    bad['level'] = 'L0'
    assert appworld_observed_phase_experiences(bad) is None


@pytest.mark.parametrize('reason', ['success', 'error', 'replan'])
def test_observed_allowlist_affects_only_success_window(monkeypatch, reason):
    from appworld_skill import office_retrieval_state
    monkeypatch.setenv('APPWORLD_POLICY_LEVEL', 'L3')
    monkeypatch.delenv('APPWORLD_RUNTIME_STATE', raising=False)
    instruction, history = public_case('downloaded')
    refs = [{'memory_id': 'new-native-card', 'content_revision': 'fixed-revision'}]
    outer = {'run_id': 'fixture-run'}
    state = {'native_context': outer, 'messages': history, 'policy': '', 'public_instruction': instruction,
             'appworld_l1_window_mode': 'observed_phase_v3', 'appworld_observed_phase_experiences': refs}
    if reason == 'error':
        history[-1]['content'] = 'Execution failed: KeyError: song_id'
    elif reason == 'replan':
        outer.update(loop_id='main', control_id='fixture-control')
        state['native_replan_context'] = 'Inspect the real result.'
    client = Client()
    observer = NativeFrameworkObserver(client, state_reader=lambda: state, project=lambda value: value)
    observer.before_model(copy.deepcopy(MESSAGES), dict(META))
    packet = client.packets[-1]
    assert packet['apply_policy'] is True
    if reason == 'success':
        assert packet['extensions']['runtime_experience_allowlist'] == refs
    else:
        assert 'runtime_experience_allowlist' not in packet['extensions']
        assert packet['state'] == office_retrieval_state('', history, instruction, lambda value: value)


@pytest.mark.parametrize('kind', ['spotify', 'gmail', 'downloaded', 'cart', 'purchase'])
def test_success_window_requires_existing_task_and_phase(kind):
    instruction, history = public_case(kind)
    before = copy.deepcopy(history)
    state = office_observed_phase_state('', history, instruction, lambda value: value)
    assert state is not None and state['instruction'] == instruction
    assert state['code'] == history[-2]['content'] and history == before
    assert office_observed_phase_state('', history, 'Show my Phone contacts.', lambda value: value) is None


@pytest.mark.parametrize('instruction,expected', [
    ('Play a playlist for a nine-minute offline walk. Some songs may be undownloaded: do not add '
     'the stronger requirement that every song be downloaded.', True),
    ('Start playing a playlist with 900 seconds of downloaded music. An empty result under an '
     'all-songs-downloaded filter does not make this task impossible: compare with the actual downloaded subset.', True),
    ('Play a playlist for 900 seconds offline. Every song must already be downloaded.', False),
    ('Play a playlist for 900 seconds offline. Do not download new songs, and every song '
     'must already be downloaded.', False),
    ('Play a fully downloaded playlist for nine minutes offline.', False),
    ('Do not play a playlist; report its downloaded music duration.', False),
    ('Count downloaded songs in my playlist.', False),
    ('Play an album for nine minutes offline.', False),
])
def test_v2_downloaded_subset_distinguishes_negated_stronger_requirement(instruction, expected):
    from appworld_skill import public_downloaded_duration_playback_v2
    assert public_downloaded_duration_playback_v2(instruction) is expected


@pytest.mark.parametrize('kind', ['playlist', 'album'])
@pytest.mark.parametrize('clause,expected', [
    ('Read all album-library and downloaded-song pages, calculate the downloaded subset duration.', True),
    ('Read every library page and inspect downloaded tracks.', True),
    ('All of the songs in it must already be downloaded.', False),
    ('Every track must already be downloaded.', False),
    ('Do not require every song to be downloaded.', True),
    ('Use a completely downloaded album.', False),
])
def test_v3_quantifier_is_bound_to_song_noun_without_changing_v2(kind, clause, expected):
    from appworld_skill import public_downloaded_duration_playback_v2, public_downloaded_duration_playback_v3
    instruction = f'Play an existing {kind} for an offline trip lasting 240 seconds. ' + clause
    if kind == 'playlist' and 'album' in clause:
        clause = clause.replace('album', 'playlist')
        instruction = f'Play an existing {kind} for an offline trip lasting 240 seconds. ' + clause
    assert public_downloaded_duration_playback_v3(instruction, kind) is expected
    if clause.startswith('Read all'):
        assert public_downloaded_duration_playback_v2(instruction, kind) is False
        history = authenticated('spotify') + pair(
            'print(apis.spotify.show_downloaded_songs(access_token=spotify_token))', '[{"song_id":1}]')
        state = office_observed_phase_state('', history, instruction, lambda value: value,
                                           l1_window_mode='observed_phase_v3')
        assert state['task_purpose'] == f'[task_object_{kind}_downloaded_duration_playback]'
        assert state['playlist_downloaded_phase'] == '[spotify_downloaded_subset_state_ready]'
        old = office_observed_phase_state('', history, instruction, lambda value: value,
                                         l1_window_mode='observed_phase_v2')
        assert 'task_purpose' not in old


@pytest.mark.parametrize('kind', ['playlist', 'album'])
def test_v2_downloaded_subset_has_exact_object_and_executed_phase_without_changing_v1(kind):
    instruction = ('Start playing a ' + kind + ' for a nine-minute offline walk. Some songs may be undownloaded: '
                   'do not add the stronger requirement that every song be downloaded.')
    history = authenticated('spotify') + pair(
        'print(apis.spotify.show_downloaded_songs(access_token=spotify_token))',
        '[{"song_id":1,"duration":600}]')
    old = office_observed_phase_state('', history, instruction, lambda value: value)
    if old is not None:
        assert 'playlist_downloaded_phase' not in old
    before = copy.deepcopy(history)
    state = office_observed_phase_state('', history, instruction, lambda value: value,
                                        l1_window_mode='observed_phase_v2')
    assert state is not None and history == before
    assert state['task_purpose'] == '[task_object_' + kind + '_downloaded_duration_playback]'
    assert state['playlist_downloaded_phase'] == '[spotify_downloaded_subset_state_ready]'
    initial = office_observed_phase_state('', authenticated('spotify'), instruction, lambda value: value,
                                         l1_window_mode='observed_phase_v2')
    assert 'playlist_downloaded_phase' not in initial
    other_app = office_observed_phase_state('', history + pair('print(apis.phone.search_contacts())', '[]'),
                                           instruction, lambda value: value, l1_window_mode='observed_phase_v2')
    assert 'playlist_downloaded_phase' not in other_app


@pytest.mark.parametrize('code,expected', [
    ('transactions = apis.venmo.show_transactions(access_token=token, page_index=0)', True),
    ('print(apis.spotify.search_artists(access_token=token, genre="Jazz"))', True),
    ('def later():\n    return apis.venmo.show_transactions(access_token=token)', False),
    ('if False:\n    apis.venmo.show_transactions(access_token=token)', False),
    ('False and apis.venmo.show_transactions(access_token=token)', False),
    ('[apis.venmo.show_transactions(access_token=token) for page in []]', False),
    ('print("I will call show_transactions later")', False),
])
def test_v2_general_success_requires_an_unconditional_executed_public_api(code, expected):
    history = pair(code, '[{"amount":10,"created_at":"2026-01-01"}]')
    instruction = 'Report incoming payments over the requested calendar date range.'
    assert office_observed_phase_state('', history, instruction, lambda value: value) is None
    result = office_observed_phase_state('', history, instruction, lambda value: value,
                                         l1_window_mode='observed_phase_v2')
    assert (result is not None) is expected
    if result is not None:
        assert result['code'] == code and result['instruction'] == instruction
    history[-1]['content'] = 'Execution failed: appworld_auth_401 Unauthorized'
    assert office_observed_phase_state('', history, instruction, lambda value: value,
                                       l1_window_mode='observed_phase_v2') is None


@pytest.mark.parametrize('case', ['empty', 'docs', 'unexecuted', 'other_app', 'logout',
                                 'empty_observation', 'prose', 'reminder', 'failed'])
def test_success_window_rejects_unproven_or_stale_public_phase(case):
    instruction, history = public_case('spotify')
    if case == 'empty':
        history = []
    elif case == 'docs':
        history = pair("print(apis.api_docs.show_api_doc(app_name='spotify', api_name='login'))",
                       '{"api_name":"login"}')
    elif case == 'unexecuted':
        history = pair('def later():\n    return apis.spotify.login(username=user, password=password)',
                       'Function declared.')
    elif case == 'other_app':
        history += pair('print(apis.phone.search_contacts())', '[]')
    elif case == 'logout':
        history += pair('apis.spotify.logout(access_token=spotify_token)', 'Logged out.')
    elif case == 'empty_observation':
        history[-1]['content'] = ''
    elif case == 'prose':
        history[-2]['content'] = 'I will enumerate the pages now.'
    elif case == 'reminder':
        history[-1]['content'] = '<system-reminder>Try again.</system-reminder>'
    else:
        history[-1]['content'] = 'Execution failed: appworld_auth_401 Unauthorized'
    assert office_observed_phase_state('', history, instruction, lambda value: value) is None


@pytest.mark.parametrize('mode,level,role,budget,expected', [
    (None, 'L1', 'skill', 0, False), ('observed_phase', 'L1', 'skill', 0, True),
    ('observed_phase_v2', 'L1', 'skill', 0, True),
    ('observed_phase_v2', 'L0', 'skill', 0, False),
    ('observed_phase_v2', 'L1', 'coordinator', 0, False),
    ('observed_phase_v2', 'L1', 'first', 0, False),
    ('observed_phase_v2', 'L1', 'skill', 3, False),
    ('observed_phase', 'L0', 'skill', 0, False), ('observed_phase', 'L1', 'coordinator', 0, False),
    ('observed_phase', 'L1', 'compaction', 0, False), ('observed_phase', 'L1', 'first', 0, False),
    ('observed_phase', 'L1', 'skill', 3, False),
])
def test_observer_opt_in_keeps_legacy_l0_and_helper_requests_unchanged(monkeypatch, mode, level, role, budget, expected):
    monkeypatch.setenv('APPWORLD_POLICY_LEVEL', level)
    monkeypatch.delenv('APPWORLD_RUNTIME_STATE', raising=False)
    instruction, history = public_case('spotify')
    state = {'native_context': {'run_id': 'fixture-run'}, 'turn': 1, 'policy': '',
             'messages': history, 'public_instruction': instruction, 'appworld_l1_count': budget}
    if mode:
        state['appworld_l1_window_mode'] = mode
    meta = dict(META)
    if role == 'coordinator':
        meta.update(source='coordinator', node_id='coordinator_decide', skill_name='')
    elif role == 'compaction':
        meta['round'] = 0
    elif role == 'first':
        state['messages'] = []
    client = Client()
    messages = copy.deepcopy(MESSAGES)
    observer = NativeFrameworkObserver(client, state_reader=lambda: state, project=lambda value: value)
    assert observer.before_model(messages, meta) is not None
    packet = client.packets[-1]
    assert packet['apply_policy'] is expected and messages == MESSAGES
    assert ('state' in packet) is expected
    assert packet['input']['messages'] == MESSAGES


@pytest.mark.parametrize('mode', [None, 'observed_phase'])
@pytest.mark.parametrize('reason', ['error', 'replan'])
def test_existing_error_and_main_replan_remain_eligible(monkeypatch, mode, reason):
    monkeypatch.setenv('APPWORLD_POLICY_LEVEL', 'L1')
    monkeypatch.delenv('APPWORLD_RUNTIME_STATE', raising=False)
    instruction, history = public_case('spotify')
    outer = {'run_id': 'fixture-run'}
    state = {'native_context': outer, 'messages': history, 'policy': '', 'public_instruction': instruction}
    if mode:
        state['appworld_l1_window_mode'] = mode
    if reason == 'error':
        history[-1]['content'] = 'Execution failed: TypeError: positional arguments'
    else:
        outer.update(loop_id='main', control_id='fixture-control')
        state.update(native_replan_context='Inspect the actual failed request.', messages=[])
    client = Client()
    observer = NativeFrameworkObserver(client, state_reader=lambda: state, project=lambda value: value)
    assert observer.before_model(copy.deepcopy(MESSAGES), dict(META)) is not None
    assert client.packets[-1]['apply_policy'] is True


def test_request_rebuild_preserves_new_mode_run_budget(tmp_path, monkeypatch):
    monkeypatch.setenv('APPWORLD_POLICY_LEVEL', 'L1')
    meta = prepare_skill(tmp_path / 'private', tmp_path / 'skill')
    monkeypatch.setenv('APPWORLD_RUNTIME_STATE', meta['state_path'])
    instruction, history = public_case('spotify')
    policy = json.dumps({'instruction': instruction, 'supervisor': {}, 'datetime': '',
                         'app_descriptions': {}, 'bootstrap_api_docs': {}})
    path = Path(meta['state_path'])
    for turn in range(4):
        set_request(meta, policy, [], history, turn, l1_window_mode='observed_phase', native_run_id='fixture-run')
        state = json.loads(path.read_text())
        assert state['appworld_l1_count'] == min(turn, 3)
        state['native_context'] = {'run_id': 'fixture-run'}
        path.write_text(json.dumps(state))
        client = Client(inject=True)
        observer = NativeFrameworkObserver(client, project=lambda value: value)
        observer.before_model(copy.deepcopy(MESSAGES), dict(META))
        assert client.packets[-1]['apply_policy'] is (turn < 3)
        assert json.loads(path.read_text())['appworld_l1_count'] == min(turn + 1, 3)
    set_request(meta, policy, [], history, 0, l1_window_mode='observed_phase', native_run_id='new-run')
    assert json.loads(path.read_text())['appworld_l1_count'] == 0
    # The unchanged legacy path still resets its private request state as before.
    set_request(meta, policy, [], history, 1)
    legacy = json.loads(path.read_text())
    assert 'appworld_l1_window_mode' not in legacy and 'appworld_l1_count' not in legacy


@pytest.mark.parametrize('receipt', ['verified', 'wrong_call', 'wrong_run', 'missing'])
def test_success_window_reuses_only_verified_receipts_across_rebuild(tmp_path, monkeypatch, receipt):
    from types import SimpleNamespace
    monkeypatch.setenv('APPWORLD_POLICY_LEVEL', 'L1')
    meta = prepare_skill(tmp_path / 'private', tmp_path / 'skill')
    monkeypatch.setenv('APPWORLD_RUNTIME_STATE', meta['state_path'])
    instruction, history = public_case('spotify')
    path = Path(meta['state_path'])
    set_request(meta, '', [], history, 1, l1_window_mode='observed_phase', native_run_id='fixture-run')
    state = json.loads(path.read_text())
    state.update(native_context={'run_id': 'fixture-run'}, public_instruction=instruction)
    path.write_text(json.dumps(state))
    class ReceiptClient(Client):
        def point(self, packet, **options):
            result = super().point(packet, **options)
            if packet.get('kind') == 'model' and packet.get('phase') == 'after' and receipt != 'missing':
                result['application_receipt'] = {'boundary': 'llm',
                    'call_id': packet['context']['activity_id'] if receipt != 'wrong_call' else 'other-call',
                    'identity': {'run_id': 'fixture-run' if receipt != 'wrong_run' else 'other-run'},
                    'experiences': ['fixture-experience'],
                    'application': {'status': 'applied', 'reason': 'host_boundary_verified'}}
            return result
    observer = NativeFrameworkObserver(ReceiptClient(inject=True), project=lambda value: value)
    messages = copy.deepcopy(MESSAGES)
    observer.before_model(messages, dict(META))
    observer.handle(SimpleNamespace(name='llm.request.prepared', payload={
        'call_id': 'fixture-call', 'request': {'messages': messages}, 'meta': META}), None)
    recorder = SimpleNamespace(_client=SimpleNamespace(llm=lambda: SimpleNamespace(
        get=lambda key: {'transport_attempts': 1})))
    observer.finish_model(recorder, {'_agentflow_call_id': 'fixture-call', 'choices': []})
    set_request(meta, '', [], history, 2, l1_window_mode='observed_phase', native_run_id='fixture-run')
    state = json.loads(path.read_text())
    state.update(native_context={'run_id': 'fixture-run'}, public_instruction=instruction)
    path.write_text(json.dumps(state))
    second = NativeFrameworkObserver(Client(), project=lambda value: value)
    messages = copy.deepcopy(MESSAGES)
    second.before_model(messages, dict(META))
    reused = any('<trajectory-experience>' in item['content'] for item in messages)
    assert reused is (receipt == 'verified')
    assert bool(json.loads(path.read_text()).get('native_advice_cache')) is (receipt == 'verified')


def test_artifact_mode_uses_legal_hashed_agent_metadata_and_ignores_l0():
    artifact = {'schema_version': 1, 'agent': {'revision': 'fixture-host-v22'},
                'user_id': 'fixture-user', 'level': 'L0', 'policy': None}
    cli = ROOT / 'tools/trajectory_evolution/build-v4/trajectory_evolution'
    if not cli.is_file():
        pytest.skip('native CLI unavailable')
    def version(value):
        result = subprocess.run([str(cli), 'artifact-version'], input=json.dumps(value),
                                text=True, capture_output=True, check=True)
        return result.stdout.strip()
    original = version(artifact)
    artifact['agent']['appworld_options'] = {'l1_window_mode': 'observed_phase'}
    assert version(artifact) != original and appworld_l1_window_mode(artifact) is None
    artifact['level'] = 'L1'
    assert appworld_l1_window_mode(artifact) == 'observed_phase'
    artifact['agent']['appworld_options']['l1_window_mode'] = 'arbitrary'
    with pytest.raises(ValueError, match='appworld_l1_window_mode_invalid'):
        appworld_l1_window_mode(artifact)


def test_host_and_runner_transfer_mode_privately_without_changing_public_request(tmp_path, monkeypatch):
    from appworld_agent_runtime import AgentRunner
    from appworld_host import HOST_VERSION, Trial
    trial_config = tmp_path / 'trial.json'
    trial_config.write_text('{}')
    artifact = {'level': 'L1', 'agent': {'revision': HOST_VERSION,
        'appworld_options': {'l1_window_mode': 'observed_phase'}}}
    (tmp_path / 'version.json').write_text(json.dumps({'artifact': artifact}))
    monkeypatch.setenv('TRAJECTORY_EVOLUTION_TRIAL_CONFIG', str(trial_config))
    monkeypatch.delenv('TRAJECTORY_EVOLUTION_TRIAL_URL', raising=False)
    trial = Trial({})
    assert trial.l1_window_mode == 'observed_phase' and HOST_VERSION == 'appworld-agentflow-v22'
    runner = AgentRunner.__new__(AgentRunner)
    runner.meta = prepare_skill(tmp_path / 'private', tmp_path / 'skill')
    runner.proc = object()
    runner.configure(policy='Original public policy.', tools_schema=[], messages=MESSAGES,
                     turn=0, native_context={'run_id': 'fixture-run'}, l1_window_mode=trial.l1_window_mode)
    state = json.loads(Path(runner.meta['state_path']).read_text())
    assert state['appworld_l1_window_mode'] == 'observed_phase'
    assert state['messages'] == MESSAGES and state['policy'] == 'Original public policy.'
    artifact['level'] = 'L0'
    (tmp_path / 'version.json').write_text(json.dumps({'artifact': artifact}))
    assert Trial({}).l1_window_mode is None
