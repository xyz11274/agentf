"""Host passes validated, explicit auth-parameter refs to the existing runtime."""
import ast
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

from appworld_host import Trial, public_auth_parameter_contracts


REF = {'memory_id': 'approved-native-card', 'content_revision': 'fixed-revision'}
OTHER_REF = {'memory_id': 'other-approved-card', 'content_revision': 'other-revision'}


def initialize(monkeypatch, tmp_path, artifact):
    monkeypatch.delenv('TRAJECTORY_EVOLUTION_TRIAL_URL', raising=False)
    config = tmp_path / 'runtime.json'
    (tmp_path / 'version.json').write_text(json.dumps({'artifact': artifact}))
    monkeypatch.setenv('TRAJECTORY_EVOLUTION_TRIAL_CONFIG', str(config))
    return Trial({})


def test_host_reads_only_explicit_exact_approved_auth_refs(monkeypatch, tmp_path):
    artifact = {'level': 'L3', 'agent': {'appworld_options': {
        'l1_auth_parameter_experiences': [dict(REF)]}},
        'policy': {'options': {'approved_experiences': [dict(REF), dict(OTHER_REF)]}}}
    trial = initialize(monkeypatch, tmp_path, artifact)
    assert trial.auth_parameter_experiences == [REF]
    assert trial.auth_fallback_experiences == [OTHER_REF]
    assert trial.l1_window_mode is None
    invalid = copy.deepcopy(artifact)
    invalid['agent']['appworld_options']['l1_auth_parameter_experiences'][0]['content_revision'] = 'unapproved'
    with pytest.raises(ValueError):
        initialize(monkeypatch, tmp_path, invalid)


@pytest.mark.parametrize('level', ['L0', 'L3'])
def test_host_default_has_no_auth_parameter_opt_in(monkeypatch, tmp_path, level):
    trial = initialize(monkeypatch, tmp_path, {'level': level, 'agent': {}, 'policy': {}})
    assert trial.auth_parameter_experiences is None
    assert trial.auth_fallback_experiences is None
    assert trial.auth_parameter_contracts is None


def configured_keywords(refs, structured=False, shape=None):
    # Invoke the actual configure call from Trial; no world or Agent is run.
    tree = ast.parse((ADAPTER / 'appworld_host.py').read_text())
    call = next(node for node in ast.walk(tree) if isinstance(node, ast.Call)
                and isinstance(node.func, ast.Attribute)
                and isinstance(node.func.value, ast.Name)
                and node.func.value.id == 'runner' and node.func.attr == 'configure')
    received = {}
    runner = SimpleNamespace(configure=lambda **kw: received.update(kw))
    state = SimpleNamespace(l1_window_mode=None, observed_phase_experiences=None,
                            auth_parameter_experiences=refs,
                            auth_fallback_experiences=[OTHER_REF] if refs is not None else None,
                            auth_parameter_contracts=[['simple_note', 'search_notes']] if refs is not None else None,
                            auth_structured_missing=structured,
                            shape_error_auth_experiences=shape,
                            saved_result_experiences=None, saved_result_contracts=None,
                            run_context={'run_id': 'real-run'})
    namespace = dict(runner=runner, self=state, prepared={'system': 'Public policy'},
                     history=[], turn=1, model={}, loop={}, loop_decision='continue')
    eval(compile(ast.Expression(call), str(ADAPTER / 'appworld_host.py'), 'eval'), namespace)
    return received


@pytest.mark.parametrize('refs', [None, [REF]])
def test_host_runtime_signature_and_opt_in_forwarding(refs):
    from appworld_agent_runtime import AgentRunner
    received = configured_keywords(refs)
    inspect.signature(AgentRunner.configure).bind(None, **received)
    if refs is None:
        assert 'auth_parameter_experiences' not in received
        assert 'auth_fallback_experiences' not in received
        assert 'auth_parameter_contracts' not in received
        assert 'l1_window_mode' not in received
        assert 'shape_error_auth_experiences' not in received
    else:
        assert received['auth_parameter_experiences'] == refs
        assert received['auth_fallback_experiences'] == [OTHER_REF]
        assert received['auth_parameter_contracts'] == [['simple_note', 'search_notes']]


def test_host_contracts_use_real_public_schema_and_do_not_mutate_it():
    docs = {app: json.loads((ADAPTER / 'runtime/world/data/api_docs/standard' / (app + '.json')).read_text())
            for app in ('simple_note', 'phone', 'amazon')}
    original = copy.deepcopy(docs)
    world = SimpleNamespace(task=SimpleNamespace(api_docs=docs))
    pairs = public_auth_parameter_contracts(world)
    assert ['simple_note', 'search_notes'] in pairs
    assert ['phone', 'show_alarms'] in pairs
    assert ['amazon', 'show_wish_list'] in pairs
    assert ['amazon', 'login'] not in pairs
    assert docs == original
    assert all(len(pair) == 2 and all(isinstance(value, str) for value in pair) for pair in pairs)


def test_host_contracts_require_exact_public_identity_and_required_token():
    doc = {'app_name': 'fixture', 'api_name': 'read', 'parameters': [
        {'name': 'access_token', 'required': False}]}
    world = SimpleNamespace(task=SimpleNamespace(api_docs={'fixture': {'read': doc}}))
    assert public_auth_parameter_contracts(world) == []
    doc['parameters'][0]['required'] = True
    assert public_auth_parameter_contracts(world) == [['fixture', 'read']]
    doc['api_name'] = 'other'
    assert public_auth_parameter_contracts(world) == []


def test_actual_runtime_configure_keeps_contracts_private_and_clears_default(tmp_path):
    from appworld_agent_runtime import AgentRunner
    from appworld_skill import prepare_skill
    runner = AgentRunner.__new__(AgentRunner)
    runner.meta = prepare_skill(tmp_path / 'private', tmp_path / 'skill')
    runner.proc = object()
    runner.configure(**configured_keywords([REF]))
    state = json.loads(Path(runner.meta['state_path']).read_text())
    assert state['appworld_auth_parameter_experiences'] == [REF]
    assert state['appworld_auth_fallback_experiences'] == [OTHER_REF]
    assert state['appworld_auth_parameter_contracts'] == [['simple_note', 'search_notes']]
    document = Path(runner.meta['skill_doc_path']).read_text()
    assert REF['memory_id'] not in document and OTHER_REF['memory_id'] not in document
    assert 'search_notes' not in document
    runner.configure(**configured_keywords(None))
    state = json.loads(Path(runner.meta['state_path']).read_text())
    assert not any(key.startswith('appworld_auth_') for key in state)


def test_new_structured_option_is_explicit_and_runtime_private(monkeypatch, tmp_path):
    from appworld_skill import appworld_auth_structured_missing, prepare_skill
    from appworld_agent_runtime import AgentRunner
    artifact={'level':'L3','agent':{'appworld_options':{
        'l1_auth_parameter_experiences':[REF], 'l1_auth_structured_missing':True}},
        'policy':{'options':{'approved_experiences':[REF]}}}
    assert initialize(monkeypatch,tmp_path,artifact).auth_structured_missing is True
    legacy=copy.deepcopy(artifact);legacy['agent']['appworld_options'].pop('l1_auth_structured_missing')
    assert initialize(monkeypatch,tmp_path,legacy).auth_structured_missing is False
    artifact['agent']['appworld_options']['l1_auth_structured_missing']='true'
    with pytest.raises(ValueError):appworld_auth_structured_missing(artifact)
    artifact['level']='L0'
    assert appworld_auth_structured_missing(artifact) is False
    runner=AgentRunner.__new__(AgentRunner);runner.meta=prepare_skill(tmp_path/'private',tmp_path/'skill');runner.proc=object()
    runner.configure(**configured_keywords([REF],True))
    assert json.loads(Path(runner.meta['state_path']).read_text())['appworld_auth_structured_missing'] is True


def test_host_shape_exclusion_is_approved_and_runtime_private(monkeypatch, tmp_path):
    from appworld_skill import prepare_skill
    from appworld_agent_runtime import AgentRunner
    artifact = {'level': 'L3', 'agent': {'appworld_options': {
        'l1_shape_error_auth_experiences': [REF], 'l1_shape_error_state_forwarding': True}}, 'policy': {
        'options': {'approved_experiences': [REF, OTHER_REF]}, 'snapshot': {
            'experiences': [dict(REF, content='Use the current login access_token.')]}}}
    trial = initialize(monkeypatch, tmp_path, artifact)
    payload = trial.shape_error_auth_experiences
    assert payload['excluded_refs'] == [REF]
    assert payload['approved_refs'] == [REF, OTHER_REF]
    keywords = configured_keywords(None, shape=payload)
    inspect.signature(AgentRunner.configure).bind(None, **keywords)
    runner = AgentRunner.__new__(AgentRunner)
    runner.meta = prepare_skill(tmp_path / 'private', tmp_path / 'skill')
    runner.proc = object()
    monkeypatch.delenv('TRAJECTORY_EVOLUTION_TRIAL_CONFIG', raising=False)
    runner.configure(**keywords)
    state = json.loads(Path(runner.meta['state_path']).read_text())
    assert state['appworld_shape_error_auth_experiences'] == payload
    document = Path(runner.meta['skill_doc_path']).read_text()
    assert REF['memory_id'] not in document
    assert payload['excluded_contents'][0] not in document
    invalid = copy.deepcopy(artifact)
    invalid['agent']['appworld_options']['l1_shape_error_auth_experiences'][0]['content_revision'] = 'wrong'
    with pytest.raises(ValueError):
        initialize(monkeypatch, tmp_path, invalid)
    artifact['level'] = 'L0'
    assert initialize(monkeypatch, tmp_path, artifact).shape_error_auth_experiences is None
    runner.configure(**configured_keywords(None))
    assert 'appworld_shape_error_auth_experiences' not in json.loads(Path(runner.meta['state_path']).read_text())


def test_shape_state_forwarding_defaults_off_for_immutable_71(monkeypatch, tmp_path):
    artifact = {'level': 'L3', 'agent': {'appworld_options': {
        'l1_shape_error_auth_experiences': [REF]}}, 'policy': {}}
    assert initialize(monkeypatch, tmp_path, artifact).shape_error_auth_experiences is None
    artifact['agent']['appworld_options']['l1_shape_error_state_forwarding'] = False
    assert initialize(monkeypatch, tmp_path, artifact).shape_error_auth_experiences is None
    artifact['agent']['appworld_options']['l1_shape_error_state_forwarding'] = 'true'
    with pytest.raises(ValueError, match='state_forwarding_invalid'):
        initialize(monkeypatch, tmp_path, artifact)
    artifact['agent']['appworld_options']['l1_shape_error_state_forwarding'] = True
    artifact['agent']['appworld_options'].pop('l1_shape_error_auth_experiences')
    with pytest.raises(ValueError, match='state_forwarding_source_invalid'):
        initialize(monkeypatch, tmp_path, artifact)
