"""Public TRAIN collection containers and dynamic read-only postconditions."""
import copy
import json
from pathlib import Path
import shutil
import sys
from types import SimpleNamespace
import pytest

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'tools/trajectory_evolution/experiments/appworld'))
sys.path.insert(0, str(ROOT / 'examples/agent/business-simulator/appworld'))
sys.path.insert(0, str(ROOT / 'packages/agentflow-runtime/src'))
import dataset
import appworld_host as host


def test_execution_curriculum_preserves_prefix_and_evaluation_files(tmp_path, monkeypatch):
    canonical = dataset.DATASETS
    active = tmp_path / 'active'; active.mkdir()
    for name in [*dataset.FILES.values(), 'manifest.json']:
        shutil.copyfile(canonical / name, active / name)
    added = dataset.public_execution_curriculum_rows()
    assert len(added) == 2
    assert {r['initial_state']['source_task_id'] for r in added} == {'287e338_2', 'b0a8eae_1'}
    new_ids = {row['task_id'] for row in added}
    original = b''.join(line for line in (active / 'train.jsonl').read_bytes().splitlines(keepends=True)
                        if json.loads(line)['task_id'] not in new_ids)
    (active / 'train.jsonl').write_bytes(original)
    monkeypatch.setattr(dataset, 'DATASETS', active)
    dataset.ensure_training_augmentation()
    updated = (active / 'train.jsonl').read_bytes()
    assert updated.startswith(original)
    assert len(dataset.rows('train')) == len(original.splitlines()) + 2
    dataset.ensure_training_augmentation()
    assert (active / 'train.jsonl').read_bytes() == updated
    for split in ('val', 'holdout', 'comparison500'):
        assert (active / dataset.FILES[split]).read_bytes() == (canonical / dataset.FILES[split]).read_bytes()
    for row in added:
        task = {'initial_state': row['initial_state']}
        assert host.synthetic_training_spec(task, train_path=active / 'train.jsonl') == row['initial_state']['training_goal']
        with pytest.raises(RuntimeError, match='scope_invalid'):
            host.synthetic_training_spec(dict(task, split='holdout'), train_path=active / 'train.jsonl')


def test_execution_histogram_uses_global_song_union_and_strict_count_types(monkeypatch):
    before = {'playlists': {1: {'song_ids': [1, 2]}, 2: {'song_ids': [1]}},
              'songs': {1: {'artists': [{'name': 'A'}, {'name': 'A'}]},
                        2: {'artists': [{'name': 'B'}]}}}
    answer = {'playlist_count': 2, 'distinct_song_count': 2,
              'artist_distinct_song_counts': {'A': 1, 'B': 1}}
    world = SimpleNamespace(apis=SimpleNamespace(supervisor=SimpleNamespace(
        show_active_task=lambda: {'answer': json.dumps(answer)})))
    monkeypatch.setattr(host, 'synthetic_training_snapshot', lambda *args: copy.deepcopy(before))
    goal = {'kind': 'spotify_library_unique_artist_histogram', 'min_playlists': 6}
    assert host.synthetic_training_metrics(world, goal, before)['strict_passed'] == 1
    for wrong in (2, True, 1.0):
        answer['artist_distinct_song_counts']['A'] = wrong
        assert host.synthetic_training_metrics(world, goal, before)['strict_passed'] == 0


def test_cross_app_summary_requires_actual_401_then_documented_successful_login(monkeypatch):
    before = {'playlists': {1: {}}, 'messages': {1: {}, 2: {}}, 'relationships': ['friend']}
    answer = {'phone_message_count': 2, 'phone_relationship_count': 1, 'spotify_playlist_count': 1}
    world = SimpleNamespace(apis=SimpleNamespace(supervisor=SimpleNamespace(
        show_active_task=lambda: {'answer': json.dumps(answer)})))
    monkeypatch.setattr(host, 'synthetic_training_snapshot', lambda *args: copy.deepcopy(before))
    goal = {'kind': 'cross_app_auth_recovery_summary', 'require_actual_401': True}
    outcomes = [
        {'failed': True, 'observation': 'HTTP 401 Unauthorized',
         'code': 'apis.phone.show_contact_relationships(access_token=str(0))'},
        {'failed': False, 'observation': '{}', 'code':
         'apis.api_docs.show_api_doc(app_name="phone", api_name="login")\n'
         'apis.api_docs.show_api_doc(app_name="phone", api_name="show_contact_relationships")'},
        {'failed': False, 'observation': '{}',
         'code': 'auth = apis.phone.login(username=profile["phone_number"], password=passwords["phone"])'},
        {'failed': False, 'observation': '[]',
         'code': 'apis.phone.show_contact_relationships(access_token=auth["access_token"])'}]
    assert host.synthetic_training_metrics(world, goal, before, outcomes)['strict_passed'] == 1
    assert host.synthetic_training_metrics(world, goal, before, [])['strict_passed'] == 0
    for index, field, value in ((0, 'failed', False), (0, 'observation', 'HTTP 422'), (2, 'failed', True),
                                (0, 'code', 'apis.phone.show_contact_relationships(access_token=str(1))')):
        wrong = copy.deepcopy(outcomes); wrong[index][field] = value
        assert host.synthetic_training_metrics(world, goal, before, wrong)['strict_passed'] == 0
    wrong = copy.deepcopy(outcomes); wrong.insert(1, copy.deepcopy(wrong[0]))
    assert host.synthetic_training_metrics(world, goal, before, wrong)['strict_passed'] == 0


def test_receive_records_real_auth_episode_for_public_validator():
    # Exercise the actual receive-path collection statement. The public
    # validator alone cannot detect a Host that accidentally passes [] to it.
    import ast
    import inspect
    import textwrap
    parsed = ast.parse(textwrap.dedent(inspect.getsource(host.Trial.receive)))
    collectors = [node for node in ast.walk(parsed) if isinstance(node, ast.If)
                  and any(isinstance(call, ast.Call) and
                          ast.unparse(call.func) == 'synthetic_execution_outcomes.append'
                          for statement in node.body for call in ast.walk(statement))]
    assert len(collectors) == 1
    statement = compile(ast.Module(body=[collectors[0]], type_ignores=[]), '<receive-outcomes>', 'exec')
    for goal, expected in (({'kind': 'cross_app_auth_recovery_summary'}, 1),
                           ({'kind': 'amazon_collection_quantity_summary', 'reproduce_container_error': 'cart'}, 1),
                           ({'kind': 'spotify_library_unique_artist_histogram'}, 0), (None, 0)):
        packet = {'synthetic_goal': goal, 'synthetic_execution_outcomes': [],
                  'code': 'apis.phone.show_contact_relationships(access_token=str(0))',
                  'observation': 'Execution failed: HTTP 401', 'failed': True}
        exec(statement, packet)
        assert len(packet['synthetic_execution_outcomes']) == expected
        if expected:
            assert packet['synthetic_execution_outcomes'][0] == {
                'code': packet['code'], 'observation': packet['observation'], 'failed': True}


def test_public_collection_sources_are_train_only_and_generation_is_read_only():
    path = dataset.DATASETS / 'train.jsonl'
    original = path.read_bytes()
    added = dataset.public_collection_training_rows()
    official = {r['initial_state']['source_task_id'] for r in dataset.rows('train')
                if not r['initial_state'].get('training_goal')}
    evaluation_sources = {r['initial_state']['source_task_id']
                          for split in ('val', 'holdout', 'comparison500') for r in dataset.rows(split)}
    assert len(added) == 4 and len({r['task_id'] for r in added}) == 4
    assert all(r['split'] == 'train' and r['provenance']['learning_exposed'] is True for r in added)
    assert all(r['initial_state']['source_task_id'] in official - evaluation_sources for r in added)
    assert path.read_bytes() == original


def test_public_collection_append_preserves_authority_and_is_idempotent(tmp_path, monkeypatch):
    canonical = dataset.DATASETS
    active = tmp_path / 'active'; active.mkdir()
    for name in [*dataset.FILES.values(), 'manifest.json']:
        shutil.copyfile(canonical / name, active / name)
    new_ids = {r['task_id'] for r in dataset.public_collection_training_rows()}
    original_rows = [r for r in dataset.rows('train') if r['task_id'] not in new_ids]
    initial = ''.join(json.dumps(r, ensure_ascii=False) + '\n' for r in original_rows)
    (active / 'train.jsonl').write_text(initial)
    monkeypatch.setattr(dataset, 'DATASETS', active)
    monkeypatch.setattr(dataset, 'public_execution_curriculum_rows', lambda: [])
    dataset.ensure_training_augmentation()
    result = dataset.rows('train')
    assert (active / 'train.jsonl').read_bytes().startswith(initial.encode())
    assert result[:len(original_rows)] == original_rows
    assert len(result) == len(original_rows) + 4
    assert {r['task_id'] for r in result[-4:]} == new_ids
    first = (active / 'train.jsonl').read_bytes()
    dataset.ensure_training_augmentation()
    assert (active / 'train.jsonl').read_bytes() == first
    assert dataset.read(active / 'manifest.json')['counts']['train'] == len(result)
    for split in ('val', 'holdout', 'comparison500'):
        assert (active / dataset.FILES[split]).read_bytes() == (canonical / dataset.FILES[split]).read_bytes()


@pytest.mark.parametrize('invalid', [None, ['orders'], ['cart', 'cart'], ['cart', {}]])
def test_collection_admission_rejects_non_public_collections(tmp_path, invalid):
    values = dataset.rows('train')
    row = copy.deepcopy(dataset.public_collection_training_rows()[0])
    row['initial_state']['training_goal']['collections'] = invalid
    path = tmp_path / 'train.jsonl'
    path.write_text('\n'.join(json.dumps(r) for r in values + [row]))
    with pytest.raises(RuntimeError, match='scope_invalid'):
        host.synthetic_training_spec({'initial_state': row['initial_state']}, train_path=path)


def test_collection_admission_binds_exact_train_fixture_and_rejects_holdout(tmp_path):
    values = dataset.rows('train')
    row = dataset.public_collection_training_rows()[0]
    values = [r for r in values if r['task_id'] != row['task_id']] + [row]
    path = tmp_path / 'train.jsonl'; path.write_text('\n'.join(json.dumps(r) for r in values))
    task = {'initial_state': row['initial_state']}
    assert host.synthetic_training_spec(task, train_path=path) == row['initial_state']['training_goal']
    with pytest.raises(RuntimeError, match='scope_invalid'):
        host.synthetic_training_spec(dict(task, split='holdout'), train_path=path)


def test_collection_snapshot_reads_real_dict_and_list_contracts(monkeypatch):
    calls = []
    def cart(**kwargs):
        calls.append('cart'); return {'cart_items': [{'product_id': 1, 'quantity': 3}]}
    def wish(**kwargs):
        calls.append('wish'); return [{'product_id': 2, 'quantity': 4}]
    def product(**kwargs):
        calls.append('product'); return {'rating': {1: 4.5, 2: 3.5}[kwargs['product_id']]}
    amazon = SimpleNamespace(login=lambda **kwargs: {'access_token': 'local-placeholder'},
                             show_cart=cart, show_wish_list=wish, show_product=product)
    world = SimpleNamespace(task=SimpleNamespace(supervisor=SimpleNamespace(email='train@example.com')),
        apis=SimpleNamespace(amazon=amazon, supervisor=SimpleNamespace(show_account_passwords=lambda:
            [{'account_name': 'amazon', 'password': 'local-placeholder'}])))
    goal = {'kind': 'amazon_collection_quantity_summary', 'collections': ['cart', 'wish_list'], 'rating_at_least': 4.0}
    snapshot = host.synthetic_training_snapshot(world, goal)
    assert snapshot == {'cart': {1: 3}, 'wish': {2: 4}, 'product_ratings': {1: 4.5, 2: 3.5}}
    assert calls == ['cart', 'wish', 'product', 'product']


@pytest.mark.parametrize('answer,changed,success', [
    ({'cart': {'product_count': 1, 'unit_count': 3}, 'wish_list': {'product_count': 1, 'unit_count': 4}}, False, True),
    ({'cart': {'product_count': 1, 'unit_count': 1}, 'wish_list': {'product_count': 1, 'unit_count': 4}}, False, False),
    ({'cart': {'product_count': True, 'unit_count': 3}, 'wish_list': {'product_count': 1, 'unit_count': 4}}, False, False),
    ({'cart': {'product_count': 1, 'unit_count': 3}, 'wish_list': {'product_count': 1, 'unit_count': 4}}, True, False),
    (None, False, False),
])
def test_collection_summary_is_dynamic_and_requires_preserved_quantities(monkeypatch, answer, changed, success):
    before = {'cart': {1: 3, 2: 8}, 'wish': {1: 4}, 'product_ratings': {1: 4.5, 2: 3.0}}
    after = copy.deepcopy(before)
    if changed: after['cart'][1] = 2
    monkeypatch.setattr(host, 'synthetic_training_snapshot', lambda world, goal: after)
    world = SimpleNamespace(apis=SimpleNamespace(supervisor=SimpleNamespace(show_active_task=lambda:
        {'status': 'success', 'answer': json.dumps(answer)})))
    goal = {'kind': 'amazon_collection_quantity_summary', 'collections': ['cart', 'wish_list'], 'rating_at_least': 4.0}
    result = host.synthetic_training_metrics(world, goal, before)
    assert result['strict_passed'] == int(success)
    assert result['official_goal_evaluated'] is False


@pytest.mark.parametrize('collection', ['wish_list', 'cart'])
def test_debug_train_requires_actual_container_failure_then_exact_public_schema(monkeypatch, collection):
    before = {'cart': {1: 3}, 'wish': {1: 4}, 'product_ratings': {1: 4.5}}
    monkeypatch.setattr(host, 'synthetic_training_snapshot', lambda world, goal: before)
    answer = {collection: {'product_count': 1, 'unit_count': 4 if collection == 'wish_list' else 3}}
    world = SimpleNamespace(apis=SimpleNamespace(supervisor=SimpleNamespace(show_active_task=lambda:
        {'status': 'success', 'answer': json.dumps(answer)})))
    goal = {'kind': 'amazon_collection_quantity_summary', 'collections': [collection],
            'rating_at_least': 4.0, 'reproduce_container_error': collection}
    method = 'show_wish_list' if collection == 'wish_list' else 'show_cart'
    read = f'container = apis.amazon.{method}(access_token=token)'
    error = 'Execution failed.\nTypeError: ' + ('list' if collection == 'wish_list' else 'string') + ' indices must be integers, not str'
    failure = {'code': read + '\nprint(container["items"])', 'observation': error, 'failed': True}
    doc = {'code': f'print(apis.api_docs.show_api_doc(app_name="amazon", api_name="{method}"))',
           'observation': 'Actual public schema', 'failed': False}
    success = {'code': read + '\nprint(container)', 'observation': 'Actual public data', 'failed': False}
    assert host.synthetic_training_metrics(world, goal, before, [failure, doc, success])['strict_passed'] == 1
    variants = [[], [dict(failure, failed=False), doc, success],
                [dict(failure, observation='Execution failed.\nTypeError: unexpected keyword access_token'), doc, success],
                [dict(failure, code='apis.venmo.search_users()'), doc, success],
                [failure, failure, doc, success], [doc, failure, success], [failure, success, doc],
                [failure, dict(doc, code=doc['code'].replace(method, 'show_product')), success],
                [failure, dict(doc, code=doc['code'] + '\n' + read), success]]
    for outcomes in variants:
        assert host.synthetic_training_metrics(world, goal, before, outcomes)['strict_passed'] == 0


def test_debug_train_snippets_are_public_read_only_and_not_answer_literals():
    debug = [row for row in dataset.public_collection_training_rows()
             if row['initial_state']['training_goal'].get('reproduce_container_error')]
    assert len(debug) == 2
    for row in debug:
        assert row['initial_state']['source_task_id'] in {'07b42fd_1', '07b42fd_2'}
        assert 'Execute the supplied faulty snippet exactly once' in row['task_input']
        assert 'Then inspect the exact public response schema in a separate code block' in row['task_input']
        assert 'apis.amazon.show_' in row['task_input']
        assert 'apis.amazon.add_' not in row['task_input']
        assert 'apis.amazon.remove_' not in row['task_input']


def test_auth_debug_sources_are_only_public_train_and_goals_remain_supported():
    original = (dataset.DATASETS / 'train.jsonl').read_bytes()
    added = dataset.public_authentication_training_rows()
    official = {row['initial_state']['source_task_id'] for row in dataset.rows('train')
                if not row['initial_state'].get('training_goal')}
    evaluation = {row['initial_state']['source_task_id'].rsplit('_', 1)[0]
                  for split in ('val', 'holdout', 'comparison500') for row in dataset.rows(split)}
    assert len(added) == len({row['task_id'] for row in added}) == 4
    assert {row['initial_state']['source_task_id'] for row in added} == {
        '07b42fd_1', '07b42fd_2', '287e338_1', '287e338_2'}
    assert all(row['initial_state']['source_task_id'] in official for row in added)
    assert all(row['initial_state']['source_task_id'].rsplit('_', 1)[0] not in evaluation for row in added)
    assert all(row['split'] == 'train' and row['provenance'] == {
        'kind': 'synthetic', 'learning_exposed': True} for row in added)
    assert (dataset.DATASETS / 'train.jsonl').read_bytes() == original
    for row in added:
        goal = row['initial_state']['training_goal']
        if goal['kind'] == 'amazon_collection_quantity_summary':
            assert set(goal) == {'kind', 'collections', 'rating_at_least'}
        else:
            assert goal['kind'] == 'spotify_artist_aggregate'
            assert set(goal) == {'kind', 'order', 'limit', 'deduplicate_songs'}
        assert row['task_input'] == row['initial_state']['training_instruction']
        assert 'do not repeat' in row['task_input']
        assert 'invent an observation' in row['task_input']
        assert 'actual login response' in row['task_input'] or 'actual successful login response' in row['task_input']


def test_auth_debug_append_preserves_all_existing_rows_and_is_idempotent(tmp_path, monkeypatch):
    monkeypatch.setattr(dataset, 'public_execution_curriculum_rows', lambda: [])
    canonical = dataset.DATASETS
    active = tmp_path / 'active'; active.mkdir()
    auth_ids = {row['task_id'] for row in dataset.public_authentication_training_rows()}
    for name in [*dataset.FILES.values(), 'manifest.json']:
        shutil.copyfile(canonical / name, active / name)
    original_rows = [row for row in dataset.rows('train') if row['task_id'] not in auth_ids]
    original = ''.join(json.dumps(row, ensure_ascii=False) + '\n' for row in original_rows)
    (active / 'train.jsonl').write_text(original)
    monkeypatch.setattr(dataset, 'DATASETS', active)
    dataset.ensure_training_augmentation()
    result = dataset.rows('train')
    assert (active / 'train.jsonl').read_bytes().startswith(original.encode())
    assert result[:len(original_rows)] == original_rows
    assert len(result) == len(original_rows) + 4
    assert {row['task_id'] for row in result[-4:]} == auth_ids
    first = (active / 'train.jsonl').read_bytes()
    dataset.ensure_training_augmentation()
    assert (active / 'train.jsonl').read_bytes() == first
    assert dataset.validate()['train'] == len(result)
    for row in result[-4:]:
        assert host.synthetic_training_spec({'initial_state': row['initial_state']},
                                            train_path=active / 'train.jsonl') == row['initial_state']['training_goal']
    for split in ('val', 'holdout', 'comparison500'):
        assert (active / dataset.FILES[split]).read_bytes() == (canonical / dataset.FILES[split]).read_bytes()


def test_auth_debug_rejects_existing_id_with_changed_body(tmp_path, monkeypatch):
    active = tmp_path / 'active'; active.mkdir()
    for name in [*dataset.FILES.values(), 'manifest.json']:
        shutil.copyfile(dataset.DATASETS / name, active / name)
    monkeypatch.setattr(dataset, 'DATASETS', active)
    dataset.ensure_training_augmentation()
    values = dataset.rows('train')
    row = next(row for row in values if row['attributes']['appworld'].get('authentication_debug'))
    row['task_input'] += ' conflicting body'
    (active / 'train.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in values))
    with pytest.raises(RuntimeError, match='auth_task_conflict'):
        dataset.ensure_training_augmentation()


def test_auth_debug_rejects_evaluation_source_family(monkeypatch):
    actual_rows = dataset.rows
    forbidden = copy.deepcopy(actual_rows('train')[0])
    forbidden['initial_state']['source_task_id'] = '07b42fd_3'
    monkeypatch.setattr(dataset, 'rows', lambda split: actual_rows(split) + [forbidden]
                        if split == 'val' else actual_rows(split))
    with pytest.raises(RuntimeError, match='public_source_overlap'):
        dataset.public_authentication_training_rows()


def test_observation_debug_sources_and_original_fixture_are_train_only():
    original = (dataset.DATASETS / 'train.jsonl').read_bytes()
    added = dataset.public_observation_training_rows()
    official = {row['initial_state']['source_task_id']: row for row in dataset.rows('train')
                if not row['initial_state'].get('training_goal')}
    evaluation = {row['initial_state']['source_task_id'].rsplit('_', 1)[0]
                  for split in ('val', 'holdout', 'comparison500') for row in dataset.rows(split)}
    assert len(added) == len({row['task_id'] for row in added}) == 4
    assert {row['initial_state']['source_task_id'] for row in added} == {'07b42fd_1', '07b42fd_2'}
    for row in added:
        initial = row['initial_state']
        source = initial['source_task_id']
        assert source in official and source.rsplit('_', 1)[0] not in evaluation
        assert {key: value for key, value in initial.items()
                if key not in {'training_instruction', 'training_goal'}} == official[source]['initial_state']
        assert row['split'] == 'train'
        assert row['provenance'] == {'kind': 'synthetic', 'learning_exposed': True}
        assert row['task_input'] == initial['training_instruction']
        assert row['attributes']['appworld']['training_source_split'] == 'train'
    read, mutation = added[:2]
    assert read['initial_state']['training_goal'] == {
        'kind': 'amazon_collection_quantity_summary', 'collections': ['wish_list'], 'rating_at_least': 4.0}
    assert 'observed_collection = apis.amazon.show_wish_list(access_token=token)' in read['task_input']
    assert 'display the saved observed_collection' in read['task_input']
    assert 'without calling show_wish_list again' in read['task_input'].replace('\n', ' ')
    assert mutation['initial_state']['training_goal'] == {'kind': 'cart_all_to_wishlist'}
    assert 'one necessary mutation' in mutation['task_input']
    assert 'one directed final verification' in mutation['task_input']
    assert 'stop querying and complete' in mutation['task_input']
    assert (dataset.DATASETS / 'train.jsonl').read_bytes() == original


def test_observation_debug_append_is_exact_idempotent_and_host_admitted(tmp_path, monkeypatch):
    monkeypatch.setattr(dataset, 'public_execution_curriculum_rows', lambda: [])
    canonical = dataset.DATASETS
    active = tmp_path / 'active'; active.mkdir()
    added = dataset.public_observation_training_rows()
    added_ids = {row['task_id'] for row in added}
    for name in [*dataset.FILES.values(), 'manifest.json']:
        shutil.copyfile(canonical / name, active / name)
    original = b''.join(line for line in (active / 'train.jsonl').read_bytes().splitlines(keepends=True)
                        if json.loads(line)['task_id'] not in added_ids)
    (active / 'train.jsonl').write_bytes(original)
    monkeypatch.setattr(dataset, 'DATASETS', active)
    before = dataset.rows('train')
    dataset.ensure_training_augmentation()
    after = dataset.rows('train')
    first = (active / 'train.jsonl').read_bytes()
    assert first.startswith(original)
    assert after[:len(before)] == before and after[len(before):] == added
    assert len(after) == len(before) + len(added)
    for row in added:
        task = {'initial_state': copy.deepcopy(row['initial_state'])}
        assert host.synthetic_training_spec(task, train_path=active / 'train.jsonl') == row['initial_state']['training_goal']
        assert task['task_id'] == row['task_id']
    dataset.ensure_training_augmentation()
    assert (active / 'train.jsonl').read_bytes() == first
    assert dataset.validate()['train'] == len(after)
    for split in ('val', 'holdout', 'comparison500'):
        assert (active / dataset.FILES[split]).read_bytes() == (canonical / dataset.FILES[split]).read_bytes()


@pytest.mark.parametrize('change', ['source', 'instruction', 'split', 'goal', 'task_id'])
def test_observation_debug_host_rejects_changed_scope(tmp_path, change):
    rows = dataset.rows('train')
    added = dataset.public_observation_training_rows()
    added_ids = {row['task_id'] for row in added}
    path = tmp_path / 'train.jsonl'
    path.write_text(''.join(json.dumps(row) + '\n' for row in rows
                            if row['task_id'] not in added_ids)
                    + ''.join(json.dumps(row) + '\n' for row in added))
    task = {'initial_state': copy.deepcopy(added[0]['initial_state'])}
    if change == 'source':
        task['initial_state']['source_task_id'] = dataset.rows('val')[0]['initial_state']['source_task_id']
    elif change == 'instruction':
        task['initial_state']['training_instruction'] += ' altered'
    elif change == 'split':
        task['split'] = 'holdout'
    elif change == 'goal':
        task['initial_state']['training_goal']['observation_debug'] = True
    else:
        task['task_id'] = 'another-task'
    with pytest.raises(RuntimeError, match='scope_invalid'):
        host.synthetic_training_spec(task, train_path=path)


def test_observation_debug_rejects_evaluation_source_family(monkeypatch):
    actual_rows = dataset.rows
    forbidden = copy.deepcopy(actual_rows('train')[0])
    forbidden['initial_state']['source_task_id'] = '07b42fd_3'
    monkeypatch.setattr(dataset, 'rows', lambda split: actual_rows(split) + [forbidden]
                        if split == 'val' else actual_rows(split))
    with pytest.raises(RuntimeError, match='public_source_overlap'):
        dataset.public_observation_training_rows()


def test_observation_debug_rejects_missing_public_source(monkeypatch):
    actual_rows = dataset.rows
    monkeypatch.setattr(dataset, 'rows', lambda split: [row for row in actual_rows(split)
        if row['initial_state']['source_task_id'] != '07b42fd_1']
        if split == 'train' else actual_rows(split))
    with pytest.raises(RuntimeError, match='public_source_missing'):
        dataset.public_observation_training_rows()


def test_observation_debug_rejects_existing_id_with_changed_body(tmp_path, monkeypatch):
    active = tmp_path / 'active'; active.mkdir()
    for name in [*dataset.FILES.values(), 'manifest.json']:
        shutil.copyfile(dataset.DATASETS / name, active / name)
    monkeypatch.setattr(dataset, 'DATASETS', active)
    dataset.ensure_training_augmentation()
    values = dataset.rows('train')
    row = next(row for row in values if row['attributes']['appworld'].get('observation_debug'))
    row['task_input'] += ' conflicting body'
    (active / 'train.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in values))
    before = (active / 'train.jsonl').read_bytes()
    with pytest.raises(RuntimeError, match='observation_task_conflict'):
        dataset.ensure_training_augmentation()
    assert (active / 'train.jsonl').read_bytes() == before


def test_observation_repeat_variants_append_only_two_and_preserve_old_sources(tmp_path, monkeypatch):
    monkeypatch.setattr(dataset, 'public_execution_curriculum_rows', lambda: [])
    canonical = dataset.DATASETS
    generated = dataset.public_observation_training_rows()
    old, new = generated[:2], generated[2:]
    existing = {row['task_id']: row for row in dataset.rows('train')}
    assert all(existing[row['task_id']] == row for row in old)
    active = tmp_path / 'active'; active.mkdir()
    new_ids = {row['task_id'] for row in new}
    for name in [*dataset.FILES.values(), 'manifest.json']:
        shutil.copyfile(canonical / name, active / name)
    original = b''.join(line for line in (active / 'train.jsonl').read_bytes().splitlines(keepends=True)
                        if json.loads(line)['task_id'] not in new_ids)
    (active / 'train.jsonl').write_bytes(original)
    monkeypatch.setattr(dataset, 'DATASETS', active)
    before = dataset.rows('train')
    dataset.ensure_training_augmentation()
    after = dataset.rows('train')
    assert (active / 'train.jsonl').read_bytes().startswith(original)
    assert after[:len(before)] == before and after[len(before):] == new
    assert dataset.read(active / 'manifest.json')['counts']['train'] == len(before) + 2
    for row in new:
        assert host.synthetic_training_spec({'initial_state': copy.deepcopy(row['initial_state'])},
            train_path=active / 'train.jsonl') == row['initial_state']['training_goal']
    read, mutation = new
    assert read['initial_state']['training_goal'] == old[0]['initial_state']['training_goal']
    assert mutation['initial_state']['training_goal'] == old[1]['initial_state']['training_goal']
    assert 'repeat that same read exactly once' in read['task_input']
    assert 'without calling the API again' in read['task_input']
    assert 'same product_id, quantity, and token exactly once' in mutation['task_input']
    assert 'do not invent an error if it returns successfully' in mutation['task_input']
    assert 'directed public readback' in mutation['task_input']
    assert 'do not repeat the mutation again' in mutation['task_input']
    for split in ('val', 'holdout', 'comparison500'):
        assert (active / dataset.FILES[split]).read_bytes() == (canonical / dataset.FILES[split]).read_bytes()


@pytest.mark.parametrize('stop_at', ['prepare', 'train', 'seed', None])
def test_new_training_scope_refreshes_complete_receipts_only_through_normal_prefix(tmp_path, monkeypatch, stop_at):
    import learning
    import reproduce
    seed = tmp_path / 'new-candidate/strategy.json'
    state = {'stage_results': {'prepare': {'status': 'complete'},
                              'train': {'status': 'complete', 'data': {'original_receipt': 'retained'}}}}
    def incomplete(controller):
        raise RuntimeError('appworld_training_not_complete')
    monkeypatch.setattr(learning, '_completed_training_tasks', incomplete)
    assert reproduce._refresh_training_scope_receipts(state, seed, stop_at) is True
    for name in ('prepare', 'train'):
        assert state['stage_results'][name]['status'] == 'pending'
        assert state['stage_results'][name]['reason'] == 'appworld_training_scope_requires_resume'
    assert state['stage_results']['train']['data']['original_receipt'] == 'retained'


@pytest.mark.parametrize('stop_at', ['pilot32', 'validation100', 'gepa_prepare',
                                    'gepa_run', 'comparison500', 'publish', None])
def test_frozen_candidate_keeps_original_training_snapshot_after_new_train_tasks(tmp_path, monkeypatch, stop_at):
    import learning
    import reproduce
    seed = tmp_path / 'frozen-candidate/strategy.json'; seed.parent.mkdir()
    seed.write_text('{"level":"L3"}')
    state = {'stage_results': {'prepare': {'status': 'complete'}, 'train': {'status': 'complete'},
                              'seed': {'status': 'complete', 'artifact': str(seed)},
                              'pilot32': {'status': 'complete', 'gate_passed': True}}}
    before = copy.deepcopy(state)
    def must_not_revalidate(controller):
        raise AssertionError('frozen validation must not change its TRAIN snapshot')
    monkeypatch.setattr(learning, '_completed_training_tasks', must_not_revalidate)
    assert reproduce._refresh_training_scope_receipts(state, seed, stop_at) is False
    assert state == before


@pytest.mark.parametrize('reason', ['appworld_training_completion_invalid',
                                    'appworld_training_capture_or_import_incomplete'])
def test_training_scope_unknown_or_wrong_native_coverage_needs_resume(tmp_path, monkeypatch, reason):
    import learning
    import reproduce
    state = {'stage_results': {'prepare': {'status': 'complete'}, 'train': {'status': 'complete'}}}
    def reject(controller):
        raise RuntimeError(reason)
    monkeypatch.setattr(learning, '_completed_training_tasks', reject)
    assert reproduce._refresh_training_scope_receipts(state, tmp_path / 'new.json', 'train') is True
    assert state['stage_results']['train']['data']['completion_error'] == reason


def test_exact_native_training_coverage_preserves_complete_receipt(tmp_path, monkeypatch):
    import learning
    import reproduce
    state = {'stage_results': {'prepare': {'status': 'complete'}, 'train': {'status': 'complete'}}}
    before = copy.deepcopy(state)
    calls = []
    monkeypatch.setattr(learning, '_completed_training_tasks', lambda controller: calls.append(controller) or {'real-training-task'})
    assert reproduce._refresh_training_scope_receipts(state, tmp_path / 'new.json', 'seed') is False
    assert calls == [reproduce._APPWORLD_LAYOUT] and state == before


def test_normal_lifecycle_reenters_train_for_added_scope_without_running_other_stages(tmp_path, monkeypatch):
    import learning
    import reproduce
    seed = tmp_path / 'published/new/strategy.json'
    identity = {'seed_label': 'new', 'seed_artifact': str(seed.resolve())}
    state_path = tmp_path / 'gepa/lifecycle.json'; state_path.parent.mkdir()
    state_path.write_text(json.dumps({'candidate': identity, 'stage_results': {
        'prepare': {'status': 'complete'}, 'train': {'status': 'complete'},
    }}))
    monkeypatch.setattr(reproduce, 'OUTPUT', tmp_path)
    monkeypatch.setattr(reproduce, '_EXPERIMENT_TRACKING', False)
    monkeypatch.setattr(reproduce._AppWorldDatasetAdapter, 'output_root', tmp_path)
    monkeypatch.setattr(reproduce._AppWorldDatasetAdapter, 'split_counts', staticmethod(lambda: {'train': 130, 'val': 12}))
    monkeypatch.setattr(reproduce, '_candidate_identity', lambda *args: identity)
    def incomplete(controller):
        raise RuntimeError('appworld_training_not_complete')
    monkeypatch.setattr(learning, '_completed_training_tasks', incomplete)
    calls = []
    monkeypatch.setattr(reproduce, 'prepare', lambda: calls.append('prepare'))
    monkeypatch.setattr(reproduce, 'train_with_recovery', lambda: calls.append('train'))
    monkeypatch.setattr(reproduce, 'run', lambda *args, **kwargs: pytest.fail('no candidate/API process is allowed'))
    result = reproduce._run_common_all(seed, 'new', stop_at='train')
    assert calls == ['prepare', 'train']
    assert result['status'] == 'complete' and result['phase'] == 'train'
    assert result['stage_results']['train']['data']['callback'] == 'appworld.training'
    assert 'seed' not in result['stage_results']
