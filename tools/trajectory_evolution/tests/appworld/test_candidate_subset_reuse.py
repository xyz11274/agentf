"""Model-free same-artifact native subset reuse; every output is temporary."""
from copy import deepcopy
import json
from pathlib import Path
import sys
from unittest.mock import patch

import pytest

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'tools/trajectory_evolution/experiments/appworld'))
import compare
import layout
import reproduce
import stage_gate
from test_appworld_compare_metrics import native_row


def test_candidate_state_preserves_live_gepa_and_inherits_review_history(tmp_path, monkeypatch):
    monkeypatch.setattr(reproduce, 'OUTPUT', tmp_path)
    monkeypatch.setattr(reproduce, '_CANDIDATE_STATE_LABEL', 'new-candidate')
    monkeypatch.setattr(reproduce, '_EXPERIMENT_LABEL', 'new-candidate')
    monkeypatch.setattr(reproduce, '_EXPERIMENT_TRACKING', True)
    global_status = tmp_path / 'gepa/status.json'
    layout.save(global_status, {'job_id': 'live-job', 'status': 'running', 'seed_search': {
        'round': 3, 'status': 'searching', 'consecutive_failures': 2,
        'attempts': [], 'reviews': [{'round': 2, 'summary': 'real retained review'}]}})
    original = global_status.read_bytes()
    reproduce._experiment_status('running', 'validation100')
    search = reproduce.seed_search()
    assert search['round'] == 3 and search['consecutive_failures'] == 2
    assert search['reviews'][0]['summary'] == 'real retained review'
    search['current_seed'] = 'new-seed'
    reproduce._save_search(search)
    assert global_status.read_bytes() == original
    isolated = tmp_path / 'gepa/candidates/new-candidate/status.json'
    assert layout.read(isolated)['experiment']['seed_label'] == 'new-candidate'
    assert layout.read(isolated)['seed_search']['current_seed'] == 'new-seed'


def test_candidate_state_does_not_take_another_global_pending_stage(tmp_path, monkeypatch):
    monkeypatch.setattr(reproduce, 'OUTPUT', tmp_path)
    monkeypatch.setattr(reproduce, '_CANDIDATE_STATE_LABEL', 'new-candidate')
    monkeypatch.setattr(reproduce, '_matching_attempt', lambda *a, **k: None)
    monkeypatch.setattr(reproduce, '_host_version', lambda: 'fixture-host')
    seed = tmp_path / 'published/new-candidate/strategy.json'
    layout.save(seed, {'policy': {'frozen': True}})
    other = tmp_path / 'gepa/seed-search-9-stage32/comparison.json'
    layout.save(tmp_path / 'gepa/status.json', {'job_id': 'live-job', 'seed_search': {
        'round': 2, 'status': 'searching', 'attempts': [{
            'attempt': 9, 'round': 2, 'status': 'pending', 'seed': 'another-seed',
            'pilot_comparison': str(other),
            'validation_comparison': str(other.parent.with_name('seed-search-9-stage100') / 'comparison.json')}]}})
    old = (tmp_path / 'gepa/status.json').read_bytes()
    attempt = reproduce.begin_seed_attempt(seed)
    root = tmp_path / 'gepa/candidates/new-candidate'
    assert attempt['attempt'] == 10
    assert Path(attempt['pilot_comparison']).is_relative_to(root)
    assert Path(attempt['validation_comparison']).is_relative_to(root)
    assert (tmp_path / 'gepa/status.json').read_bytes() == old
    assert reproduce.seed_search()['attempts'][0]['seed'] == 'another-seed'


def test_candidate_state_common_receipt_does_not_reset_running_gepa(tmp_path, monkeypatch):
    monkeypatch.setattr(reproduce, 'OUTPUT', tmp_path)
    monkeypatch.setattr(reproduce, '_CANDIDATE_STATE_LABEL', 'new-candidate')
    monkeypatch.setattr(reproduce._AppWorldDatasetAdapter, 'output_root', tmp_path)
    monkeypatch.setattr(reproduce._AppWorldDatasetAdapter, 'split_counts',
                        staticmethod(lambda: {'train': 132, 'val': 12}))
    monkeypatch.setattr(reproduce, '_refresh_training_scope_receipts', lambda *a: None)
    monkeypatch.setattr(reproduce, '_resume_pending_comparison_receipts', lambda *a: None)
    monkeypatch.setattr(reproduce, '_host_version', lambda: 'fixture-host')
    monkeypatch.setattr(reproduce, 'run', lambda *a, **k: pytest.fail('completed training rerun'))
    seed = tmp_path / 'published/new-candidate/strategy.json'
    layout.save(seed, {'policy': {'frozen': True}})
    global_path = tmp_path / 'gepa/lifecycle.json'
    layout.save(global_path, {'candidate': {'seed_label': 'gepa-parent'},
                            'status': 'running', 'phase': 'gepa_run'})
    original = global_path.read_bytes()
    local = tmp_path / 'gepa/candidates/new-candidate/lifecycle.json'
    layout.save(local, {'candidate': reproduce._candidate_identity(seed, 'new-candidate'),
        'stage_results': {name: {'status': 'complete'} for name in ('prepare', 'train')}})
    result = reproduce._run_common_all(seed, 'new-candidate', stop_at='train')
    assert result['status'] == 'complete' and result['phase'] == 'train'
    assert global_path.read_bytes() == original
    assert layout.read(local)['candidate']['seed_label'] == 'new-candidate'


@pytest.mark.parametrize('action', ['--gepa-prepare', '--gepa-run', '--all', '--publish'])
def test_candidate_state_rejects_second_native_gepa_writer(tmp_path, monkeypatch, action):
    monkeypatch.setattr(reproduce, 'OUTPUT', tmp_path)
    monkeypatch.setattr(reproduce, '_CANDIDATE_STATE_LABEL', None)
    monkeypatch.setattr(sys, 'argv', ['reproduce', action, '--candidate-state', '--seed-label', 'new-candidate'])
    with pytest.raises(SystemExit) as error:
        reproduce.main()
    assert error.value.code == 2
    assert not list(tmp_path.rglob('*.json'))


def test_default_controller_state_paths_remain_unchanged(tmp_path, monkeypatch):
    monkeypatch.setattr(reproduce, 'OUTPUT', tmp_path)
    monkeypatch.setattr(reproduce, '_CANDIDATE_STATE_LABEL', None)
    monkeypatch.setattr(reproduce, '_EXPERIMENT_TRACKING', True)
    monkeypatch.setattr(reproduce, '_EXPERIMENT_LABEL', 'legacy')
    reproduce._experiment_status('running', 'gepa_run')
    assert layout.read(tmp_path / 'gepa/status.json')['experiment']['seed_label'] == 'legacy'
    assert not (tmp_path / 'gepa/candidates').exists()


@pytest.mark.parametrize('invalid', [None, 'version', 'scope', 'unpublished', 'incomplete', 'new-seed'])
def test_frozen_validation_reuses_only_real_completed_training_prefix(tmp_path, monkeypatch, invalid):
    monkeypatch.setattr(reproduce, 'OUTPUT', tmp_path)
    monkeypatch.setattr(reproduce, '_CANDIDATE_STATE_LABEL', 'frozen')
    monkeypatch.setattr(reproduce._APPWORLD_LAYOUT, 'ids', lambda split: ['train-old', 'train-new'])
    monkeypatch.setattr(reproduce._APPWORLD_LAYOUT, 'version', lambda artifact: 'actual-native-version')
    seed = tmp_path / 'published/frozen/strategy.json'
    layout.save(seed, {'policy': {'immutable': True}})
    manifest = {'published': True, 'version': 'actual-native-version',
                'training_input_tasks': ['train-old'], 'planned_training_count': 1}
    prior = {'status': 'running', 'phase': 'gepa_run',
             'dataset': {'split_counts': {'train': 1}},
             'stage_results': {name: {'status': 'complete', 'data': {'real_receipt': name}}
                               for name in ('prepare', 'train')}}
    if invalid == 'version': manifest['version'] = 'different-version'
    if invalid == 'scope': manifest['training_input_tasks'] = ['train-new']
    if invalid == 'unpublished': manifest['published'] = False
    if invalid == 'incomplete': prior['stage_results']['train']['status'] = 'running'
    layout.save(seed.with_name('manifest.json'), manifest)
    path = tmp_path / 'gepa/lifecycle.json'
    layout.save(path, prior)
    original = path.read_bytes()
    local = {'stage_results': {'train': {'status': 'failed', 'reason': 'another_training_running'}}}
    inherited = reproduce._inherit_frozen_training_receipts(
        local, seed, 'seed' if invalid == 'new-seed' else 'validation100')
    assert inherited is (invalid is None)
    if inherited:
        assert local['stage_results']['train'] == prior['stage_results']['train']
        assert local['frozen_training_receipt_source']['candidate_training_count'] == 1
    else:
        assert local['stage_results']['train']['status'] == 'failed'
    assert path.read_bytes() == original


@pytest.mark.parametrize('wrong', [None, 'version', 'scope'])
def test_frozen_gepa_materialization_keeps_current_train_and_requires_native_version(tmp_path, monkeypatch, wrong):
    import dataset
    from types import SimpleNamespace
    monkeypatch.setattr(dataset, 'CACHE', tmp_path / 'cache')
    monkeypatch.setattr(dataset, 'validate', lambda: {'train': 3, 'val': 1, 'holdout': 1})
    values = {'train': [{'task_id': 'old-1'}, {'task_id': 'old-2'}, {'task_id': 'new-3'}],
              'val': [{'task_id': 'val'}], 'holdout': [{'task_id': 'holdout'}]}
    monkeypatch.setattr(dataset, 'rows', lambda split: deepcopy(values[split]))
    calls = []
    def native(command, **kwargs):
        calls.append(command)
        if 'build' in command:
            built = Path(command[command.index('--output-root') + 1])
            config = Path(command[command.index('--config') + 1])
            tasks = [json.loads(line) for line in (config.parent / 'tasks.jsonl').read_text().splitlines()]
            built.mkdir()
            for split in ('train', 'val', 'holdout'):
                (built / (split + '.jsonl')).write_text(''.join(json.dumps(t) + '\n' for t in tasks if t['split'] == split))
            dataset.write(built / 'manifest.json', {'metadata': {
                'dataset_version': 'changed' if wrong == 'version' else 'real-native-original'}})
        return SimpleNamespace(returncode=0, stdout='', stderr='')
    monkeypatch.setattr(dataset.subprocess, 'run', native)
    current = dataset.CACHE / 'gepa'
    current.mkdir(parents=True)
    for split, rows in [('train', values['train']), ('val', values['val']), ('holdout', values['holdout'])]:
        (current / (split + '.jsonl')).write_text(''.join(json.dumps(r) + '\n' for r in rows))
    dataset.write(current / 'manifest.json', {'metadata': {'dataset_version': 'current-134'}})
    original = {p.name: p.read_bytes() for p in current.iterdir()}
    frozen = ['old-1', 'new-3'] if wrong == 'scope' else ['old-1', 'old-2']
    if wrong:
        with pytest.raises(RuntimeError, match='appworld_frozen_gepa_'):
            dataset.materialize(Path('native'), view='gepa', frozen_training_tasks=frozen,
                                expected_dataset_version='real-native-original')
        assert {p.name: p.read_bytes() for p in current.iterdir()} == original
    else:
        result = dataset.materialize(Path('native'), view='gepa', frozen_training_tasks=frozen,
                                     expected_dataset_version='real-native-original')
        assert result['counts']['train'] == 3
        assert [json.loads(line)['task_id'] for line in (current / 'train.jsonl').read_text().splitlines()] == frozen
        assert values['train'][-1]['task_id'] == 'new-3'
        assert len(calls) == 2


def test_prepare_preserves_running_gepa_dataset_scope(tmp_path, monkeypatch):
    import dataset
    monkeypatch.setattr(reproduce, 'OUTPUT', tmp_path)
    layout.save(tmp_path / 'gepa/submission.json', {'job_id': 'live'})
    layout.save(tmp_path / 'gepa/workspace/agent-evaluation/state.json', {'jobs': {'live': {'status': 'running'}}})
    views = []
    monkeypatch.setattr(dataset, 'materialize', lambda binary, *, view: views.append(view))
    monkeypatch.setattr(reproduce._APPWORLD_LAYOUT, 'prepare', lambda: None)
    reproduce.prepare()
    assert views == [view for view in dataset.VIEWS if view != 'gepa']


@pytest.fixture
def retained(tmp_path, monkeypatch):
    artifact = {'level': 'L3', 'agent': {'revision': 'fixture-host'}, 'policy': {'unchanged': True}}
    seed = tmp_path / 'published/frozen/strategy.json'
    source = tmp_path / 'gepa/seed-search-1-stage500/comparison.json'
    shard = source.parent / 'shards/shard-00'
    report = shard / 'regrade/report.json'
    l0 = tmp_path / 'gepa/l0-current-stage500/result.json'
    dataset = tmp_path / 'training/datasets/comparison500'
    projection = layout.judge_projection()
    ids = [f'task-{i:03d}' for i in range(40)]
    tasks = [{'task_id': t, 'task_version': 'task-v1'} for t in ids]
    variant = {'id': 'candidate', 'artifact': artifact, 'version': 'frozen-version',
               'command': [str(layout.PYTHON), '-B', str(layout.HOST), '--run-entry']}
    rows = []
    for i, task in enumerate(ids):
        row = native_row(task, quality=1, verdict='passed' if i < 25 else 'failed')
        row.update(task_version='task-v1', variant_id='candidate')
        row['checks'] = [{'check_id': 'model', 'status': 'passed'},
                         {'check_id': 'strict', 'status': 'passed' if i < 25 else 'failed'}]
        row['evidence']['loaded_version'] = 'frozen-version'
        rows.append(row)
    compact = [compare.compact_row(row, str(shard)) for row in rows]
    for row in compact:
        row.update(review_status='complete', loop_review_status='complete')
    progress = {'artifact': artifact, 'planned_tasks': 40, 'seed': str(seed),
                'items': {r['task_id']: r for r in compact},
                'shards': {'0': {'label': 'shard-00', 'complete': True, 'execution_complete': True,
                                 'judge_projection': projection, 'source_report': str(report),
                                 'task_count': 40}}}
    comparison = {'seed': str(seed), 'status': 'complete', 'host_version': 'fixture-host',
                  'judge_projection': projection, 'items': compact, 'progress': str(source.with_name('progress.json')),
                  'metrics': {'frozen': compare.summarize(compact, 40)}}
    native = {'complete': True, 'run_id': 'original-native-run', 'dataset_version': 'dataset-v1',
              'configuration': {'judge_fingerprint': 'existing-frozen-fp', 'model_enabled': True,
                                'allow_unreviewed': False, 'variants': [{'id': 'candidate', 'version': 'frozen-version'}]},
              'trials': rows}
    manifest = {'complete': True, 'run_id': native['run_id'], 'dataset_version': 'dataset-v1',
                'files': {'report.json': 'existing-native-report-digest'},
                'summary': {'planned_trials': 40, 'scored': 40, 'errors': 0, 'uncertain': 0}}
    def write(path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value))
    write(seed, artifact)
    write(source, comparison)
    write(source.with_name('progress.json'), progress)
    write(shard.with_suffix('.json'), dict(progress['shards']['0'], artifact=artifact, task_ids=ids))
    write(shard / 'config.json', {'variants': [variant], 'judge': projection})
    write(report, native)
    write(report.with_name('manifest.json'), manifest)
    write(dataset / 'manifest.json', {'metadata': {'dataset_version': 'dataset-v1'}})
    (dataset / 'holdout.jsonl').write_text(''.join(json.dumps(t) + '\n' for t in tasks))
    baseline_rows = deepcopy(compact)
    for i, row in enumerate(baseline_rows):
        row.update(verdict='passed' if i < 22 else 'failed', quality=1 if i < 22 else 0,
                   invalid_loop_count=1, loop_waste_count=1)
    write(l0, {'status': 'complete', 'host_version': 'fixture-host', 'judge_projection': projection,
               'items': baseline_rows, 'source_report': str(report)})
    monkeypatch.setattr(reproduce, 'OUTPUT', tmp_path)
    monkeypatch.setattr(reproduce, '_host_version', lambda: 'fixture-host')
    monkeypatch.setattr(layout, 'host_version', lambda: 'fixture-host')
    monkeypatch.setattr(reproduce._APPWORLD_LAYOUT, 'version', lambda a: 'frozen-version')
    monkeypatch.setattr(reproduce, '_stage_l0_source', lambda count, comparison=None: l0)
    return dict(root=tmp_path, seed=seed, source=source, report=report, l0=l0, ids=ids,
                native=native, manifest=manifest, progress=progress, comparison=comparison, write=write)


def test_normal_subset_view_keeps_source_scores_cost_unknown_and_no_agent_or_judge(retained, monkeypatch):
    f = retained
    for row in f['comparison']['items']:
        row.update(tokens=None, model_calls=None, latency_ms=None, tokens_complete=False,
                   execution_costs_complete=False)
    f['progress']['items'] = {r['task_id']: r for r in f['comparison']['items']}
    f['write'](f['source'], f['comparison'])
    f['write'](f['source'].with_name('progress.json'), f['progress'])
    originals = {p: p.read_bytes() for p in (f['seed'], f['source'], f['report'], f['report'].with_name('manifest.json'))}
    monkeypatch.setattr(reproduce, 'run', lambda *a, **k: pytest.fail('healthy execution or judge rerun'))
    path = reproduce.reuse_completed_candidate_subset(f['seed'], 32)
    assert path == f['source'].parent / 'stage32/comparison.json'
    value = layout.read(path)
    assert [r['task_id'] for r in value['items']] == f['ids'][:32]
    assert value['items'] == f['comparison']['items'][:32]
    assert value['metrics']['frozen']['tokens'] is None
    assert value['metrics']['frozen']['execution_costs_complete'] is False
    assert value['candidate_source_comparison'] == str(f['source'])
    assert all(p.read_bytes() == old for p, old in originals.items())
    assert stage_gate._current_judge_profiles(value, f['l0'])
    value['items'][0] = dict(value['items'][0], quality=.5)
    assert not stage_gate._current_judge_profiles(value, f['l0'])


@pytest.fixture
def delivery_source(retained, monkeypatch):
    f = retained
    ids = [f'task-{i:03d}' for i in range(500)]
    rows = []
    for i, task in enumerate(ids):
        row = native_row(task, quality=1, verdict='passed' if i < 338 else 'failed')
        row.update(task_version='task-v1', variant_id='candidate')
        row['checks'] = [{'check_id': 'model', 'status': 'passed'},
                         {'check_id': 'strict', 'status': 'passed' if i < 338 else 'failed'}]
        row['evidence']['loaded_version'] = 'frozen-version'
        rows.append(row)
    compact = [compare.compact_row(row, 'shard-00') for row in rows]
    for row in compact:
        row.update(review_status='complete', loop_review_status='complete',
                   tokens=None, model_calls=None, tokens_complete=False,
                   execution_costs_complete=False)
    f['native']['trials'] = rows
    f['manifest']['summary'].update(planned_trials=500, scored=500)
    f['progress'].update(planned_tasks=500, items={r['task_id']: r for r in compact})
    f['progress']['shards']['0']['task_count'] = 500
    f['comparison'].update(items=compact, source_l0=str(f['l0']),
                           metrics={'frozen': compare.summarize(compact, 500)})
    f['write'](f['source'], f['comparison'])
    f['write'](f['source'].with_name('progress.json'), f['progress'])
    shard = f['source'].parent / 'shards/shard-00'
    checkpoint = layout.read(shard.with_suffix('.json'))
    checkpoint.update(task_ids=ids, task_count=500)
    f['write'](shard.with_suffix('.json'), checkpoint)
    f['write'](f['report'], f['native'])
    f['write'](f['report'].with_name('manifest.json'), f['manifest'])
    f['write'](f['seed'].with_name('manifest.json'), {
        'published': True, 'version': 'frozen-version', 'training_input_tasks': ['real-train'],
        'planned_training_count': 1, 'parent_artifact': 'historical-description'})
    dataset = f['root'] / 'training/datasets/comparison500'
    (dataset / 'holdout.jsonl').write_text(''.join(json.dumps({
        'task_id': task, 'task_version': 'task-v1'}) + '\n' for task in ids))
    baseline = layout.read(f['l0'])
    baseline['items'] = deepcopy(compact)
    for i, row in enumerate(baseline['items']):
        row.update(verdict='passed' if i < 322 else 'failed', quality=1 if i < 322 else 0,
                   invalid_loop_count=1, loop_waste_count=1)
    f['write'](f['l0'], baseline)
    f['write'](f['root'] / 'gepa/status.json', {'status': 'running', 'job_id': 'unchanged-job'})
    f['write'](f['root'] / 'gepa/lifecycle.json', {'status': 'running', 'phase': 'gepa_run'})
    monkeypatch.setattr(reproduce._APPWORLD_LAYOUT, 'baseline_result',
                        lambda count, output_root=None: f['l0'])
    # Baseline certification has its own existing regression suite; this fixture
    # exercises the complete candidate source and the unchanged formal goal.
    monkeypatch.setattr(reproduce, '_current_l0500_ready', lambda path: path == f['l0'])
    monkeypatch.setattr(reproduce, '_baseline_native_header', lambda path: {
        'configuration': {'judge_fingerprint': 'existing-frozen-fp'}})
    monkeypatch.setattr(reproduce, 'run', lambda *a, **k: pytest.fail('API or Agent call'))
    f['ids'] = ids
    return f


def test_explicit_delivery_keeps_failed_goal_cost_unknown_and_running_gepa(delivery_source):
    f = delivery_source
    protected = [f['seed'], f['source'], f['report'], f['report'].with_name('manifest.json'),
                 f['root'] / 'gepa/status.json', f['root'] / 'gepa/lifecycle.json']
    before = {path: path.read_bytes() for path in protected}
    manifest = reproduce.deliver_best_available(f['seed'], f['source'])
    target = f['root'] / 'published/strategy'
    assert {p.name for p in target.iterdir()} == {'strategy.json', 'manifest.json', 'comparison.json'}
    assert layout.read(target / 'strategy.json') == layout.read(f['seed'])
    assert manifest['version'] == 'frozen-version'
    assert manifest['release_status'] == 'best_available_not_qualified'
    assert manifest['goal_passed'] is False and manifest['gepa_completion_claimed'] is False
    assert manifest['goal_checks']['candidate']['strict_passed'] == 338
    assert layout.read(target / 'comparison.json')['items'] == f['comparison']['items']
    assert layout.read(target / 'comparison.json')['metrics']['frozen']['tokens'] is None
    assert all(path.read_bytes() == original for path, original in before.items())
    observed = reproduce.status(f['seed'])
    assert observed['delivery']['release_status'] == 'best_available_not_qualified'
    assert observed['published']['status'] == 'pending'
    assert observed['gepa']['status'] == 'running'
    assert observed['lifecycle']['status'] == 'running'
    f['write'](f['root'] / 'published/gepa-final/manifest.json', {
        'release_status': 'published_goal_passed', 'goal_passed': True, 'version': 'qualified-version'})
    observed = reproduce.status(f['seed'])
    assert observed['published']['release_status'] == 'published_goal_passed'
    assert observed['published']['goal_passed'] is True
    assert observed['delivery']['goal_passed'] is False


@pytest.mark.parametrize('change', ['partial', 'version', 'judge'])
def test_explicit_delivery_rejects_partial_changed_version_or_judge(delivery_source, change):
    f = delivery_source
    if change == 'partial':
        f['comparison']['status'] = 'running'
        f['write'](f['source'], f['comparison'])
    elif change == 'version':
        manifest = layout.read(f['seed'].with_name('manifest.json'))
        manifest['version'] = 'wrong-native-version'
        f['write'](f['seed'].with_name('manifest.json'), manifest)
    else:
        f['native']['configuration']['judge_fingerprint'] = 'wrong-frozen-judge'
        f['write'](f['report'], f['native'])
    with pytest.raises(RuntimeError, match='appworld_(delivery|candidate_subset)_'):
        reproduce.deliver_best_available(f['seed'], f['source'])
    assert not (f['root'] / 'published/strategy').exists()


def test_fresh_guard_checks_real_training_lock_without_mutating_sources(tmp_path, monkeypatch):
    import fcntl
    monkeypatch.setattr(reproduce, 'OUTPUT', tmp_path)
    monkeypatch.setattr(reproduce, '_active_output_processes', lambda: [])
    lock_path = tmp_path / 'training/full-training.lock'
    lock_path.parent.mkdir()
    lock_path.write_text('existing lock identity')
    original = lock_path.read_bytes()
    with lock_path.open('r') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        with pytest.raises(RuntimeError, match='appworld_fresh_training_writer_active'):
            reproduce.assert_idle_for_fresh()
    reproduce.assert_idle_for_fresh()
    assert lock_path.read_bytes() == original


@pytest.mark.parametrize('case', ['live', 'stale', 'closed', 'malformed'])
def test_fresh_guard_does_not_delete_or_contact_another_session(tmp_path, monkeypatch, case):
    monkeypatch.setattr(reproduce, 'OUTPUT', tmp_path)
    monkeypatch.setattr(reproduce, '_active_output_processes', lambda: [123] if case == 'live' else [])
    path = tmp_path / 'gepa/workspace/agent-evaluation/state.json'
    layout.save(path, {'jobs': {'original': {'status': 'cancelled' if case == 'closed' else 'running'}}})
    if case == 'malformed':
        path.write_text('{truncated')
    before = path.read_bytes()
    if case == 'closed':
        reproduce.assert_idle_for_fresh()
    else:
        with pytest.raises(RuntimeError, match='appworld_fresh_'):
            reproduce.assert_idle_for_fresh()
    assert path.read_bytes() == before


@pytest.mark.parametrize('change', ['artifact', 'fingerprint', 'task_version', 'loaded_version',
                                   'manifest', 'compact', 'duplicate', 'unknown_fp', 'partial', 'run'])
def test_subset_rejects_unproven_or_changed_source(retained, change):
    f = retained
    if change == 'artifact':
        f['progress']['artifact'] = {'changed': True}
        f['write'](f['source'].with_name('progress.json'), f['progress'])
    elif change == 'fingerprint': f['native']['configuration']['judge_fingerprint'] = 'different'
    elif change == 'unknown_fp': del f['native']['configuration']['judge_fingerprint']
    elif change == 'task_version': f['native']['trials'][0]['task_version'] = 'changed'
    elif change == 'loaded_version': f['native']['trials'][0]['evidence']['loaded_version'] = 'changed'
    elif change == 'manifest': f['manifest']['complete'] = False
    elif change == 'run': f['manifest']['run_id'] = 'wrong-run'
    elif change == 'partial': f['native']['trials'][0]['evidence']['loop_review']['status'] = 'uncertain'
    elif change == 'duplicate': f['native']['trials'][1]['task_id'] = f['ids'][0]
    elif change == 'compact':
        f['comparison']['items'][0]['quality'] = .5
        f['progress']['items'][f['ids'][0]]['quality'] = .5
        f['write'](f['source'], f['comparison'])
        f['write'](f['source'].with_name('progress.json'), f['progress'])
    f['write'](f['report'], f['native'])
    f['write'](f['report'].with_name('manifest.json'), f['manifest'])
    # Baseline FP remains the frozen authority independently of candidate changes.
    with patch.object(reproduce, '_baseline_native_header', return_value={
            'configuration': {'judge_fingerprint': 'existing-frozen-fp'}}):
        with pytest.raises(RuntimeError, match='appworld_candidate_subset_'):
            reproduce._validated_candidate_subset(f['source'], f['seed'], f['ids'][:32], f['l0'])


def test_complete_header_does_not_trust_corrupt_or_truncated_tail(retained):
    f = retained
    f['report'].write_text(f['report'].read_text()[:-10])
    with pytest.raises((ValueError, RuntimeError)):
        reproduce._validated_candidate_subset(f['source'], f['seed'], f['ids'][:32], f['l0'])


def test_normal_stage_uses_verified_view_before_any_execution(retained, monkeypatch):
    f = retained
    monkeypatch.setattr(reproduce, 'ensure_search_allowed', lambda: None)
    monkeypatch.setattr(reproduce, 'verify', lambda seed, count, comparison=None: 0)
    monkeypatch.setattr(reproduce, 'begin_seed_attempt', lambda *a: pytest.fail('new capture scheduled'))
    monkeypatch.setattr(reproduce, 'run', lambda *a, **k: pytest.fail('model or Agent rerun'))
    import dataset
    monkeypatch.setattr(dataset, 'materialize', lambda *a, **k: {})
    reproduce.stage(f['seed'], 32)
    assert (f['source'].parent / 'stage32/comparison.json').is_file()
