"""Saved-result prevention uses a new artifact and actual public execution only."""
import copy
import inspect
import json
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[4]
ADAPTER = ROOT / 'examples/agent/business-simulator/appworld'
sys.path.insert(0, str(ROOT / 'packages/agentflow-runtime/src'))
sys.path.insert(0, str(ADAPTER))
from appworld_skill import (NativeFrameworkObserver, appworld_saved_result_experiences,
                           prepare_skill, saved_result_failure_prevention_state)
from appworld_host import Trial, public_saved_result_contracts
from appworld_agent_runtime import AgentRunner

REF = {'memory_id': 'native-approved-saved-result', 'content_revision': 'approved-revision'}
OLD = {'memory_id': 'old-valid-phase-card', 'content_revision': 'unchanged'}
CONTENT = 'Show the already saved API result instead of calling the same read again.'
CONTRACTS = [['amazon', 'show_wish_list']]
META = {'source': 'skill_executor', 'skill_name': 'appworld-office', 'exec_skill_key': 'office',
        'node_id': 'skill_executor', 'round': 1, 'task_id': 'fixture'}


def pair(code, observation='Execution successful.'):
    return [{'role': 'assistant', 'content': code}, {'role': 'user', 'content': observation}]


def history(code='saved = apis.amazon.show_wish_list(access_token=token)', observation='Execution successful.'):
    return pair('token = apis.amazon.login(username=user, password=password)["access_token"]',
                'Authenticated.') + pair(code, observation)


def qualify(messages, contracts=CONTRACTS):
    return saved_result_failure_prevention_state('', messages, 'Read the public collection.',
                                                lambda x: x, public_contracts=contracts)


def artifact(level='L3'):
    return {'level': level, 'agent': {'appworld_options': {'l1_saved_result_experiences': [REF]}},
            'policy': {'options': {'approved_experiences': [REF, OLD]},
                       'snapshot': {'experiences': [{**REF, 'content': CONTENT}]}}}


def test_successful_assignment_has_honest_causal_metadata_and_no_future_code():
    messages = history()
    before = copy.deepcopy(messages)
    result = qualify(messages)
    assert result['saved_result_contract'] == '[saved_public_collection_result]'
    assert result['saved_result_phase'] == '[undisplayed_api_assignment]'
    assert result['saved_result_evidence'] == {'api': 'apis.amazon.show_wish_list',
        'assigned_name': 'saved', 'value_observed': False, 'contract_source': 'public_api_registry'}
    assert 'print' not in result['code'] and messages == before


@pytest.mark.parametrize('code,observation', [
    ('apis.amazon.show_wish_list(access_token=token)', 'Execution successful.'),
    ('saved = apis.amazon.show_wish_list(access_token=token)\nprint(saved)', '[]'),
    ('saved = apis.amazon.show_wish_list(access_token=unknown)', 'Execution successful.'),
    ('saved = apis.amazon.show_wish_list(**parameters)', 'Execution successful.'),
    ('saved = apis.amazon.show_wish_list(access_token=get_token())', 'Execution successful.'),
    ('saved = apis.amazon.move_product_from_cart_to_wish_list(access_token=token)', 'Execution successful.'),
    ('saved = apis.amazon.login(username=user, password=password)', 'Execution successful.'),
    ('saved = apis.api_docs.show_api_doc(app_name="amazon", api_name="show_wish_list")', 'Execution successful.'),
    ('saved = apis.supervisor.show_account_passwords()', 'Execution successful.'),
    ('credential_token = apis.amazon.show_wish_list(access_token=token)', 'Execution successful.'),
    ('a = saved = apis.amazon.show_wish_list(access_token=token)', 'Execution successful.'),
    ('container["saved"] = apis.amazon.show_wish_list(access_token=token)', 'Execution successful.'),
    ('if condition:\n saved = apis.amazon.show_wish_list(access_token=token)', 'Execution successful.'),
    ('saved = apis.amazon.show_wish_list(access_token=token)', 'Traceback\nTypeError: invalid'),
    ('saved = apis.amazon.show_wish_list(access_token=token)', '401 Unauthorized'),
    ('saved = apis.amazon.show_wish_list(access_token=token)', '[]'),
    ('saved = apis.amazon.show_wish_list(access_token=token)', ''),
    ('saved = apis.amazon.show_wish_list(access_token=token)', '<system-reminder>Execution successful.</system-reminder>'),
])
def test_unsupported_noncausal_displayed_unknown_or_non_read_windows_reject(code, observation):
    assert qualify(history(code, observation)) is None


def test_no_contract_reassigned_or_conditionally_bound_names_reject():
    assert qualify(history(), []) is None
    assert qualify(pair('saved = []') + history()) is None
    assert qualify(pair('if ready:\n token = "fixture"') + pair(
        'saved = apis.amazon.show_wish_list(access_token=token)')) is None
    assert qualify(pair('token = apis.amazon.login()', 'Execution failed: 401 Unauthorized') + pair(
        'saved = apis.amazon.show_wish_list(access_token=token)')) is None
    assert qualify(history()[:-2] + pair('del token') + history()[-2:]) is None
    assert qualify(pair('apis = other_object') + history()) is None
    for observation in (None, {}, [], 1):
        assert qualify(history(observation=observation)) is None


def test_public_registry_is_read_only_collection_only_and_not_changed():
    docs = {app: json.loads((ADAPTER / 'runtime/world/data/api_docs/standard' / (app + '.json')).read_text())
            for app in ('amazon', 'spotify', 'supervisor', 'api_docs')}
    before = copy.deepcopy(docs)
    contracts = public_saved_result_contracts(SimpleNamespace(task=SimpleNamespace(api_docs=docs)))
    assert ['amazon', 'show_wish_list'] in contracts
    assert ['amazon', 'move_product_from_cart_to_wish_list'] not in contracts
    assert ['amazon', 'login'] not in contracts
    assert all(pair[0] not in ('api_docs', 'supervisor') for pair in contracts)
    assert docs == before
    docs['amazon']['show_wish_list']['api_name'] = 'different'
    assert ['amazon', 'show_wish_list'] not in public_saved_result_contracts(
        SimpleNamespace(task=SimpleNamespace(api_docs=docs)))


def test_refs_require_native_approval_exact_revision_unique_and_l0_is_off():
    assert appworld_saved_result_experiences(artifact()) == {'refs': [REF], 'contents': [CONTENT]}
    assert appworld_saved_result_experiences(artifact('L0')) is None
    for refs in ([REF, REF], [{**REF, 'content_revision': 'wrong'}], [], [{**REF, 'extra': True}]):
        changed = artifact(); changed['agent']['appworld_options']['l1_saved_result_experiences'] = refs
        with pytest.raises(ValueError): appworld_saved_result_experiences(changed)
    changed = artifact(); changed['policy']['snapshot']['experiences'] = []
    with pytest.raises(ValueError): appworld_saved_result_experiences(changed)
    changed = artifact(); changed['agent']['appworld_options'] = {}
    assert appworld_saved_result_experiences(changed) is None


@pytest.mark.parametrize('enabled,count,skill', [(False, 0, 'appworld-office'),
                                               (True, 0, 'appworld-office'),
                                               (True, 3, 'appworld-office'),
                                               (True, 0, 'coordinator')])
def test_whole_before_model_opt_in_real_round_skill_and_three_application_budget(monkeypatch, enabled, count, skill):
    monkeypatch.setenv('APPWORLD_POLICY_LEVEL', 'L3')
    monkeypatch.delenv('APPWORLD_RUNTIME_STATE', raising=False)
    state = {'native_context': {'run_id': 'same-run'}, 'messages': history(), 'policy': '',
             'public_instruction': 'Read the collection.', 'appworld_l1_count': count}
    if enabled:
        state['appworld_saved_result_experiences'] = appworld_saved_result_experiences(artifact())
        state['appworld_saved_result_contracts'] = CONTRACTS
    class Recorder:
        def __init__(self): self.packets = []
        def point(self, packet, **kwargs):
            self.packets.append(copy.deepcopy(packet))
            return {'context': {'run_id': 'same-run', 'activity_id': 'point'},
                    'input': copy.deepcopy(packet.get('input', {}))}
    recorder = Recorder()
    observer = NativeFrameworkObserver(recorder, state_reader=lambda: state, project=lambda x: x)
    meta = {**META, 'skill_name': skill}
    observer.before_model([{'role': 'system', 'content': 'Original.'}], meta)
    packet = recorder.packets[-1]
    allowed = enabled and count < 3 and skill == 'appworld-office'
    assert packet['apply_policy'] is allowed
    if allowed:
        assert packet['extensions']['runtime_experience_allowlist'] == [REF]
        assert packet['state']['saved_result_evidence']['value_observed'] is False


def test_runtime_keeps_new_refs_and_contracts_private_and_default_clears(tmp_path):
    runner = AgentRunner.__new__(AgentRunner)
    runner.meta = prepare_skill(tmp_path / 'private', tmp_path / 'skill')
    runner.proc = object()
    options = dict(policy='Public policy.', tools_schema=[], messages=history(), turn=1,
                   native_context={'run_id': 'same-run'},
                   saved_result_experiences=appworld_saved_result_experiences(artifact()),
                   saved_result_contracts=CONTRACTS)
    inspect.signature(AgentRunner.configure).bind(runner, **options)
    runner.configure(**options)
    state = json.loads(Path(runner.meta['state_path']).read_text())
    assert state['appworld_saved_result_experiences']['refs'] == [REF]
    assert state['appworld_saved_result_contracts'] == CONTRACTS
    document = Path(runner.meta['skill_doc_path']).read_text()
    assert REF['memory_id'] not in document and 'public_api_registry' not in document
    runner.configure(policy='Public policy.', tools_schema=[], messages=[], turn=2)
    assert 'appworld_saved_result_experiences' not in json.loads(Path(runner.meta['state_path']).read_text())


def test_host_initialization_validates_new_refs_and_default_is_off(monkeypatch, tmp_path):
    monkeypatch.delenv('TRAJECTORY_EVOLUTION_TRIAL_URL', raising=False)
    config = tmp_path / 'runtime.json'
    monkeypatch.setenv('TRAJECTORY_EVOLUTION_TRIAL_CONFIG', str(config))
    (tmp_path / 'version.json').write_text(json.dumps({'artifact': artifact()}))
    trial = Trial({})
    assert trial.saved_result_experiences['refs'] == [REF]
    changed = artifact(); changed['agent']['appworld_options'] = {}
    (tmp_path / 'version.json').write_text(json.dumps({'artifact': changed}))
    assert Trial({}).saved_result_experiences is None


def test_unqualified_window_does_not_reuse_saved_result_cache(monkeypatch):
    monkeypatch.setenv('APPWORLD_POLICY_LEVEL', 'L3')
    monkeypatch.delenv('APPWORLD_RUNTIME_STATE', raising=False)
    state = {'native_context': {'run_id': 'same-run'}, 'messages': history(
        'saved = apis.amazon.show_wish_list(access_token=token)', 'Execution failed: KeyError: product_id'),
        'policy': '', 'public_instruction': 'Read the collection.',
        'appworld_saved_result_experiences': appworld_saved_result_experiences(artifact()),
        'appworld_saved_result_contracts': CONTRACTS,
        'appworld_auth_parameter_experiences': [OLD], 'appworld_auth_fallback_experiences': [REF]}
    class Recorder:
        def point(self, packet, **kwargs):
            return {'context': {'run_id': 'same-run', 'activity_id': 'point'},
                    'input': copy.deepcopy(packet.get('input', {}))}
    observer = NativeFrameworkObserver(Recorder(), state_reader=lambda: state, project=lambda x: x)
    monkeypatch.setattr(observer, '_cached_cards', lambda scope: ['<trajectory-experience>' + CONTENT + '</trajectory-experience>'])
    messages = [{'role': 'system', 'content': 'Original.'}]
    observer.before_model(messages, META)
    assert messages == [{'role': 'system', 'content': 'Original.'}]
