"""Independent public TRAIN contracts and active-pack isolation."""
import copy
import json
from pathlib import Path
import sys
from types import SimpleNamespace
import pytest

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'tools/trajectory_evolution/experiments/appworld'))
sys.path.insert(0, str(ROOT / 'examples/agent/business-simulator/appworld'))
sys.path.insert(0, str(ROOT / 'packages/agentflow-runtime/src'))
import dataset
import appworld_host as host


def world_with_answer(answer=None):
    return SimpleNamespace(apis=SimpleNamespace(supervisor=SimpleNamespace(
        show_active_task=lambda: {'status': 'success', 'answer': answer})))


def test_pending_pack_is_nine_train_sources_without_active_mutation():
    path = dataset.DATASETS / 'train.jsonl'
    original = path.read_bytes()
    rows = dataset.pending_training_rows()
    assert len(rows) == 9 and len({r['task_id'] for r in rows}) == 9
    assert all(r['split'] == 'train' for r in rows)
    official = {r['initial_state']['source_task_id'] for r in dataset.rows('train')
                if not r['initial_state'].get('training_goal')}
    assert all(r['initial_state']['source_task_id'] in official for r in rows)
    assert path.read_bytes() == original
    assert dataset.pending_training_rows() == rows


@pytest.mark.parametrize('index', range(9))
def test_pending_admission_requires_exact_official_train_fixture(tmp_path, monkeypatch, index):
    rows = dataset.pending_training_rows()
    path = tmp_path / 'public-train.jsonl'
    path.write_text('\n'.join(json.dumps(r) for r in rows))
    monkeypatch.setenv('APPWORLD_SYNTHETIC_TRAIN_PACK', str(path))
    task = {'initial_state': copy.deepcopy(rows[index]['initial_state'])}
    assert host.synthetic_training_spec(task) == rows[index]['initial_state']['training_goal']
    assert task['task_id'] == rows[index]['task_id']
    bad = copy.deepcopy(task)
    bad['initial_state']['source_task_id'] = '0de03ea_1'
    with pytest.raises(RuntimeError, match='public_source_invalid'):
        host.synthetic_training_spec(bad)
    with pytest.raises(RuntimeError, match='scope_invalid'):
        host.synthetic_training_spec(dict(task, split='holdout'))


@pytest.mark.parametrize('queue,success', [([1, 2, 3], True), ([1, 2], False), ([4, 5, 6], False)])
def test_offline_album_checks_downloaded_subset_and_full_album_queue(monkeypatch, queue, success):
    goal = {'kind': 'offline_album_downloaded_duration', 'min_duration_seconds': 400}
    before = {'albums': {10: [1, 2, 3], 11: [4, 5, 6]}, 'downloaded': {1: 210, 2: 220, 4: 300}, 'queue': []}
    after = dict(before, queue=[{'song_id': s, 'is_current': i == 0, 'is_playing': True}
                               for i, s in enumerate(queue)])
    monkeypatch.setattr(host, 'synthetic_training_snapshot', lambda world, goal: after)
    result = host.synthetic_training_metrics(world_with_answer(), goal, before)
    assert result['strict_passed'] == int(success)
    assert result['official_goal_evaluated'] is False


@pytest.mark.parametrize('following,success', [({7, 10, 11}, True), ({7, 10}, False), ({7, 10, 11, 12}, False)])
def test_genre_alternatives_deduplicate_artists_and_preserve_existing(monkeypatch, following, success):
    goal = {'kind': 'spotify_genre_artist_set', 'genres': ['classical', 'indie']}
    before = {'playlists': {1: {'song_ids': [1, 2, 3]}}, 'following': {7}, 'songs': {
        1: {'genre': ' Classical ', 'artists': [{'id': 10}]},
        2: {'genre': 'INDIE', 'artists': [{'id': 10}, {'id': 11}]},
        3: {'genre': 'pop', 'artists': [{'id': 12}]}}}
    after = dict(before, following=following)
    monkeypatch.setattr(host, 'synthetic_training_snapshot', lambda world, goal: after)
    assert host.synthetic_training_metrics(world_with_answer(), goal, before)['strict_passed'] == int(success)


@pytest.mark.parametrize('answer,success', [('3.30', True), ('1.10', False), ('33.30', False), ('NaN', False)])
def test_date_window_includes_both_dates_excludes_opposite_direction(monkeypatch, answer, success):
    goal = {'kind': 'venmo_date_direction_total', 'collection': 'transactions', 'direction': 'received', 'days': 5}
    def row(day, amount, receiver='me@example.com'):
        return {'created_at': day, 'amount': amount,
                'sender': {'email': 'other@example.com'}, 'receiver': {'email': receiver}}
    before = {'today': '2023-06-04', 'email': 'me@example.com', 'rows': {
        1: row('2023-05-31T00:00:00', 1.1), 2: row('2023-06-04T23:59:59', 2.2),
        3: row('2023-05-30T23:59:59', 10), 4: row('2023-06-05T00:00:00', 20),
        5: row('2023-06-02T12:00:00', 30, 'other@example.com')}}
    monkeypatch.setattr(host, 'synthetic_training_snapshot', lambda world, goal: before)
    assert host.synthetic_training_metrics(world_with_answer(answer), goal, before)['strict_passed'] == int(success)


def test_activation_appends_exactly_nine_without_rewriting_existing_rows(tmp_path, monkeypatch):
    import shutil
    canonical = dataset.DATASETS
    active = tmp_path / 'active'
    active.mkdir()
    for name in [*dataset.FILES.values(), 'manifest.json']:
        shutil.copyfile(canonical / name, active / name)
    monkeypatch.setattr(dataset, 'DATASETS', active)
    pending_ids = {row['task_id'] for row in dataset.pending_training_rows()}
    original_rows = [row for row in dataset.rows('train') if row['task_id'] not in pending_ids]
    (active / 'train.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in original_rows))
    manifest = dataset.read(active / 'manifest.json')
    manifest['counts']['train'] = len(original_rows)
    dataset.write(active / 'manifest.json', manifest)
    original = (active / 'train.jsonl').read_bytes()
    pending = tmp_path / 'pending'
    (pending / 'native').mkdir(parents=True)
    (pending / 'native/manifest.json').write_text('{}')
    prepared = dataset.pending_training_rows()
    (pending / 'public-train.jsonl').write_text('\n'.join(json.dumps(r) for r in prepared) + '\n')
    additions = dataset.activate_pending_training(pending)
    assert additions == [r['task_id'] for r in prepared]
    assert (active / 'train.jsonl').read_bytes().startswith(original)
    assert dataset.validate()['train'] == len(original.splitlines()) + 9
    once = (active / 'train.jsonl').read_bytes()
    assert dataset.activate_pending_training(pending) == []
    assert (active / 'train.jsonl').read_bytes() == once
    for split in ('val', 'holdout', 'comparison500'):
        assert (active / dataset.FILES[split]).read_bytes() == (canonical / dataset.FILES[split]).read_bytes()
