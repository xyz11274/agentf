"""Native judge recovery preserves executions and does not restart Agent calls."""
from __future__ import annotations
import copy
import json
import math
import sys
from pathlib import Path
from unittest.mock import Mock
import pytest

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'tools/trajectory_evolution/experiments/appworld'))
import training


def save(path: Path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value))



def isolate_learning_service(runtime, monkeypatch):
    """A training-main fixture must own every file the private installer writes."""
    service = runtime / 'training-service.json'
    save(service, {'runtime_config': str(runtime / 'runtime.json')})
    monkeypatch.setattr(training.controller, 'TRAINING_SERVICE', service)


@pytest.fixture(autouse=True)
def preserve_production_learning_and_frozen_inputs():
    """Catch incomplete path isolation before a fixture can silently pollute production."""
    runtime = ROOT / 'trace_output/appworld/training/runtime'
    paths = [runtime / 'runtime.json', runtime / 'training-service.json',
             runtime / 'service.json', runtime / 'learning/runtime.json']
    for directory in (runtime / 'appworld-prompts', runtime / 'learning/appworld-prompts'):
        if directory.is_dir():
            paths.extend(directory.glob('*.txt'))
    paths.extend(ROOT / 'trace_output/appworld/published' / name / 'strategy.json'
                 for name in ('candidate-typeerror-causal-current', 'candidate-auth-recovery-current'))
    snapshots = {}
    for path in paths:
        if path.is_file():
            stat = path.stat()
            snapshots[path] = ((stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns), path.read_bytes())
    yield
    for path, (identity, data) in snapshots.items():
        stat = path.stat()
        assert (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns) == identity, str(path)
        assert path.read_bytes() == data, str(path)


def captured_row(task='appworld-fixture', *, judged=True):
    row = {'task_id': task, 'trial_id': task + '-trial', 'execution_status': 'completed',
           'verdict': 'passed' if judged else 'error', 'variant_id': 'candidate',
           'duration_ms': 12, 'metrics': {'tool_calls': 2},
           'evidence': {'output': {'final': {'appworld_reward': 1, 'quality_score': 1}},
                        'trace': {'events': [{'kind': 'run.start'}, {'kind': 'run.end'}]},
                        'assessment': {'quality': 1},
                        'loop_review': {'status': 'complete', 'count': 0}}}
    if not judged:
        row['error_code'] = 'evaluation_judge_failed'
        row['checks'] = [{'check_id': 'model', 'status': 'error', 'reason': 'invalid_json'}]
        row['evidence']['loop_review'] = {'status': 'uncertain', 'count': None}
    return row


def _aux_transport_pair():
    row = captured_row()
    row['task_version'] = 'original-task'
    row['evidence']['output']['quality_score'] = 0.9166666666666665
    row['evidence']['trace']['events'][-1]['output'] = {
        'quality_score': 0.9166666666666665, 'unchanged': True}
    original = {'complete': True, 'trials': [row]}
    return original, copy.deepcopy(original)


@pytest.mark.parametrize('direction', [-math.inf, math.inf])
@pytest.mark.parametrize('paths', ['output', 'run_end', 'both'])
def test_same_capture_accepts_only_adjacent_aux_quality_transport(direction, paths):
    original, reviewed = _aux_transport_pair()
    before = copy.deepcopy(original)
    evidence = reviewed['trials'][0]['evidence']
    if paths in ('output', 'both'):
        evidence['output']['quality_score'] = math.nextafter(evidence['output']['quality_score'], direction)
    if paths in ('run_end', 'both'):
        terminal = evidence['trace']['events'][-1]['output']
        terminal['quality_score'] = math.nextafter(terminal['quality_score'], direction)
    assert training.same_capture(original, reviewed, ['appworld-fixture'])
    assert original == before


@pytest.mark.parametrize('value', [math.nan, math.inf, -math.inf])
@pytest.mark.parametrize('path', ['output', 'run_end'])
def test_same_capture_rejects_nonfinite_aux_even_when_both_records_match(value, path):
    original, reviewed = _aux_transport_pair()
    for report in (original, reviewed):
        evidence = report['trials'][0]['evidence']
        target = evidence['output'] if path == 'output' else evidence['trace']['events'][-1]['output']
        target['quality_score'] = value
    assert not training.same_capture(original, reviewed, ['appworld-fixture'])


@pytest.mark.parametrize('changed', ['task_id', 'trial_id', 'variant_id', 'task_version',
    'cost', 'latency', 'code', 'reward', 'nested_quality', 'nonterminal_quality',
    'output_2ulp', 'terminal_2ulp', 'nan', 'infinity', 'bool', 'missing_key', 'traces'])
def test_same_capture_aux_transport_keeps_identity_execution_cost_and_other_values_strict(changed):
    original, reviewed = _aux_transport_pair()
    row = reviewed['trials'][0]; evidence = row['evidence']
    if changed in ('task_id', 'trial_id', 'variant_id', 'task_version'):
        row[changed] = 'wrong-original-identity'
    elif changed == 'cost':
        row['metrics']['tool_calls'] += 1
    elif changed == 'latency':
        row['duration_ms'] = math.nextafter(float(row['duration_ms']), math.inf)
    elif changed == 'code':
        original['trials'][0]['evidence']['trace']['events'][0]['code'] = 'original()'
        evidence['trace']['events'][0]['code'] = 'different()'
    elif changed == 'reward':
        evidence['output']['final']['appworld_reward'] = 0
    elif changed == 'nested_quality':
        evidence['output']['final']['quality_score'] = math.nextafter(1.0, 0.0)
    elif changed == 'nonterminal_quality':
        original['trials'][0]['evidence']['trace']['events'][0]['output'] = {'quality_score': 0.5}
        evidence['trace']['events'][0]['output'] = {'quality_score': math.nextafter(0.5, 1.0)}
    elif changed in ('output_2ulp', 'terminal_2ulp'):
        target = evidence['output'] if changed == 'output_2ulp' else evidence['trace']['events'][-1]['output']
        target['quality_score'] = math.nextafter(math.nextafter(target['quality_score'], math.inf), math.inf)
    elif changed in ('nan', 'infinity', 'bool'):
        evidence['output']['quality_score'] = {'nan': math.nan, 'infinity': math.inf, 'bool': True}[changed]
    elif changed == 'missing_key':
        del evidence['output']['quality_score']
    elif changed == 'traces':
        original['trials'][0]['evidence']['traces'] = [copy.deepcopy(original['trials'][0]['evidence']['trace'])]
        evidence['traces'] = [copy.deepcopy(evidence['trace'])]
        terminal = evidence['traces'][0]['events'][-1]['output']
        terminal['quality_score'] = math.nextafter(terminal['quality_score'], math.inf)
    assert not training.same_capture(original, reviewed, ['appworld-fixture'])


def test_complete_native_regrade_is_reused_without_agent_or_judge_calls(tmp_path, monkeypatch):
    source = tmp_path / 'report.json'
    graded_path = tmp_path / 'regrade/report.json'
    raw = {'complete': True, 'trials': [captured_row(judged=False)]}
    graded = {'complete': True, 'trials': [captured_row()]}
    save(source, raw); save(source.with_name('manifest.json'), {'native': 'original-binding'})
    save(graded_path, graded); save(graded_path.with_name('manifest.json'), {'native': 'regrade-binding'})
    before = {path: path.read_bytes() for path in (source, source.with_name('manifest.json'), graded_path, graded_path.with_name('manifest.json'))}
    judge = Mock(side_effect=AssertionError('already completed review must be reused'))
    monkeypatch.setattr(training.controller, 'regrade_report', judge)
    actual, path = training.reviewed_source(raw, source, ['appworld-fixture'], tmp_path / 'config.json', {})
    assert actual == graded and path == graded_path
    assert actual['trials'][0]['verdict'] == 'passed'
    assert training.review_complete(actual['trials'][0])
    judge.assert_not_called()
    assert all(path.read_bytes() == data for path, data in before.items())


def test_regrade_cannot_change_original_official_reward_or_trace(tmp_path, monkeypatch):
    raw = {'complete': True, 'trials': [captured_row(judged=False)]}
    source = tmp_path / 'report.json'; save(source, raw)
    changed = {'complete': True, 'trials': [captured_row()]}
    changed['trials'][0]['evidence']['output']['final']['appworld_reward'] = 0
    monkeypatch.setattr(training.controller, 'regrade_report', Mock(return_value=changed))
    with pytest.raises(RuntimeError, match='changed_execution_or_scope'):
        training.reviewed_source(raw, source, ['appworld-fixture'], tmp_path / 'config.json', {})
    assert json.loads(source.read_text()) == raw


def test_judge_failure_is_bounded_and_retains_original_execution(tmp_path, monkeypatch):
    raw = {'complete': True, 'trials': [captured_row(judged=False)]}
    source = tmp_path / 'report.json'; save(source, raw)
    judge = Mock(side_effect=RuntimeError('fixture judge temporarily unavailable'))
    monkeypatch.setattr(training.controller, 'regrade_report', judge)
    with pytest.raises(training.ReviewIncomplete):
        training.reviewed_source(raw, source, ['appworld-fixture'], tmp_path / 'config.json', {})
    assert judge.call_count == 2
    assert json.loads(source.read_text()) == raw


def test_failed_review_defers_batch_and_continues_remaining_training(tmp_path, monkeypatch):
    runtime = tmp_path / 'runtime'; published = tmp_path / 'published'
    save(runtime / 'runtime.json', {'model': {}})
    isolate_learning_service(runtime, monkeypatch); save(published / 'L0.json', {'level': 'L0'})
    successful = {'complete': True, 'trials': [captured_row('appworld-second')]}
    success_path = tmp_path / 'second/report.json'; save(success_path, successful)
    monkeypatch.setattr(training, 'TRAINING', tmp_path)
    monkeypatch.setattr(training, 'CAPTURES', tmp_path / 'full-captures')
    monkeypatch.setattr(training, 'STATUS', tmp_path / 'status.json')
    monkeypatch.setattr(training.controller, 'RUNTIME', runtime)
    monkeypatch.setattr(training.controller, 'PUBLISHED', published)
    import dataset
    monkeypatch.setattr(dataset, 'ensure_training_augmentation', Mock(return_value=[]))
    monkeypatch.setattr(dataset, 'materialize', Mock())
    monkeypatch.setattr(training.controller, 'prepare', Mock())
    monkeypatch.setattr(training.controller, 'state', Mock())
    monkeypatch.setattr(training.controller, 'environment', Mock(return_value={}))
    monkeypatch.setattr(training.learning_engine, 'install_prompts', Mock())
    monkeypatch.setattr(training, 'official_ids', Mock(return_value=['appworld-first', 'appworld-second']))
    monkeypatch.setattr(training, 'build_dataset', Mock())
    capture = Mock(side_effect=[training.ReviewIncomplete('fixture judge outage'),
                               (successful, success_path, False)])
    monkeypatch.setattr(training, 'capture_batch', capture)
    learning = Mock(return_value={'usable': True})
    monkeypatch.setattr(training.learning_engine, 'learn', learning)
    monkeypatch.setattr(sys, 'argv', ['training.py', '--batch-size', '1'])
    assert training.main() == 3
    status = json.loads(training.STATUS.read_text())
    assert capture.call_count == 2 and learning.call_count == 1
    assert status['status'] == 'learning_incomplete'
    assert status['completed_tasks'] == 1 and status['imported_batches'] == 1
    assert len(status['deferred_reviews']) == 1


def test_training_main_counts_all_nine_canonical_synthetic_tasks(tmp_path, monkeypatch):
    import dataset
    runtime, published = tmp_path / 'runtime', tmp_path / 'published'
    save(runtime / 'runtime.json', {'model': {}})
    isolate_learning_service(runtime, monkeypatch)
    save(published / 'L0.json', {'level': 'L0'})
    canonical = [{'task_id': f'official-{i}', 'initial_state': {}} for i in range(90)]
    canonical += [{'task_id': f'synthetic-{i}', 'initial_state': {'training_goal': {'kind': 'archive_read_nonstarred_inbox'}}}
                  for i in range(9)]
    monkeypatch.setattr(dataset, 'rows', lambda split: canonical)
    # The default augmentation generator still returns its own original six.
    monkeypatch.setattr(dataset, 'ensure_training_augmentation', Mock(return_value=[f'synthetic-{i}' for i in range(6)]))
    refreshed = Mock(return_value={'path': 'native99', 'reused': False})
    monkeypatch.setattr(dataset, 'materialize', refreshed)
    monkeypatch.setattr(training, 'TRAINING', tmp_path)
    monkeypatch.setattr(training, 'CAPTURES', tmp_path / 'full-captures')
    monkeypatch.setattr(training, 'STATUS', tmp_path / 'status.json')
    monkeypatch.setattr(training.controller, 'RUNTIME', runtime)
    monkeypatch.setattr(training.controller, 'PUBLISHED', published)
    monkeypatch.setattr(training.controller, 'prepare', Mock())
    monkeypatch.setattr(training.controller, 'state', Mock())
    monkeypatch.setattr(training.controller, 'environment', Mock(return_value={}))
    monkeypatch.setattr(training.controller, 'metrics', Mock(return_value={}))
    monkeypatch.setattr(training.learning_engine, 'install_prompts', Mock())
    monkeypatch.setattr(training, 'build_dataset', Mock())
    def capture(label, ids, artifact, workers, env):
        return {'complete': True, 'trials': [captured_row(task) for task in ids]}, tmp_path / label / 'report.json', False
    capture_mock = Mock(side_effect=capture)
    monkeypatch.setattr(training, 'capture_batch', capture_mock)
    monkeypatch.setattr(training, 'learning_source', lambda label, report, source, ids: (report, source))
    learning = Mock(return_value={'usable': True})
    monkeypatch.setattr(training.learning_engine, 'learn', learning)
    monkeypatch.setattr(sys, 'argv', ['training.py', '--batch-size', '16'])
    assert training.main() == 0
    status = json.loads(training.STATUS.read_text())
    assert status['official_planned_tasks'] == 90 and status['synthetic_planned_tasks'] == 9
    assert status['planned_tasks'] == status['completed_tasks'] == 99
    assert status['planned_batches'] == status['completed_batches'] == status['imported_batches'] == 7
    assert capture_mock.call_count == learning.call_count == 7
    assert capture_mock.call_args_list[-1].args[1] == [f'synthetic-{i}' for i in range(9)]
    refreshed.assert_called_once_with(training.BINARY, view='native')


@pytest.mark.parametrize('framework_status', ['incomplete', 'error', 'unavailable'])
def test_partial_framework_observer_keeps_execution_but_cannot_score_or_learn(framework_status):
    row = captured_row()
    row['evidence']['output']['final']['framework_capture'] = {'status': framework_status}
    official = copy.deepcopy(row['evidence']['output']['final'])
    assert not training.review_complete(row)
    assert training.controller.assessed_quality(row) is None
    assert training.controller._invalid_loops(row) is None
    assert not training.controller._trace_complete(row)
    metrics = training.controller.metrics([row])
    assert metrics['completed'] == 1 and metrics['scored'] == 0
    assert metrics['quality'] is None and metrics['avg_invalid_loop'] is None
    assert row['evidence']['output']['final'] == official
    # Existing historical rows are not silently relabelled. A complete
    # observer on a new row restores the ordinary native score contract.
    row['evidence']['output']['final'].pop('framework_capture')
    assert training.review_complete(row)
    row['evidence']['output']['final']['framework_capture'] = {'status': 'complete'}
    assert training.review_complete(row)


def test_missing_framework_trace_defers_without_futile_judge_calls(tmp_path, monkeypatch):
    row = captured_row()
    row['evidence']['output']['final']['framework_capture'] = {'status': 'incomplete'}
    report = {'complete': True, 'trials': [row]}
    source = tmp_path / 'report.json'; save(source, report)
    judge = Mock(side_effect=AssertionError('a judge cannot recreate missing runtime events'))
    monkeypatch.setattr(training.controller, 'regrade_report', judge)
    with pytest.raises(training.ReviewIncomplete, match='framework_capture_incomplete'):
        training.reviewed_source(report, source, ['appworld-fixture'], tmp_path / 'config.json', {})
    judge.assert_not_called()


def task_failure(task='appworld-valid-failure'):
    row = captured_row(task)
    row['verdict'] = 'failed'
    row['evidence']['assessment']['quality'] = 0
    row['evidence']['output']['final'].update(appworld_reward=0, quality_score=0,
                                            evaluation_status='completed', error_code='')
    return row


def test_recapture_distinguishes_task_failure_from_infrastructure_and_judge_errors():
    assert not training.capture_recovery_required(task_failure())
    assert not training.capture_recovery_required(captured_row(judged=False))
    for change in ('framework', 'runner', 'host', 'trace'):
        row = captured_row()
        if change == 'framework':
            row['evidence']['output']['final']['framework_capture'] = {'status': 'incomplete'}
        elif change == 'runner':
            row.update(execution_status='error', verdict='error', error_code='evaluation_runner_failed')
        elif change == 'host':
            row['evidence']['output']['final'].update(evaluation_status='errored', error_code='world_unavailable')
        else:
            row['evidence']['trace']['events'].pop()
        assert training.capture_recovery_required(row), change


def test_capture_repairs_only_bad_trial_and_resumes_authoritative_native_source(tmp_path, monkeypatch):
    captures = tmp_path / 'captures'
    destination = captures / 'batch'
    source = destination / 'report.json'
    healthy = task_failure()
    broken = captured_row('appworld-missing-events')
    broken['evidence']['output']['final']['framework_capture'] = {'status': 'incomplete'}
    original = {'complete': True, 'trials': [healthy, broken]}
    save(source, original)
    save(source.with_name('manifest.json'), {'native': 'original-binding'})
    immutable = {path: path.read_bytes() for path in (source, source.with_name('manifest.json'))}
    monkeypatch.setattr(training, 'CAPTURES', captures)
    monkeypatch.setattr(training, 'evaluation_config', lambda *args: {})
    shared_capture = Mock(side_effect=AssertionError('native resume must bypass complete early return'))
    monkeypatch.setattr(training.controller.shared, 'capture', shared_capture)
    commands = []
    def run(command, **kwargs):
        commands.append(command)
        assert command[command.index('--resume-report') + 1] == str(source)
        ids = [command[index + 1] for index, value in enumerate(command) if value == '--resume-rerun-trial']
        assert ids == [broken['trial_id']]
        repaired = copy.deepcopy(broken)
        repaired['trial_id'] = 'repaired-native-trial'
        repaired['evidence']['output']['final']['framework_capture'] = {'status': 'complete'}
        target = Path(command[command.index('--output-root') + 1]) / 'report.json'
        save(target, {'complete': True, 'trials': [copy.deepcopy(healthy), repaired]})
        save(target.with_name('manifest.json'), {'native': 'repaired-binding'})
        return type('Result', (), {'returncode': 0})()
    monkeypatch.setattr(training.subprocess, 'run', run)
    ids = [healthy['task_id'], broken['task_id']]
    repaired, native_source, reused = training.capture_batch('batch', ids, {}, 16, {})
    assert not reused and repaired['trials'][0] == healthy
    assert native_source == destination / 'capture-retry-01/report.json'
    assert json.loads((destination / 'metrics.json').read_text())['source_report'] == str(native_source)
    again, reused_source, reused = training.capture_batch('batch', ids, {}, 16, {})
    assert reused and again == repaired and reused_source == native_source
    assert len(commands) == 1
    shared_capture.assert_not_called()
    assert all(path.read_bytes() == value for path, value in immutable.items())


def test_failed_recapture_advances_native_resume_source_instead_of_sticking_to_old_report(tmp_path, monkeypatch):
    captures = tmp_path / 'captures'
    source = captures / 'batch/report.json'
    broken = captured_row('appworld-gap')
    broken['evidence']['output']['final']['framework_capture'] = {'status': 'incomplete'}
    save(source, {'complete': True, 'trials': [broken]})
    save(source.with_name('manifest.json'), {'native': 'original'})
    monkeypatch.setattr(training, 'CAPTURES', captures)
    monkeypatch.setattr(training, 'evaluation_config', lambda *args: {})
    commands = []
    def run(command, **kwargs):
        commands.append(command)
        previous = Path(command[command.index('--resume-report') + 1])
        row = copy.deepcopy(json.loads(previous.read_text())['trials'][0])
        row['trial_id'] = f'retry-native-{len(commands)}'
        if len(commands) == 2:
            row['evidence']['output']['final']['framework_capture'] = {'status': 'complete'}
        target = Path(command[command.index('--output-root') + 1]) / 'report.json'
        save(target, {'complete': True, 'trials': [row]})
        save(target.with_name('manifest.json'), {'native': 'retry'})
        return type('Result', (), {'returncode': 0})()
    monkeypatch.setattr(training.subprocess, 'run', run)
    with pytest.raises(training.ReviewIncomplete, match='training_capture_recovery_incomplete'):
        training.capture_batch('batch', [broken['task_id']], {}, 16, {})
    result, latest, reused = training.capture_batch('batch', [broken['task_id']], {}, 16, {})
    assert len(commands) == 2 and not reused
    assert commands[1][commands[1].index('--resume-report') + 1] == str(captures / 'batch/capture-retry-01/report.json')
    assert commands[1][commands[1].index('--resume-rerun-trial') + 1] == 'retry-native-1'
    assert latest == captures / 'batch/capture-retry-02/report.json'
    assert training.review_complete(result['trials'][0])


def test_partial_native_repair_is_resumed_without_discarding_finished_tasks(tmp_path, monkeypatch):
    captures = tmp_path / 'captures'
    source = captures / 'batch/report.json'
    old = task_failure('appworld-first')
    finished = task_failure('appworld-second')
    save(source, {'complete': False, 'trials': [old]})
    save(source.with_name('manifest.json'), {'native': 'original'})
    partial = captures / 'batch/capture-retry-01/report.json'
    save(partial, {'complete': False, 'trials': [old, finished]})
    save(partial.with_name('manifest.json'), {'native': 'partial-repair'})
    save(captures / 'batch/metrics.json', {'source_report': str(partial)})
    monkeypatch.setattr(training, 'CAPTURES', captures)
    monkeypatch.setattr(training, 'evaluation_config', lambda *args: {})
    def run(command, **kwargs):
        assert command[command.index('--resume-report') + 1] == str(partial)
        assert '--resume-rerun-trial' not in command
        target = Path(command[command.index('--output-root') + 1]) / 'report.json'
        save(target, {'complete': True, 'trials': [old, finished, captured_row('appworld-third')]})
        save(target.with_name('manifest.json'), {'native': 'completed-repair'})
        return type('Result', (), {'returncode': 0})()
    monkeypatch.setattr(training.subprocess, 'run', run)
    result, native_source, reused = training.capture_batch('batch', ['appworld-first', 'appworld-second', 'appworld-third'], {}, 16, {})
    assert not reused and result['trials'][:2] == [old, finished]
    assert native_source == captures / 'batch/capture-retry-02/report.json'


def test_status_reports_actual_capture_separately_from_resume_learning_cursor(tmp_path, monkeypatch):
    import dataset
    import reproduce
    monkeypatch.setattr(reproduce, 'OUTPUT', tmp_path)
    tasks = ['appworld-first', 'appworld-second', 'appworld-third', 'appworld-pending']
    monkeypatch.setattr(dataset, 'rows', lambda split: [{'task_id': task} for task in tasks])
    captures = tmp_path / 'training/full-captures'
    first = captured_row(tasks[0])
    authoritative = copy.deepcopy(first)
    authoritative['evidence']['assessment']['quality'] = 0.5
    save(captures / 'batch-01/report.json', {'complete': True, 'trials': [first]})
    reviewed = captures / 'batch-01/regrade/report.json'
    save(reviewed, {'complete': True, 'trials': [authoritative]})
    save(captures / 'batch-01/metrics.json', {'source_report': str(reviewed)})
    second = captured_row(tasks[1])
    save(captures / 'batch-02/report.json', {'complete': True, 'trials': [second]})
    third = task_failure(tasks[2])
    save(captures / 'batch-03/trials/finished.json', third)
    save(captures / 'unrelated/report.json', {'complete': True, 'trials': [captured_row('appworld-dev')]})
    save(tmp_path / 'training/status.json', {'phase': 'learning', 'learning_tasks': 16})
    status = reproduce.training_status({'status': 'running', 'completed_tasks': 0, 'planned_tasks': 4})
    assert status['completed_tasks'] == status['captured_tasks'] == 3
    assert status['learned_tasks'] == 0 and status['planned_tasks'] == 4 and status['phase'] == 'learning'
    assert status['metrics'] == training.controller.metrics([authoritative, second, third], planned=4)
    assert status['metrics']['quality_status'] == 'partial' and status['metrics']['missing'] == 1


def test_status_uses_raw_native_report_when_authority_is_temporarily_missing(tmp_path, monkeypatch):
    import dataset
    import reproduce
    monkeypatch.setattr(reproduce, 'OUTPUT', tmp_path)
    monkeypatch.setattr(dataset, 'rows', lambda split: [{'task_id': 'appworld-first'}])
    batch = tmp_path / 'training/full-captures/batch-01'
    row = task_failure('appworld-first')
    save(batch / 'report.json', {'complete': True, 'trials': [row]})
    save(batch / 'metrics.json', {'source_report': str(batch / 'pending-regrade/report.json')})
    # The report is authoritative; trial files cannot replace its grades.
    save(batch / 'trials/duplicate.json', captured_row('appworld-first'))
    status = reproduce.training_status({'status': 'running', 'completed_tasks': 1})
    assert status['completed_tasks'] == status['learned_tasks'] == 1
    assert status['metrics']['strict_passed'] == 0 and status['metrics']['quality'] == 0


def test_all_expected_semantic_runs_require_success_including_skipped_zero_counters(tmp_path):
    from types import SimpleNamespace
    import learning
    controller = SimpleNamespace(TRAINING=tmp_path, OWNER='fixture',
        read=lambda path: json.loads(Path(path).read_text()))
    identities = {('conversation', name) for name in ('successful', 'skipped', 'missing', 'failed')}
    folder = tmp_path / 'evolution/by-user/fixture/evaluation/conversation'
    save(folder / 'successful.evaluation.json', {'model_requested_count': '1', 'model_failed_count': '0',
        'dialogue_assessment': {'status': 'MODEL_CALL_STATUS_SUCCEEDED'}})
    save(folder / 'skipped.evaluation.json', {'dialogue_assessment': {'status': 'MODEL_CALL_STATUS_SKIPPED',
        'error_code': 'model_request_too_large'}})
    save(folder / 'failed.evaluation.json', {'model_requested_count': 1, 'model_failed_count': 1,
        'dialogue_assessment': {'status': 'MODEL_CALL_STATUS_INVALID_RESPONSE', 'error_code': 'invalid_response'}})
    failures, successful = learning.semantic_analysis_results(controller, identities)
    assert successful == ['successful']
    assert {row['run_id'] for row in failures} == {'skipped', 'missing', 'failed'}
    skipped = next(row for row in failures if row['run_id'] == 'skipped')
    assert skipped['model_requested_count'] == skipped['model_failed_count'] == 0
    assert skipped['reason'] == 'model_request_too_large'


def test_cached_learning_status_cannot_hide_expected_skipped_analysis(tmp_path):
    from types import SimpleNamespace
    import learning
    service = tmp_path / 'service.json'
    save(service, {'sharing': {'enabled': False}})
    controller = SimpleNamespace(TRAINING=tmp_path, OWNER='fixture', TRAINING_SERVICE=service,
        read=lambda path: json.loads(Path(path).read_text()))
    report = tmp_path / 'report.json'
    save(report, {'trials': [{'task_id': 'training-task', 'evidence': {'trace': {'identity': {
        'conversation_id': 'conversation', 'run_id': 'expected-run'}}}}]})
    cache = tmp_path / 'evolution/by-user/fixture/evaluation/conversation/expected-run.evaluation.json'
    save(cache, {'dialogue_assessment': {'status': 'MODEL_CALL_STATUS_SKIPPED',
                                       'error_code': 'model_request_too_large'}})
    # Historical status lacks the new failure marker but is otherwise terminal.
    status = {'shared_status': 'idle', 'run_outcomes': {'completed': 1},
              'appworld_input': {'original_report': str(report),
                                 'admitted_training_tasks': ['training-task']}}
    assert not learning.learning_usable(controller, status)
    save(cache, {'model_requested_count': '1', 'model_failed_count': '0',
                 'dialogue_assessment': {'status': 'MODEL_CALL_STATUS_SUCCEEDED'}})
    assert learning.learning_usable(controller, status)
    cache.unlink()
    assert not learning.learning_usable(controller, status)


def test_regraded_scores_resume_the_original_admitted_learning_report(tmp_path, monkeypatch):
    monkeypatch.setattr(training, 'CAPTURES', tmp_path)
    batch = tmp_path / 'batch'
    original = {'complete': True, 'trials': [captured_row()]}
    original['trials'][0]['checks'] = [{'check_id': 'model', 'status': 'passed'}]
    source = batch / 'regrade/report.json'
    save(source, original); save(source.with_name('manifest.json'), {'native': 'original'})
    save(source.parent / 'learning-status.json', {'appworld_input': {'original_report': str(source)},
        'appworld_model_analysis_failures': [{'run_id': 'pending'}]})
    changed = copy.deepcopy(original)
    changed['trials'][0]['verdict'] = 'failed'
    changed['trials'][0]['checks'][0]['status'] = 'failed'
    changed['trials'][0]['evidence']['assessment']['quality'] = 0
    changed['trials'][0]['evidence']['loop_review']['count'] = 3
    score_source = batch / 'loop-regrade/report.json'; save(score_source, changed)
    before = source.read_bytes()
    report, path = training.learning_source('batch', changed, score_source, ['appworld-fixture'])
    assert report == original and path == source.resolve()
    assert report['trials'][0]['verdict'] == 'passed'
    assert changed['trials'][0]['verdict'] == 'failed' and source.read_bytes() == before


def test_first_learning_admission_uses_current_report_and_trace_changes_are_rejected(tmp_path, monkeypatch):
    monkeypatch.setattr(training, 'CAPTURES', tmp_path)
    original = {'complete': True, 'trials': [captured_row()]}
    source = tmp_path / 'batch/report.json'; save(source, original)
    report, path = training.learning_source('batch', original, source, ['appworld-fixture'])
    assert report is original and path == source
    save(source.with_name('manifest.json'), {'native': 'first'})
    save(source.parent / 'learning-status.json', {'appworld_input': {'original_report': str(source)}})
    changed = copy.deepcopy(original)
    changed['trials'][0]['evidence']['trace']['events'][0]['input'] = 'different Agent execution'
    with pytest.raises(training.ReviewIncomplete, match='execution_mismatch'):
        training.learning_source('batch', changed, source, ['appworld-fixture'])


def test_training_extension_preserves_old_six_executions_and_admits_native_nine(tmp_path, monkeypatch):
    monkeypatch.setattr(training, 'CAPTURES', tmp_path)
    batch = tmp_path / 'batch'
    old = {'complete': True, 'trials': [captured_row(f'old-{i}') for i in range(6)]}
    original = batch / 'capture-old/report.json'
    save(original, old); save(original.with_name('manifest.json'), {'native': 'old'})
    save(original.parent / 'learning-status.json', {'appworld_input': {'original_report': str(original)}})
    current = copy.deepcopy(old)
    current['trials'] += [captured_row(f'new-{i}') for i in range(3)]
    source = batch / 'capture-next/report.json'
    save(source, current); save(source.with_name('manifest.json'), {'native': 'next'})
    before = original.read_bytes()
    ids = [row['task_id'] for row in current['trials']]
    actual, path = training.learning_source('batch', current, source, ids)
    assert actual is current and path == source
    assert original.read_bytes() == before
    # A subsequent judge retry retains the nine-task native admission.
    save(source.parent / 'learning-status.json', {'appworld_input': {'original_report': str(source)}})
    regraded = copy.deepcopy(current)
    regraded['trials'][0]['evidence']['assessment']['quality'] = 0
    actual, path = training.learning_source('batch', regraded, batch / 'regrade/report.json', ids)
    assert actual == current and path == source.resolve()


@pytest.mark.parametrize('mutation', ['trace', 'trial_id', 'duration_ms', 'metrics', 'output', 'missing', 'unbound'])
def test_training_extension_rejects_changed_old_execution_or_incomplete_native_source(tmp_path, monkeypatch, mutation):
    monkeypatch.setattr(training, 'CAPTURES', tmp_path)
    old = {'complete': True, 'trials': [captured_row('old')]}
    original = tmp_path / 'batch/old/report.json'
    save(original, old); save(original.with_name('manifest.json'), {'native': 'old'})
    save(original.parent / 'learning-status.json', {'appworld_input': {'original_report': str(original)}})
    current = copy.deepcopy(old)
    current['trials'].append(captured_row('new'))
    row = current['trials'][0]
    if mutation == 'trace': row['evidence']['trace']['events'][0]['input'] = 'changed'
    elif mutation == 'trial_id': row['trial_id'] = 'different-run'
    elif mutation == 'duration_ms': row['duration_ms'] += 1
    elif mutation == 'metrics': row['metrics']['tool_calls'] += 1
    elif mutation == 'output': row['evidence']['output']['final']['appworld_reward'] = 0
    elif mutation == 'missing': current['trials'].pop()
    source = tmp_path / 'batch/next/report.json'
    save(source, current)
    if mutation != 'unbound': save(source.with_name('manifest.json'), {'native': 'next'})
    with pytest.raises(training.ReviewIncomplete, match='execution_mismatch'):
        training.learning_source('batch', current, source, ['old', 'new'])


def test_training_admission_chain_rejects_extra_scope_and_non_nested_sources(tmp_path, monkeypatch):
    monkeypatch.setattr(training, 'CAPTURES', tmp_path)
    first = {'complete': True, 'trials': [captured_row('first')]}
    second = {'complete': True, 'trials': [captured_row('second')]}
    for label, value in [('first', first), ('second', second)]:
        source = tmp_path / 'batch' / label / 'report.json'
        save(source, value); save(source.with_name('manifest.json'), {'native': label})
        save(source.parent / 'learning-status.json', {'appworld_input': {'original_report': str(source)}})
    current = {'complete': True, 'trials': first['trials'] + second['trials']}
    with pytest.raises(training.ReviewIncomplete, match='provenance_conflict'):
        training.learning_source('batch', current, tmp_path / 'current/report.json', ['first', 'second'])
    with pytest.raises(training.ReviewIncomplete, match='execution_mismatch'):
        training.learning_source('batch', current, tmp_path / 'current/report.json', ['first'])


@pytest.mark.parametrize('change', [None, 'trace', 'identity', 'cost', 'missing_pair', 'unusable'])
def test_incremental_native_learning_reuses_only_healthy_unchanged_source(tmp_path, monkeypatch, change):
    import learning
    from types import SimpleNamespace
    old = {'complete': True, 'trials': [captured_row(f'old-{i}') for i in range(6)]}
    original = tmp_path / 'batch/old/report.json'
    save(original, old); save(original.with_name('manifest.json'), {'native': 'old'})
    save(original.parent / 'learning-status.json', {'native_import_progressed': True,
        'appworld_input': {'original_report': str(original), 'admitted_training_tasks': [r['task_id'] for r in old['trials']]}})
    current = copy.deepcopy(old)
    current['trials'] += [captured_row(f'new-{i}') for i in range(3)]
    if change == 'trace': current['trials'][0]['evidence']['trace']['events'].append({'kind': 'different-execution'})
    if change == 'identity': current['trials'][0]['trial_id'] = 'different-trial'
    if change == 'cost': current['trials'][0]['metrics']['tool_calls'] += 1
    if change == 'missing_pair': original.with_name('manifest.json').unlink()
    c = SimpleNamespace(read=lambda path: json.loads(Path(path).read_text()))
    monkeypatch.setattr(learning, 'learning_usable', lambda controller, status: change != 'unusable')
    monkeypatch.setattr(learning, 'learning_progressed', lambda status: True)
    ids = [r['task_id'] for r in current['trials']]
    if change in ('trace', 'identity', 'cost', 'missing_pair'):
        with pytest.raises(RuntimeError, match='appworld_training_reused_'):
            learning._reused_training_sources(c, current, tmp_path / 'batch/new/report.json', ids)
    else:
        reused, sources = learning._reused_training_sources(c, current, tmp_path / 'batch/new/report.json', ids)
        assert reused == (set() if change == 'unusable' else {f'old-{i}' for i in range(6)})
        assert sources == ([] if change == 'unusable' else [str(original.resolve())])


def test_incremental_native_learning_reuses_six_then_nine_before_eleven(tmp_path, monkeypatch):
    import learning
    from types import SimpleNamespace
    first = {'complete': True, 'trials': [captured_row(f'task-{i}') for i in range(6)]}
    second = copy.deepcopy(first)
    second['trials'] += [captured_row(f'task-{i}') for i in range(6, 9)]
    third = copy.deepcopy(second)
    third['trials'] += [captured_row(f'task-{i}') for i in range(9, 11)]
    paths = []
    for number, report in enumerate((first, second), 1):
        source = tmp_path / 'batch' / f'capture-retry-{number:02d}' / 'report.json'
        save(source, report)
        save(source.with_name('manifest.json'), {'native': number})
        ids = [row['task_id'] for row in report['trials']]
        save(source.parent / 'learning-status.json', {
            'native_import_progressed': True, 'imported': 6 if number == 1 else 3,
            'appworld_input': {'original_report': str(source), 'admitted_training_tasks': ids,
                'native_import_training_tasks': ids if number == 1 else ids[6:],
                'reused_training_tasks': [] if number == 1 else ids[:6],
                'reused_training_sources': [] if number == 1 else [str(paths[0])]}})
        paths.append(source.resolve())
    controller = SimpleNamespace(read=lambda path: json.loads(Path(path).read_text()))
    monkeypatch.setattr(learning, 'learning_usable', lambda *args: True)
    monkeypatch.setattr(learning, 'learning_progressed', lambda *args: True)
    reused, sources = learning._reused_training_sources(controller, third,
        tmp_path / 'batch/capture-retry-03/report.json', [row['task_id'] for row in third['trials']])
    assert reused == {f'task-{i}' for i in range(9)}
    assert sources == sorted(map(str, paths))
    assert [row['task_id'] for row in third['trials'] if row['task_id'] not in reused] == ['task-9', 'task-10']


@pytest.mark.parametrize('review', ['regrade', 'regrade-retry', 'final-review/retry'])
def test_nested_training_regrade_reuses_same_batch_eleven_and_imports_only_new_one(tmp_path, monkeypatch, review):
    import learning
    from types import SimpleNamespace
    batch = tmp_path / 'full-captures/train-full-07'
    old = {'complete': True, 'trials': [captured_row(f'task-{i}') for i in range(11)]}
    prior = batch / 'capture-retry-07/report.json'
    save(prior, old); save(prior.with_name('manifest.json'), {'native': 'durable-old-eleven'})
    status = {'native_import_progressed': True, 'imported': 2,
              'appworld_input': {'original_report': str(prior),
                  'admitted_training_tasks': [row['task_id'] for row in old['trials']]}}
    save(prior.with_name('learning-status.json'), status)
    sibling = tmp_path / 'full-captures/train-full-08/capture/report.json'
    save(sibling, {'complete': True, 'trials': old['trials'][:1]})
    save(sibling.with_name('manifest.json'), {'native': 'unrelated-batch'})
    save(sibling.with_name('learning-status.json'), {'native_import_progressed': True,
        'appworld_input': {'original_report': str(sibling), 'admitted_training_tasks': ['task-0']}})
    current = copy.deepcopy(old)
    current['trials'].append(captured_row('new-one'))
    source = batch / 'capture-retry-08' / review / 'report.json'
    save(source, current); save(source.with_name('manifest.json'), {'native': 'current-twelve'})
    before = {path: path.read_bytes() for path in (prior, prior.with_name('manifest.json'), source,
                                                 source.with_name('manifest.json'))}
    controller = SimpleNamespace(read=lambda path: json.loads(Path(path).read_text()))
    monkeypatch.setattr(learning, 'learning_usable', lambda *args: True)
    monkeypatch.setattr(learning, 'learning_progressed', lambda *args: True)
    reused, sources = learning._reused_training_sources(controller, current, source,
        [row['task_id'] for row in current['trials']])
    assert reused == {f'task-{i}' for i in range(11)}
    assert sources == [str(prior.resolve())]
    assert [row['task_id'] for row in current['trials'] if row['task_id'] not in reused] == ['new-one']
    assert all(path.read_bytes() == value for path, value in before.items())


def test_frozen_seed_provenance_includes_full_capture_and_native_regrade(tmp_path, monkeypatch):
    from types import SimpleNamespace
    import learning
    runtime, published = tmp_path / 'runtime', tmp_path / 'published'
    owner, agent, scope = 'fixture-owner', 'fixture-agent', 'fixture-scope'
    memory = tmp_path / 'evolution/by-user' / owner / 'memory/evolution-memory.json'
    save(memory, {'entries': []})
    save(runtime / 'service.json', {'runtime_options': {}})
    save(published / 'L0.json', {'schema_version': 1, 'user_id': owner, 'level': 'L0', 'policy': None,
                              'agent': {'name': agent, 'revision': 'fixture-revision'}})
    save(tmp_path / 'full-training-status.json', {
        'status': 'complete', 'scope': 'official_train', 'batch_size': 1,
        'planned_tasks': 2, 'completed_tasks': 2, 'planned_batches': 2,
        'completed_batches': 2, 'imported_batches': 2, 'deferred_reviews': [],
        'last_learning': {'usable': True}})
    save(tmp_path / 'full-captures/train-full-01/report.json',
         {'complete': True, 'trials': [captured_row('appworld-first')]})
    save(tmp_path / 'full-captures/train-full-02/regrade/report.json',
         {'complete': True, 'trials': [captured_row('appworld-second')]})
    save(tmp_path / 'full-captures/train-full-01/learning-status.json',
         {'usable': True, 'appworld_input': {'admitted_training_tasks': ['appworld-first']}})
    save(tmp_path / 'full-captures/train-full-02/regrade/learning-status.json',
         {'usable': True, 'appworld_input': {'admitted_training_tasks': ['appworld-second']}})
    save(tmp_path / 'full-captures/train-full-03/learning-status.json',
         {'usable': False, 'appworld_input': {'admitted_training_tasks': ['appworld-pending']}})
    experience = {'memory_id': 'fixture-memory', 'content_revision': 'fixture-1',
                  'target_scope': scope, 'content': 'Fixture evidence', 'control_target_type': ''}
    native = Mock(side_effect=[json.dumps({'status': 'completed', 'model_calls': 0, 'refreshed_evidence': 1}),
                              json.dumps({'experiences': [experience]}),
                              json.dumps({'snapshot': {'experiences': [experience]}, 'options': {}})])
    controller = SimpleNamespace(TRAINING=tmp_path, RUNTIME=runtime, PUBLISHED=published,
        ROOT=ROOT, OWNER=owner, AGENT=agent, SCOPE=scope, BINARY=Path('fixture-native'),
        TRAINING_SERVICE=runtime / 'training-service.json',
        read=lambda path: json.loads(Path(path).read_text()), save=save, native=native,
        ids=lambda split: ['appworld-first', 'appworld-second'] if split == 'train' else ['appworld-dev'],
        environment=lambda: {}, version=lambda artifact: 'fixture-version', evaluation_config=lambda *args: {},
        _trial_error_code=training.controller._trial_error_code,
        framework_capture_complete=training.controller.framework_capture_complete,
        assessed_quality=training.controller.assessed_quality,
        _has_complete_loop_review=training.controller._has_complete_loop_review)
    monkeypatch.setattr(learning, '_readiness', lambda controller: memory)
    monkeypatch.setattr(learning, 'learning_usable', lambda controller, status, returncode: status['usable'])
    def generated(controller, policy, tasks, diagnostic):
        compiled = copy.deepcopy(policy)
        compiled['options']['loop_rules'] = [{'source_experience': {'memory_id': policy['snapshot']['experiences'][0]['memory_id']}}]
        return compiled
    monkeypatch.setattr(learning, 'generate_seed_controls', generated)
    def generated_english(controller, policy, tasks, diagnostic):
        assert policy['options']['loop_rules'][0]['source_experience']
        diagnostic['english_trigger_learning'] = {'status': 'compiled', 'runtime_profile': {'new_matched_windows': 1}}
        return policy
    english = Mock(side_effect=generated_english)
    monkeypatch.setattr(learning, 'generate_english_triggers', english)
    learning.freeze_seed(controller)
    manifest = json.loads((published / 'seed0/manifest.json').read_text())
    assert manifest['training_input_tasks'] == ['appworld-first', 'appworld-second']
    assert manifest['planned_training_count'] == 2
    assert native.call_count == 3
    assert english.call_count == 1  # default fresh path compiles real-window English conditions
    assert native.call_args_list[0].args[2] == ['learn', '--config', str(controller.TRAINING_SERVICE), '--refresh-evidence']
    assert native.call_args_list[1].args[2] == ['shadow', 'compile']
    assert native.call_args_list[2].args[2] == ['runtime', 'compile']
    diagnostic = json.loads((tmp_path / 'seed-compile-status.json').read_text())
    assert diagnostic['evidence_refresh']['model_calls'] == 0 and diagnostic['evidence_refresh']['refreshed_evidence'] == 1
    original_bytes = (published / 'seed0/strategy.json').read_bytes()
    native.side_effect = [json.dumps({'status': 'completed', 'model_calls': 0, 'refreshed_evidence': 0}),
                         json.dumps({'experiences': [experience]}),
                          json.dumps({'snapshot': {'experiences': [experience]}, 'options': {}})]
    no_second_generation = Mock(side_effect=AssertionError('frozen policy must resume without model calls'))
    monkeypatch.setattr(learning, 'generate_seed_controls', no_second_generation)
    english.reset_mock()
    resumed = learning.freeze_seed(controller)
    english.assert_not_called()
    assert resumed == json.loads(original_bytes) and no_second_generation.call_count == 0
    assert (published / 'seed0/strategy.json').read_bytes() == original_bytes
    monkeypatch.setattr(learning, 'generate_seed_controls', generated)
    # A genuinely changed trained experience compiles to a new immutable
    # candidate, while the earlier published seed remains byte-for-byte frozen.
    updated = dict(experience, memory_id='new-training-memory', content_revision='fixture-2',
                   content='Additional successful training evidence')
    save(memory, {'entries': [{'candidate': updated}]})
    native.side_effect = [json.dumps({'status': 'completed', 'model_calls': 0, 'refreshed_evidence': 0}),
                         json.dumps({'experiences': [updated]}),
                         json.dumps({'snapshot': {'experiences': [updated]}, 'options': {}})]
    artifact = learning.freeze_seed(controller, label='candidate-next')
    assert json.loads((published / 'candidate-next/strategy.json').read_text()) == artifact
    assert artifact['policy']['snapshot']['experiences'][0]['memory_id'] == 'new-training-memory'
    assert (published / 'seed0/strategy.json').read_bytes() == original_bytes
    native.side_effect = [json.dumps({'status': 'completed', 'model_calls': 0, 'refreshed_evidence': 0}),
                         json.dumps({'experiences': [updated]}),
                         json.dumps({'snapshot': {'experiences': [updated]}, 'options': {}})]
    # Existing labels resume their whole immutable artifact even if later
    # training changes the current store; new candidates use a new label.
    native.reset_mock()
    assert learning.freeze_seed(controller, label='seed0') == json.loads(original_bytes)
    native.assert_not_called()
    assert (published / 'seed0/strategy.json').read_bytes() == original_bytes


@pytest.mark.parametrize('label', ['', '.', '..', '../seed', '/tmp/seed', 'seed/child',
                                  'seed\\child', 'L0', 'l0', 'gepa-final', 'a' * 65])
def test_candidate_label_rejects_path_escape_and_reserved_targets(label):
    import learning
    with pytest.raises(ValueError, match='appworld_seed_label_invalid'):
        learning.validate_seed_label(label)


def test_candidate_label_cannot_follow_symlink_outside_published(tmp_path):
    from types import SimpleNamespace
    import learning
    published, outside = tmp_path / 'published', tmp_path / 'outside'
    published.mkdir(); outside.mkdir()
    (published / 'candidate-escape').symlink_to(outside, target_is_directory=True)
    with pytest.raises(ValueError, match='outside_published'):
        learning.freeze_seed(SimpleNamespace(PUBLISHED=published), label='candidate-escape')


def test_seed_cli_freezes_requested_candidate_and_status_tracks_it(tmp_path, monkeypatch):
    import learning, reproduce, layout
    published = tmp_path / 'published'
    monkeypatch.setattr(reproduce, 'OUTPUT', tmp_path)
    monkeypatch.setattr(layout, 'PUBLISHED', published)
    artifact = {'level': 'L3', 'policy': {'real-training': 'fixture'}}
    calls = []
    def freeze(controller, *, label):
        calls.append(label)
        save(controller.PUBLISHED / label / 'strategy.json', artifact)
        return artifact
    monkeypatch.setattr(learning, 'freeze_seed', freeze)
    candidate = published / 'candidate-next/strategy.json'
    monkeypatch.setattr(sys, 'argv', ['reproduce.py', '--seed', '--seed-artifact', str(candidate)])
    assert reproduce.main() == 0
    assert calls == ['candidate-next']
    assert reproduce.status()['seed'] == {'status': 'ready', 'artifact': str(candidate),
                                         'label': 'candidate-next'}
    monkeypatch.setattr(sys, 'argv', ['reproduce.py', '--seed', '--seed-label', 'candidate-other'])
    assert reproduce.main() == 0
    assert calls == ['candidate-next', 'candidate-other']
    assert reproduce.status()['seed']['label'] == 'candidate-other'
    stage = Mock()
    monkeypatch.setattr(reproduce, 'stage', stage)
    monkeypatch.setattr(sys, 'argv', ['reproduce.py', '--pilot32'])
    assert reproduce.main() == 0
    stage.assert_called_once_with(published / 'candidate-other/strategy.json', 32)
    monkeypatch.setattr(sys, 'argv', ['reproduce.py', '--seed', '--seed-artifact', str(tmp_path / 'outside/strategy.json')])
    with pytest.raises(RuntimeError, match='must_match_publish_label'):
        reproduce.main()
    assert len(calls) == 2


def test_long_task_capture_ceiling_is_shared_without_expanding_agent_budget(monkeypatch):
    controller = training.controller
    baseline = {'level': 'L0', 'policy': None,
                'agent': {'name': controller.AGENT, 'revision': 'fixture-revision'}}
    monkeypatch.setattr(controller, 'version', lambda artifact: 'fixture-version')
    monkeypatch.setattr(controller, 'ids', lambda split: ['appworld-train'] if split == 'train' else [])
    configs = [training.evaluation_config(['appworld-train'], baseline, 16),
               controller.evaluation_config(['appworld-train'], baseline, 16),
               controller.evaluation_config(['appworld-val'], baseline, 16)]
    for config in configs:
        assert config['max_trace_bytes'] == 64 * 1024 * 1024
        assert config['parallelism'] == 16 and config['max_trials'] == 1
        assert config['timeout_ms'] == 1800000
        assert config['judge']['max_model_calls'] == 8
        assert config['judge']['max_input_bytes'] == 2097152
        assert config['variants'][0]['version'] == 'fixture-revision'
    row = captured_row()
    row['metrics'] = {'tokens': 415275, 'llm_calls': 50, 'input_tokens': 402012,
                      'output_tokens': 13263}
    row['duration_ms'] = 338305.531042
    metrics = controller.metrics([row])
    assert metrics['tokens'] == 415275 and metrics['model_calls'] == 50
    assert metrics['latency_ms'] == 338305.531042


def test_prepared_capture_and_learning_limits_admit_the_same_long_trace(tmp_path, monkeypatch):
    controller = training.controller
    runtime = tmp_path / 'training/runtime'
    for name, path in [('TRAINING', tmp_path / 'training'), ('GEPA', tmp_path / 'gepa'),
                       ('PUBLISHED', tmp_path / 'published'), ('RUNTIME', runtime),
                       ('TRAINING_SERVICE', runtime / 'training-service.json')]:
        monkeypatch.setattr(controller, name, path)
    monkeypatch.setattr(controller, 'environment', lambda: {
        'AF_LLM_BASE_URL': 'https://example.invalid/v1', 'AF_LLM_MODEL': 'fixture'})
    monkeypatch.setattr(controller, 'version', lambda artifact: 'fixture-version')
    monkeypatch.setattr(controller, 'ids', lambda split: ['appworld-train'] if split == 'train' else ['appworld-val'])
    # Runtime tuning must be private and must not invalidate existing source
    # analyses or alter the validated-evidence admission rules.
    import learning
    business_path = ROOT / 'tools/trajectory_evolution/config/standard-runtime.json'
    business_before = business_path.read_bytes()
    boundary_path = ROOT / 'tools/trajectory_evolution/config/boundary-service.json'
    boundary_before = boundary_path.read_bytes()
    cached = tmp_path / 'training/evolution/by-user/appworld-training/evaluation/cached.evaluation.json'
    save(cached, {'dialogue_assessment': {'status': 'MODEL_CALL_STATUS_SUCCEEDED'},
                  'model_requested_count': 1, 'model_failed_count': 0})
    cached_before = cached.read_bytes()
    controller.prepare()
    private = json.loads((runtime / 'runtime.json').read_text())
    assert private['evolution']['full_curation_new_entry_threshold'] == controller.CURATION_NEW_ENTRY_THRESHOLD
    assert private['evolution']['memory_review_max_attempts'] == controller.CURATION_REVIEW_MAX_ATTEMPTS
    assert private['evolution']['enable_memory_semantic_matching'] is True
    # Reinstalling analysis prompts during resume retains the same tuning;
    # this local phase does not launch analysis or overwrite cached results.
    no_model = Mock(side_effect=AssertionError('prompt installation must not reanalyse'))
    monkeypatch.setattr(controller, 'native', no_model)
    installed = learning.install_prompts(controller)
    again = json.loads((runtime / 'runtime.json').read_text())
    assert again['evolution'] == private['evolution']
    assert again['model'] == private['model']
    assert installed['model_calls'] == 0
    no_model.assert_not_called()
    assert cached.read_bytes() == cached_before
    assert business_path.read_bytes() == business_before
    assert boundary_path.read_bytes() == boundary_before
    assert json.loads(boundary_before).get('preserve_runtime_policy_state', False) is False
    assert json.loads(business_before)['evolution']['full_curation_new_entry_threshold'] == 20
    for name in ['service.json', 'training-service.json']:
        service = json.loads((runtime / name).read_text())
        assert service['preserve_runtime_policy_state'] is True
        assert service['max_run_bytes'] == 64 * 1024 * 1024
        assert service['learning']['max_batch_bytes'] >= service['max_run_bytes']
        assert service['http_persistence']['max_buffer_bytes'] >= service['max_run_bytes']
        assert service['runtime_options']['loop_rules'][0]['max_iterations'] == 30


@pytest.mark.parametrize('next_prepare', ['same_provider', 'changed_provider', 'missing_prompt'])
def test_learning_runtime_is_private_and_normal_prepare_preserves_frozen_judge(tmp_path, monkeypatch, next_prepare):
    controller = training.controller
    runtime = tmp_path / 'training/runtime'
    for name, path in [('TRAINING', tmp_path / 'training'), ('GEPA', tmp_path / 'gepa'),
                       ('PUBLISHED', tmp_path / 'published'), ('RUNTIME', runtime),
                       ('TRAINING_SERVICE', runtime / 'training-service.json')]:
        monkeypatch.setattr(controller, name, path)
    provider = {'AF_LLM_BASE_URL': 'https://example.invalid/v1', 'AF_LLM_MODEL': 'fixture'}
    monkeypatch.setattr(controller, 'environment', lambda: dict(provider))
    monkeypatch.setattr(controller, 'version', lambda artifact: 'fixture-version')
    monkeypatch.setattr(controller, 'ids', lambda split: ['appworld-train'] if split == 'train' else ['appworld-val'])
    monkeypatch.setattr(controller, 'native', Mock(side_effect=AssertionError('no model or native mutation')))
    controller.prepare()
    grading = runtime / 'runtime.json'
    original = grading.read_bytes()
    original_config = json.loads(original)
    originals = {value: (runtime / value).read_bytes() for value in original_config['prompts'].values()}
    cached = tmp_path / 'training/evolution/cached.evaluation.json'
    save(cached, {'existing_assessment': 'unchanged'})
    cached_bytes = cached.read_bytes()
    private = training.install_learning_runtime()
    assert private == runtime / 'learning/runtime.json'
    assert grading.read_bytes() == original
    assert all((runtime / value).read_bytes() == content for value, content in originals.items())
    config = json.loads(private.read_text())
    assert config['model']['max_retries'] == 0 and config['model']['max_request_bytes'] == 3 * 1024 * 1024
    assert json.loads(controller.TRAINING_SERVICE.read_text())['runtime_config'] == str(private)
    for key in ('dialogue_system', 'standard_dialogue_system', 'dialogue_user'):
        text = (private.parent / config['prompts'][key]).read_text()
        assert '执行结果观察机制：' in text
        assert '真实USEFUL因果链' in text and '无stdout不等于执行失败' in text
        assert '不得普遍禁止重查' in text
    assert cached.read_bytes() == cached_bytes
    if next_prepare == 'changed_provider':
        provider['AF_LLM_MODEL'] = 'new-explicit-provider'
    elif next_prepare == 'missing_prompt':
        broken = copy.deepcopy(original_config)
        broken['prompts']['dialogue_system'] = 'missing-prompt.txt'
        save(grading, broken)
    controller.prepare()
    if next_prepare == 'same_provider':
        assert grading.read_bytes() == original
    else:
        refreshed = json.loads(grading.read_text())
        assert refreshed['model']['model_id'] == provider['AF_LLM_MODEL']
        assert all((runtime / value).is_file() for value in refreshed['prompts'].values())
    training.install_learning_runtime()
    assert json.loads(controller.TRAINING_SERVICE.read_text())['runtime_config'] == str(private)
    assert cached.read_bytes() == cached_bytes
    controller.native.assert_not_called()


@pytest.mark.parametrize('problem', ['missing_status', 'running', 'deferred',
                                    'missing_import', 'missing_task', 'capture_error'])
def test_standalone_seed_freeze_requires_all_90_official_tasks(tmp_path, monkeypatch, problem):
    import learning
    controller = training.controller
    expected = [f'appworld-fixture-{index:02d}' for index in range(90)]
    monkeypatch.setattr(controller, 'TRAINING', tmp_path)
    monkeypatch.setattr(controller, 'PUBLISHED', tmp_path/'published')
    monkeypatch.setattr(controller, 'environment', lambda: {})
    monkeypatch.setattr(controller, 'ids', lambda split: expected if split == 'train' else [])
    monkeypatch.setattr(learning, 'learning_usable', lambda *args: True)
    readiness = Mock(side_effect=AssertionError('incomplete training cannot compile memory'))
    monkeypatch.setattr(learning, '_readiness', readiness)
    status = {'status': 'complete', 'scope': 'official_train', 'batch_size': 16,
              'planned_tasks': 90, 'completed_tasks': 90, 'planned_batches': 6,
              'completed_batches': 6, 'imported_batches': 6, 'deferred_reviews': [],
              'last_learning': {'usable': True}}
    tasks = expected[:-1] if problem == 'missing_task' else expected
    rows = [captured_row(task) for task in tasks]
    if problem == 'running':
        status['status'] = 'running'
    elif problem == 'deferred':
        status['deferred_reviews'] = [{'task_id': expected[-1]}]
    elif problem == 'missing_import':
        status['imported_batches'] = 5
    elif problem == 'capture_error':
        rows[-1]['evidence']['output']['final']['framework_capture'] = {'status': 'incomplete'}
    if problem != 'missing_status':
        save(tmp_path / 'full-training-status.json', status)
    path = tmp_path / 'full-captures/train-full-01'
    save(path / 'report.json', {'complete': True, 'trials': rows})
    save(path / 'learning-status.json', {'appworld_input': {'admitted_training_tasks': tasks}})
    with pytest.raises(RuntimeError, match='appworld_training_'):
        learning.freeze_seed(controller)
    readiness.assert_not_called()


@pytest.mark.parametrize('problem', [None, 'six_batches', 'missing_synthetic',
                                    'unusable_import', 'rejected_import', 'wrong_scope',
                                    'wrong_partition', 'capture_error', 'incomplete_loop'])
def test_completed_training_requires_all_seven_mixed_batches(tmp_path, monkeypatch, problem):
    from types import SimpleNamespace
    import dataset
    import learning
    official = [f'official-{index:02d}' for index in range(90)]
    synthetic = [f'synthetic-{index:02d}' for index in range(6)]
    expected = official + synthetic
    canonical = [{'task_id': task, 'initial_state': {}} for task in official]
    canonical += [{'task_id': task, 'initial_state': {'training_goal': {'kind': 'fixture'}}}
                  for task in synthetic]
    monkeypatch.setattr(dataset, 'rows', lambda split: canonical if split == 'train' else [])
    monkeypatch.setattr(learning, 'learning_usable', lambda controller, result, returncode: result['usable'])
    controller = SimpleNamespace(TRAINING=tmp_path, ids=lambda split: expected,
        read=lambda path: json.loads(Path(path).read_text()),
        _trial_error_code=training.controller._trial_error_code,
        framework_capture_complete=training.controller.framework_capture_complete,
        assessed_quality=training.controller.assessed_quality,
        _has_complete_loop_review=training.controller._has_complete_loop_review)
    batches = training.training_batches(expected, 16)
    assert list(map(len, batches)) == [16, 16, 16, 16, 16, 10, 6]
    status = {'status': 'complete', 'scope': 'official_and_public_synthetic_train',
              'batch_size': 16, 'planned_tasks': 96, 'completed_tasks': 96,
              'official_planned_tasks': 90, 'synthetic_planned_tasks': 6,
              'planned_batches': 7, 'completed_batches': 7, 'imported_batches': 7,
              'deferred_reviews': [], 'last_learning': {'usable': True}}
    if problem == 'six_batches':
        status.update(planned_batches=6, completed_batches=6, imported_batches=6)
    elif problem == 'wrong_scope':
        status['scope'] = 'official_train'
    elif problem == 'wrong_partition':
        status.update(official_planned_tasks=96, synthetic_planned_tasks=0)
    save(tmp_path/'full-training-status.json', status)
    for index, batch in enumerate(batches):
        path = tmp_path/'full-captures'/f'train-full-{index+1:02d}'
        rows = [captured_row(task) for task in batch]
        imported = {'usable': True, 'appworld_input': {'admitted_training_tasks': batch,
                    'original_report': str(path/'report.json')}}
        if index == 6:
            if problem == 'missing_synthetic':
                rows.pop()
            elif problem == 'unusable_import':
                imported['usable'] = False
            elif problem == 'rejected_import':
                imported['rejected'] = 1
            elif problem == 'capture_error':
                rows[-1]['evidence']['output']['final']['framework_capture'] = {'status': 'incomplete'}
            elif problem == 'incomplete_loop':
                rows[-1]['evidence']['loop_review']['status'] = 'uncertain'
        save(path/'report.json', {'complete': True, 'trials': rows})
        save(path/'learning-status.json', imported)
    if problem is None:
        assert learning._completed_training_tasks(controller) == set(expected)
    else:
        with pytest.raises(RuntimeError, match='appworld_training_'):
            learning._completed_training_tasks(controller)


def test_reused_unresolved_learning_failure_continues_without_opening_seed_gate(tmp_path, monkeypatch):
    import dataset
    monkeypatch.setattr(dataset, 'materialize', Mock())
    runtime, published = tmp_path / 'runtime', tmp_path / 'published'
    save(runtime / 'runtime.json', {'model': {}})
    isolate_learning_service(runtime, monkeypatch)
    save(published / 'L0.json', {'level': 'L0'})
    first_path, second_path = tmp_path / 'first/report.json', tmp_path / 'second/report.json'
    first = {'complete': True, 'trials': [captured_row('appworld-first')]}
    second = {'complete': True, 'trials': [captured_row('appworld-second')]}
    save(first_path, first); save(second_path, second)
    save(first_path.parent / 'learning-status.json', {'usable': False})
    save(second_path.parent / 'learning-status.json', {'usable': True})
    monkeypatch.setattr(training, 'TRAINING', tmp_path)
    monkeypatch.setattr(training, 'CAPTURES', tmp_path / 'full-captures')
    monkeypatch.setattr(training, 'STATUS', tmp_path / 'status.json')
    monkeypatch.setattr(training.controller, 'RUNTIME', runtime)
    monkeypatch.setattr(training.controller, 'PUBLISHED', published)
    monkeypatch.setattr(training.controller, 'prepare', Mock())
    monkeypatch.setattr(training.controller, 'state', Mock())
    monkeypatch.setattr(training.controller, 'environment', Mock(return_value={}))
    monkeypatch.setattr(training.learning_engine, 'install_prompts', Mock())
    monkeypatch.setattr(training, 'official_ids', Mock(return_value=['appworld-first', 'appworld-second']))
    monkeypatch.setattr(training, 'build_dataset', Mock())
    capture = Mock(side_effect=[(first, first_path, True), (second, second_path, True)])
    monkeypatch.setattr(training, 'capture_batch', capture)
    monkeypatch.setattr(training.learning_engine, 'learning_usable',
                        lambda controller, prior, returncode: prior['usable'])
    learning = Mock(side_effect=RuntimeError('fixture model temporarily unavailable'))
    monkeypatch.setattr(training.learning_engine, 'learn', learning)
    monkeypatch.setattr(sys, 'argv', ['training.py', '--batch-size', '1'])
    assert training.main() == 3
    status = json.loads(training.STATUS.read_text())
    assert capture.call_count == 2 and learning.call_count == 1
    assert status['status'] == 'learning_incomplete'
    assert status['completed_tasks'] == 2 and status['imported_batches'] == 1
    assert json.loads(first_path.read_text()) == first
    assert json.loads(second_path.read_text()) == second


def _control_local_fixture(tmp_path):
    from types import SimpleNamespace
    import learning
    sentinel = 'LOCAL_PRIVATE_CODE_OBSERVATION_USER_ANSWER'
    raw_rows = []
    scenarios = [
        ('passed', [('failed', 'error_a', 'first'), ('completed', None, 'corrected')]),
        ('passed', [('completed', None, 'clean')]),
        ('failed', [('failed', 'error_b', 'same'), ('failed', 'error_b', 'same'),
                    ('failed', 'error_b', 'changed')]),
        ('failed', [('failed', 'error_c', 'other')]),
        ('passed', [('failed', 'error_d', 'fail'), ('completed', None, 'recovered')]),
        ('failed', [('failed', 'error_a', 'again')]),
    ]
    for index, (verdict, actions) in enumerate(scenarios):
        events = []
        for ordinal, (status, code, value) in enumerate(actions):
            aid = f'LOCAL_ACTION_{index}_{ordinal}'
            events.append({'kind': 'action.start', 'event_id': 'LOCAL_EVENT_PRIVATE',
                'action_id': aid, 'loop_id': 'main', 'iteration': ordinal,
                'operation': 'appworld.execute', 'attributes': {'semantic': 'action'},
                'input': {'code': sentinel + value}})
            events.append({'kind': 'action.end', 'event_id': 'LOCAL_EVENT_END_PRIVATE',
                'action_id': aid, 'loop_id': 'main', 'status': status,
                'error': {'code': code} if code else {},
                'output': {'observation': sentinel}})
        raw_rows.append({'task_id': f'LOCAL_PRIVATE_TASK_{index}_1', 'split': 'train',
            'trial_id': f'LOCAL_PRIVATE_TRIAL_{index}', 'verdict': verdict,
            'fixture_quality': int(verdict == 'passed'), 'fixture_loop': index,
            'evidence': {'trace': {'identity': {'run_id': 'LOCAL_PRIVATE_RUN'}, 'events': events},
                         'loop_review': {'status': 'complete'}}})
    source = tmp_path / 'full-captures/batch/report.json'
    save(source, {'complete': True, 'trials': raw_rows})
    save(source.parent / 'metrics.json', {'source_report': str(source)})
    runtime = tmp_path / 'runtime'
    save(runtime / 'service.json', {'control_learning': {'enabled': False,
        'prompt_file': 'existing-native-control-prompt.txt', 'actions': {'appworld.execute': ['code']},
        'max_rules': 16}})
    memory = tmp_path / 'evolution/by-user/fixture/memory/evolution-memory.json'
    save(memory, {'entries': [{'memory_id': 'experience-a', 'candidate': {
        'support_evidence': ['LOCAL_EVENT_SUPPORT'], 'support_episode_count': '3',
        'support_conversation_count': '2'}}]})
    c = SimpleNamespace(TRAINING=tmp_path, RUNTIME=runtime, OWNER='fixture', ROOT=ROOT,
        BINARY=Path('/tmp/trajectory_evolution-controls-helper'),
        read=lambda path: json.loads(Path(path).read_text()), _trial_error_code=lambda row: None,
        framework_capture_complete=lambda row: True,
        assessed_quality=lambda row: row['fixture_quality'],
        _has_complete_loop_review=lambda row: True,
        _invalid_loops=lambda row: row['fixture_loop'])
    return learning, c, raw_rows, sentinel


def test_control_feedback_scans_all_tasks_and_keeps_raw_evidence_local(tmp_path):
    learning, c, rows, sentinel = _control_local_fixture(tmp_path)
    feedback = learning.seed_control_feedback(c, {row['task_id'] for row in rows})
    assert feedback['scanned_tasks'] == feedback['measurable_tasks'] == 6
    assert len(feedback['trials']) >= 4
    assert set(feedback['covered_failure_mechanisms']) == {'error_a', 'error_b', 'error_c', 'error_d'}
    assert any(row['verdict'] == 'passed' and row['failed_actions'] for row in feedback['trials'])
    assert sentinel in json.dumps(feedback['trials'])
    stats = feedback['statistics']
    assert stats['tasks'] == 6 and stats['successful_tasks'] == 3
    assert stats['reviewed_invalid_iterations'] == 15
    assert stats['operations']['appworld.execute'] == {'attempts': 10, 'failed': 7, 'completed': 3,
        'same_input_repeated_failures': 1, 'changed_input_retries': 1,
        'failed_then_successful_actions': 2}
    assert stats['failure_mechanisms']['error_a']['tasks'] == 2
    assert stats['failure_mechanisms']['error_a']['successfully_completed_tasks_after_failure'] == 1


def test_control_model_input_contains_only_approved_memory_contract_and_counts(tmp_path):
    import subprocess
    learning, c, rows, sentinel = _control_local_fixture(tmp_path)
    helper = Path('/tmp/appworld-control-cli-fixture')
    if not c.BINARY.is_file() or not helper.is_file():
        pytest.skip('isolated native control CLI is unavailable')
    policy = json.loads(subprocess.check_output([str(helper)], text=True))
    c.native = training.controller.native
    packet, local, controls = learning.seed_control_input(c, policy, {row['task_id'] for row in rows})
    approved = learning.approved_control_packet(packet)
    for name in ('memories', 'control_sources'):
        assert all(set(card) == {'memory_id', 'scope', 'trigger', 'problem', 'content'} for card in approved[name])
    assert 'target_type' not in json.dumps(approved)
    wire = json.dumps(approved)
    for forbidden in (sentinel, 'LOCAL_PRIVATE_TASK', 'LOCAL_PRIVATE_TRIAL', 'LOCAL_PRIVATE_RUN',
                      'LOCAL_EVENT', 'LOCAL_ACTION', str(tmp_path), 'source_report', 'observation'):
        assert forbidden not in wire
    assert packet['training_statistics']['scanned_training_tasks'] == 6
    assert packet['training_statistics']['memory_support'] == [{
        'memory_id': 'experience-a', 'evidence_support_count': 1,
        'episode_support_count': 3, 'conversation_support_count': 2,
        'direct_failure_support_count': 0, 'failure_support': {}}, {
        'memory_id': 'orchestration-z', 'evidence_support_count': None,
        'episode_support_count': None, 'conversation_support_count': None,
        'direct_failure_support_count': None, 'failure_support': {}}]
    assert packet['memories'][0]['memory_id'] == 'experience-a'
    assert local['trials'] and controls['enabled'] is True
    assert c.read(c.RUNTIME / 'service.json')['control_learning']['enabled'] is False
    diagnostic = {}
    compiled = learning.compile_seed_controls(c, policy, controls,
        {'action_rules': [], 'loop_rules': [], 'schedule_rules': []}, diagnostic)
    assert compiled == policy
    assert diagnostic == {'status': 'no_controls', 'generated_rules': {
        'action_rules': 0, 'loop_rules': 0, 'schedule_rules': 0}}



def test_native_l1_only_sources_are_normalized_before_control_payload_validation(tmp_path):
    import subprocess
    learning, c, rows, sentinel = _control_local_fixture(tmp_path)
    c.BINARY = training.controller.BINARY
    helper = Path('/tmp/appworld-control-cli-fixture')
    if not c.BINARY.is_file() or not helper.is_file():
        pytest.skip('native control CLI is unavailable')
    base = json.loads(subprocess.check_output([str(helper)], text=True))
    options = copy.deepcopy(base['options'])
    options['approved_experiences'] = [item for item in options['approved_experiences']
                                       if item['memory_id'] == 'experience-a']
    c.native = training.controller.native
    policy = json.loads(c.native(c.BINARY, ROOT, ['runtime', 'recompile'],
                                 {'policy': base, 'options': options}))
    controls = c.read(c.RUNTIME / 'service.json')['control_learning']
    controls['enabled'] = True
    native_packet = json.loads(c.native(c.BINARY, ROOT, ['runtime', 'control-input'],
                                       {'policy': policy, 'control_learning': controls}))
    assert 'control_sources' not in native_packet  # Actual native empty-array omission.
    assert len(native_packet['memories']) == 1
    packet, _, _ = learning.seed_control_input(c, policy, {row['task_id'] for row in rows})
    assert packet['control_sources'] == []
    approved = learning.approved_control_packet(packet)
    assert approved['control_sources'] == []
    assert set(approved) == learning.CONTROL_MODEL_FIELDS
    assert sentinel not in json.dumps(approved)
    # Normalization must not admit extra/raw fields or invented controls.
    packet['raw_events'] = []
    with pytest.raises(ValueError, match='payload_fields_invalid'):
        learning.approved_control_packet(packet)


def test_control_payload_local_allowlist_rejects_raw_fields_and_fake_counts(tmp_path):
    import subprocess
    learning, c, rows, _ = _control_local_fixture(tmp_path)
    helper = Path('/tmp/appworld-control-cli-fixture')
    if not c.BINARY.is_file() or not helper.is_file():
        pytest.skip('isolated native control CLI is unavailable')
    policy = json.loads(subprocess.check_output([str(helper)], text=True))
    c.native = training.controller.native
    packet, _, _ = learning.seed_control_input(c, policy, {row['task_id'] for row in rows})
    with pytest.raises(ValueError, match='payload_fields'):
        learning.approved_control_packet(dict(packet, events=[{'code': 'private'}]))
    changed = copy.deepcopy(packet)
    changed['memories'][0]['source_event_ids'] = ['private-event']
    with pytest.raises(ValueError, match='memory_fields'):
        learning.approved_control_packet(changed)
    changed = copy.deepcopy(packet)
    changed['training_statistics']['failure_mechanisms']['error_a']['observation'] = 'private'
    with pytest.raises(ValueError, match='counter_invalid'):
        learning.approved_control_packet(changed)
    changed = copy.deepcopy(packet)
    changed['training_statistics']['operations']['appworld.execute']['attempts'] = True
    with pytest.raises(ValueError, match='counter_invalid'):
        learning.approved_control_packet(changed)


def _control_native_policy(c):
    import subprocess
    helper = Path('/tmp/appworld-control-cli-fixture')
    if not c.BINARY.is_file() or not helper.is_file():
        pytest.skip('isolated native control CLI is unavailable')
    c.native = training.controller.native
    return json.loads(subprocess.check_output([str(helper)], text=True))


def _valid_control_draft():
    return {'action_rules': [], 'schedule_rules': [], 'loop_rules': [{
        'memory_id': 'experience-a', 'max_iterations': 30, 'max_no_progress': 2,
        'max_replans': 1, 'error_codes': ['error_a'], 'reason': 'Recover from observed repeated failure.'}]}


def test_control_generation_uses_native_approval_and_only_two_format_repairs(tmp_path, monkeypatch):
    learning, c, rows, _ = _control_local_fixture(tmp_path)
    policy = _control_native_policy(c)
    c.save = save
    bad = _valid_control_draft(); bad['loop_rules'][0]['memory_id'] = 'unknown-memory'
    model = Mock(side_effect=[ValueError('invalid json'), (bad, {'total_tokens': 2}),
                            (_valid_control_draft(), {'total_tokens': 3})])
    monkeypatch.setattr(learning, '_control_model_reply', model)
    diagnostic = {'published': False}
    compiled = learning.generate_seed_controls(c, policy, {r['task_id'] for r in rows}, diagnostic)
    status = diagnostic['control_learning']
    assert status['model_attempts'] == 3 and status['model_calls'] == 0 and status['status'] == 'generated'
    assert [row['status'] for row in status['attempts']] == ['format_invalid', 'native_contract_rejected', 'generated']
    assert model.call_args_list[1].args[2] == 'format_invalid'
    assert model.call_args_list[2].args[2].startswith('native_contract_rejected:')
    assert status['attempts'][1]['native_error_code']
    assert status['attempts'][1]['draft_structure']['loop_rules'][0]['approved_source'] is False
    assert compiled['options']['loop_rules'][-1]['source_experience']['memory_id'] == 'experience-a'
    assert max(int(rule['max_iterations']) for rule in compiled['options']['loop_rules']) <= 30
    assert json.loads((tmp_path / 'seed-compile-status.json').read_text()) == diagnostic


def test_control_generation_does_not_publish_empty_or_failed_model_draft(tmp_path, monkeypatch):
    learning, c, rows, _ = _control_local_fixture(tmp_path)
    policy = _control_native_policy(c)
    c.save = save
    empty = {'action_rules': [], 'loop_rules': [], 'schedule_rules': []}
    model = Mock(return_value=(empty, {'total_tokens': 1}))
    monkeypatch.setattr(learning, '_control_model_reply', model)
    diagnostic = {'published': False}
    with pytest.raises(RuntimeError, match='no_supported_control_draft'):
        learning.generate_seed_controls(c, policy, {r['task_id'] for r in rows}, diagnostic)
    assert diagnostic['published'] is False
    assert diagnostic['control_learning']['status'] == 'no_controls'
    assert model.call_count == 1
    # Empty drafts cannot present previously compiled controls as new learning.
    existing = copy.deepcopy(policy)
    existing['options']['loop_rules'][-1]['source_experience'] = {'memory_id': 'experience-a', 'content_revision': '1'}
    native_empty = Mock(return_value=json.dumps(existing))
    c.native = native_empty
    native_status = {}
    learning.compile_seed_controls(c, existing, {}, empty, native_status)
    assert native_status['status'] == 'no_controls'


def test_control_model_transport_sends_only_explicitly_authorized_fields(tmp_path, monkeypatch):
    import urllib.request
    learning, c, rows, sentinel = _control_local_fixture(tmp_path)
    policy = _control_native_policy(c)
    packet, _, _ = learning.seed_control_input(c, policy, {r['task_id'] for r in rows})
    prompt = tmp_path / 'native-prompt.txt'; prompt.write_text('Native control contract.')
    config = c.read(c.RUNTIME / 'service.json'); config['control_learning']['prompt_file'] = str(prompt)
    save(c.RUNTIME / 'service.json', config)
    endpoint = 'https://approved-plan.invalid/api/plan/v3/chat/completions'
    save(c.RUNTIME / 'runtime.json', {'model': {'endpoint': endpoint, 'model_id': 'fixture-model',
        'api_key_env': 'OPENAI_API_KEY', 'max_output_tokens': 4096, 'thinking_mode': 'disabled'}})
    c.environment = lambda: {'OPENAI_API_KEY': 'synthetic-test-key'}
    seen = []
    class Response:
        def __enter__(self): return self
        def __exit__(self, *args): pass
        def read(self, limit): return json.dumps({'choices': [{'message': {'content': json.dumps(_valid_control_draft())}}],
            'usage': {'prompt_tokens': 4, 'completion_tokens': 2, 'total_tokens': 6}}).encode()
    class Opener:
        def open(self, request, timeout):
            seen.append(request)
            return Response()
    build = Mock(return_value=Opener())
    monkeypatch.setattr(urllib.request, 'build_opener', build)
    transport = {'model_calls': 0}
    draft, usage = learning._control_model_reply(c, packet,
        repair_reason='native_contract_rejected:control_loop_relaxes_condition', transport=transport)
    assert transport['model_calls'] == 1
    assert draft == _valid_control_draft() and usage['total_tokens'] == 6
    assert seen[0].full_url == endpoint
    request = json.loads(seen[0].data)
    assert 'control_loop_relaxes_condition' in request['messages'][0]['content']
    assert 'Loop rules do not accept primitive_id or target_scope' in request['messages'][0]['content']
    assert 'at most one loop rule for each inherited scope/actor pair' in request['messages'][0]['content']
    assert 'count must not exceed limits.max_rules' in request['messages'][0]['content']
    external = json.loads(request['messages'][1]['content'])
    for name in ('memories', 'control_sources'):
        assert all(set(card) == {'memory_id', 'scope', 'trigger', 'problem', 'content'} for card in external[name])
    wire = seen[0].data.decode()
    for forbidden in (sentinel, 'target_type', 'LOCAL_PRIVATE_TASK', 'LOCAL_PRIVATE_RUN', 'LOCAL_EVENT', str(tmp_path)):
        assert forbidden not in wire
    assert request['thinking'] == {'type': 'disabled'}
    assert build.call_args.args[0].proxies == {}


def test_control_format_repair_exhaustion_stops_after_three_calls(tmp_path, monkeypatch):
    learning, c, rows, _ = _control_local_fixture(tmp_path)
    policy = _control_native_policy(c); c.save = save
    model = Mock(side_effect=ValueError('broken json'))
    monkeypatch.setattr(learning, '_control_model_reply', model)
    diagnostic = {'published': False}
    with pytest.raises(RuntimeError, match='format_repair_exhausted'):
        learning.generate_seed_controls(c, policy, {r['task_id'] for r in rows}, diagnostic)
    assert model.call_count == 3 and diagnostic['control_learning']['status'] == 'format_repair_exhausted'
    assert diagnostic['published'] is False


def _native_review_pair(path, rows, *, complete=True, manifest_complete=True, run='fixture-native-run'):
    report = {'complete': complete, 'run_id': run, 'dataset_version': 'fixture-dataset', 'trials': rows}
    save(path, report)
    save(path.with_name('manifest.json'), {'complete': manifest_complete, 'run_id': run,
        'dataset_version': 'fixture-dataset', 'files': {'report.json': 'existing-native-binding'}})
    return report


def test_capture_reuses_complete_nested_native_review_without_more_api_calls(tmp_path, monkeypatch):
    captures = tmp_path / 'captures'; batch = captures / 'batch'
    tasks = ['appworld-first', 'appworld-second']
    raw = [captured_row(task, judged=False) for task in tasks]
    original = _native_review_pair(batch / 'report.json', raw)
    final = [captured_row(tasks[0]), task_failure(tasks[1])]
    # Keep exactly the original capture even though the valid task verdict
    # becomes failed. A score is never a criterion for reuse.
    final[1]['evidence']['output'] = copy.deepcopy(raw[1]['evidence']['output'])
    candidate = batch / 'final-review/retry-3/report.json'
    reviewed = _native_review_pair(candidate, final, run='final-native-run')
    # A later partial judge retry must not hide the earlier complete pair.
    partial_path = batch / 'regrade/report.json'
    _native_review_pair(partial_path, raw, run='incomplete-review-run')
    save(batch / 'metrics.json', {'source_report': str(partial_path)})
    immutable = {p: p.read_bytes() for p in (candidate, candidate.with_name('manifest.json'))}
    monkeypatch.setattr(training, 'CAPTURES', captures)
    monkeypatch.setattr(training, 'evaluation_config', lambda *args: {})
    no_capture = Mock(side_effect=AssertionError('healthy original execution must be reused'))
    no_judge = Mock(side_effect=AssertionError('completed native review must be reused'))
    monkeypatch.setattr(training.controller.shared, 'capture', no_capture)
    monkeypatch.setattr(training.controller, 'regrade_report', no_judge)
    graded, source, reused = training.capture_batch('batch', tasks, {}, 16, {})
    assert reused and source == candidate and graded == reviewed
    assert graded['trials'][1]['verdict'] == 'failed'
    assert json.loads((batch / 'metrics.json').read_text())['source_report'] == str(candidate)
    no_capture.assert_not_called(); no_judge.assert_not_called()
    assert all(p.read_bytes() == data for p, data in immutable.items())
    assert training.same_capture(original, graded, tasks)


def test_complete_review_reuse_rejects_missing_manifest_scope_cost_or_trace_changes(tmp_path):
    rows = [captured_row('appworld-first'), captured_row('appworld-second')]
    original = {'complete': True, 'trials': rows}
    tasks = [row['task_id'] for row in rows]
    missing = tmp_path / 'missing/report.json'
    _native_review_pair(missing, rows); missing.with_name('manifest.json').unlink()
    incomplete = tmp_path / 'incomplete/report.json'
    _native_review_pair(incomplete, rows, manifest_complete=False)
    scope = tmp_path / 'scope/report.json'
    _native_review_pair(scope, rows[:1])
    costs = copy.deepcopy(rows); costs[0]['metrics']['tool_calls'] += 1
    _native_review_pair(tmp_path / 'costs/report.json', costs)
    changed = copy.deepcopy(rows); changed[0]['evidence']['trace']['events'].append({'kind': 'action.end'})
    _native_review_pair(tmp_path / 'trace/report.json', changed)
    mismatched = tmp_path / 'binding/report.json'
    _native_review_pair(mismatched, rows)
    m = json.loads(mismatched.with_name('manifest.json').read_text()); m['run_id'] = 'other-run'
    save(mismatched.with_name('manifest.json'), m)
    assert training.complete_native_review(original, tmp_path, tasks) is None


def test_complete_review_profile_filter_preserves_first_healthy_source(tmp_path):
    import os
    rows = [captured_row('appworld-first')]
    original = {'complete': True, 'trials': rows}
    tasks = [rows[0]['task_id']]
    first = tmp_path / 'first/report.json'
    later = tmp_path / 'later/report.json'
    wrong = tmp_path / 'wrong/report.json'
    for index, path in enumerate((first, later, wrong), 1):
        _native_review_pair(path, rows, run=f'original-native-{index}')
        os.utime(path, ns=(index * 1000000000, index * 1000000000))
    unchanged = {p: p.read_bytes() for p in (first, later, wrong)}
    assert training.complete_native_review(original, tmp_path, tasks)[1] == wrong
    # Newer wrong-profile evidence cannot mask an earlier current-profile
    # pair; the ordinary default retains its established recency behavior.
    selected = training.complete_native_review(original, tmp_path, tasks,
        source_compatible=lambda path: path != wrong)
    assert selected[1] == later
    selected = training.complete_native_review(original, tmp_path, tasks,
        source_compatible=lambda path: path != wrong, prefer_earliest=True)
    assert selected[1] == first
    assert all(path.read_bytes() == data for path, data in unchanged.items())


@pytest.mark.parametrize('count', [64, 65, 97])
def test_seed_shadow_capacity_grows_only_for_native_exclusion_and_never_drops_card(tmp_path, count):
    import learning
    from types import SimpleNamespace
    options = {'max_policy_entries': 64, 'max_advice_items': 2,
        'max_advice_bytes': 2048, 'max_query_bytes': 32768, 'max_snapshot_bytes': 262144,
        'require_validated_evidence': True}
    packets = []
    def native(binary, root, command, packet):
        assert command == ['shadow', 'compile']
        packets.append(copy.deepcopy(packet))
        limit = packet['options']['max_policy_entries']
        return json.dumps({'experiences': [{'memory_id': f'card-{i}'} for i in range(min(count, limit))],
                           'excluded_counts': {'capacity': max(0, count-limit)}})
    controller = SimpleNamespace(native=native, BINARY=Path('fixture-native'), ROOT=tmp_path)
    if count > 96:
        with pytest.raises(RuntimeError, match='appworld_seed_shadow_capacity_exhausted'):
            learning._compile_seed_snapshot(controller, tmp_path/'memory.json', {}, options)
    else:
        snapshot = learning._compile_seed_snapshot(controller, tmp_path/'memory.json', {}, options)
        assert len(snapshot['experiences']) == count
        assert snapshot['excluded_counts']['capacity'] == 0
    assert len(packets) == (1 if count <= 64 else 2)
    assert options['max_policy_entries'] == (64 if count <= 64 else 96)
    assert {key:value for key,value in packets[-1]['options'].items() if key != 'max_policy_entries'} == {
        key:value for key,value in packets[0]['options'].items() if key != 'max_policy_entries'}


def test_seed_shadow_capacity_retains_65_with_real_native_compiler(tmp_path):
    import learning
    from types import SimpleNamespace
    binary = ROOT / 'tools/trajectory_evolution/build-v4/trajectory_evolution'
    if not binary.is_file():
        pytest.skip('native binary unavailable')
    entries = [{'memory_id': f'fixture-{i}', 'status': 'EVOLUTION_MEMORY_STATUS_CANDIDATE',
        'candidate': {'schema_version': 1, 'candidate_id': f'candidate-{i}',
            'generator_version': 'scoped-experience-generator-v1',
            'target_type': 'CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION',
            'target_scope': 'coordinator', 'status': 'EVOLUTION_CANDIDATE_STATUS_PROPOSED',
            'trigger_pattern': '分页读取尚未找到目标记录', 'observed_problem': '只读取首页',
            'proposed_change': '读取完整分页后再进行需要目标记录的操作。', 'support_user_count': 1,
            'support_evidence': [{'evidence_id': f'evidence-{i}', 'source': {
                'user_id': 'fixture-user', 'dialogue_id': f'dialogue-{i}'},
                'verdict': 'SEMANTIC_VERDICT_USEFUL'}]}} for i in range(65)]
    memory = tmp_path / 'memory.json'
    save(memory, {'schema_version': 1, 'store_kind': 'trajectory_evolution_memory',
        'store_version': 'evolution-memory-store-v1', 'revision': '1', 'entries': entries})
    options = {'target_scopes': ['coordinator'], 'max_policy_entries': 64,
        'max_advice_items': 2, 'max_advice_bytes': 2048, 'max_query_bytes': 32768,
        'max_memory_bytes': 16777216, 'max_snapshot_bytes': 262144,
        'max_match_terms': 128, 'min_matched_terms': 2, 'require_validated_evidence': True}
    identity = {'agent_id': 'fixture-agent', 'user_id': 'fixture-user',
                'conversation_id': 'fixture-conversation', 'run_id': 'fixture-run'}
    controller = SimpleNamespace(native=training.controller._policy_native, BINARY=binary, ROOT=ROOT)
    before = memory.read_bytes()
    snapshot = learning._compile_seed_snapshot(controller, memory, identity, options)
    assert len(snapshot['experiences']) == 65
    assert set(item['memory_id'] for item in snapshot['experiences']) == {entry['memory_id'] for entry in entries}
    assert not snapshot.get('excluded_counts', {}).get('capacity')
    assert options['max_policy_entries'] == 96
    assert snapshot['options']['max_advice_items'] == 2
    assert snapshot['options']['max_snapshot_bytes'] == 262144
    assert memory.read_bytes() == before


@pytest.mark.parametrize('count', [95, 111, 129])
def test_v2_shadow_capacity_expands_only_after_actual_96_shortage_without_changing_advice_budget(tmp_path, count):
    import learning
    from types import SimpleNamespace
    options = {'max_policy_entries': 64, 'max_advice_items': 1, 'max_advice_bytes': 2048,
               'max_query_bytes': 32768, 'max_snapshot_bytes': 262144}
    packets = []
    def native(binary, root, command, packet):
        assert command == ['shadow', 'compile']
        packets.append(copy.deepcopy(packet))
        limit = packet['options']['max_policy_entries']
        return json.dumps({'experiences': [{'memory_id': str(index)} for index in range(min(count, limit))],
                           'excluded_counts': {'capacity': str(count-limit)} if count > limit else {}})
    c = SimpleNamespace(native=native, ROOT=tmp_path, BINARY=Path('fixture-native'),
                        environment=lambda: {'APPWORLD_SEED_L1_WINDOW_MODE': 'observed_phase_v2'})
    if count > 128:
        with pytest.raises(RuntimeError, match='shadow_capacity_exhausted'):
            learning._compile_seed_snapshot(c, tmp_path/'memory.json', {}, options)
    else:
        snapshot = learning._compile_seed_snapshot(c, tmp_path/'memory.json', {}, options)
        assert len(snapshot['experiences']) == count
    assert [packet['options']['max_policy_entries'] for packet in packets] == (
        [64, 96] if count <= 96 else [64, 96, 128])
    assert all({key:value for key,value in packet['options'].items() if key != 'max_policy_entries'} == {
        key:value for key,value in packets[0]['options'].items() if key != 'max_policy_entries'} for packet in packets)


def test_partial_checkpoint_keeps_native_healthy_trial_and_excludes_unclosed(tmp_path):
    batch = tmp_path / 'batch'
    interrupted = batch / 'capture-retry-01'
    started = {'run_id': 'native-started', 'dataset_version': 'native-dataset',
               'planned_trials': 2, 'complete': False}
    save(interrupted / 'started.json', started)
    healthy, unclosed = captured_row('healthy'), captured_row('unclosed')
    unclosed['evidence']['trace']['events'].pop()
    originals = {}
    for row in [healthy, unclosed]:
        path = interrupted / 'trials' / (row['trial_id'] + '.json')
        save(path, row)
        originals[path] = path.read_bytes()
    checkpoint = training.partial_native_checkpoint(batch, ['healthy', 'unclosed'])
    assert checkpoint == (interrupted, started, [healthy])
    assert all(path.read_bytes() == before for path, before in originals.items())


def test_partial_checkpoint_real_native_reuses_healthy_and_runs_missing_stub_once(tmp_path, monkeypatch):
    import os
    import subprocess
    binary = ROOT / 'tools/trajectory_evolution/build-v4/trajectory_evolution'
    if not binary.is_file():
        pytest.skip('native binary unavailable')
    source = tmp_path/'tasks.jsonl'
    source.write_text('\n'.join(json.dumps({'task_id': task, 'group_id': task,
        'task_input': '计算给定总量 ' + task, 'expected_behavior': '总量20', 'difficulty': 'easy',
        'category': 'data', 'initial_state': {'value': 20}, 'split': 'train',
        'review_status': 'reviewed'}) for task in ['one', 'two']))
    dataset_options = json.loads((ROOT/'tools/trajectory_evolution/config/dataset.json').read_text())
    dataset_options['sources'] = [{'kind': 'golden', 'path': str(source)}]
    save(tmp_path/'dataset-config.json', dataset_options)
    dataset = tmp_path/'dataset'
    env = dict(os.environ, TRAJECTORY_UNCONDITIONAL_RESUME='1')
    built = subprocess.run([str(binary), 'dataset', 'build', '--config', str(tmp_path/'dataset-config.json'),
        '--output-root', str(dataset)], text=True, capture_output=True, env=env)
    assert built.returncode == 0, built.stderr
    counter = tmp_path/'stub-calls.jsonl'
    host = tmp_path/'host.py'
    host.write_text('import json, sys\nfrom pathlib import Path\n'
        'request=json.loads(sys.stdin.read())\n'
        f'with Path({str(counter)!r}).open("a") as stream: stream.write(request["task"]["task_id"]+"\\n")\n'
        'print(json.dumps({"schema_version":1,"trial_id":request["trial_id"],'
        '"execution_status":"completed","output":"fixture completed","state":{"value":20}}))\n')
    config = {'schema_version': 1, 'splits': ['train'], 'task_ids': ['one'], 'repetitions': 1,
        'parallelism': 1, 'timeout_ms': 5000, 'max_output_bytes': 65536,
        'variants': [{'id': 'candidate', 'version': 'fixture-v1', 'command': [sys.executable, '-B', str(host)]}],
        'checks': {'*': [{'id': 'value', 'path': '/state/value', 'op': 'equals', 'value': 20}]},
        'judge': {'enabled': False}, 'artifact_retention': {'retain_trial_files': True}}
    config_path = tmp_path/'config.json'
    save(config_path, config)
    first = tmp_path/'first'
    captured = subprocess.run([str(binary), 'dataset', 'run', str(dataset), '--config', str(config_path),
        '--output-root', str(first)], text=True, capture_output=True, env=env)
    assert captured.returncode == 0, captured.stderr
    original = json.loads((first/'report.json').read_text())
    row = original['trials'][0]
    raw = first/'trials'/(row['trial_id']+'.json')
    before = raw.read_bytes()
    config['task_ids'] = ['one', 'two']; save(config_path, config)
    destination = tmp_path/'resumed'; destination.mkdir()
    monkeypatch.setattr(training, 'BINARY', binary)
    monkeypatch.setattr(training, 'FULL_DATASET', dataset)
    checkpoint = (first, json.loads((first/'started.json').read_text()), [row])
    resumed, path = training.resume_partial_checkpoint(checkpoint, destination, config_path, env)
    assert resumed['complete'] is True
    assert resumed['summary']['reused_trials'] == 1
    assert counter.read_text().splitlines() == ['one', 'two']
    assert next(item for item in resumed['trials'] if item['task_id'] == 'one') == row
    assert raw.read_bytes() == before
    assert not list(destination.glob('.checkpoint-*'))


def test_seed_freeze_requires_completed_zero_model_native_evidence_refresh(tmp_path, monkeypatch):
    from types import SimpleNamespace
    import learning
    c = SimpleNamespace(TRAINING=tmp_path, save=save, PUBLISHED=tmp_path / 'published',
        BINARY=Path('fixture-native'), ROOT=ROOT,
        TRAINING_SERVICE=tmp_path / 'service.json', native=Mock(return_value=json.dumps({
            'status': 'error', 'model_calls': 0, 'reason': 'evidence_refresh_learning_busy'})))
    c.environment = lambda: {}
    monkeypatch.setattr(learning, '_completed_training_tasks', lambda controller: {'appworld-fixture'})
    compile_not_reached = Mock(side_effect=AssertionError('failed refresh cannot reach policy compilation'))
    monkeypatch.setattr(learning, '_readiness', compile_not_reached)
    with pytest.raises(RuntimeError, match='native_evidence_refresh_incomplete'):
        learning.freeze_seed(c)
    assert c.native.call_count == 1 and compile_not_reached.call_count == 0
    assert not (c.PUBLISHED / 'seed0/strategy.json').exists()


def test_private_native_learn_passes_principal_environment_and_preserves_error_code(monkeypatch):
    from types import SimpleNamespace
    c = training.controller
    run = Mock(return_value=SimpleNamespace(returncode=0, stdout='{"status":"completed","model_calls":0}', stderr=''))
    env = {'EVOLUTION_APPWORLD_TOKEN': 'private-principal-fixture-at-least-24', 'NO_PROXY': '*'}
    monkeypatch.setattr(c, 'environment', lambda: env)
    monkeypatch.setattr(c.subprocess, 'run', run)
    assert json.loads(c.native(Path('fixture-native'), ROOT,
                              ['learn', '--config', 'fixture.json', '--refresh-evidence'], {}))['model_calls'] == 0
    assert run.call_args.kwargs['env'] == env
    run.return_value = SimpleNamespace(returncode=2, stdout='',
        stderr='ERROR: authentication_secret_missing_or_short\n')
    with pytest.raises(RuntimeError) as actual:
        c.native(Path('fixture-native'), ROOT, ['learn', '--refresh-evidence'], {})
    assert actual.value.native_reason == 'authentication_secret_missing_or_short'
    assert actual.value.native_returncode == 2


@pytest.mark.parametrize('code', [13, 26])
@pytest.mark.parametrize('operation', [['runtime', 'query'], ['learn', '--refresh-evidence']])
def test_native_retries_only_target_executable_preexec_faults(monkeypatch, code, operation):
    from types import SimpleNamespace
    c = training.controller
    binary = Path('temporarily-replaced-native')
    fault = OSError(code, 'temporary fixture', str(binary))
    result = SimpleNamespace(returncode=0, stdout='{"status":"completed"}', stderr='')
    run = Mock(side_effect=[fault, fault, result]);sleep = Mock()
    monkeypatch.setattr(c.subprocess, 'run', run)
    monkeypatch.setattr(c.time, 'sleep', sleep)
    monkeypatch.setattr(c, 'environment', lambda: {})
    assert json.loads(c.native(binary, ROOT, operation, {}))['status'] == 'completed'
    assert run.call_count == 3 and sleep.call_count == 2
    run.side_effect = fault;run.reset_mock();sleep.reset_mock()
    with pytest.raises(OSError):
        c.native(binary, ROOT, operation, {})
    assert run.call_count == 9 and sleep.call_count == 8


@pytest.mark.parametrize('failure', [
    OSError(13, 'other path', '/different-path'),
    OSError(2, 'missing executable', 'temporarily-replaced-native'),
    __import__('subprocess').TimeoutExpired('already-started-native', 30)])
def test_native_does_not_retry_non_preexec_failures(monkeypatch, failure):
    c = training.controller
    run = Mock(side_effect=failure);sleep = Mock()
    monkeypatch.setattr(c.subprocess, 'run', run)
    monkeypatch.setattr(c.time, 'sleep', sleep)
    with pytest.raises(type(failure)):
        c.native(Path('temporarily-replaced-native'), ROOT, ['runtime', 'query'], {})
    assert run.call_count == 1;sleep.assert_not_called()


@pytest.mark.parametrize('stdout', [
    'ERROR: training_task_not_admissible\npassword="benchmark-password-fixture"\n',
    '{"status":"error","error_code":"training_task_not_admissible"}',
    '["invalid native status"]',
])
def test_native_learning_failure_persists_redacted_diagnostics_and_fixed_reason(tmp_path, monkeypatch, stdout):
    import learning
    from types import SimpleNamespace
    baseline = {'level': 'L0', 'agent': {'revision': 'host-version'}}
    save(tmp_path / 'published/L0.json', baseline)
    row = captured_row('fixture-task')
    row['evidence']['loaded_version'] = 'artifact-version'
    row['evidence']['trace']['identity'] = {'user_id': 'owner', 'agent_id': 'agent',
        'conversation_id': 'conversation', 'run_id': 'run'}
    row['evidence']['trace']['events'][0]['attributes'] = {
        'agent_version_id': 'artifact-version', 'evaluation_trial_id': row['trial_id']}
    report = {'trials': [row], 'configuration': {'variants': [{'id': 'candidate', 'version': 'host-version'}]}}
    source = tmp_path / 'capture/report.json'; save(source, report)
    c = SimpleNamespace(PUBLISHED=tmp_path / 'published', ROOT=ROOT, BINARY=Path('fixture-native'),
        TRAINING_SERVICE=tmp_path / 'service.json', DATASET=tmp_path / 'native99',
        OWNER='owner', AGENT='agent', FULL_TRAIN_IDS=['fixture-task'], save=save,
        read=lambda path: json.loads(Path(path).read_text()), version=lambda artifact: 'artifact-version',
        framework_capture_complete=lambda captured: True,
        output=lambda captured: captured['evidence']['output']['final'], state=Mock())
    monkeypatch.setattr(learning, '_readiness', lambda controller: tmp_path / 'memory.json')
    process = SimpleNamespace(returncode=2, communicate=Mock(return_value=(stdout,
        'ERROR: training_task_not_admissible\nAuthorization: Bearer bearer-credential-fixture\n'
        'api_key="unknown-key-fixture"\nprincipal-credential-fixture\n')), poll=lambda: 2)
    spawn = Mock(return_value=process)
    monkeypatch.setattr(learning.subprocess, 'Popen', spawn)
    result = learning._learn_unlocked(c, report, source, {'EVOLUTION_APPWORLD_TOKEN': 'principal-credential-fixture'})
    assert result['usable'] is False and result['reason'] == 'training_task_not_admissible'
    diagnostic = json.loads(source.with_name('learning-status.json').read_text())
    assert diagnostic['error_code'] == 'training_task_not_admissible' and diagnostic['exit_code'] == 2
    log = source.with_name('learning.log').read_text()
    assert 'training_task_not_admissible' in log
    assert all(value not in log for value in ('benchmark-password-fixture', 'bearer-credential-fixture',
                                           'unknown-key-fixture', 'principal-credential-fixture'))
    spawn.assert_called_once()


def test_expanded_batch_imports_only_new_three_and_keeps_original_nine_bytes(tmp_path, monkeypatch):
    import learning
    from types import SimpleNamespace
    save(tmp_path / 'published/L0.json', {'level': 'L0', 'agent': {'revision': 'host-version'}})
    rows = []
    for i in range(9):
        row = captured_row(f'fixture-{i}')
        row['task_version'] = 'old-task-version' if i < 6 else 'current-task-version'
        row['evidence']['loaded_version'] = 'artifact-version'
        row['evidence']['trace']['identity'] = {'user_id': 'owner', 'agent_id': 'agent',
            'conversation_id': f'conversation-{i}', 'run_id': f'run-{i}'}
        row['evidence']['trace']['events'][0]['attributes'] = {
            'agent_version_id': 'artifact-version', 'evaluation_trial_id': row['trial_id']}
        rows.append(row)
    report = {'complete': True, 'trials': rows,
        'configuration': {'variants': [{'id': 'candidate', 'version': 'host-version'}]}}
    source = tmp_path / 'capture/report.json'; save(source, report)
    save(source.with_name('manifest.json'), {'files': {'report.json': 'original-native-binding'}})
    before = source.read_bytes(), source.with_name('manifest.json').read_bytes()
    c = SimpleNamespace(PUBLISHED=tmp_path / 'published', ROOT=ROOT, BINARY=Path('fixture-native'),
        TRAINING_SERVICE=tmp_path / 'service.json', DATASET=tmp_path / 'native99',
        OWNER='owner', AGENT='agent', FULL_TRAIN_IDS=[r['task_id'] for r in rows], save=save,
        read=lambda path: json.loads(Path(path).read_text()), version=lambda artifact: 'artifact-version',
        framework_capture_complete=lambda captured: True,
        output=lambda captured: captured['evidence']['output']['final'], state=Mock())
    monkeypatch.setattr(learning, '_readiness', lambda controller: tmp_path / 'memory.json')
    old_ids = {r['task_id'] for r in rows[:6]}
    monkeypatch.setattr(learning, '_reused_training_sources', lambda *args: (old_ids, ['verified-original-source']))
    def spawn(command, **kwargs):
        imported = Path(command[command.index('--training-evaluation') + 1])
        derived = json.loads((imported / 'report.json').read_text())
        assert derived['trials'] == rows[6:]
        assert derived['configuration'] == report['configuration']
        assert (imported / 'manifest.json').is_file()
        return SimpleNamespace(returncode=2, communicate=lambda **kwargs: ('', 'ERROR: fixture_native_exit'), poll=lambda: 2)
    native = Mock(side_effect=spawn)
    monkeypatch.setattr(learning.subprocess, 'Popen', native)
    result = learning._learn_unlocked(c, report, source, {})
    assert result['admissible_trials'] == 9
    provenance = json.loads(source.with_name('learning-status.json').read_text())['appworld_input']
    assert set(provenance['admitted_training_tasks']) == {r['task_id'] for r in rows}
    assert set(provenance['reused_training_tasks']) == old_ids
    assert provenance['native_import_training_tasks'] == [r['task_id'] for r in rows[6:]]
    assert provenance['original_scores_and_events_preserved'] is True
    assert (source.read_bytes(), source.with_name('manifest.json').read_bytes()) == before
    assert not (source.parent / '.learning-input').exists()
    native.assert_called_once()


def test_seed_refresh_call_failure_is_persisted_before_compilation(tmp_path, monkeypatch):
    import learning
    from types import SimpleNamespace
    error = RuntimeError('native_learn_failed')
    error.native_reason = 'authentication_secret_missing_or_short'
    c = SimpleNamespace(TRAINING=tmp_path, PUBLISHED=tmp_path/'published', ROOT=ROOT,
        BINARY=Path('fixture-native'), TRAINING_SERVICE=tmp_path/'service.json', save=save,
        native=Mock(side_effect=error))
    c.environment = lambda: {}
    monkeypatch.setattr(learning, '_completed_training_tasks', lambda controller: {'fixture-task'})
    readiness = Mock(side_effect=AssertionError('failed refresh must not compile'))
    monkeypatch.setattr(learning, '_readiness', readiness)
    with pytest.raises(RuntimeError, match='native_learn_failed'):
        learning.freeze_seed(c)
    diagnostic = json.loads((tmp_path/'seed-compile-status.json').read_text())
    assert diagnostic['status'] == 'evidence_refresh_failed'
    assert diagnostic['evidence_refresh']['reason'] == 'authentication_secret_missing_or_short'
    readiness.assert_not_called()


@pytest.mark.parametrize('prefix,suffix', [('', ''), ('```json\n', '\n```'),
    ('```\n', '\n```'), ('```JSON\r\n', '\r\n```'), ('  \n``` Json \n', '\n```\t')])
def test_control_reply_accepts_only_whole_native_json_fences(prefix, suffix):
    import learning
    expected = _valid_control_draft()
    assert learning._parse_control_draft(prefix + json.dumps(expected) + suffix) == expected
    optional_schedule = {key: value for key, value in expected.items() if key != 'schedule_rules'}
    assert learning._parse_control_draft(prefix + json.dumps(optional_schedule) + suffix) == expected


@pytest.mark.parametrize('content', [
    'Explanation\n```json\n{}\n```', '```json\n{}\n```\nExplanation',
    '```python\n{}\n```', '```json\n{}', '```json\n{}\n```\n```json\n{}\n```',
    '{"action_rules":[],"action_rules":[],"loop_rules":[]}',
    '{"action_rules":[],"loop_rules":[{"max_iterations":NaN}]}',
    '{"action_rules":[],"loop_rules":', '{"action_rules":[],"loop_rules":[],"schedule_rules":"[]"}',
])
def test_control_reply_rejects_prose_truncation_duplicates_and_invalid_shape(content):
    import learning
    with pytest.raises(ValueError):
        learning._parse_control_draft(content)


def test_private_native_control_compile_retains_safe_specific_rejection(monkeypatch):
    from types import SimpleNamespace
    c = training.controller
    run = Mock(return_value=SimpleNamespace(returncode=2,
        stdout='{"status":"error","reason":"control_loop_relaxes_limit"}', stderr=''))
    monkeypatch.setattr(c.subprocess, 'run', run)
    with pytest.raises(RuntimeError, match='native_runtime_control-compile_failed') as actual:
        c.native(Path('fixture-native'), ROOT, ['runtime', 'control-compile'], {})
    assert actual.value.native_reason == 'control_loop_relaxes_limit'
    assert run.call_args.kwargs['env'] is None
    run.return_value.stdout = '{"status":"error","reason":"SECRET values cannot be diagnostic"}'
    with pytest.raises(RuntimeError) as unsafe:
        c.native(Path('fixture-native'), ROOT, ['runtime', 'control-compile'], {})
    assert unsafe.value.native_reason == 'native_learning_failed'


def test_loop_boundary_statistics_preserve_unknown_native_counters_and_real_actor():
    import learning
    events = []
    for index, (progress, code) in enumerate([(0, 'PRIVATE_A'), (0, 'PRIVATE_B'), (0, 'PRIVATE_B'), (1, 'PRIVATE_C')]):
        events += [{'kind': 'step.start', 'loop_id': 'main', 'execution_id': 'PRIVATE_EXEC',
            'actor_id': 'unspecified', 'attributes': {'type': 'boundary.control', 'policy_scope': 'default'},
            'input': {'progress': progress, 'last_code': code}},
            {'kind': 'loop.decision', 'loop_id': 'main', 'execution_id': 'PRIVATE_EXEC',
             'actor_id': 'unspecified', 'decision': 'continue'}]
    actual = learning._control_loop_observations(events, training.controller.SCOPE)[0]
    assert actual['policy_scope'] == 'default' and actual['actor_id'] == 'unspecified'
    assert actual['target_scope'] == training.controller.SCOPE
    assert actual['unchanged_progress_pairs'] == 2 and actual['max_unchanged_progress_streak'] == 2
    assert actual['equal_state_pairs'] == 1 and actual['max_equal_state_streak'] == 1
    assert actual['native_no_progress_samples'] == 0 and actual['native_no_progress_max'] is None
    assert actual['continue_decisions'] == 4 and actual['replan_decisions'] == 0
    assert 'PRIVATE_' not in json.dumps(actual)


def test_memory_mechanism_statistics_bind_only_exact_positive_cited_support():
    import learning
    candidate = {'support_evidence': [
        {'source': {'dialogue_id': 'LOCAL_RUN'}, 'verdict': 'SEMANTIC_VERDICT_NECESSARY_RETRY',
         'evidence_event_ids': ['LOCAL_FAILED_EVENT']},
        {'source': {'dialogue_id': 'LOCAL_RUN'}, 'verdict': 'SEMANTIC_VERDICT_UNASSESSED',
         'evidence_event_ids': ['LOCAL_FAILED_EVENT']},
        {'source': {'dialogue_id': 'UNKNOWN_RUN'}, 'verdict': 'SEMANTIC_VERDICT_USEFUL',
         'evidence_event_ids': ['LOCAL_FAILED_EVENT']}]}
    direct, statistics = learning._memory_failure_support(candidate, {'LOCAL_RUN': {
        'task_id': 'LOCAL_TASK', 'verdict': 'passed', 'event_mechanisms': {'LOCAL_FAILED_EVENT': 'appworld_auth_401'}}})
    assert direct == 1
    assert statistics == {'appworld_auth_401': {'validated_supports': 1, 'source_tasks': 1,
        'successful_source_tasks': 1, 'necessary_retry_supports': 1}}
    assert 'LOCAL_' not in json.dumps(statistics)


def test_empty_controls_get_only_one_supported_clarification(tmp_path, monkeypatch):
    learning, c, rows, _ = _control_local_fixture(tmp_path)
    policy = _control_native_policy(c); c.save = save
    packet, feedback, controls = learning.seed_control_input(c, policy, {r['task_id'] for r in rows})
    support = packet['training_statistics']['memory_support'][0]
    support['direct_failure_support_count'] = 1
    support['failure_support'] = {'error_a': {'validated_supports': 1, 'source_tasks': 1,
        'successful_source_tasks': 1, 'necessary_retry_supports': 1}}
    monkeypatch.setattr(learning, 'seed_control_input', lambda *args: (packet, feedback, controls))
    empty = {'action_rules': [], 'loop_rules': [], 'schedule_rules': []}
    model = Mock(return_value=(empty, {})); monkeypatch.setattr(learning, '_control_model_reply', model)
    diagnostic = {}
    with pytest.raises(RuntimeError, match='evidence_recovery_no_cited_failure_pair'):
        learning.generate_seed_controls(c, policy, {r['task_id'] for r in rows}, diagnostic)
    assert model.call_count == 2
    assert model.call_args_list[1].args[2] == 'no_controls_with_observed_recovery'
    assert diagnostic['control_learning']['status'] == 'no_controls'
    assert diagnostic['control_learning']['evidence_fallback']['status'] == 'insufficient_evidence'
    assert diagnostic['control_learning']['evidence_fallback']['reason'] == 'appworld_evidence_recovery_no_cited_failure_pair'
    support['failure_support']['error_a']['task_ids'] = ['LOCAL_PRIVATE_TASK']
    with pytest.raises(ValueError, match='mechanism_fields_invalid'):
        learning.approved_control_packet(packet)


def _fallback_fixture(tmp_path):
    learning, c, rows, _ = _control_local_fixture(tmp_path)
    policy = _control_native_policy(c); c.save = save
    for index, row in enumerate(rows):
        row['evidence']['trace']['identity'] = {'user_id': 'fixture',
            'conversation_id': 'LOCAL_CONVERSATION_' + str(index), 'run_id': 'LOCAL_RUN_' + str(index)}
        for ordinal, event in enumerate(row['evidence']['trace']['events']):
            event['event_id'] = f'LOCAL_EVENT_{index}_{ordinal}'
            event.update(execution_id='LOCAL_EXECUTION', actor_id='unspecified')
    chosen = rows[2]
    chosen['verdict'] = 'passed'; chosen['fixture_quality'] = 1
    ends = [event for event in chosen['evidence']['trace']['events'] if event['kind'] == 'action.end']
    for event in ends[:2]: event['error'] = {'code': 'appworld_auth_401'}
    source = tmp_path/'full-captures/batch/report.json'
    save(source, {'complete': True, 'trials': rows})
    memory_path = tmp_path/'evolution/by-user/fixture/memory/evolution-memory.json'
    save(memory_path, {'entries': [{'memory_id': 'experience-a', 'candidate': {'support_evidence': [{
        'evidence_id': 'LOCAL_NATIVE_EVIDENCE', 'source': {'user_id': 'fixture',
         'conversation_id': 'LOCAL_CONVERSATION_2', 'dialogue_id': 'LOCAL_RUN_2'},
        'episode_ids': ['LOCAL_EPISODE'], 'evidence_event_ids': [event['event_id'] for event in ends[:2]],
        'verdict': 'SEMANTIC_VERDICT_NECESSARY_RETRY'}]}}]})
    feedback = learning.seed_control_feedback(c, {row['task_id'] for row in rows})
    return learning, c, rows, policy, feedback, memory_path


def test_evidence_fallback_uses_cited_pair_and_real_native_compile_without_extra_model(tmp_path, monkeypatch):
    learning, c, rows, policy, feedback, _ = _fallback_fixture(tmp_path)
    draft, provenance = learning._evidence_bounded_control_draft(c, policy, feedback)
    assert draft['action_rules'] == draft['schedule_rules'] == []
    rule = draft['loop_rules'][0]
    assert rule['memory_id'] == 'experience-a' and 'actor_id' not in rule
    assert (rule['max_iterations'], rule['max_no_progress'], rule['max_replans']) == (30, 3, 1)
    assert rule['replan_on_failure'] is True and rule['error_codes'] == ['appworld_auth_401']
    assert provenance['matched_failure_pairs'] == 1 and provenance['successful_source_tasks'] == 1
    model = Mock(return_value=({'action_rules': [], 'loop_rules': [], 'schedule_rules': []}, {}))
    monkeypatch.setattr(learning, '_control_model_reply', model)
    diagnostic = {}
    compiled = learning.generate_seed_controls(c, policy, {row['task_id'] for row in rows}, diagnostic)
    assert model.call_count == 2 and compiled['snapshot'] == policy['snapshot']
    assert diagnostic['control_learning']['generation_method'] == 'deterministic_evidence_fallback'
    fallback = diagnostic['control_learning']['evidence_fallback']
    assert fallback['status'] == 'native_compiled' and fallback['model_calls'] == 0
    assert [attempt['status'] for attempt in diagnostic['control_learning']['attempts']] == ['no_controls', 'no_controls']
    generated = [item for item in compiled['options']['loop_rules'] if item.get('source_experience')]
    assert len(generated) == 1 and generated[0]['source_experience']['memory_id'] == 'experience-a'
    assert json.loads((tmp_path/'seed-compile-status.json').read_text()) == diagnostic


@pytest.mark.parametrize('responses', ['mixed_native_rejections', 'invalid_json'])
def test_control_exhaustion_recovers_only_from_real_cited_native_evidence(tmp_path, monkeypatch, responses):
    learning, c, rows, policy, feedback, _ = _fallback_fixture(tmp_path)
    production = ROOT / 'tools/trajectory_evolution/build-v4/trajectory_evolution'
    if production.is_file():
        c.BINARY = production
    valid, _ = learning._evidence_bounded_control_draft(c, policy, feedback)
    duplicate = copy.deepcopy(valid)
    duplicate['loop_rules'] *= 2
    excess = copy.deepcopy(valid)
    excess['loop_rules'] *= 65
    empty = {'action_rules': [], 'loop_rules': [], 'schedule_rules': []}
    drafts = iter((empty, duplicate, excess))
    def reply(controller, packet, repair_reason, transport):
        transport['model_calls'] += 1
        if responses == 'invalid_json':
            raise ValueError('broken json')
        return next(drafts), {'total_tokens': 7}
    model = Mock(side_effect=reply)
    monkeypatch.setattr(learning, '_control_model_reply', model)
    diagnostic = {'published': False}
    result = learning.generate_seed_controls(c, policy, {r['task_id'] for r in rows}, diagnostic)
    status = diagnostic['control_learning']
    assert model.call_count == status['model_calls'] == status['model_attempts'] == 3
    assert status['generation_method'] == 'deterministic_evidence_fallback'
    assert status['evidence_fallback']['status'] == 'native_compiled'
    assert result['snapshot'] == policy['snapshot'] and diagnostic['published'] is False
    learned = [r for r in result['options']['loop_rules'] if r.get('source_experience')]
    assert len(learned) == 1 and learned[0]['source_experience']['memory_id'] == 'experience-a'
    assert learned[0]['error_codes'] == ['appworld_auth_401'] and learned[0]['max_replans'] == 1
    if responses == 'mixed_native_rejections':
        assert [r['status'] for r in status['attempts']] == ['no_controls', 'native_contract_rejected', 'native_contract_rejected']
        assert [r.get('native_error_code') for r in status['attempts'][1:]] == ['control_duplicate_loop_scope', 'control_draft_limits_invalid']
    assert json.loads((tmp_path / 'seed-compile-status.json').read_text()) == diagnostic


@pytest.mark.parametrize('admissible', [True, False])
def test_exhausted_control_budget_resume_never_calls_model_and_keeps_source_gates(tmp_path, monkeypatch, admissible):
    learning, c, rows, policy, _, memory_path = _fallback_fixture(tmp_path)
    if not admissible:
        memory = c.read(memory_path)
        memory['entries'][0]['candidate']['support_evidence'][0]['source']['conversation_id'] = 'unbound'
        save(memory_path, memory)
    model = Mock(side_effect=AssertionError('fourth paid call is prohibited'))
    monkeypatch.setattr(learning, '_control_model_reply', model)
    attempts = [{'attempt': i + 1, 'status': 'native_contract_rejected', 'usage': {'total_tokens': 7}} for i in range(3)]
    diagnostic = {'control_learning': {'model_calls': 3, 'model_attempts': 3,
                                     'status': 'format_repair_exhausted', 'attempts': attempts}}
    if admissible:
        result = learning.generate_seed_controls(c, policy, {r['task_id'] for r in rows}, diagnostic)
        assert result['snapshot'] == policy['snapshot']
        assert diagnostic['control_learning']['evidence_fallback']['status'] == 'native_compiled'
    else:
        with pytest.raises(RuntimeError, match='no_cited_failure_pair'):
            learning.generate_seed_controls(c, policy, {r['task_id'] for r in rows}, diagnostic)
        assert diagnostic['control_learning']['evidence_fallback']['status'] == 'insufficient_evidence'
    model.assert_not_called()
    assert diagnostic['control_learning']['model_calls'] == 3
    assert diagnostic['control_learning']['attempts'] == attempts
    assert diagnostic['control_learning']['bounded_model_budget'] == 'already_exhausted'


def test_evidence_recovery_prioritizes_supported_typeerror_over_more_auth_pairs(tmp_path):
    learning, c, rows, policy, feedback, memory_path = _fallback_fixture(tmp_path)
    view = feedback['local_source_evidence']['LOCAL_RUN_2']
    ends = [event for event in view['events'] if event['kind'] == 'action.end']
    memory = c.read(memory_path);support = memory['entries'][0]['candidate']['support_evidence'][0]
    for event in ends[:2]:event['error']['code'] = 'appworld_type_error_positional'
    for ordinal in range(4):
        before, after = copy.deepcopy(view['events'][0]), copy.deepcopy(ends[0])
        action = 'extra-auth-' + str(ordinal)
        before.update(action_id=action, event_id=action+'-before', kind='action.start', operation='appworld.execute')
        after.update(action_id=action, event_id=action+'-after', kind='action.end', status='failed', error={'code':'appworld_auth_401'})
        view['events'].extend([before, after]);support['evidence_event_ids'].append(after['event_id'])
    save(memory_path, memory)
    draft, provenance = learning._evidence_bounded_control_draft(c, policy, feedback)
    assert {rule['error_codes'][0] for rule in draft['loop_rules']} == {'appworld_type_error_positional'}
    assert all(rule['max_replans'] == 1 for rule in draft['loop_rules'])
    assert all(row['matched_failure_pairs'] >= 1 for row in provenance['rules'])


def _settled_auth_fallback_fixture(tmp_path):
    learning, c, rows, policy, feedback, memory_path = _fallback_fixture(tmp_path)
    view = feedback['local_source_evidence']['LOCAL_RUN_2']
    view['identity']['agent_id'] = 'fixture'
    for event in view['events']:
        if event['kind'] == 'action.start': event['policy_scope'] = 'default'
    ends = [event for event in view['events'] if event['kind'] == 'action.end']
    for event in ends[:2]:
        event['error']['code'] = 'appworld_missing_access_token'
    memory = c.read(memory_path)
    scope = 'agent:fixture/actor:default'
    for experience in policy['snapshot']['experiences']: experience['target_scope'] = scope
    for rule in policy['options']['loop_rules']: rule['target_scope'] = scope
    memory['entries'][0]['candidate']['target_scope'] = scope
    view['identity']['agent_id'] = memory['entries'][0]['candidate']['target_scope'].split('/')[0].split(':',1)[1]
    first = memory['entries'][0]['candidate']['support_evidence'][0]
    second = copy.deepcopy(first)
    first.update(evidence_id='FIRST_ORIGINAL', evidence_event_ids=[ends[0]['event_id']],
                 verdict='SEMANTIC_VERDICT_NECESSARY_RETRY')
    second.update(evidence_id='SECOND_ORIGINAL', evidence_event_ids=[ends[1]['event_id']],
                  verdict='SEMANTIC_VERDICT_INVALID')
    memory['entries'][0]['candidate']['support_evidence'] = [first, second]
    save(memory_path, memory)
    policy['options']['max_context_bytes'] = 2048
    return learning, c, policy, feedback, memory_path


def test_settled_auth_failure_recovery_preserves_each_real_citation_and_source_context(tmp_path):
    learning, c, policy, feedback, _ = _settled_auth_fallback_fixture(tmp_path)
    draft, proof = learning._evidence_bounded_control_draft(c, policy, feedback,
        preferred_failure_codes={'appworld_missing_access_token'})
    rule = draft['loop_rules'][0]
    source = next(e for e in policy['snapshot']['experiences'] if e['memory_id'] == rule['memory_id'])
    assert rule['replan_context'] == source['content']
    assert rule['error_codes'] == ['appworld_missing_access_token']
    assert rule['max_replans'] == 1 and 'actor_id' not in rule
    citation = proof['evidence_examples'][0]
    assert citation['evidence_id'] is None
    assert citation['evidence_ids'] == ['FIRST_ORIGINAL', 'SECOND_ORIGINAL']
    assert [e['evidence_event_ids'] for e in citation['source_evidence_citations']] == [[citation['event_ids'][0]], [citation['event_ids'][1]]]


@pytest.mark.parametrize('content', ["Use apis.spotify.show_playlist_library with the returned access_token.",
    "Read show_api_doc(app_name='spotify', api_name='show_playlist_library') before retrying."])
def test_global_auth_recovery_rejects_app_specific_context_but_preserves_l1_source(tmp_path, content):
    learning, c, policy, feedback, _ = _settled_auth_fallback_fixture(tmp_path)
    policy['snapshot']['experiences'][0]['content'] = content
    with pytest.raises(RuntimeError, match='evidence_recovery_(no_cited_failure_pair|preferred_failure_unsupported)'):
        learning._evidence_bounded_control_draft(c, policy, feedback,
            preferred_failure_codes={'appworld_missing_access_token'})
    assert policy['snapshot']['experiences'][0]['content'] == content
    assert policy['options']['approved_experiences']


def test_uncovered_auth_preference_uses_reviewed_invalid_failures_before_necessary_retries(tmp_path):
    learning, c, policy, feedback, memory_path = _settled_auth_fallback_fixture(tmp_path)
    view = feedback['local_source_evidence']['LOCAL_RUN_2']
    end = next(e for e in view['events'] if e['kind'] == 'action.end')
    memory = c.read(memory_path)
    positive = copy.deepcopy(memory['entries'][0]['candidate']['support_evidence'][0])
    positive.update(evidence_id='OTHER_NECESSARY_RETRIES', evidence_event_ids=[])
    for ordinal in range(4):
        action = 'other-auth-' + str(ordinal)
        before = {'event_id': action+'-start', 'kind': 'action.start', 'action_id': action, 'operation': 'appworld.execute'}
        after = copy.deepcopy(end)
        after.update(event_id=action+'-end', action_id=action, error={'code':'appworld_auth_401'})
        view['events'].extend([before, after]);positive['evidence_event_ids'].append(after['event_id'])
    memory['entries'][0]['candidate']['support_evidence'].append(positive)
    save(memory_path, memory)
    old, _ = learning._evidence_bounded_control_draft(c, policy, feedback)
    assert old['loop_rules'][0]['error_codes'] == ['appworld_auth_401']
    new, _ = learning._evidence_bounded_control_draft(c, policy, feedback,
        preferred_failure_codes={'appworld_auth_401','appworld_missing_access_token'})
    assert new['loop_rules'][0]['error_codes'] == ['appworld_missing_access_token']


@pytest.mark.parametrize('problem', ['unknown', 'mix_run', 'wrong_source', 'scope', 'unapproved',
    'only_second_cited', 'different_actor', 'different_execution', 'success_between', 'different_code', 'over_budget',
    'missing_user', 'missing_conversation', 'missing_dialogue', 'missing_evidence_id', 'duplicate_evidence_id',
    'missing_activity_scope', 'wrong_activity_scope', 'wrong_agent_scope'])
def test_settled_auth_failure_recovery_rejects_unbound_records_or_nonfailure_pairs(tmp_path, problem):
    learning, c, policy, feedback, memory_path = _settled_auth_fallback_fixture(tmp_path)
    memory = c.read(memory_path)
    records = memory['entries'][0]['candidate']['support_evidence']
    view = feedback['local_source_evidence']['LOCAL_RUN_2']
    ends = [e for e in view['events'] if e['kind'] == 'action.end']
    if problem == 'unknown': records[1]['verdict'] = 'SEMANTIC_VERDICT_UNASSESSED'
    elif problem == 'mix_run': records[1]['source']['dialogue_id'] = 'OTHER_RUN'
    elif problem == 'wrong_source': records[1]['source']['conversation_id'] = 'OTHER'
    elif problem == 'scope': records[1]['source']['target_scope'] = 'OTHER_SCOPE'
    elif problem == 'unapproved': policy['options']['approved_experiences'] = []
    elif problem == 'only_second_cited': records[0]['evidence_event_ids'] = []
    elif problem == 'different_actor': ends[1]['actor_id'] = 'OTHER_ACTOR'
    elif problem == 'different_execution': ends[1]['execution_id'] = 'OTHER_EXECUTION'
    elif problem == 'success_between': ends[0]['status'] = 'completed'
    elif problem == 'different_code': ends[1]['error']['code'] = 'appworld_auth_401'
    elif problem == 'over_budget': policy['options']['max_context_bytes'] = 1
    elif problem.startswith('missing_') and problem.split('missing_',1)[1] in ('user','conversation','dialogue'):
        key = problem.split('missing_',1)[1] + '_id'
        for record in records: record['source'].pop(key)
    elif problem == 'missing_evidence_id': records[1].pop('evidence_id')
    elif problem == 'duplicate_evidence_id': records[1]['evidence_id'] = records[0]['evidence_id']
    elif problem == 'missing_activity_scope':
        for event in view['events']:
            if event['kind'] == 'action.start': event.pop('policy_scope')
    elif problem == 'wrong_activity_scope':
        for event in view['events']:
            if event['kind'] == 'action.start': event['policy_scope'] = 'OTHER'
    elif problem == 'wrong_agent_scope': view['identity']['agent_id'] = 'OTHER'
    save(memory_path, memory)
    with pytest.raises(RuntimeError, match='evidence_recovery_(no_cited_failure_pair|preferred_failure_unsupported|source_context_over_budget)'):
        learning._evidence_bounded_control_draft(c, policy, feedback,
            preferred_failure_codes={'appworld_missing_access_token'})


@pytest.mark.parametrize('problem', ['first_failure_only', 'success_between', 'different_code',
    'wrong_source', 'unassessed', 'unapproved', 'different_execution'])
def test_evidence_fallback_rejects_unbound_or_nonconsecutive_failures(tmp_path, problem):
    learning, c, rows, policy, feedback, memory_path = _fallback_fixture(tmp_path)
    memory = c.read(memory_path); support = memory['entries'][0]['candidate']['support_evidence'][0]
    view = feedback['local_source_evidence']['LOCAL_RUN_2']
    ends = [event for event in view['events'] if event['kind'] == 'action.end']
    if problem == 'first_failure_only': support['evidence_event_ids'] = support['evidence_event_ids'][:1]
    elif problem == 'wrong_source': support['source']['conversation_id'] = 'OTHER'
    elif problem == 'unassessed': support['verdict'] = 'SEMANTIC_VERDICT_UNASSESSED'
    elif problem == 'unapproved': policy['options']['approved_experiences'] = []
    elif problem == 'success_between': ends[0]['status'] = 'completed'
    elif problem == 'different_code': ends[1]['error']['code'] = 'appworld_validation_422'
    elif problem == 'different_execution': ends[1]['execution_id'] = 'OTHER_EXECUTION'
    save(memory_path, memory)
    with pytest.raises(RuntimeError, match='evidence_recovery_no_cited_failure_pair'):
        learning._evidence_bounded_control_draft(c, policy, feedback)


@pytest.mark.parametrize('window_mode', [None, 'observed_phase', 'observed_phase_v2'])
def test_frozen_seed_resume_is_zero_calls_and_preserves_whole_artifact_and_original_costs(tmp_path, monkeypatch, window_mode):
    import learning
    from types import SimpleNamespace
    c = SimpleNamespace(TRAINING=tmp_path/'training', PUBLISHED=tmp_path/'published',
        OWNER='fixture-owner', AGENT='fixture-agent', SCOPE='agent:fixture-agent/actor:default',
        read=lambda path: json.loads(Path(path).read_text()), save=save,
        native=Mock(side_effect=AssertionError('frozen resume must not compile or call a model')),
        environment=lambda: {'APPWORLD_SEED_L1_WINDOW_MODE': window_mode} if window_mode else {})
    artifact = {'schema_version': 1, 'agent': {'name': c.AGENT, 'revision': 'frozen-original'},
        'user_id': c.OWNER, 'level': 'L3', 'policy': {'snapshot': {
            'options': {'require_validated_evidence': True}, 'experiences': [
            {'memory_id': 'genuine-source', 'target_scope': c.SCOPE}]}, 'options': {
            'loop_rules': [{'source_experience': {'memory_id': 'genuine-source'},
                            'max_iterations': 30, 'max_no_progress': 3, 'max_replans': 1}]}}}
    target = c.PUBLISHED/'candidate-3/strategy.json'; save(target, artifact)
    save(target.with_name('manifest.json'), {'validated_evidence_required': True, 'training_input_tasks': ['original']})
    diagnostics = c.TRAINING/'seed-compile-status.json'
    save(diagnostics, {'seed_label': 'candidate-3', 'published': True, 'control_learning': {
        'model_calls': 2, 'evidence_fallback': {'matched_failure_pairs': 2}}})
    unchanged = {p: p.read_bytes() for p in (target, target.with_name('manifest.json'))}
    no_training = Mock(side_effect=AssertionError('a frozen seed needs no current-store reanalysis'))
    monkeypatch.setattr(learning, '_completed_training_tasks', no_training)
    assert learning.freeze_seed(c, label='candidate-3') == artifact
    c.native.assert_not_called(); no_training.assert_not_called()
    assert all(p.read_bytes() == original for p, original in unchanged.items())
    final = c.read(diagnostics)
    assert final['published'] is True and final['resume']['model_calls'] == 0
    assert final['control_learning']['model_calls'] == 2
    assert final['control_learning']['evidence_fallback']['matched_failure_pairs'] == 2
    artifact['user_id'] = 'wrong-owner'; save(target, artifact)
    with pytest.raises(RuntimeError, match='frozen_seed_schema_or_scope_invalid'):
        learning.freeze_seed(c, label='candidate-3')
    c.native.assert_not_called()


def _english_trigger_fixture(tmp_path):
    from types import SimpleNamespace
    import learning
    scope = 'agent:fixture-agent/actor:default'
    experience = {'memory_id': 'auth-experience', 'content_revision': 'approved-revision',
        'target_scope': scope, 'trigger_pattern': '调用需要认证的接口时',
        'content': '先 login 获取 access_token，后续调用携带 access_token。', 'control_target_type': ''}
    policy = {'snapshot': {'experiences': [experience], 'identity': {'user_id': 'fixture'}},
        'options': {'approved_experiences': [{'memory_id': 'auth-experience', 'content_revision': 'approved-revision'}],
        'max_context_bytes': 2048, 'loop_rules': [{'max_iterations': 30, 'max_replans': 1}]}}
    identity = {'user_id': 'fixture', 'conversation_id': 'local-conversation', 'run_id': 'local-run'}
    event = {'event_id': 'LOCAL_AUTH_EVENT', 'kind': 'action.end', 'output': {
        'public_observation': 'login produced access_token; subsequent call uses access_token'}}
    report_path = tmp_path/'full-captures/fixture/report.json'
    model_step = {'event_id': 'model-step', 'step_id': 'actual-skill-model', 'kind': 'step.start',
        'attributes': {'apply_policy': True, 'semantic': 'model'}}
    model = {'event_id': 'actual-model-event', 'step_id': 'actual-skill-model', 'kind': 'action.start',
        'actor_id': 'skill_executor', 'attributes': {'semantic': 'model',
            'request_binding': {'mode': 'system_message', 'pointer': '/messages'}},
        'input': {'messages': [{'role': 'user', 'content': 'login produced access_token; subsequent call uses access_token'}]}}
    start = {'event_id': 'run-start', 'kind': 'run.start', 'input': {'instruction': 'Complete the supplied office task'}}
    save(report_path, {'complete': True, 'trials': [{'evidence': {'trace': {
        'identity': identity, 'events': [event, start, model_step, model]}}}]})
    view = {'identity': identity, 'events': [event], 'source_report': str(report_path),
            'event_mechanisms': {}, 'successful': True}
    support = {'source': {'user_id': 'fixture', 'conversation_id': 'local-conversation', 'dialogue_id': 'local-run'},
        'evidence_event_ids': ['LOCAL_AUTH_EVENT'], 'verdict': 'SEMANTIC_VERDICT_USEFUL'}
    memory = {'entries': [{'memory_id': 'auth-experience', 'candidate': {
        'target_scope': scope, 'support_evidence': [support]}}]}
    c = SimpleNamespace(TRAINING=tmp_path, RUNTIME=tmp_path/'runtime', PUBLISHED=tmp_path/'published',
        OWNER='fixture', SCOPE=scope, ROOT=ROOT, BINARY=Path('fixture-native'),
        read=lambda path: json.loads(Path(path).read_text()), save=save)
    save(tmp_path/'evolution/by-user/fixture/memory/evolution-memory.json', memory)
    feedback = {'local_source_evidence': {'local-run': view}, 'scanned_tasks': 90}
    def native(binary, root, operation, packet):
        if operation == ['runtime', 'recompile']:
            result = copy.deepcopy(packet['policy']);result['options'] = copy.deepcopy(packet['options'])
            return json.dumps(result)
        overrides = packet['policy']['options'].get('experience_overrides', [])
        def decide(query):
            text = query['advice_query']['task'] + query['advice_query']['state']
            refs = [{'memory_id': item['source_experience']['memory_id']} for item in overrides
                if all(term in text.lower() for term in learning._l1_terms(item['trigger_pattern']))]
            return {'experiences': refs}
        if operation == ['runtime', 'query-batch']:
            consumed, decisions = set(), []
            for query in packet['queries']:
                result = decide(query)
                result['experiences'] = [ref for ref in result['experiences'] if ref['memory_id'] not in consumed]
                consumed.update(ref['memory_id'] for ref in result['experiences'])
                decisions.append(result)
            return json.dumps(decisions)
        return json.dumps(decide(packet['query']))
    c.native = Mock(side_effect=native)
    return learning, c, policy, feedback, memory


def test_english_trigger_sources_require_exact_positive_cited_event_and_scope(tmp_path):
    learning, c, policy, feedback, memory = _english_trigger_fixture(tmp_path)
    sources = learning._english_trigger_sources(c, policy, feedback)
    assert sources['auth-experience']['evidence'][0]['anchors'] == {'login', 'access_token'}
    candidate = memory['entries'][0]['candidate']
    for field, value in [('verdict', 'SEMANTIC_VERDICT_UNASSESSED'),
                         ('evidence_event_ids', ['uncited-event'])]:
        changed = copy.deepcopy(memory);changed['entries'][0]['candidate']['support_evidence'][0][field] = value
        save(c.TRAINING/'evolution/by-user/fixture/memory/evolution-memory.json', changed)
        assert learning._english_trigger_sources(c, policy, feedback) == {}
    changed = copy.deepcopy(memory);changed['entries'][0]['candidate']['target_scope'] = 'another-scope'
    save(c.TRAINING/'evolution/by-user/fixture/memory/evolution-memory.json', changed)
    assert learning._english_trigger_sources(c, policy, feedback) == {}


def test_english_trigger_sources_keep_verified_siblings_when_one_run_has_no_policy_window(tmp_path):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    original = feedback['local_source_evidence']['local-run']
    other = copy.deepcopy(original)
    other['identity']['run_id'] = 'windowless-run'
    path = tmp_path/'full-captures/windowless/report.json'
    other['source_report'] = str(path)
    report = c.read(Path(original['source_report']))
    trace = report['trials'][0]['evidence']['trace']
    trace['identity'] = other['identity']
    trace['events'][-2]['attributes']['apply_policy'] = False
    save(path, report)
    # Place the ineligible run first to catch the former global early exit.
    feedback['local_source_evidence'] = {'windowless-run': other, 'local-run': original}
    sources = learning._english_trigger_sources(c, policy, feedback)
    assert set(sources) == {'auth-experience'}
    assert set(sources.runtime_runs) == {'local-run'}
    assert sources.runtime_windowless_runs == ['windowless-run']
    assert len(sources['auth-experience']['evidence']) == 1
    feedback['local_source_evidence'] = {'windowless-run': other}
    with pytest.raises(RuntimeError, match='runtime_windows_missing'):
        learning._english_trigger_sources(c, policy, feedback)
    # Window exclusion must never mask a forged identity.
    other['identity'] = dict(other['identity'], user_id='forged-owner')
    with pytest.raises(RuntimeError, match='runtime_identity_mismatch'):
        learning._english_trigger_sources(c, policy, feedback)
    c.native.assert_not_called()


@pytest.mark.parametrize('draft,reason', [
    ({'triggers': [{'memory_id': 'unknown-source', 'trigger_pattern': 'login access_token'}]}, 'positive_source_missing'),
    ({'triggers': [{'memory_id': 'auth-experience', 'trigger_pattern': 'appworld api task error'}]}, 'independent_anchors_missing'),
    ({'triggers': [{'memory_id': 'auth-experience', 'trigger_pattern': 'login access_token', 'content': 'changed'}]}, 'fields_invalid'),
    ({'triggers': [{'memory_id': 'auth-experience', 'trigger_pattern': 'login access_token 认证'}]}, 'text_invalid'),
])
def test_english_trigger_compilation_rejects_fabricated_generic_or_extra_fields(tmp_path, draft, reason):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    sources = learning._english_trigger_sources(c, policy, feedback)
    with pytest.raises(ValueError, match=reason):
        learning._compile_english_triggers(c, policy, draft, sources)
    c.native.assert_not_called()


def test_english_trigger_compilation_preserves_all_sources_content_controls_and_checks_actual_evidence(tmp_path):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    before = copy.deepcopy(policy)
    sources = learning._english_trigger_sources(c, policy, feedback)
    result = learning._compile_english_triggers(c, policy, {'triggers': [
        {'memory_id': 'auth-experience', 'trigger_pattern': 'login access_token'}]}, sources)
    assert policy == before and result['snapshot'] == before['snapshot']
    assert result['options']['loop_rules'] == before['options']['loop_rules']
    rewrite = result['options']['experience_overrides'][0]
    assert rewrite['content'] == before['snapshot']['experiences'][0]['content']
    assert rewrite['source_experience'] == before['options']['approved_experiences'][0]
    assert c.native.call_args_list[0].args[2] == ['runtime', 'recompile']
    assert c.native.call_count == 5  # compiler, two unrelated queries, baseline+candidate runtime replay
    c.native = Mock(side_effect=lambda *args: json.dumps({'snapshot': {}, 'options': {}}))
    with pytest.raises(RuntimeError, match='native_changed_frozen_sources'):
        learning._compile_english_triggers(c, policy, {'triggers': [
            {'memory_id': 'auth-experience', 'trigger_pattern': 'login access_token'}]}, sources)


def test_english_trigger_generation_repairs_at_most_twice_and_keeps_original_policy(tmp_path, monkeypatch):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    packet = {}; before = copy.deepcopy(policy)
    monkeypatch.setattr(learning, 'seed_control_input', lambda *args: (packet, feedback, {}))
    invalid = {'triggers': [{'memory_id': 'auth-experience', 'trigger_pattern': 'appworld api'}]}
    model = Mock(return_value=(invalid, {'total_tokens': 3}))
    monkeypatch.setattr(learning, '_english_trigger_model_reply', model)
    diagnostic = {}
    result = learning.generate_english_triggers(c, policy, set(), diagnostic)
    assert model.call_count == 3 and policy == before
    assert result['snapshot'] == before['snapshot']
    status = diagnostic['english_trigger_learning']
    assert status['status'] == 'compiled' and status['evidence_fallback']['model_calls'] == 0
    assert len(status['attempts']) == 3 and status['evidence_fallback']['status'] == 'compiled'
    assert status['attempts'][0]['draft'][0]['trigger_pattern'] == 'appworld api'


def test_english_trigger_model_egress_is_only_five_cards_and_anonymous_counts(tmp_path, monkeypatch):
    import urllib.request
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    save(c.RUNTIME/'runtime.json', {'model': {'endpoint': 'https://approved.invalid/plan/v3',
        'model_id': 'fixture-model', 'max_output_tokens': 4096}})
    c.environment = lambda: {'OPENAI_API_KEY': 'synthetic-test-key'}
    packet = {'memories': [{'memory_id': 'auth-experience', 'scope': c.SCOPE,
        'trigger': '认证失败', 'problem': '认证参数缺失', 'content': policy['snapshot']['experiences'][0]['content']}],
        'training_statistics': {'scanned_training_tasks': 90}}
    monkeypatch.setattr(learning, 'approved_control_packet', lambda value: value)
    seen = []
    class Response:
        def __enter__(self): return self
        def __exit__(self, *args): pass
        def read(self, limit): return json.dumps({'choices': [{'message': {'content': '```json\n{"triggers":[{"memory_id":"auth-experience","trigger_pattern":"login access_token"}]}\n```'}}],
            'usage': {'total_tokens': 8}}).encode()
    class Opener:
        def open(self, request, timeout): seen.append(request);return Response()
    build = Mock(return_value=Opener());monkeypatch.setattr(urllib.request, 'build_opener', build)
    transport = {'model_calls': 0}
    draft, usage = learning._english_trigger_model_reply(c, packet, transport=transport)
    assert draft['triggers'][0]['trigger_pattern'] == 'login access_token'
    external = json.loads(json.loads(seen[0].data)['messages'][1]['content'])
    assert set(external) == {'memories', 'training_statistics'}
    assert all(set(card) == {'memory_id', 'scope', 'trigger', 'problem', 'content'} for card in external['memories'])
    assert 'LOCAL_AUTH_EVENT' not in seen[0].data.decode() and 'local-run' not in seen[0].data.decode()
    assert usage['total_tokens'] == 8 and transport['model_calls'] == 1
    assert build.call_args.args[0].proxies == {}
    with pytest.raises(ValueError, match='repair_reason_invalid'):
        learning._english_trigger_model_reply(c, packet, repair_reason={
            'reason': 'appworld_english_trigger_native_candidate_competition',
            'memory_id': 'RAW_UNKNOWN_ID'}, transport=transport)
    assert len(seen) == 1 and transport['model_calls'] == 1


@pytest.mark.parametrize('window_mode', [None, 'observed_phase', 'observed_phase_v2'])
def test_english_candidate_freeze_reuses_whole_frozen_source_without_control_regeneration(tmp_path, monkeypatch, window_mode):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    c.AGENT = 'fixture-agent'
    policy['options']['loop_rules'][0]['source_experience'] = policy['options']['approved_experiences'][0]
    policy['snapshot']['options'] = {'require_validated_evidence': True}
    artifact = {'schema_version': 1, 'agent': {'name': c.AGENT}, 'user_id': c.OWNER,
        'level': 'L3', 'policy': policy}
    source = c.PUBLISHED/'seed0/strategy.json';save(source, artifact)
    save(source.with_name('manifest.json'), {'validated_evidence_required': True,
        'experience_count': 1, 'training_input_tasks': ['original-training']})
    original = {path: path.read_bytes() for path in (source, source.with_name('manifest.json'))}
    c.environment = lambda: {'APPWORLD_ENGLISH_TRIGGERS': '1', 'APPWORLD_ENGLISH_TRIGGERS_FROM': 'seed0'}
    if window_mode:
        c.environment = lambda: {'APPWORLD_ENGLISH_TRIGGERS': '1', 'APPWORLD_ENGLISH_TRIGGERS_FROM': 'seed0',
                                 'APPWORLD_SEED_L1_WINDOW_MODE': window_mode}
    c.version = lambda value: 'native-generated-version'
    monkeypatch.setattr(learning, '_completed_training_tasks', lambda controller: {'original-training'})
    no_controls = Mock(side_effect=AssertionError('do not regenerate frozen L3 controls'))
    monkeypatch.setattr(learning, 'generate_seed_controls', no_controls)
    def generate(controller, original_policy, tasks, diagnostic):
        assert original_policy == policy and tasks == {'original-training'}
        changed = copy.deepcopy(original_policy)
        changed['options']['experience_overrides'] = [{'source_experience': changed['options']['approved_experiences'][0],
            'content': changed['snapshot']['experiences'][0]['content'], 'trigger_pattern': 'login access_token'}]
        return changed
    generated = Mock(side_effect=generate);monkeypatch.setattr(learning, 'generate_english_triggers', generated)
    result = learning.freeze_seed(c, label='candidate-english')
    assert result['agent'].get('appworld_options', {}).get('l1_window_mode') == window_mode
    assert result['policy']['snapshot'] == policy['snapshot']
    assert result['policy']['options']['loop_rules'] == policy['options']['loop_rules']
    assert all(path.read_bytes() == data for path, data in original.items())
    no_controls.assert_not_called();c.native.assert_not_called()
    generated.reset_mock()
    assert learning.freeze_seed(c, label='candidate-english') == result
    generated.assert_not_called()
    assert c.read(c.TRAINING/'seed-compile-status.json')['published'] is True


def test_english_trigger_failure_diagnosis_does_not_accept_isolated_only_match(tmp_path):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    original = c.native.side_effect
    def native(binary, root, operation, packet):
        if operation == ['runtime', 'query-batch']:
            return json.dumps([{'experiences': []} for _ in packet['queries']])
        return original(binary, root, operation, packet)
    c.native = Mock(side_effect=native)
    sources = learning._english_trigger_sources(c, policy, feedback)
    with pytest.raises(ValueError, match='native_runtime_not_matched') as actual:
        learning._compile_english_triggers(c, policy, {'triggers': [
            {'memory_id': 'auth-experience', 'trigger_pattern': 'login access_token'}]}, sources)
    assert actual.value.memory_id == 'auth-experience'
    assert not policy['options'].get('disabled_injections')
    assert c.native.call_count == 5


def test_english_trigger_model_gets_only_supported_original_cards_and_precise_repair_id(tmp_path, monkeypatch):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    cards = [{'memory_id': 'auth-experience', 'scope': c.SCOPE, 'condition': '认证失败',
              'problem': '缺少认证参数', 'experience': policy['snapshot']['experiences'][0]['content']},
             {'memory_id': 'unsupported-card', 'scope': c.SCOPE, 'condition': '未判条件',
              'problem': '未判问题', 'experience': '仅泛化中文内容'}]
    packet = {'memories': cards, 'training_statistics': {'scanned_training_tasks': 90}}
    before = copy.deepcopy(packet)
    monkeypatch.setattr(learning, 'seed_control_input', lambda *args: (packet, feedback, {}))
    seen = []
    def model(controller, translated_packet, repair_reason=None, transport=None):
        seen.append(copy.deepcopy((translated_packet, repair_reason)))
        transport['model_calls'] += 1
        trigger = 'appworld api' if len(seen) == 1 else 'login access_token'
        return {'triggers': [{'memory_id': 'auth-experience', 'trigger_pattern': trigger}]}, {'total_tokens': 8}
    monkeypatch.setattr(learning, '_english_trigger_model_reply', model)
    diagnostic = {}
    result = learning.generate_english_triggers(c, policy, set(), diagnostic)
    assert packet == before and result['snapshot'] == policy['snapshot']
    assert all(len(value[0]['memories']) == 1 for value in seen)
    assert seen[0][0]['memories'][0] == cards[0]
    assert all(value[0]['training_statistics'] == packet['training_statistics'] for value in seen)
    assert seen[1][1] == {'reason': 'appworld_english_trigger_independent_anchors_missing',
                           'memory_id': 'auth-experience'}
    assert diagnostic['english_trigger_learning']['model_calls'] == 2


def test_english_trigger_observable_terms_include_original_condition_and_split_dotted_api_names(tmp_path):
    learning, c, policy, feedback, memory = _english_trigger_fixture(tmp_path)
    card = policy['snapshot']['experiences'][0]
    card.update(trigger_pattern='调用 file_system 的 create_file 返回参数错误', content='先查询 directory_path 与 file_path 的准确参数')
    report = Path(feedback['local_source_evidence']['local-run']['source_report'])
    raw = c.read(report)
    raw['trials'][0]['evidence']['trace']['events'][-1]['input']['messages'][0]['content'] = (
        'apis.file_system.create_file(...) failed validation')
    save(report, raw)
    sources = learning._english_trigger_sources(c, policy, feedback)
    assert {'file_system', 'create_file'} <= sources['auth-experience']['evidence'][0]['anchors']
    assert learning._l1_terms('apis.file_system.create_file') == {'file_system', 'create_file'}
    result = learning._compile_english_triggers(c, policy, {'triggers': [
        {'memory_id': 'auth-experience', 'trigger_pattern': 'file_system create_file'}]}, sources)
    assert result['snapshot'] == policy['snapshot']
    assert result['options']['experience_overrides'][0]['content'] == card['content']


def test_english_trigger_http_failure_status_is_an_independent_observable_anchor():
    import learning
    assert learning._l1_terms('401 error access_token parameter API call') == {'401', 'access_token'}
    assert learning._l1_terms('HTTP 400 499 500 599 access_token') >= {'400', '499', '500', '599', 'access_token'}


def test_english_trigger_http_status_anchor_excludes_ids_and_other_numbers():
    import learning
    text = 'appworld-401 task_401 task-401 1401 4010 401_seconds 399 600 32 100'
    assert not learning._l1_terms(text) & {'401', '399', '600', '32', '100', '1401', '4010'}


def test_existing_three_failed_english_model_calls_resume_into_evidence_fallback_without_fourth_call(tmp_path, monkeypatch):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    monkeypatch.setattr(learning, 'seed_control_input', lambda *args: ({}, feedback, {}))
    no_model = Mock(side_effect=AssertionError('do not repeat three exhausted actual model attempts'))
    monkeypatch.setattr(learning, '_english_trigger_model_reply', no_model)
    diagnostic = {'english_trigger_learning': {'status': 'format_repair_exhausted', 'model_calls': 3,
        'attempts': [{'attempt': i, 'status': 'rejected', 'reason': 'appworld_english_trigger_independent_anchors_missing'} for i in (1, 2, 3)]}}
    result = learning.generate_english_triggers(c, policy, set(), diagnostic)
    no_model.assert_not_called()
    assert result['snapshot'] == policy['snapshot']
    assert result['options']['loop_rules'] == policy['options']['loop_rules']
    status = diagnostic['english_trigger_learning']
    assert status['model_calls'] == 3 and len(status['attempts']) == 3
    assert status['evidence_fallback']['status'] == 'compiled' and status['evidence_fallback']['model_calls'] == 0
    assert len(status['evidence_fallback']['attempts']) <= 12


def test_english_trigger_outer_code_is_not_a_deployment_positive(tmp_path):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    report = Path(feedback['local_source_evidence']['local-run']['source_report'])
    raw = c.read(report)
    raw['trials'][0]['evidence']['trace']['events'][-1]['input']['messages'][0]['content'] = 'Compute a triangle area'
    save(report, raw)
    sources = learning._english_trigger_sources(c, policy, feedback)
    assert sources['auth-experience']['evidence'][0]['anchors'] == set()
    with pytest.raises(ValueError, match='native_runtime_not_matched'):
        learning._compile_english_triggers(c, policy, {'triggers': [
            {'memory_id': 'auth-experience', 'trigger_pattern': 'login access_token'}]}, sources)
    with pytest.raises(RuntimeError, match='no_evidence_supported_fallback'):
        learning._derive_english_trigger_fallback(c, policy, sources, {})


def test_english_trigger_faithful_translation_does_not_require_existing_english_card_words(tmp_path):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    policy['snapshot']['experiences'][0].update(trigger_pattern='调用需要认证的接口时',
        content='先登录获取令牌，再执行需要认证的接口')
    sources = learning._english_trigger_sources(c, policy, feedback)
    assert sources['auth-experience']['evidence'][0]['anchors'] == set()
    compiled = learning._compile_english_triggers(c, policy, {'triggers': [
        {'memory_id': 'auth-experience', 'trigger_pattern': 'login access_token'}]}, sources)
    assert compiled['snapshot'] == policy['snapshot']
    assert sources.last_profile['new_matched_windows'] == 1


def test_english_trigger_validated_conditions_allow_unchanged_runtime_coverage(tmp_path):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    original = c.native.side_effect
    def native(binary, root, operation, packet):
        if operation == ['runtime', 'query-batch']:
            return json.dumps([{'experiences': [{'memory_id': 'auth-experience'}]} for _ in packet['queries']])
        return original(binary, root, operation, packet)
    c.native.side_effect = native
    sources = learning._english_trigger_sources(c, policy, feedback)
    compiled = learning._compile_english_triggers(c, policy, {'triggers': [
        {'memory_id': 'auth-experience', 'trigger_pattern': 'login access_token'}]}, sources)
    assert compiled['snapshot'] == policy['snapshot']
    assert sources.last_profile['new_matched_windows'] == 0
    assert sources.last_profile['lost_matched_windows'] == 0


def test_english_trigger_windows_use_real_skill_model_binding_and_current_messages(tmp_path):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    report = Path(feedback['local_source_evidence']['local-run']['source_report'])
    raw = c.read(report);trace = raw['trials'][0]['evidence']['trace']
    trace['events'][-1]['input']['messages'] = [
        {'role': 'system', 'content': 'private old advice login access_token'},
        {'role': 'user', 'content': 'Current office request'},
        {'role': 'tool', 'content': 'old consumed login access_token'},
        {'role': 'assistant', 'content': 'Inspect the remaining file'},
        {'role': 'tool', 'content': 'Current document inspection'}]
    queries = learning._l1_runtime_queries(c, policy, trace, {})
    assert len(queries) == 1
    assert 'Current office request' in queries[0]['advice_query']['state']
    assert 'Current document inspection' in queries[0]['advice_query']['state']
    assert 'old consumed' not in queries[0]['advice_query']['state']
    assert 'private old advice' not in queries[0]['advice_query']['state']
    trace['events'][-2]['attributes']['apply_policy'] = False
    assert learning._l1_runtime_queries(c, policy, trace, {}) == []


@pytest.mark.parametrize('mutation', [None, 'missing_cause', 'future_outer', 'future_ancestor', 'unknown_time',
                                     'initial_turn', 'failed_observation', 'wrong_actor'])
def test_observed_phase_replay_requires_real_earlier_causal_training_input(tmp_path, mutation):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    trace = c.read(Path(feedback['local_source_evidence']['local-run']['source_report']))['trials'][0]['evidence']['trace']
    start, step, model = trace['events'][-3:]
    start.update(timestamp_us='1', input={'instruction':
        'Delete all my read Gmail threads from inbox/outbox, except priority starred ones.'})
    outer = {'event_id': 'captured-public-input', 'kind': 'step.start', 'timestamp_us': '10',
        'input': {'system': '', 'turn': 1, 'messages': [
            {'role': 'assistant', 'content': 'gmail_token = apis.gmail.login(username=user, password=password)["access_token"]'},
            {'role': 'user', 'content': 'Authenticated.'}]}}
    step.update(timestamp_us='20', caused_by=[outer['event_id']])
    step['attributes']['apply_policy'] = False
    model.update(timestamp_us='21', caused_by=[step['event_id']])
    trace['events'] = [start, outer, step, model]
    if mutation == 'missing_cause':model['caused_by'] = []
    elif mutation == 'future_outer':outer['timestamp_us'] = '22'
    elif mutation == 'future_ancestor':step['timestamp_us'] = '5'
    elif mutation == 'unknown_time':step.pop('timestamp_us')
    elif mutation == 'initial_turn':outer['input']['turn'] = 0
    elif mutation == 'failed_observation':outer['input']['messages'][-1]['content'] = 'Execution failed: appworld_auth_401 Unauthorized'
    elif mutation == 'wrong_actor':model['actor_id'] = 'coordinator'
    before = copy.deepcopy(trace)
    assert learning._l1_runtime_queries(c, policy, trace, {}, public_state=True) == []
    kinds, codes, observations = {}, {}, {}
    queries = learning._l1_runtime_queries(c, policy, trace, {}, public_state=True,
        observed_phase=True, window_profile=kinds, code_profile=codes, observation_profile=observations)
    assert len(queries) == (1 if mutation is None else 0)
    assert trace == before
    if mutation is None:
        key = ('local-run', model['event_id'])
        assert kinds == {key: 'reconstructed_observed_phase'}
        assert 'gmail_token =' in codes[key]
        assert '<redacted:credential_field>' in codes[key]
        assert observations[key] == 'Authenticated.'
        assert 'task_gmail_inbox_complete_enumeration' in queries[0]['advice_query']['state']
        assert 'gmail_authenticated_state_ready' in queries[0]['advice_query']['state']
    c.native.assert_not_called()


@pytest.mark.parametrize('window_mode', ['observed_phase', 'observed_phase_v2'])
def test_seed_observed_phase_options_are_explicit_new_artifact_data_and_skip_l0(tmp_path, window_mode):
    from types import SimpleNamespace
    import learning
    artifact = {'level': 'L3', 'agent': {'name': 'fixture', 'revision': 'unchanged'},
                'policy': {'snapshot': {'experiences': []}}}
    before = copy.deepcopy(artifact)
    c = SimpleNamespace(environment=lambda: {})
    assert learning._seed_l1_window_options(c, artifact) is artifact
    c.environment = lambda: {'APPWORLD_SEED_L1_WINDOW_MODE': window_mode}
    enabled = learning._seed_l1_window_options(c, artifact)
    assert artifact == before and enabled['policy'] == artifact['policy']
    assert enabled['agent'] == dict(artifact['agent'], appworld_options={'l1_window_mode': window_mode})
    l0 = {'level': 'L0', 'agent': {'name': 'fixture', 'revision': 'unchanged'}, 'policy': None}
    assert learning._seed_l1_window_options(c, l0) is l0
    c.environment = lambda: {'APPWORLD_SEED_L1_WINDOW_MODE': 'unknown_mode'}
    with pytest.raises(RuntimeError, match='seed_l1_window_mode_invalid'):
        learning._seed_l1_window_options(c, artifact)


@pytest.mark.parametrize('unknown_time', [False, True])
def test_english_runtime_queries_follow_observed_time_and_keep_unknown_order(tmp_path, unknown_time):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    trace = c.read(Path(feedback['local_source_evidence']['local-run']['source_report']))['trials'][0]['evidence']['trace']
    start, step, model = copy.deepcopy(trace['events'][1:])
    start['timestamp_us'] = 1
    first_step, second_step = copy.deepcopy(step), copy.deepcopy(step)
    first_model, second_model = copy.deepcopy(model), copy.deepcopy(model)
    for index, (owner, event) in enumerate(((first_step, first_model), (second_step, second_model)), 1):
        owner.update(event_id=f'step-{index}', step_id=f'model-{index}', timestamp_us=index * 10)
        event.update(event_id=f'query-{index}', step_id=f'model-{index}', timestamp_us=index * 10 + 1)
        event['input']['messages'] = [{'role': 'user', 'content': f'current window {index}'}]
    # A producer may serialize a later completed step before an earlier one.
    trace['events'] = [start, second_step, second_model, first_step, first_model]
    if unknown_time:
        first_step.pop('timestamp_us')
    original = copy.deepcopy(trace)
    queries = learning._l1_runtime_queries(c, policy, trace, {})
    assert [row['advice_query']['query_id'] for row in queries] == (
        ['query-2', 'query-1'] if unknown_time else ['query-1', 'query-2'])
    assert trace == original


def test_english_runtime_queries_preserve_equal_timestamp_order(tmp_path):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    trace = c.read(Path(feedback['local_source_evidence']['local-run']['source_report']))['trials'][0]['evidence']['trace']
    trace['events'] = trace['events'][1:]
    for event in trace['events']:
        event['timestamp_us'] = 1
    second = copy.deepcopy(trace['events'][-1]);second['event_id'] = 'query-second'
    trace['events'].append(second)
    assert [row['advice_query']['query_id'] for row in learning._l1_runtime_queries(c, policy, trace, {})] == [
        'actual-model-event', 'query-second']


def test_english_trigger_subset_retains_true_source_after_bad_siblings(tmp_path):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    sources = learning._english_trigger_sources(c, policy, feedback)
    extra = copy.deepcopy(policy['snapshot']['experiences'][0]);extra['memory_id'] = 'unmatched-source'
    policy['snapshot']['experiences'].append(extra)
    policy['options']['approved_experiences'].append({'memory_id': extra['memory_id'],
        'content_revision': extra['content_revision']})
    sources[extra['memory_id']] = {'experience': extra, 'evidence': copy.deepcopy(sources['auth-experience']['evidence'])}
    draft = {'triggers': [
        {'memory_id': 'auth-experience', 'trigger_pattern': 'api error'},
        {'memory_id': 'unknown-source', 'trigger_pattern': 'login access_token'},
        {'memory_id': ['invalid-id'], 'trigger_pattern': 'login access_token'},
        {'memory_id': 'unmatched-source', 'trigger_pattern': 'directory file_path'},
        {'memory_id': 'auth-experience', 'trigger_pattern': 'login access_token'}]}
    # The synthetic draft can include extra malformed rows; its final supported
    # subset never exceeds the immutable approved snapshot.
    for _ in range(3):
        clone = copy.deepcopy(extra);clone['memory_id'] += str(_)
        policy['snapshot']['experiences'].append(clone)
    before = copy.deepcopy(policy);status = {}
    result = learning._compile_english_triggers(c, policy, draft, sources, status)
    assert policy == before and result['snapshot'] == before['snapshot']
    assert status['accepted_triggers'] == [draft['triggers'][-1]]
    assert status['runtime_profile']['new_matched_windows'] == 1
    assert len(status['rejected']) == 4
    assert any(row['reason'].endswith('native_runtime_not_matched') for row in status['rejected'])
    assert [row['source_experience']['memory_id'] for row in result['options']['experience_overrides']] == ['auth-experience']


def test_english_trigger_reuses_saved_safe_draft_without_another_model_call(tmp_path, monkeypatch):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    monkeypatch.setattr(learning, 'seed_control_input', lambda *args: ({}, feedback, {}))
    no_model = Mock(side_effect=AssertionError('saved responses must be revalidated locally'))
    monkeypatch.setattr(learning, '_english_trigger_model_reply', no_model)
    diagnostic = {'english_trigger_learning': {'status': 'compiled', 'model_calls': 3,
        'attempts': [{'attempt': 1, 'status': 'rejected', 'draft': [
            {'approved_source': True, 'memory_id': 'auth-experience', 'trigger_pattern': 'login access_token'}]},
            {'attempt': 2, 'status': 'rejected'}, {'attempt': 3, 'status': 'rejected'}]}}
    previous = copy.deepcopy(diagnostic['english_trigger_learning']['attempts'])
    result = learning.generate_english_triggers(c, policy, set(), diagnostic)
    no_model.assert_not_called()
    assert result['snapshot'] == policy['snapshot']
    status = diagnostic['english_trigger_learning']
    assert status['model_calls'] == 3 and status['draft_reuse']['model_calls'] == 0
    assert status['attempts'] == previous and status['override_count'] == 1


def test_english_draft_reuse_checks_actual_frozen_objects_and_source_refs(tmp_path):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    artifact = {'schema_version': 1, 'level': 'L3', 'user_id': c.OWNER, 'policy': policy}
    base = c.PUBLISHED/'seed0/strategy.json';old = c.PUBLISHED/'strategy-next/strategy.json'
    save(base, artifact);save(old, artifact)
    save(old.with_name('manifest.json'), {'source_seed_label': 'seed0'})
    previous = {'seed_label': 'strategy-next', 'control_learning': {'source_seed_label': 'seed0'}}
    assert learning._same_english_draft_source(c, previous, artifact, 'seed0')
    for section in ('snapshot', 'options'):
        changed = copy.deepcopy(artifact);changed['policy'][section]['unexpected'] = 'different-source'
        assert not learning._same_english_draft_source(c, previous, changed, 'seed0')
    changed = copy.deepcopy(artifact)
    changed['policy']['options']['approved_experiences'][0]['content_revision'] = 'wrong-revision'
    save(old, changed)
    assert not learning._same_english_draft_source(c, previous, artifact, 'seed0')
    save(old, artifact)
    changed = copy.deepcopy(artifact)
    changed['policy']['options']['experience_overrides'] = [{'source_experience': {
        'memory_id': 'auth-experience', 'content_revision': 'approved-revision'}, 'content': 'mutated-body',
        'trigger_pattern': 'login access_token'}]
    save(old, changed)
    assert not learning._same_english_draft_source(c, previous, artifact, 'seed0')


@pytest.mark.parametrize('stored_attempts', [True, False])
def test_compiled_prior_three_calls_never_buy_three_more_after_saved_drafts_fail(tmp_path, monkeypatch, stored_attempts):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    monkeypatch.setattr(learning, 'seed_control_input', lambda *args: ({}, feedback, {}))
    no_model = Mock(side_effect=AssertionError('three original calls exhausted the source budget'))
    monkeypatch.setattr(learning, '_english_trigger_model_reply', no_model)
    diagnostic = {'english_trigger_learning': {'status': 'compiled', 'model_calls': 3,
        'attempts': [{'attempt': 1, 'status': 'rejected', 'draft': [
            {'approved_source': True, 'memory_id': 'auth-experience', 'trigger_pattern': 'api error'}]},
            {'attempt': 2, 'status': 'rejected'}, {'attempt': 3, 'status': 'rejected'}]}}
    if not stored_attempts:
        diagnostic['english_trigger_learning'].pop('attempts')
    result = learning.generate_english_triggers(c, policy, set(), diagnostic)
    no_model.assert_not_called()
    status = diagnostic['english_trigger_learning']
    assert status['model_calls'] == 3 and len(status['attempts']) == (3 if stored_attempts else 0)
    if stored_attempts:
        assert status['specific_conditions']['model_calls'] == 0
        assert status['specific_conditions']['accepted_triggers'][0]['trigger_pattern'] == 'access_token login'
    else:
        assert status['evidence_fallback']['model_calls'] == 0
    assert result['snapshot'] == policy['snapshot']


def test_public_train_augmentation_preserves_original_rows_and_is_idempotent(tmp_path, monkeypatch):
    import dataset
    canonical = dataset.DATASETS
    values = [copy.deepcopy(row) for row in dataset.rows('train')
              if not row.get('initial_state', {}).get('training_goal')]
    for name in [*(filename for split, filename in dataset.FILES.items() if split != 'train'),
                 'manifest.json']:
        (tmp_path / name).write_bytes((canonical / name).read_bytes())
    initial = ''.join(json.dumps(row, ensure_ascii=False) + '\n' for row in values)
    path = tmp_path / 'train.jsonl'; path.write_text(initial)
    monkeypatch.setattr(dataset, 'DATASETS', tmp_path)
    added = dataset.ensure_training_augmentation()
    after = path.read_bytes()
    assert added and len(set(added)) == len(added)
    assert after.startswith(initial.encode())
    rows = dataset.rows('train')
    assert rows[:len(values)] == values
    assert len(rows) == len(values) + len(added)
    assert {row['task_id'] for row in rows[len(values):]} == set(added)
    assert all(row['split'] == 'train' and row['provenance']['kind'] == 'synthetic'
               and row['provenance']['learning_exposed'] is True for row in rows[len(values):])
    assert {row['attributes']['appworld'].get('observation_debug') for row in rows
            if row['attributes']['appworld'].get('observation_debug')} == {
                'saved_read_result', 'silent_mutation_readback',
                'repeated_read_saved_result', 'repeated_mutation_readback'}
    assert dataset.ensure_training_augmentation() == added
    assert path.read_bytes() == after
    assert dataset.validate()['train'] == len(rows)


def test_added_training_tasks_do_not_change_healthy_official_batch_boundaries(monkeypatch):
    import dataset
    official = [f'appworld-official-{number}' for number in range(90)]
    added = [f'appworld-synthetic-{number}' for number in range(6)]
    monkeypatch.setattr(dataset, 'rows', lambda split: [
        {'task_id': task_id, 'initial_state': {'training_goal': {'kind': 'cart_all_to_wishlist'}}}
        for task_id in added])
    batches = training.training_batches(official + added, 16)
    assert [len(batch) for batch in batches] == [16, 16, 16, 16, 16, 10, 6]
    assert batches[:6] == [official[start:start + 16] for start in range(0, 90, 16)]
    assert batches[-1] == added


@pytest.mark.parametrize('reinstall', [False, True])
def test_appworld_curator_templates_keep_api_contracts_private(tmp_path, reinstall):
    from types import SimpleNamespace
    import learning
    runtime = tmp_path / 'training/runtime'
    runtime.mkdir(parents=True)
    prompt_root = ROOT / 'tools/trajectory_evolution/config/prompts'
    keys = ('dialogue_system', 'standard_dialogue_system', 'dialogue_user',
            'memory_curator_system', 'memory_curator_user')
    shared = {key: (prompt_root / (key.replace('_', '-') + '.txt')).read_bytes()
              for key in keys}
    model = {'base_url': 'https://example.invalid/v1', 'model': 'unchanged-fixture'}
    evolution = {'enable_memory_semantic_matching': True,
                 'min_memory_duplicate_confidence': .9,
                 'min_memory_relation_confidence': .7}
    save(runtime / 'runtime.json', {'model': model, 'evolution': evolution,
                                  'prompts': {key: 'prompts/' + key.replace('_', '-') + '.txt'
                                              for key in keys}})
    native = Mock(side_effect=AssertionError('installation must not call a model or mutate memory'))
    controller = SimpleNamespace(ROOT=ROOT, RUNTIME=runtime, read=lambda path: json.loads(path.read_text()),
                                 save=save, native=native, CURATION_NEW_ENTRY_THRESHOLD=512,
                                 CURATION_REVIEW_MAX_ATTEMPTS=1)
    installed = learning.install_prompts(controller)
    before = {path: path.read_bytes() for path in runtime.rglob('*') if path.is_file()}
    if reinstall:
        assert learning.install_prompts(controller) == installed
        assert {path: path.read_bytes() for path in runtime.rglob('*') if path.is_file()} == before
    actual = json.loads((runtime / 'runtime.json').read_text())
    assert actual['model'] == model
    assert actual['evolution']['min_memory_duplicate_confidence'] == .9
    assert actual['evolution']['min_memory_relation_confidence'] == .7
    assert actual['evolution']['enable_memory_semantic_matching'] is True
    assert installed['analysis_prompt_keys'] == list(keys[:3])
    assert installed['curation_prompt_keys'] == list(keys[3:])
    assert installed['agent_prompt_changed'] is False and installed['model_calls'] == 0
    native.assert_not_called()
    for key in keys:
        shared_path = prompt_root / (key.replace('_', '-') + '.txt')
        assert shared_path.read_bytes() == shared[key]
        private_path = runtime / actual['prompts'][key]
        expected = shared[key].decode().rstrip() + '\n\n'
        constraint = learning.CURATION_COMPATIBILITY if key in keys[3:] else learning.CLASSIFICATION
        assert private_path.read_text() == expected + constraint + '\n'
    for key in keys[3:]:
        text = (runtime / actual['prompts'][key]).read_text()
        assert '{{memory_json}}' in text if key == 'memory_curator_user' else 'DUPLICATE' in text
        assert 'appworld.execute' in text and 'mark_thread_archived' in text
        assert 'archive/delete' in text and 'quantity' in text
        assert 'UNRESOLVED' in text and '不得改写任一候选' in text


@pytest.mark.parametrize('has_override', [False, True])
@pytest.mark.parametrize('change', [None, 'content', 'target_scope', 'trigger_pattern', 'control_target_type'])
def test_history_english_inheritance_binds_current_approved_card_without_revision_blocker(tmp_path, change, has_override):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    c.GEPA = tmp_path / 'gepa'
    c.ids = lambda split: ['fixed-task']
    original = copy.deepcopy(policy)
    original['options']['experience_overrides'] = [{
        'source_experience': original['options']['approved_experiences'][0],
        'content': original['snapshot']['experiences'][0]['content'],
        'trigger_pattern': 'login access_token'}]
    if not has_override:
        original['options']['experience_overrides'] = []
    old = c.PUBLISHED / 'historical/strategy.json'
    save(old, {'policy': original})
    report = {'status': 'complete', 'items': [{'task_id': 'fixed-task'}], 'seed': str(old),
              'metrics': {'L0_reused': {}, 'seed_historical': {'scored': 1, 'planned': 1,
                'quality': 1, 'judge_errors': 0, 'avg_invalid_loop': 1}}}
    save(c.GEPA / 'seed-search-1-stage32/comparison.json', report)
    policy['snapshot']['experiences'][0]['content_revision'] = 'new-support-revision'
    policy['options']['approved_experiences'][0]['content_revision'] = 'new-support-revision'
    if change is not None:
        policy['snapshot']['experiences'][0][change] = 'changed-real-contract'
    sources = {'auth-experience': {'experience': policy['snapshot']['experiences'][0]}}
    before = copy.deepcopy(policy)
    inherited = learning._historical_english_inheritance(c, policy, sources)
    assert policy == before
    assert inherited['diagnostic']['historical_score_usage'] == 'proposal_selection_only_not_current_performance'
    assert inherited['fixed_conditions'] == ({'auth-experience': 'login access_token'} if change is None and has_override else {})
    assert inherited['incremental_ids'] == (set() if change is None and has_override else {'auth-experience'})
    c.native.assert_not_called()


def test_history_english_inheritance_chooses_complete_best_same_tasks_only(tmp_path):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    c.GEPA = tmp_path / 'gepa';c.ids = lambda split: ['fixed-task']
    for index, quality, status, task in [(1, .5, 'complete', 'fixed-task'),
                                      (2, .75, 'complete', 'fixed-task'),
                                      (3, 1., 'partial', 'fixed-task'),
                                      (4, 1., 'complete', 'wrong-task')]:
        artifact = c.PUBLISHED / f'historical-{index}/strategy.json'
        previous = copy.deepcopy(policy)
        previous['options']['experience_overrides'] = [{
            'source_experience': previous['options']['approved_experiences'][0],
            'content': previous['snapshot']['experiences'][0]['content'],
            'trigger_pattern': 'login access_token'}]
        save(artifact, {'policy': previous})
        save(c.GEPA / f'seed-search-{index}-stage32/comparison.json', {
            'status': status, 'items': [{'task_id': task}], 'seed': str(artifact),
            'metrics': {'L0_reused': {}, 'seed': {'scored': 1, 'planned': 1, 'quality': quality}}})
    result = learning._historical_english_inheritance(c, policy, {'auth-experience': {}})
    assert result['diagnostic']['source_strategy'].endswith('historical-2/strategy.json')
    assert result['diagnostic']['historical_selection_quality'] == .75


def test_history_inheritance_uses_durable_l0_scope_and_rejects_partial_metric_candidate(tmp_path):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    c.GEPA = tmp_path / 'gepa'; c.ids = lambda split: ['wrong-task']
    save(c.GEPA / 'l0-current-stage32/result.json', {'items': [{'task_id': 'fixed-task'}]})
    for index, quality, loop_status in [(1, .75, 'complete'), (2, 1., 'partial')]:
        artifact = c.PUBLISHED / f'durable-{index}/strategy.json'
        previous = copy.deepcopy(policy)
        previous['options']['experience_overrides'] = [{
            'source_experience': previous['options']['approved_experiences'][0],
            'content': previous['snapshot']['experiences'][0]['content'],
            'trigger_pattern': 'login access_token'}]
        save(artifact, {'policy': previous})
        save(c.GEPA / f'seed-search-{index}-stage32/comparison.json', {
            'status': 'complete', 'items': [{'task_id': 'fixed-task'}], 'seed': str(artifact),
            'metrics': {'L0_reused': {}, 'seed': {
                'scored': 1, 'planned': 1, 'quality': quality,
                'quality_complete': True, 'invalid_loop_status': loop_status,
                'execution_costs_complete': True, 'tokens_complete': True}}})
    result = learning._historical_english_inheritance(c, policy,
        {'auth-experience': {'experience': policy['snapshot']['experiences'][0]}})
    assert result['diagnostic']['source_strategy'].endswith('durable-1/strategy.json')
    assert result['diagnostic']['baseline_task_scope_source'].endswith(
        'l0-current-stage32/result.json')


def test_incremental_english_trigger_requires_two_positive_windows(tmp_path):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    sources = learning._english_trigger_sources(c, policy, feedback)
    with pytest.raises(RuntimeError, match='no_specific_supported_pair'):
        learning._specific_english_pairs(c, policy, sources, [], {},
            incremental_ids={'auth-experience'}, min_incremental_support=2)


def test_specific_english_projection_discards_stale_inherited_override(tmp_path):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    policy['options']['experience_overrides'] = [{
        'source_experience': {'memory_id': 'stale-source', 'content_revision': 'old'},
        'content': 'stale', 'trigger_pattern': 'stale trigger'}]
    sources = learning._english_trigger_sources(c, policy, feedback)
    compiled = learning._specific_english_pairs(c, policy, sources, [], {},
        fixed_conditions={'auth-experience': 'login access_token'}, incremental_ids=set())
    ids = {(row.get('source_experience') or {}).get('memory_id')
           for row in compiled['options']['experience_overrides']}
    assert 'stale-source' not in ids
    assert 'auth-experience' in ids


def test_specific_english_inheritance_keeps_fixed_pair_and_ignores_old_sibling_generation(tmp_path):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    sources = learning._english_trigger_sources(c, policy, feedback)
    before = copy.deepcopy(policy)
    diagnostic = {}
    compiled = learning._specific_english_pairs(c, policy, sources,
        [[{'approved_source': True, 'memory_id': 'auth-experience', 'trigger_pattern': 'different alias'}]],
        diagnostic, fixed_conditions={'auth-experience': 'login access_token'}, incremental_ids=set())
    assert policy == before and compiled['snapshot'] == before['snapshot']
    assert compiled['options']['approved_experiences'] == before['options']['approved_experiences']
    assert compiled['options']['loop_rules'] == before['options']['loop_rules']
    assert compiled['options']['experience_overrides'][0]['trigger_pattern'] == 'login access_token'


def test_specific_english_current_phase_guard_overrides_inherited_generic_alias(tmp_path, monkeypatch):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    sources = learning._english_trigger_sources(c, policy, feedback)
    diagnostic = {}
    guard = Mock(return_value=(policy, {'auth-experience': ('login', 'access_token')}))
    monkeypatch.setattr(learning, '_compile_public_phase_base', guard)
    compiled = learning._specific_english_pairs(c, policy, sources, [], diagnostic,
        fixed_conditions={'auth-experience': 'password authentication'}, incremental_ids=set())
    assert compiled['options']['experience_overrides'][0]['trigger_pattern'] == 'login access_token'


@pytest.mark.parametrize('text, fixed', [
    ('累计总时长≥900秒，选择播放列表。', True),
    ('Choose a playlist with duration >= 900 seconds.', True),
    ('Play at least 15 minutes of downloaded songs.', True),
    ('Use page_limit=100 and max_iterations=30.', False),
    ('累计时长达到当前任务要求；page_limit=20。', False)])
def test_business_duration_threshold_is_not_a_public_api_capacity(text, fixed):
    import learning
    assert learning._experience_fixed_duration({'content': text}) is fixed


def test_public_library_object_contract_and_bare_methods_are_preserved():
    import learning
    card = {'trigger_pattern': '查播放列表', 'content': 'show_playlist_library返回song_ids，累计已下载歌曲时长。'}
    assert learning._experience_operation_names(card) == {'show_playlist_library'}
    assert learning._experience_task_compatible(card, 'Play a downloaded playlist for fifteen minutes.')
    assert not learning._experience_task_compatible(card, 'Play an album for my walk.')
    assert learning._experience_operation_names({'content': 'page_limit=20, max_iterations=30'}) == set()
    assert not learning._experience_fixed_duration({'trigger_pattern': '总时长≥540秒的播放列表',
        'content': '累计已下载歌曲的时长，达到当前公开请求的时长。'})


def test_fixed_duration_card_is_disabled_without_mutating_approved_memory(tmp_path):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    card = policy['snapshot']['experiences'][0]
    card.update(content='选择总时长≥900秒的播放列表。', trigger_pattern='播放列表的duration和song_ids')
    policy['options']['experience_overrides'] = [{'source_experience': policy['options']['approved_experiences'][0],
        'content': card['content'], 'trigger_pattern': 'duration song_ids'}]
    sources = learning._english_trigger_sources(c, policy, feedback)
    before = copy.deepcopy(policy)
    compiled, conditions = learning._compile_public_phase_base(c, policy, sources, {})
    assert policy == before and compiled['snapshot'] == before['snapshot'] and conditions == {}
    assert compiled['options']['approved_experiences'] == before['options']['approved_experiences']
    assert compiled['options']['loop_rules'] == before['options']['loop_rules']
    assert compiled['options']['experience_overrides'] == []
    assert compiled['options']['disabled_injections'] == policy['options']['approved_experiences']


def test_private_training_extraction_generalizes_business_values_but_keeps_api_contracts():
    import learning
    assert '900秒' in learning.CLASSIFICATION and '不能固化为通用成功条件' in learning.CLASSIFICATION
    assert 'page_limit' in learning.CLASSIFICATION and 'playlist与album' in learning.CLASSIFICATION


def test_english_generation_pure_inheritance_uses_zero_model_calls(tmp_path, monkeypatch):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    monkeypatch.setattr(learning, 'seed_control_input', lambda *args: ({}, feedback, {}))
    monkeypatch.setattr(learning, '_historical_english_inheritance', lambda *args: {
        'fixed_conditions': {'auth-experience': 'login access_token'}, 'incremental_ids': set(),
        'diagnostic': {'model_calls': 0, 'eligible_conditions': 1}})
    no_model = Mock(side_effect=AssertionError('inheritance must not translate unchanged cards'))
    monkeypatch.setattr(learning, '_english_trigger_model_reply', no_model)
    before = copy.deepcopy(policy);diagnostic = {}
    result = learning.generate_english_triggers(c, policy, set(), diagnostic)
    assert policy == before and result['snapshot'] == before['snapshot']
    assert diagnostic['english_trigger_learning']['model_calls'] == 0
    assert diagnostic['english_trigger_learning']['inheritance']['eligible_conditions'] == 1
    no_model.assert_not_called()


@pytest.mark.parametrize('failure', [
    ValueError('appworld_english_trigger_no_valid_subset'),
    RuntimeError('appworld_english_trigger_no_specific_supported_pair')])
@pytest.mark.parametrize('spent_calls', [0, 1, 2, 3])
def test_rejected_english_inheritance_regenerates_current_sources_with_original_call_budget(
        tmp_path, monkeypatch, failure, spent_calls):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    packet = {'memories': [{'memory_id': 'auth-experience'}, {'memory_id': 'unsupported-card'}]}
    monkeypatch.setattr(learning, 'seed_control_input', lambda *args: (packet, feedback, {}))
    monkeypatch.setattr(learning, '_historical_english_inheritance', lambda *args: {
        'fixed_conditions': {'auth-experience': 'obsolete aliases'}, 'incremental_ids': set(),
        'diagnostic': {'model_calls': 0, 'eligible_conditions': 1}})
    original = learning._specific_english_pairs
    calls = []
    def select(controller, current, sources, drafts, diagnostic, **options):
        calls.append(options)
        if len(calls) == 1:
            diagnostic['rejected'] = [{'memory_id': 'auth-experience',
                                      'reason': 'appworld_english_trigger_native_runtime_not_matched'}]
            raise failure
        assert options.get('fixed_conditions') is None
        assert options.get('incremental_ids') is None
        return original(controller, current, sources, drafts, diagnostic, **options)
    monkeypatch.setattr(learning, '_specific_english_pairs', select)
    def reply(controller, outgoing, repair_reason, transport):
        assert spent_calls < 3
        assert outgoing['memories'] == [{'memory_id': 'auth-experience'}]
        transport['model_calls'] += 1
        return {'triggers': [] if spent_calls == 2 else [{'memory_id': 'auth-experience',
                              'trigger_pattern': 'login access_token'}]}, {}
    model = Mock(side_effect=reply)
    monkeypatch.setattr(learning, '_english_trigger_model_reply', model)
    before = copy.deepcopy(policy)
    diagnostic = {'english_trigger_learning': {'model_calls': spent_calls,
        'attempts': [{'draft': [{'approved_source': True, 'memory_id': 'auth-experience',
                                 'trigger_pattern': 'obsolete aliases'}]}]}}
    result = learning.generate_english_triggers(c, policy, set(), diagnostic)
    status = diagnostic['english_trigger_learning']
    assert policy == before and result['snapshot'] == before['snapshot']
    assert status['status'] == 'compiled'
    assert status['inheritance']['current_validation']['reason'] == str(failure)
    assert status['inheritance']['current_validation']['rejected'][0]['memory_id'] == 'auth-experience'
    assert status['model_calls'] == (spent_calls + 1 if spent_calls < 3 else 3)
    assert model.call_count == (1 if spent_calls < 3 else 0)
    assert len(calls) == 2


def test_english_inheritance_retains_valid_sibling_without_model_when_an_alias_is_stale(tmp_path, monkeypatch):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    stale = copy.deepcopy(policy['snapshot']['experiences'][0])
    stale.update(memory_id='stale-experience', content_revision='stale-revision')
    policy['snapshot']['experiences'].append(stale)
    policy['options']['approved_experiences'].append({key: stale[key]
        for key in ('memory_id', 'content_revision')})
    sources = learning._english_trigger_sources(c, policy, feedback)
    sources['stale-experience'] = copy.deepcopy(sources['auth-experience'])
    sources['stale-experience']['experience'] = stale
    monkeypatch.setattr(learning, '_english_trigger_sources', lambda *args: sources)
    monkeypatch.setattr(learning, 'seed_control_input', lambda *args: ({}, feedback, {}))
    monkeypatch.setattr(learning, '_historical_english_inheritance', lambda *args: {
        'fixed_conditions': {'auth-experience': 'login access_token',
                             'stale-experience': 'login nonexistent_parameter'},
        'incremental_ids': set(), 'diagnostic': {'eligible_conditions': 2}})
    model = Mock(side_effect=AssertionError('a validated inherited sibling needs no translation'))
    monkeypatch.setattr(learning, '_english_trigger_model_reply', model)
    before = copy.deepcopy(policy)
    diagnostic = {}
    result = learning.generate_english_triggers(c, policy, set(), diagnostic)
    assert policy == before and result['snapshot'] == before['snapshot']
    assert [row['source_experience']['memory_id']
            for row in result['options']['experience_overrides']] == ['auth-experience']
    assert diagnostic['english_trigger_learning']['model_calls'] == 0
    assert diagnostic['english_trigger_learning']['status'] == 'compiled'
    model.assert_not_called()


@pytest.mark.parametrize('reject_second_batch', [False, True])
@pytest.mark.parametrize('unrelated_previous_budget', [False, True])
def test_incremental_english_batches_cover_untranslated_cards_keep_siblings_and_resume_without_model(
        tmp_path, monkeypatch, reject_second_batch, unrelated_previous_budget):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    original_source = learning._english_trigger_sources(c, policy, feedback)
    template = copy.deepcopy(policy['snapshot']['experiences'][0])
    cards = [dict(template, memory_id=f'card-{i:02d}', content_revision=f'revision-{i:02d}')
             for i in range(72)]
    policy['snapshot']['experiences'] = cards
    policy['options']['approved_experiences'] = [{key: card[key]
        for key in ('memory_id', 'content_revision')} for card in cards]
    sources = learning._EnglishTriggerSources(runtime_runs=original_source.runtime_runs)
    for card in cards:
        source = copy.deepcopy(original_source['auth-experience'])
        source['experience'] = card;sources[card['memory_id']] = source
    packet = {'memories': [{'memory_id': card['memory_id'], 'scope': c.SCOPE,
        'condition': card['trigger_pattern'], 'problem': 'A reviewed missing authentication argument',
        'experience': card['content']} for card in cards], 'training_statistics': {'count': 90}}
    monkeypatch.setattr(learning, 'seed_control_input', lambda *args: (packet, feedback, {}))
    monkeypatch.setattr(learning, '_english_trigger_sources', lambda *args: sources)
    fixed = {card['memory_id']: 'login access_token' for card in cards[:13]}
    monkeypatch.setattr(learning, '_historical_english_inheritance', lambda *args: {
        'fixed_conditions': fixed, 'incremental_ids': set(sources) - set(fixed),
        'diagnostic': {'eligible_conditions': 13, 'incremental_sources': 59}})
    def select(controller, original, current_sources, drafts, diagnostic, **options):
        rows = dict(options['fixed_conditions'])
        for draft in drafts:
            for item in draft:
                assert item['memory_id'] in options['incremental_ids']
                rows[item['memory_id']] = item['trigger_pattern']
        assert set(fixed) <= set(rows)
        accepted = [{'memory_id': mid, 'trigger_pattern': trigger} for mid, trigger in sorted(rows.items())]
        diagnostic.update(accepted_count=len(accepted), accepted_triggers=accepted)
        current_sources.last_profile = {'validated_condition_count': len(accepted)}
        result = copy.deepcopy(original)
        result['options']['experience_overrides'] = [{
            'source_experience': {key: current_sources[row['memory_id']]['experience'][key]
                                  for key in ('memory_id', 'content_revision')},
            'content': current_sources[row['memory_id']]['experience']['content'],
            'trigger_pattern': row['trigger_pattern']} for row in accepted]
        return result
    monkeypatch.setattr(learning, '_specific_english_pairs', select)
    submitted = []
    def reply(controller, outgoing, repair_reason, transport):
        assert repair_reason is None
        transport['model_calls'] += 1
        submitted.append([card['memory_id'] for card in outgoing['memories']])
        assert len(outgoing['memories']) <= 20
        assert all(set(card) == {'memory_id', 'scope', 'condition', 'problem', 'experience'}
                   for card in outgoing['memories'])
        if reject_second_batch and len(submitted) == 2:
            return {'triggers': []}, {}
        triggers = [{'memory_id': card['memory_id'], 'trigger_pattern': 'login access_token'}
                    for card in outgoing['memories']]
        return {'triggers': triggers + [triggers[0],
            {'memory_id': ['malformed-id'], 'trigger_pattern': 'login access_token'},
            {'memory_id': 'unsupported', 'trigger_pattern': 'login access_token'}]}, {}
    model = Mock(side_effect=reply)
    monkeypatch.setattr(learning, '_english_trigger_model_reply', model)
    before = copy.deepcopy(policy)
    diagnostic = {'seed_label': 'next-candidate', 'control_learning': {'status': 'reused_frozen'}}
    if unrelated_previous_budget:
        diagnostic['english_trigger_learning'] = {'model_calls': 3, 'incremental_generation': {
            'source_experiences': [{'memory_id': 'other-source', 'content_revision': 'other-revision'}],
            'model_calls': 3, 'model_attempts': 3,
            'batches': [{'status': 'rejected', 'memory_ids': ['other-source']}]}}
    result = learning.generate_english_triggers(c, policy, set(), diagnostic)
    status = diagnostic['english_trigger_learning'];generation = status['incremental_generation']
    assert policy == before and result['snapshot'] == before['snapshot']
    assert len(submitted) == 3
    assert set(sum(submitted, [])) == set(sources) - set(fixed)
    assert len(sum(submitted, [])) == 59
    assert generation['model_calls'] == generation['model_attempts'] == 3
    assert set(fixed) <= {row['source_experience']['memory_id']
                         for row in result['options']['experience_overrides']}
    assert status['override_count'] == (52 if reject_second_batch else 72)
    assert c.read(c.TRAINING/'seed-compile-status.json')['seed_label'] == 'next-candidate'
    previous_calls = model.call_count
    resumed = learning.generate_english_triggers(c, policy, set(), diagnostic)
    assert model.call_count == previous_calls
    assert len(resumed['options']['experience_overrides']) == (52 if reject_second_batch else 72)
    assert diagnostic['english_trigger_learning']['incremental_generation']['model_calls'] == 3
    c.native.assert_not_called()


@pytest.mark.parametrize('recovery', ['saved_pending', 'legacy_usage_only', 'interrupted_after_response'])
def test_incremental_paid_response_recovery_keeps_budget_and_uncovered_cards(tmp_path, monkeypatch, recovery):
    learning, c, policy, feedback, _ = _english_trigger_fixture(tmp_path)
    sources = learning._english_trigger_sources(c, policy, feedback)
    base = copy.deepcopy(sources['auth-experience'])
    cards = [dict(base['experience'], memory_id=f'new-{i}', content_revision=f'new-revision-{i}')
             for i in range(4)]
    for card in cards:
        policy['snapshot']['experiences'].append(card)
        policy['options']['approved_experiences'].append({key: card[key]
            for key in ('memory_id', 'content_revision')})
        sources[card['memory_id']] = dict(copy.deepcopy(base), experience=card)
    fixed = [{'memory_id': 'auth-experience', 'trigger_pattern': 'login access_token'}]
    compiled = copy.deepcopy(policy)
    packet = {'memories': [{'memory_id': card['memory_id']} for card in cards]}
    bindings = [{key: card[key] for key in ('memory_id', 'content_revision')} for card in cards]
    usage = {'prompt_tokens': 15372, 'completion_tokens': 1147, 'total_tokens': 16519}
    initial = {'status': 'pending', 'attempt': 1, 'memory_ids': ['new-0', 'new-1'], 'usage': usage}
    if recovery == 'saved_pending':
        initial['draft'] = [{'approved_source': True, 'memory_id': mid,
            'trigger_pattern': 'login access_token'} for mid in initial['memory_ids']]
    recorded_calls = 0 if recovery == 'legacy_usage_only' else 1
    previous = {} if recovery == 'interrupted_after_response' else {'model_calls': recorded_calls,
        'incremental_generation': {'source_experiences': bindings, 'model_attempts': 1,
            'model_calls': recorded_calls, 'batches': [initial]}}
    submitted, native_calls = [], []
    def reply(controller, outgoing, reason, transport):
        transport['model_calls'] += 1
        submitted.append([card['memory_id'] for card in outgoing['memories']])
        return {'triggers': [{'memory_id': mid, 'trigger_pattern': 'login access_token'}
                             for mid in submitted[-1]]}, usage
    monkeypatch.setattr(learning, '_english_trigger_model_reply', Mock(side_effect=reply))
    def select(controller, original, current_sources, drafts, status, **options):
        native_calls.append(drafts)
        if recovery == 'interrupted_after_response' and len(native_calls) == 1:
            saved = c.read(c.TRAINING/'seed-compile-status.json')['english_trigger_learning']
            row = saved['incremental_generation']['batches'][0]
            assert row['draft'] and row['usage'] == usage and row['status'] == 'draft_ready'
            assert saved['model_calls'] == saved['incremental_generation']['model_calls'] == 1
            assert all(set(item) <= {'approved_source', 'memory_id', 'trigger_length', 'trigger_pattern'}
                       for item in row['draft'])
            raise PermissionError(13, 'fixture native preexec failure', 'fixture-native')
        accepted = dict(options['fixed_conditions'])
        accepted.update({row['memory_id']: row['trigger_pattern'] for draft in drafts for row in draft})
        status.update(accepted_triggers=[{'memory_id': mid, 'trigger_pattern': trigger}
            for mid, trigger in accepted.items()], accepted_count=len(accepted))
        current_sources.last_profile = {'local_validation': 'complete'}
        return copy.deepcopy(original)
    monkeypatch.setattr(learning, '_specific_english_pairs', select)
    diagnostic = {'seed_label': 'recovery', 'english_trigger_learning': {
        'model_calls': 0, 'attempts': [], 'specific_conditions': {'accepted_triggers': fixed}}}
    if recovery == 'interrupted_after_response':
        with pytest.raises(PermissionError):
            learning._expand_english_trigger_subset(c, policy, compiled, sources, packet, diagnostic, previous)
        previous = c.read(c.TRAINING/'seed-compile-status.json')['english_trigger_learning']
        diagnostic['english_trigger_learning'] = {'model_calls': 0, 'attempts': [],
                                                  'specific_conditions': {'accepted_triggers': fixed}}
    learning._expand_english_trigger_subset(c, policy, compiled, sources, packet, diagnostic, previous)
    status = diagnostic['english_trigger_learning'];generation = status['incremental_generation']
    assert generation['model_attempts'] == generation['model_calls'] == status['model_calls'] == 3
    assert status['override_count'] == 5
    if recovery == 'legacy_usage_only':
        assert len(submitted) == 2 and set(sum(submitted, [])) == {card['memory_id'] for card in cards}
        assert generation['batches'][0]['status'] == 'draft_missing'
    elif recovery == 'saved_pending':
        assert sum(submitted, []) == ['new-2', 'new-3']
    else:
        assert len(submitted) == 3 and len(sum(submitted, [])) == 4
        assert len(set(sum(submitted, []))) == 4
    c.native.assert_not_called()


@pytest.mark.parametrize('kind, broad, valid', [
    ('duration', 'task_object_playlist show_playlist_library', 'task_object_playlist_downloaded_duration_playback show_playlist_library'),
    ('quantity', 'move_product_from_cart_to_wish_list show_cart', 'task_cart_to_wishlist_full_quantity cart_quantity_state_ready'),
    ('subsetquantity', 'move_product_from_cart_to_wish_list quantity', 'task_cart_to_wishlist_quantity cart_quantity_state_ready')])
def test_specialized_english_conditions_cannot_bypass_public_purpose_or_scope(kind, broad, valid):
    import learning
    content = ('show_playlist_library 返回 song_ids，show_downloaded_songs 返回已下载歌曲 duration，累计时长。'
        if kind == 'duration' else 'apis.amazon.move_product_from_cart_to_wish_list(quantity=...)，quantity传完整数量，通过show_cart验证目标数量。'
        if kind == 'subsetquantity' else 'apis.amazon.move_product_from_cart_to_wish_list(quantity=...)，quantity传完整数量，通过show_cart验证购物车为空。')
    card = {'memory_id': 'source', 'content_revision': 'approved',
            'trigger_pattern': '播放列表' if kind == 'duration' else '', 'content': content}
    sources = {'source': {'experience': card}}
    with pytest.raises(ValueError, match='public_(purpose|cart_scope)_missing'):
        learning._english_trigger_item({'memory_id': 'source', 'trigger_pattern': broad}, sources, set(), {})
    mid, rewrite = learning._english_trigger_item({'memory_id': 'source', 'trigger_pattern': valid}, sources, set(), {})
    assert mid == 'source' and rewrite['content'] == content
    assert rewrite['source_experience'] == {'memory_id': 'source', 'content_revision': 'approved'}


def test_cart_quantity_phase_preserves_subset_scope_and_inherited_source(tmp_path):
    learning, c, policy, _, _ = _english_trigger_fixture(tmp_path)
    card = policy['snapshot']['experiences'][0]
    card.update(trigger_pattern='', content='Use show_cart to obtain quantity, then move_product_from_cart_to_wish_list with the full quantity for each requested product.')
    pair = 'task_cart_to_wishlist_quantity cart_quantity_state_ready'
    assert learning._public_phase_conditions({'experience': card}) == tuple(pair.split())
    assert learning._experience_task_compatible(card, 'Move all units of products rated below three from my Amazon cart to my wish list.')
    assert not learning._experience_task_compatible(card, 'Move every product from my wish list to my cart.')
    def query(key, state):
        return {'advice_query': {'identity': policy['snapshot']['identity'], 'target_scope': c.SCOPE,
            'query_id': key, 'decision_id': key, 'task': 'Move products from my cart to my wish list.', 'state': state},
            'execution_id': 'run', 'boundary': 'llm', 'actor_id': 'skill_executor'}
    windows = [query('docs', 'move_product_from_cart_to_wish_list quantity show_cart task_cart_to_wishlist_quantity'),
               query('cart', pair), query('later', pair)]
    sources = learning._EnglishTriggerSources({'auth-experience': {'experience': card,
        'evidence': [{'run_id': 'training-run', 'query_id': key, 'task': 'Move products from my cart to my wish list.'}
                     for key in ('docs', 'cart', 'later')]}}, runtime_runs={'training-run': windows})
    before = copy.deepcopy(policy)
    inherited = {'source_experience': {'memory_id': card['memory_id'], 'content_revision': card['content_revision']},
                 'content': card['content'], 'trigger_pattern': 'move_product_from_cart_to_wish_list quantity'}
    policy['options']['experience_overrides'] = [inherited]
    result, conditions = learning._compile_public_phase_base(c, policy, sources, {})
    assert conditions == {'auth-experience': tuple(pair.split())}
    assert result['options']['experience_overrides'][0]['trigger_pattern'] == pair
    assert result['snapshot'] == before['snapshot']
    assert result['options']['approved_experiences'] == before['options']['approved_experiences']
    assert result['options']['loop_rules'] == before['options']['loop_rules']
    hits = learning._native_l1_profile(c, result, sources)
    assert not hits[('training-run', 'docs')]
    assert hits[('training-run', 'cart')] == {'auth-experience'}
    assert not hits[('training-run', 'later')]


def test_object_scoped_pagination_can_build_specific_pair_without_a_named_api():
    import learning
    card = {'trigger_pattern': '获取播放列表等分页数据时使用page_index和page_limit。',
            'content': 'page_limit设为20，返回少于page_limit时停止。'}
    assert learning._experience_operation_names(card) == set()
    assert learning._experience_task_marker(card) == 'task_object_playlist'
    assert learning._experience_task_compatible(card, 'Count all songs across my playlists.')
    assert not learning._experience_task_compatible(card, 'Archive every Gmail message.')


def test_inherited_playlist_alias_is_regenerated_when_current_purpose_guard_is_missing(tmp_path):
    learning, c, policy, _, _ = _english_trigger_fixture(tmp_path)
    card = policy['snapshot']['experiences'][0]
    card.update(trigger_pattern='播放列表下载时长', content='show_playlist_library 获取播放列表，累计已下载歌曲 duration。')
    marker = 'task_object_playlist_downloaded_duration_playback'
    task = 'Play an existing playlist for my fifteen-minute offline walk.'
    def query(key, instruction, state):
        return {'advice_query': {'identity': policy['snapshot']['identity'], 'target_scope': c.SCOPE,
            'query_id': key, 'decision_id': key, 'task': instruction, 'state': state},
            'execution_id': key, 'boundary': 'llm', 'actor_id': 'skill_executor'}
    positive = query('positive', task, marker + ' show_playlist_library')
    negative = query('negative', 'Count artists in my playlists.', 'task_object_playlist show_playlist_library')
    sources = learning._EnglishTriggerSources({'auth-experience': {'experience': card,
        'evidence': [{'run_id': 'positive-run', 'query_id': 'positive', 'task': task}]}},
        runtime_runs={'positive-run': [positive], 'negative-run': [negative]})
    sources.runtime_codes = {('positive-run', 'positive'): 'apis.spotify.show_playlist_library()',
                             ('negative-run', 'negative'): 'apis.spotify.show_playlist_library()'}
    before = copy.deepcopy(policy)
    result = learning._specific_english_pairs(c, policy, sources, [], {},
        fixed_conditions={'auth-experience': 'task_object_playlist show_playlist_library'}, incremental_ids=set())
    rewritten = result['options']['experience_overrides'][0]
    assert marker in rewritten['trigger_pattern'] and 'show_playlist_library' in rewritten['trigger_pattern']
    assert result['snapshot'] == before['snapshot'] and policy == before
    assert result['options']['approved_experiences'] == before['options']['approved_experiences']
    assert result['options']['loop_rules'] == before['options']['loop_rules']


def test_gmail_pagination_phase_requires_an_actual_inbox_page_contract():
    import learning
    card = {'trigger_pattern': '读取邮件分页',
            'content': 'show_inbox_threads 使用 page_index 和 page_limit 逐页读取直到最后一页。'}
    assert learning._experience_gmail_inbox_pagination(card)
    assert learning._public_phase_conditions({'experience': card}) == (
        'task_gmail_inbox_complete_enumeration', 'gmail_authenticated_state_ready')
    assert not learning._experience_gmail_inbox_pagination(dict(card, content='show_inbox_threads page_limit'))
    assert not learning._experience_gmail_inbox_pagination(dict(card, content='show_outbox_threads page_index page_limit'))
    assert learning._experience_task_compatible(card, 'Archive all read, non-starred email threads in my Gmail inbox.')
    for instruction in ('Send an email from my Gmail account.',
                        'Delete all threads in my Gmail outbox.',
                        'Read every text message in my Phone inbox.'):
        assert not learning._experience_task_compatible(card, instruction)


def test_gmail_pagination_drafts_cannot_bypass_public_authentication_phase():
    import learning
    card = {'memory_id': 'inbox-source', 'content_revision': 'approved', 'trigger_pattern': '',
            'content': 'show_inbox_threads 使用 page_index 和 page_limit 逐页读取。'}
    sources = {'inbox-source': {'experience': card}}
    with pytest.raises(ValueError, match='public_inbox_scope_missing'):
        learning._english_trigger_item({'memory_id': 'inbox-source',
            'trigger_pattern': 'page_index show_inbox_threads'}, sources, set(), {})
    mid, rewrite = learning._english_trigger_item({'memory_id': 'inbox-source',
        'trigger_pattern': 'task_gmail_inbox_complete_enumeration gmail_authenticated_state_ready'}, sources, set(), {})
    assert mid == 'inbox-source' and rewrite['content'] == card['content']
    assert rewrite['source_experience'] == {'memory_id': 'inbox-source', 'content_revision': 'approved'}


@pytest.mark.parametrize('has_authenticated_window', [False, True])
def test_gmail_inherited_pagination_is_rechecked_before_first_read(tmp_path, has_authenticated_window):
    learning, c, policy, _, _ = _english_trigger_fixture(tmp_path)
    card = policy['snapshot']['experiences'][0]
    card.update(trigger_pattern='', content='show_inbox_threads 使用 page_index 和 page_limit 逐页读取直到最后一页。')
    pair = 'task_gmail_inbox_complete_enumeration gmail_authenticated_state_ready'
    def query(key, instruction, state):
        return {'advice_query': {'identity': policy['snapshot']['identity'], 'target_scope': c.SCOPE,
            'query_id': key, 'decision_id': key, 'task': instruction, 'state': state},
            'execution_id': key, 'boundary': 'llm', 'actor_id': 'skill_executor'}
    task = 'Archive all read, non-starred email threads in my Gmail inbox.'
    positive = [query('docs-before-login', task, 'show_inbox_threads page_index page_limit task_gmail_inbox_complete_enumeration')]
    if has_authenticated_window:
        positive.append(query('authenticated-first-read', task, pair))
    negatives = [query('phone', 'Read every Phone text message.', 'gmail_authenticated_state_ready'),
                 query('send', 'Send an email from Gmail.', 'gmail_authenticated_state_ready'),
                 query('outbox', 'Read every Gmail outbox message.', 'gmail_authenticated_state_ready')]
    sources = learning._EnglishTriggerSources({'auth-experience': {'experience': card,
        'evidence': [{'run_id': 'gmail-run', 'query_id': row['advice_query']['query_id'], 'task': task}
                     for row in positive]}}, runtime_runs={'gmail-run': positive, 'other-run': negatives})
    before = copy.deepcopy(policy)
    if not has_authenticated_window:
        # No supported safe phase is a legitimate bounded rejection, never a
        # reason to keep the old docs-only condition.
        result, conditions = learning._compile_public_phase_base(c, policy, sources, {})
        assert conditions == {} and result['options']['experience_overrides'] == []
        assert result['options']['disabled_injections'] == [{'memory_id': card['memory_id'], 'content_revision': card['content_revision']}]
        with pytest.raises(RuntimeError, match='no_specific_supported_pair'):
            learning._specific_english_pairs(c, policy, sources, [], {},
                fixed_conditions={'auth-experience': 'page_index show_inbox_threads'}, incremental_ids=set())
        assert result['snapshot'] == before['snapshot'] and policy == before
        return
    result = learning._specific_english_pairs(c, policy, sources, [], {},
        fixed_conditions={'auth-experience': 'page_index show_inbox_threads'}, incremental_ids=set())
    assert result['snapshot'] == before['snapshot'] and policy == before
    assert result['options']['approved_experiences'] == before['options']['approved_experiences']
    assert result['options']['loop_rules'] == before['options']['loop_rules']
    rows = result['options'].get('experience_overrides', [])
    assert rows[0]['trigger_pattern'] == pair and rows[0]['content'] == card['content']
    hits = learning._native_l1_profile(c, result, sources)
    assert not hits[('gmail-run', 'docs-before-login')]
    assert hits[('gmail-run', 'authenticated-first-read')] == {'auth-experience'}
    assert all(not hits[('other-run', row['advice_query']['query_id'])] for row in negatives)


@pytest.mark.parametrize('problem', [None, 'source_changed', 'revision_changed', 'receipt_missing'])
def test_v3_paid_replay_restores_exact_frozen_snapshot_and_preserves_controls(tmp_path, problem):
    import learning
    from types import SimpleNamespace
    cards = [{'memory_id': f'm{i}', 'content_revision': f'r{i}', 'content': f'approved {i}'} for i in range(3)]
    policy = {'snapshot': {'experiences': cards, 'options': {'require_validated_evidence': True}},
              'options': {'approved_experiences': [{'memory_id': 'm0', 'content_revision': 'r0'}],
                          'loop_rules': [{'source_experience': {'memory_id': 'm0', 'content_revision': 'r0'}}],
                          'max_context_bytes': 2048}, 'policy_id': 'old'}
    frozen = {'agent': {'appworld_options': {'l1_window_mode': 'observed_phase_v2'}}, 'policy': policy}
    source = tmp_path/'published'/'source'/'strategy.json';save(source, frozen);before = source.read_bytes()
    previous = {'seed_label': 'source', 'published': True, 'english_trigger_learning': {
        'status': 'compiled', 'model_calls': 3, 'incremental_generation': {
            'source_experiences': [{'memory_id': 'm2', 'content_revision': 'r2'}],
            'batches': [{'draft': [{'memory_id': 'm2', 'trigger_pattern': 'album duration'}]}]}}}
    if problem == 'receipt_missing': previous = {}
    if problem == 'revision_changed':
        previous['english_trigger_learning']['incremental_generation']['source_experiences'][0]['content_revision'] = 'wrong'
    calls=[]
    def native(binary, root, command, packet):
        calls.append(command)
        result=copy.deepcopy(packet['policy']);result['options']=packet['options'];result['policy_id']='native-new'
        return json.dumps(result)
    c=SimpleNamespace(TRAINING=tmp_path/'training', ROOT=tmp_path, BINARY=Path('native'),
                      read=lambda p: json.loads(p.read_text()), save=save, native=native)
    artifact=copy.deepcopy(frozen);artifact['agent']['appworld_options']['l1_window_mode']='observed_phase_v3'
    if problem == 'source_changed':
        receipt={'source_artifact': {'policy': {}}, 'english_trigger_learning': previous['english_trigger_learning']}
        save(c.TRAINING/'seed-english-replay/source.json',receipt);previous={}
    if problem:
        with pytest.raises(RuntimeError, match='paid_replay'):
            learning._restore_paid_replay_source(c,source,artifact,previous)
        assert not calls
    else:
        restored, receipt=learning._restore_paid_replay_source(c,source,artifact,previous)
        assert restored['policy']['snapshot'] == policy['snapshot']
        assert len(restored['policy']['options']['approved_experiences']) == 3
        assert restored['policy']['options']['loop_rules'] == policy['options']['loop_rules']
        assert restored['policy']['options']['max_context_bytes'] == 2048
        assert receipt['paid_replay_source']['inherited_model_calls'] == 3
        assert learning._restore_paid_replay_source(c,source,artifact,{})[0] == restored
        assert calls == [['runtime','recompile']]*2
    assert source.read_bytes() == before and frozen['policy'] == policy


def test_v3_paid_replay_forbids_network_before_transport(tmp_path):
    import learning
    from types import SimpleNamespace
    c=SimpleNamespace(environment=lambda: {'APPWORLD_REPLAY_ENGLISH_DRAFTS_ONLY':'1'})
    with pytest.raises(RuntimeError,match='paid_replay_network_forbidden'):
        learning._generic_card_response(c,{},'')


def test_v3_paid_replay_revalidates_drafts_and_valid_siblings_without_model(monkeypatch,tmp_path):
    import learning
    from types import SimpleNamespace
    c=SimpleNamespace(TRAINING=tmp_path,save=save,
        environment=lambda:{'APPWORLD_REPLAY_ENGLISH_DRAFTS_ONLY':'1'})
    policy={'options':{'experience_overrides':[{'source_experience':{'memory_id':'m0'},'trigger_pattern':'existing phase'}]}}
    sources=learning._EnglishTriggerSources({'m0':{},'m1':{}},runtime_runs={'run':[{}]},
        runtime_window_evidence={('run','q'):'reconstructed_observed_phase'})
    sources.window_mode='observed_phase_v3';sources.last_profile={'after':{'matched_windows':1}}
    monkeypatch.setattr(learning,'seed_control_input',lambda *args:({}, {'scanned_tasks':120},{}))
    monkeypatch.setattr(learning,'_english_trigger_sources',lambda *args:sources)
    monkeypatch.setattr(learning,'_english_trigger_model_reply',lambda *args:pytest.fail('new model call'))
    monkeypatch.setattr(learning,'_historical_english_inheritance',lambda *args:pytest.fail('unrelated inheritance'))
    def specific(controller, original, actual, drafts, status, **kwargs):
        assert drafts == [[{'memory_id':'m1','trigger_pattern':'album duration'}]]
        assert kwargs['fixed_conditions'] == {'m0':'existing phase'}
        status.update(accepted_count=2,accepted_triggers=[{'memory_id':'m0'},{'memory_id':'m1'}])
        return original
    monkeypatch.setattr(learning,'_specific_english_pairs',specific)
    diagnostic={'english_trigger_learning':{'status':'compiled','model_calls':3,'incremental_generation':{
        'batches':[{'draft':[{'memory_id':'m1','trigger_pattern':'album duration'}]}]}}}
    assert learning.generate_english_triggers(c,policy,set(),diagnostic) == policy
    status=diagnostic['english_trigger_learning']
    assert status['status']=='compiled' and status['model_calls']==0 and status['inherited_model_calls']==3
    assert status['override_count']==2 and status['scanned_training_tasks']==120


def test_private_purchase_isolation_phase_binds_approved_contract_and_rejects_late_add_alias():
    import learning
    card = {'memory_id':'purchase-source', 'content_revision':'source-revision',
        'target_scope':'agent:fixture-agent/actor:default',
        'trigger_pattern':'只下单目标商品并保留原购物车',
        'content':'记录原购物车商品及数量，clear_cart后add_product_to_cart完整目标数量，place_order后恢复原购物车商品及数量。'}
    before = copy.deepcopy(card)
    assert learning._experience_cart_purchase_isolation(card)
    assert learning._public_phase_conditions({'experience':card}) == (
        'task_wishlist_target_purchase', 'cart_purchase_state_ready')
    assert learning._experience_task_compatible(card,
        'Place an order for all wrench sets in my amazon wish list.')
    assert not learning._experience_task_compatible(card,
        'Move all products from my Amazon wish list to my cart.')
    sources = {'purchase-source':{'experience':card}}
    with pytest.raises(ValueError, match='public_purchase_scope_missing'):
        learning._english_trigger_item({'memory_id':'purchase-source',
            'trigger_pattern':'add_product_to_cart amazon'}, sources, set(), {})
    mid, rewrite = learning._english_trigger_item({'memory_id':'purchase-source',
        'trigger_pattern':'task_wishlist_target_purchase cart_purchase_state_ready'}, sources, set(), {})
    assert mid == card['memory_id'] and rewrite['content'] == before['content'] and card == before


def _native_experience_rendering_fixture(tmp_path):
    import learning
    from types import SimpleNamespace
    actual_root = Path(training.__file__).resolve().parents[4]
    binary = actual_root / 'tools/trajectory_evolution/build-v4/trajectory_evolution'
    if not binary.is_file():
        pytest.skip('native binary unavailable')
    scope = 'agent:fixture-agent/actor:default'
    content = ('Read show_inbox_threads with page_index and page_limit. '
               'Increment page_index until an empty or short page; '
               'complete enumeration before applying the requested inbox action.')
    memory = tmp_path / 'memory.json'
    save(memory, {'schema_version': 1, 'store_kind': 'trajectory_evolution_memory',
        'store_version': 'evolution-memory-store-v1', 'revision': '1', 'entries': [{
        'memory_id': 'fixture-gmail-pagination', 'status': 'EVOLUTION_MEMORY_STATUS_CANDIDATE',
        'candidate': {'schema_version': 1, 'candidate_id': 'fixture-pagination-candidate',
            'generator_version': 'scoped-experience-generator-v1',
            'target_type': 'CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION',
            'target_scope': scope, 'status': 'EVOLUTION_CANDIDATE_STATUS_PROPOSED',
            'trigger_pattern': 'gmail_collection authenticated_inbox',
            'observed_problem': 'Reading only the first inbox page misses requested records.',
            'proposed_change': content, 'support_user_count': 1,
            'support_evidence': [{'evidence_id': 'fixture-public-evidence', 'source': {
                'user_id': 'fixture-user', 'dialogue_id': 'fixture-run'},
                'verdict': 'SEMANTIC_VERDICT_USEFUL'}]}}]})
    identity = {'agent_id': 'fixture-agent', 'user_id': 'fixture-user',
                'conversation_id': 'fixture-conversation', 'run_id': 'fixture-run'}
    shadow = {'target_scopes': [scope], 'max_policy_entries': 64,
        'max_advice_items': 2, 'max_advice_bytes': 2048, 'max_query_bytes': 32768,
        'max_memory_bytes': 16777216, 'max_snapshot_bytes': 262144,
        'max_match_terms': 128, 'min_matched_terms': 2, 'require_validated_evidence': True}
    c = SimpleNamespace(native=training.controller._policy_native, BINARY=binary, ROOT=actual_root)
    snapshot = json.loads(c.native(binary, actual_root, ['shadow', 'compile'],
        {'memory_path': str(memory), 'identity': identity, 'options': shadow}))
    assert len(snapshot['experiences']) == 1
    options = {'requested_level': 'L3', 'host_capabilities': ['context.request', 'action.filter', 'loop.decide'],
        'fallback': 'strict', 'allowed_users': ['fixture-user'],
        'approved_experiences': [{key: card[key] for key in ('memory_id', 'content_revision')}
                                for card in snapshot['experiences']],
        'experience_template': (actual_root / 'tools/trajectory_evolution/config/prompts/runtime-experience.txt').read_text(),
        'max_context_bytes': 2048, 'request_context_pointer': '/system', 'request_context_mode': 'text'}
    policy = json.loads(c.native(binary, actual_root, ['runtime', 'compile'],
        {'memory_path': str(memory), 'identity': identity, 'shadow_options': shadow, 'options': options}))
    query = {'advice_query': {'identity': identity, 'target_scope': scope,
        'query_id': 'fixture-query', 'decision_id': 'fixture-decision',
        'task': 'Complete the requested Gmail inbox collection.',
        'state': 'gmail_collection authenticated_inbox'},
        'execution_id': 'fixture-model', 'boundary': 'llm', 'actor_id': 'skill_executor'}
    return learning, c, policy, query, content


def test_appworld_template_native_recompile_preserves_all_sources_and_budgets(tmp_path):
    learning, c, policy, query, content = _native_experience_rendering_fixture(tmp_path)
    before = copy.deepcopy(policy)
    compiled = learning._compile_appworld_experience_template(c, policy)
    assert policy == before
    assert compiled['policy_id'] != policy['policy_id']
    assert {key: value for key, value in compiled.items() if key not in ('options', 'policy_id')} == {
        key: value for key, value in policy.items() if key not in ('options', 'policy_id')}
    assert {key: value for key, value in compiled['options'].items() if key != 'experience_template'} == {
        key: value for key, value in policy['options'].items() if key != 'experience_template'}
    assert compiled['options']['experience_template'].count('{{experience}}') == 1
    assert len(compiled['options']['experience_template'].encode()) <= len(policy['options']['experience_template'].encode())
    c.native = Mock(side_effect=AssertionError('unchanged or L0 policies must not recompile'))
    assert learning._compile_appworld_experience_template(c, compiled) is compiled
    l0 = {'options': {'requested_level': 'L0'}}
    assert learning._compile_appworld_experience_template(c, l0) is l0
    c.native.assert_not_called()


def test_specific_english_revalidates_disabled_source_before_native_injection(tmp_path):
    learning, c, policy, query, _ = _native_experience_rendering_fixture(tmp_path)
    card = policy['snapshot']['experiences'][0]
    c.SCOPE = card['target_scope']
    options = copy.deepcopy(policy['options'])
    options['disabled_injections'] = [{key: card[key]
        for key in ('memory_id', 'content_revision')}]
    disabled = json.loads(c.native(c.BINARY, c.ROOT, ['runtime', 'recompile'],
        {'policy': policy, 'options': options}))
    query['advice_query']['task'] = 'Archive every read, non-starred thread in my Gmail inbox.'
    query['advice_query']['state'] = 'task_gmail_inbox_complete_enumeration gmail_authenticated_state_ready'
    before = json.loads(c.native(c.BINARY, c.ROOT, ['runtime', 'query'],
        {'policy': disabled, 'query': query}))
    assert before['experiences'] == []
    sources = learning._EnglishTriggerSources({card['memory_id']: {'experience': card,
        'requires_observed_failure': False, 'evidence': [{'run_id': 'fixture-run',
            'query_id': query['advice_query']['query_id'], 'task': query['advice_query']['task']}]}},
        runtime_runs={'fixture-run': [query]})
    compiled = learning._specific_english_pairs(c, disabled, sources, [], {})
    after = json.loads(c.native(c.BINARY, c.ROOT, ['runtime', 'query'],
        {'policy': compiled, 'query': query}))
    assert after['experiences'] == [{key: card[key] for key in ('memory_id', 'content_revision')}]
    assert compiled['snapshot'] == disabled['snapshot']
    assert compiled['options']['approved_experiences'] == disabled['options']['approved_experiences']


def test_appworld_template_native_render_retains_complete_scope_and_stop_contract(tmp_path):
    learning, c, policy, query, content = _native_experience_rendering_fixture(tmp_path)
    compiled = learning._compile_appworld_experience_template(c, policy)
    old = json.loads(c.native(c.BINARY, c.ROOT, ['runtime', 'query'], {'policy': policy, 'query': query}))
    new = json.loads(c.native(c.BINARY, c.ROOT, ['runtime', 'query'], {'policy': compiled, 'query': query}))
    assert new['experiences'] == old['experiences']
    assert len(new['experiences']) == 1
    assert content in new['context']
    assert 'agent:fixture-agent/actor:default' in new['context']
    assert 'Current user requirements, permissions and budgets prevail.' in new['context']
    assert 'Ignore unsupported or mismatched records.' in new['context']
    assert len(new['context'].encode()) <= len(old['context'].encode()) <= 2048


@pytest.mark.parametrize('mutation', ['budget', 'snapshot'])
def test_appworld_template_rejects_native_source_or_budget_mutation(tmp_path, mutation):
    learning, c, policy, _, _ = _native_experience_rendering_fixture(tmp_path)
    changed = copy.deepcopy(policy)
    changed['options']['experience_template'] = learning.APPWORLD_EXPERIENCE_TEMPLATE
    if mutation == 'budget':
        changed['options']['max_context_bytes'] += 1
    else:
        changed['snapshot']['experiences'][0]['content'] = 'Altered authority'
    c.native = Mock(return_value=json.dumps(changed))
    with pytest.raises(RuntimeError, match='appworld_experience_template_changed_authoritative_policy'):
        learning._compile_appworld_experience_template(c, policy)


def _native_runtime_projection_fixture(tmp_path):
    """Compile three real native cards for the deployment projection tests.

    The projection must narrow the deployment approvals while retaining the
    native snapshot.  Building the fixture through ``shadow compile`` and
    ``runtime compile`` matters here: a hand-built policy can accidentally
    omit the native content revisions and would not exercise the source
    binding checks used by AppWorld.
    """
    import learning
    from types import SimpleNamespace

    actual_root = Path(training.__file__).resolve().parents[4]
    binary = actual_root / 'tools/trajectory_evolution/build-v4/trajectory_evolution'
    if not binary.is_file():
        pytest.skip('native binary unavailable')
    scope = 'agent:fixture-agent/actor:default'
    owner = 'fixture-user'

    def entry(memory_id, target_type, trigger, content):
        return {
            'memory_id': memory_id,
            'status': 'EVOLUTION_MEMORY_STATUS_CANDIDATE',
            'candidate': {
                'schema_version': 1,
                'candidate_id': 'candidate-' + memory_id,
                'generator_version': 'scoped-experience-generator-v1',
                'target_type': target_type,
                'target_scope': scope,
                'status': 'EVOLUTION_CANDIDATE_STATUS_PROPOSED',
                'trigger_pattern': trigger,
                'observed_problem': 'Fixture evidence is complete.',
                'proposed_change': content,
                'support_user_count': 1,
                'support_evidence': [{
                    'evidence_id': 'fixture-' + memory_id,
                    'source': {'user_id': owner, 'dialogue_id': 'fixture-run'},
                    'verdict': 'SEMANTIC_VERDICT_USEFUL',
                }],
            },
        }

    memory = tmp_path / 'memory.json'
    save(memory, {
        'schema_version': 1,
        'store_kind': 'trajectory_evolution_memory',
        'store_version': 'evolution-memory-store-v1',
        'revision': '1',
        'entries': [
            entry('fixture-accepted', 'CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION',
                  'gmail inbox collection', 'Read every inbox page before acting.'),
            entry('fixture-control', 'CANDIDATE_TARGET_TYPE_LOOP_STOP_CONDITION',
                  'AppWorld repeated failure', 'Stop after the observed failure.'),
            entry('fixture-excluded', 'CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION',
                  'unmatched card', 'This card must not be exposed.'),
        ],
    })
    identity = {
        'agent_id': 'fixture-agent', 'user_id': owner,
        'conversation_id': 'fixture-conversation', 'run_id': 'fixture-run',
    }
    shadow = {
        'target_scopes': [scope], 'max_policy_entries': 64,
        'max_advice_items': 4, 'max_advice_bytes': 4096,
        'max_query_bytes': 32768, 'max_memory_bytes': 16777216,
        'max_snapshot_bytes': 262144, 'max_match_terms': 128,
        'min_matched_terms': 1, 'require_validated_evidence': True,
    }
    c = SimpleNamespace(native=training.controller._policy_native,
                        BINARY=binary, ROOT=actual_root)
    snapshot = json.loads(c.native(binary, actual_root, ['shadow', 'compile'], {
        'memory_path': str(memory), 'identity': identity, 'options': shadow,
    }))
    assert {item['memory_id'] for item in snapshot['experiences']} == {
        'fixture-accepted', 'fixture-control', 'fixture-excluded'}
    refs = [{key: item[key] for key in ('memory_id', 'content_revision')}
            for item in snapshot['experiences']]
    control_ref = next(item for item in refs if item['memory_id'] == 'fixture-control')
    options = {
        'requested_level': 'L3',
        'host_capabilities': ['context.request', 'action.filter', 'loop.decide'],
        'fallback': 'strict', 'allowed_users': [owner],
        'approved_experiences': refs,
        'experience_template': '授权经验：\n{{experience}}',
        'max_context_bytes': 4096,
        'request_context_pointer': '/system', 'request_context_mode': 'text',
        'action_rules': [],
        'loop_rules': [{
            'rule_id': 'fixture-loop', 'target_scope': scope,
            'max_iterations': 30, 'max_no_progress': 3,
            'no_progress_decision': 'replan', 'max_replans': 1,
            'replan_context': 'Inspect the previous real tool failure and correct the request.',
            'reason': 'Bounded observed failure recovery', 'match_mode': '',
            'replan_on_failure': False, 'actor_id': '', 'error_codes': [],
            'after_action_rejection': False, 'require_finalization': False,
            'source_experience': control_ref,
        }],
        'schedule_rules': [], 'experience_overrides': [],
        'disabled_injections': [],
    }
    policy = json.loads(c.native(binary, actual_root, ['runtime', 'compile'], {
        'memory_path': str(memory), 'identity': identity,
        'shadow_options': shadow, 'options': options,
    }))
    query = {
        'advice_query': {
            'identity': identity, 'target_scope': scope,
            'query_id': 'fixture-query', 'decision_id': 'fixture-decision',
            # Both cards match this request before projection.  The excluded
            # card therefore proves that runtime query filtering is real.
            'task': 'gmail inbox collection unmatched card',
            'state': 'gmail inbox collection unmatched card',
        },
        'execution_id': 'fixture-model', 'boundary': 'llm',
        'actor_id': 'skill_executor',
    }
    return learning, c, policy, query


def _projection_diagnostic(accepted_id, revision):
    return {'english_trigger_learning': {'specific_conditions': {
        'accepted_triggers': [{'memory_id': accepted_id,
                               'content_revision': revision,
                               'trigger_pattern': 'gmail inbox collection'}],
    }}}


def test_appworld_runtime_projection_native_keeps_accepted_and_control_sources(tmp_path):
    learning, c, policy, query = _native_runtime_projection_fixture(tmp_path)
    before = copy.deepcopy(policy)
    accepted = next(item for item in policy['snapshot']['experiences']
                    if item['memory_id'] == 'fixture-accepted')
    diagnostic = _projection_diagnostic('fixture-accepted', accepted['content_revision'])

    projected = learning._prune_appworld_runtime_experiences(c, policy, diagnostic)

    assert policy == before
    assert projected['snapshot'] == before['snapshot']
    assert projected['options']['approved_experiences'] == [
        {'memory_id': 'fixture-accepted', 'content_revision': accepted['content_revision']},
        {'memory_id': 'fixture-control', 'content_revision': next(
            item['content_revision'] for item in before['snapshot']['experiences']
            if item['memory_id'] == 'fixture-control')},
    ]
    # The loop source remains deployable even though it has no accepted L1
    # trigger of its own; controls are retained by their native source card.
    assert projected['options']['loop_rules'] == before['options']['loop_rules']
    for key in ('max_context_bytes', 'experience_template', 'request_context_pointer',
                'request_context_mode', 'host_capabilities', 'fallback', 'allowed_users'):
        assert projected['options'][key] == before['options'][key]
    status = diagnostic['runtime_experience_projection']
    assert status['status'] == 'compiled'
    assert status['source_count'] == 3 and status['kept_count'] == 2
    assert status['accepted_trigger_count'] == 1 and status['control_source_count'] == 1


def test_appworld_runtime_projection_native_query_keeps_support_and_excludes_card(tmp_path):
    learning, c, policy, query = _native_runtime_projection_fixture(tmp_path)
    accepted = next(item for item in policy['snapshot']['experiences']
                    if item['memory_id'] == 'fixture-accepted')
    diagnostic = _projection_diagnostic('fixture-accepted', accepted['content_revision'])
    projected = learning._prune_appworld_runtime_experiences(c, policy, diagnostic)

    old = json.loads(c.native(c.BINARY, c.ROOT, ['runtime', 'query'],
                              {'policy': policy, 'query': query}))
    new = json.loads(c.native(c.BINARY, c.ROOT, ['runtime', 'query'],
                              {'policy': projected, 'query': query}))
    assert [item['memory_id'] for item in old['experiences']] == [
        'fixture-accepted', 'fixture-excluded']
    assert [item['memory_id'] for item in new['experiences']] == ['fixture-accepted']
    assert 'Read every inbox page before acting.' in new['context']
    assert 'This card must not be exposed.' not in new['context']


def test_appworld_runtime_projection_without_accepted_triggers_is_not_complete(tmp_path):
    learning, c, policy, _ = _native_runtime_projection_fixture(tmp_path)
    before = copy.deepcopy(policy)
    c.native = Mock(side_effect=AssertionError('no accepted trigger must not recompile'))
    diagnostic = {'english_trigger_learning': {'specific_conditions': {
        'accepted_triggers': [],
    }}}

    projected = learning._prune_appworld_runtime_experiences(c, policy, diagnostic)

    assert projected == before
    assert (diagnostic.get('runtime_experience_projection') or {}).get('status') not in {
        'compiled', 'complete', 'published',
    }
    c.native.assert_not_called()


@pytest.mark.parametrize('mutation', ['budget', 'body'])
def test_appworld_runtime_projection_rejects_native_budget_or_body_mutation(tmp_path, mutation):
    learning, c, policy, _ = _native_runtime_projection_fixture(tmp_path)
    accepted = next(item for item in policy['snapshot']['experiences']
                    if item['memory_id'] == 'fixture-accepted')
    diagnostic = _projection_diagnostic('fixture-accepted', accepted['content_revision'])
    native = c.native

    def mutate(binary, root, operation, packet):
        result = json.loads(native(binary, root, operation, packet))
        if mutation == 'budget':
            result['options']['max_context_bytes'] += 1
        else:
            result['snapshot']['experiences'][0]['content'] = 'native altered authority'
        return json.dumps(result)

    c.native = Mock(side_effect=mutate)
    with pytest.raises(RuntimeError, match='appworld_runtime_experience_projection_changed_source'):
        learning._prune_appworld_runtime_experiences(c, policy, diagnostic)


def _generic_playlist_page_card():
    return {'memory_id': 'source-playlist-pagination', 'content_revision': 'source-revision',
        'target_scope': 'agent:fixture-agent/actor:default',
        'trigger_pattern': '需要获取播放列表或已下载歌曲等分页数据时，使用page_index和page_limit循环获取所有页。',
        'content': '使用while循环，page_limit设为20，当返回结果少于page_limit时停止，确保获取全部数据。'}


def test_generic_playlist_page_phase_preserves_api_page_capacity_and_excludes_duration():
    import learning
    card = _generic_playlist_page_card()
    assert learning._experience_spotify_library_pagination(card)
    assert learning._public_phase_conditions({'experience': card}) == (
        'task_spotify_playlist_library_complete_enumeration', 'spotify_authenticated_state_ready')
    assert learning._experience_task_marker(card) == learning._public_phase_conditions({'experience': card})[0]
    assert not learning._experience_fixed_duration(card)
    duration = copy.deepcopy(card)
    duration['content'] += '只选择已下载歌曲总时长达到任务要求的播放列表。'
    assert not learning._experience_spotify_library_pagination(duration)
    assert learning._experience_downloaded_duration(duration)
    assert learning._experience_task_marker(duration) == 'task_object_playlist_downloaded_duration_playback'
    mutation = copy.deepcopy(card)
    mutation['content'] = '删除播放列表前读取page_index与page_limit。'
    assert not learning._experience_spotify_library_pagination(mutation)


def test_generic_playlist_page_phase_rejects_old_late_page_trigger():
    import learning
    card = _generic_playlist_page_card()
    sources = {card['memory_id']: {'experience': card}}
    with pytest.raises(ValueError, match='public_library_scope_missing'):
        learning._english_trigger_item({'memory_id': card['memory_id'],
            'trigger_pattern': 'task_object_playlist page_index'}, sources, set(), {})
    _, override = learning._english_trigger_item({'memory_id': card['memory_id'],
        'trigger_pattern': 'task_spotify_playlist_library_complete_enumeration spotify_authenticated_state_ready'},
        sources, set(), {})
    assert override['content'] == card['content']
    assert override['source_experience'] == {key: card[key] for key in ('memory_id', 'content_revision')}


def test_generic_playlist_page_phase_is_bound_to_current_complete_library_purpose():
    import learning
    card = _generic_playlist_page_card()
    assert learning._experience_task_compatible(card, 'Count all of my Spotify playlists across every page.')
    assert learning._experience_task_compatible(card, 'Which of my Spotify playlists has the most songs in my entire library?')
    assert not learning._experience_task_compatible(card, 'Show the first page of my Spotify playlists.')
    assert not learning._experience_task_compatible(card, 'Play the album I selected in Spotify.')
    assert not learning._experience_task_compatible(card, 'Enumerate all my Gmail inbox messages.')


def test_generic_playlist_pagination_inherited_alias_survives_complete_compiler(tmp_path):
    learning, c, policy, _, _ = _english_trigger_fixture(tmp_path)
    card = policy['snapshot']['experiences'][0]
    source = _generic_playlist_page_card()
    card.update(trigger_pattern=source['trigger_pattern'], content=source['content'])
    pair = 'task_spotify_playlist_library_complete_enumeration spotify_authenticated_state_ready'
    task = 'Find the Spotify playlists with the most songs across my entire library.'
    def query(key, instruction, state):
        return {'advice_query': {'identity': policy['snapshot']['identity'], 'target_scope': c.SCOPE,
            'query_id': key, 'decision_id': key, 'task': instruction, 'state': state},
            'execution_id': key, 'boundary': 'llm', 'actor_id': 'skill_executor'}
    positive = [query('docs-before-login', task, 'task_object_playlist page_index page_limit'),
                query('authenticated-first-read', task, pair), query('after-first-page', task, pair)]
    negatives = [query('album', 'Read every Spotify album.', 'spotify_authenticated_state_ready'),
                 query('first-page', 'Show the first page of my Spotify playlists.', 'spotify_authenticated_state_ready'),
                 query('phone', 'Read every Phone message.', 'spotify_authenticated_state_ready')]
    sources = learning._EnglishTriggerSources({'auth-experience': {'experience': card,
        'evidence': [{'run_id': 'spotify-run', 'query_id': row['advice_query']['query_id'], 'task': task}
                     for row in positive]}}, runtime_runs={'spotify-run': positive, 'other-run': negatives})
    before = copy.deepcopy(policy)
    result = learning._specific_english_pairs(c, policy, sources, [], {},
        fixed_conditions={'auth-experience': 'task_object_playlist page_index'}, incremental_ids=set())
    assert policy == before and result['snapshot'] == before['snapshot']
    assert result['options']['approved_experiences'] == before['options']['approved_experiences']
    assert result['options']['loop_rules'] == before['options']['loop_rules']
    row = next(item for item in result['options']['experience_overrides']
               if item['source_experience']['memory_id'] == card['memory_id'])
    assert row['trigger_pattern'] == pair and row['content'] == card['content']
    assert card['memory_id'] not in {ref['memory_id'] for ref in result['options']['disabled_injections']}
    hits = learning._native_l1_profile(c, result, sources)
    assert not hits[('spotify-run', 'docs-before-login')]
    assert hits[('spotify-run', 'authenticated-first-read')] == {card['memory_id']}
    assert not hits[('spotify-run', 'after-first-page')]
    assert all(not hits[('other-run', row['advice_query']['query_id'])] for row in negatives)


def _partial_lifecycle_review(seed, count=100):
    return {'status': 'partial', 'seed': str(seed.resolve()),
            'items': [{'task_id': f'task-{i}', 'execution_status': 'completed'}
                      for i in range(count)],
            'metrics': {'L0_reused': {}, 'seed_fixture': {
                'planned': count, 'completed': count, 'missing': 0,
                'execution_errors': 0, 'judge_errors': 1}},
            'review_distribution': {'complete': count - 1, 'uncertain': 1}}


def test_pending_review_lifecycle_reenters_only_same_validation_callback(tmp_path, monkeypatch):
    import reproduce
    from common_lifecycle import LifecycleController, StageResult
    from types import SimpleNamespace
    seed = tmp_path / 'published/fixture/strategy.json'
    comparison = tmp_path / 'stage100/comparison.json'
    value = _partial_lifecycle_review(seed)
    save(comparison, value)
    monkeypatch.setattr(reproduce, 'stage_path',
                        lambda current, count: comparison if count == 100 else tmp_path / 'stage32/comparison.json')
    candidate = {'seed_artifact': str(seed.resolve()), 'seed_version': 'original-version'}
    state = {'candidate': candidate, 'status': 'blocked', 'stage_results': {
        name: {'status': 'complete', 'gate_passed': True}
        for name in ('prepare', 'train', 'seed', 'pilot32')}}
    old = {'status': 'complete', 'gate_passed': False,
           'metrics': {'original': 'retained'}, 'data': {
               'callback': 'appworld.stage100', 'comparison': str(comparison),
               'gate_error': 'appworld_seed_stage100_goal_gate_not_passed'}}
    state['stage_results']['validation100'] = old
    assert reproduce._resume_pending_comparison_receipts(state, seed, candidate, 'validation100')
    assert state['stage_results']['validation100']['status'] == 'pending'
    assert state['stage_results']['validation100']['gate_passed'] is None
    assert state['stage_results']['validation100']['metrics'] == old['metrics']
    path = tmp_path / 'lifecycle.json'; save(path, state)
    calls = []
    def callback(name, context):
        calls.append(name)
        return StageResult('complete', gate_passed=True)
    adapter = SimpleNamespace(name='fixture', split_counts=lambda: {}, output_root=tmp_path)
    result = LifecycleController(adapter, callback, path).run(stop_at='validation100')
    assert calls == ['validation100']
    assert result['status'] == 'complete'
    assert json.loads(comparison.read_text()) == value


@pytest.mark.parametrize('case', ['complete_fail', 'complete_pass', 'wrong_seed', 'wrong_candidate',
                                  'wrong_path', 'duplicate_task', 'execution_error', 'missing',
                                  'no_judge_error', 'no_uncertain', 'incomplete_execution'])
def test_pending_review_lifecycle_keeps_complete_gates_and_scope_strict(tmp_path, monkeypatch, case):
    import reproduce
    seed = tmp_path / 'published/fixture/strategy.json'
    comparison = tmp_path / 'stage100/comparison.json'
    value = _partial_lifecycle_review(seed)
    candidate = {'seed_artifact': str(seed.resolve()), 'seed_version': 'original-version'}
    state = {'candidate': copy.deepcopy(candidate), 'stage_results': {'validation100': {
        'status': 'complete', 'gate_passed': False, 'data': {
            'callback': 'appworld.stage100', 'comparison': str(comparison)}}}}
    metric = value['metrics']['seed_fixture']
    if case.startswith('complete_'):
        value['status'] = 'complete'
        if case == 'complete_pass': state['stage_results']['validation100']['gate_passed'] = True
    elif case == 'wrong_seed': value['seed'] = str(tmp_path / 'other/strategy.json')
    elif case == 'wrong_candidate': state['candidate']['seed_version'] = 'other-version'
    elif case == 'wrong_path': state['stage_results']['validation100']['data']['comparison'] = str(tmp_path / 'other.json')
    elif case == 'duplicate_task': value['items'][-1]['task_id'] = value['items'][0]['task_id']
    elif case == 'execution_error': metric['execution_errors'] = 1
    elif case == 'missing': metric['missing'] = 1
    elif case == 'no_judge_error': metric['judge_errors'] = 0
    elif case == 'no_uncertain': value['review_distribution']['uncertain'] = 0
    elif case == 'incomplete_execution': value['items'][-1]['execution_status'] = 'error'
    save(comparison, value)
    monkeypatch.setattr(reproduce, 'stage_path',
                        lambda current, count: comparison if count == 100 else tmp_path / 'stage32/comparison.json')
    before = copy.deepcopy(state)
    assert not reproduce._resume_pending_comparison_receipts(state, seed, candidate, 'validation100')
    assert state == before


@pytest.fixture
def candidate_replay(tmp_path):
    from types import SimpleNamespace
    import learning
    refs = [{'memory_id': value, 'content_revision': value + '-revision'}
            for value in ('auth-source', 'recovery-source', 'phase-source')]
    policy = {'options': {'approved_experiences': refs}}
    artifact = {'level': 'L3', 'policy': policy, 'agent': {'appworld_options': {
        'l1_window_mode': 'observed_phase_v3',
        'l1_observed_phase_experiences': [refs[2]],
        'l1_auth_parameter_experiences': [refs[0]],
        'l1_auth_structured_missing': True}}}
    docs = tmp_path / 'world/data/api_docs/standard/amazon.json'
    save(docs, {'move_product_from_cart_to_wish_list': {'app_name': 'amazon',
        'api_name': 'move_product_from_cart_to_wish_list',
        'parameters': [{'name': 'access_token', 'required': True}]}})
    controller = SimpleNamespace(ROOT=ROOT, WORLD=tmp_path/'world', read=lambda path: json.loads(path.read_text()))
    options = learning._candidate_l1_replay_options(controller, policy, artifact)
    def replay(code, observation, earlier=None, round_id='framework:2:skill_executor:office:1'):
        messages = list(earlier or []) + [{'role': 'assistant', 'content': code},
                                          {'role': 'user', 'content': observation}]
        return learning._candidate_l1_replay_packet(controller, options,
            {'system': 'Public instruction.', 'messages': messages, 'turn': 2},
            'Move the selected Amazon product.', round_id, 'actual-source-run', 'L3')
    return controller, policy, artifact, options, refs, replay


@pytest.mark.parametrize('observation', [
    'Execution failed: 401 Unauthorized in apis.amazon.move_product_from_cart_to_wish_list(',
    '{"status":"rejected","error_code":"appworld_missing_access_token","message":"The host blocked a protected call without access_token. Reuse the token variable returned by the successful login; do not login again."}',
])
def test_candidate_replay_uses_real_auth_qualification(candidate_replay, observation):
    _, _, _, _, refs, replay = candidate_replay
    packet = replay('apis.amazon.move_product_from_cart_to_wish_list(product_id=p, quantity=q)', observation)
    assert packet['apply_policy'] is True
    assert packet['extensions']['runtime_experience_allowlist'] == [refs[0]]
    assert 'requires access_token' in packet['state']['causal_auth_parameter_failure']


@pytest.mark.parametrize('earlier', [None, [
    {'role':'assistant','content':'apis.amazon.move_product_from_cart_to_wish_list(product_id=p)'},
    {'role':'user','content':'401 Unauthorized'}]])
def test_candidate_replay_real_422_uses_host_fallback_not_stale_auth(candidate_replay, earlier):
    _, _, _, _, refs, replay = candidate_replay
    packet = replay('apis.amazon.move_product_from_cart_to_wish_list(access_token=t, product_id=p, quantity=q)',
                    "Execution failed: 422 The product is not available in the cart.", earlier)
    assert packet['apply_policy'] is True
    assert packet['extensions']['runtime_experience_allowlist'] == refs[1:]
    assert '422' in packet['state']['observation']


def test_candidate_replay_success_keeps_observed_phase_precedence(candidate_replay):
    _, _, _, _, refs, replay = candidate_replay
    packet = replay('cart = apis.amazon.show_cart(access_token=t)', 'Execution successful.')
    assert packet['apply_policy'] is True
    assert packet['extensions']['runtime_experience_allowlist'] == [refs[2]]
    assert '422' not in packet['state'].get('observation','')


def test_candidate_replay_documentation_and_first_round_do_not_create_failure(candidate_replay):
    _, _, _, _, _, replay = candidate_replay
    packet = replay("print(apis.api_docs.show_api_doc(app_name='amazon', api_name='login'))",
                    'Public schema describes 401 and 422 status codes.')
    # The observed successful docs may have a success window, but never authorize auth advice.
    assert not packet['apply_policy'] or packet['extensions'].get('runtime_experience_allowlist') != [{'memory_id':'auth-source','content_revision':'auth-source-revision'}]
    assert replay('apis.amazon.show_cart()', '401 Unauthorized', round_id='framework:0') is None


def test_candidate_replay_missing_option_does_not_manually_exclude_auth(candidate_replay):
    import learning
    controller, policy, artifact, _, _, _ = candidate_replay
    artifact = copy.deepcopy(artifact);artifact['agent']['appworld_options'].pop('l1_auth_parameter_experiences')
    artifact['agent']['appworld_options'].pop('l1_auth_structured_missing')
    options = learning._candidate_l1_replay_options(controller,policy,artifact)
    packet = learning._candidate_l1_replay_packet(controller,options,
        {'system':'Public policy.','messages':[{'role':'assistant','content':'apis.amazon.show_cart(access_token=t)'},
        {'role':'user','content':'422 Validation failed'}],'turn':2},'Inspect my cart.',
        'framework:2:skill_executor:office:1','source-run','L3')
    assert packet['apply_policy'] is True
    assert 'runtime_experience_allowlist' not in packet['extensions']


@pytest.mark.parametrize('problem',['revision','unapproved','duplicate','policy'])
def test_candidate_replay_rejects_invalid_artifact_sources(candidate_replay,problem):
    import learning
    controller, policy, artifact, _, refs, _ = candidate_replay
    artifact=copy.deepcopy(artifact)
    if problem=='revision':artifact['agent']['appworld_options']['l1_auth_parameter_experiences'][0]['content_revision']='wrong'
    elif problem=='unapproved':artifact['agent']['appworld_options']['l1_auth_parameter_experiences'][0]['memory_id']='other'
    elif problem=='duplicate':artifact['agent']['appworld_options']['l1_auth_parameter_experiences'].append(refs[0])
    else:artifact['policy']={'options':{'approved_experiences':[]}}
    with pytest.raises((ValueError,RuntimeError)):
        learning._candidate_l1_replay_options(controller,policy,artifact)


def test_candidate_replay_allowlist_refs_are_not_lexical_anchors():
    import learning
    query={'advice_query':{'state':json.dumps({'retrieval_state':'Actual 422 operation.',
        'runtime_experience_allowlist':[{'memory_id':'invented-domain-anchor','content_revision':'secret-revision'}]})}}
    assert learning._l1_query_state(query)=='Actual 422 operation.'
    assert learning._l1_terms(learning._l1_query_state(query))==learning._l1_terms('Actual 422 operation.')


@pytest.mark.parametrize('completed,expected', [(True, [1, 2, 3, 3]), (False, [1, 1, 2, 3])])
def test_candidate_replay_counts_actual_cards_and_cache_before_fourth_window(candidate_replay, completed, expected):
    import learning
    from types import SimpleNamespace
    controller, policy, _, options, refs, _ = candidate_replay
    queries = [{'advice_query': {'query_id': str(index)}} for index in range(4)]
    decisions = [{'experiences': [refs[0]], 'context': 'A bounded verified native card.'},
        {'experiences': [], 'context': ''},
        {'experiences': [refs[1]], 'context': 'A second bounded native card.'},
        {'experiences': [refs[2]], 'context': 'A fourth-window native card.'}]
    calls = []
    def native(binary, root, operation, value):
        calls.append(value['queries'])
        return json.dumps(decisions)
    controller.native = native; controller.BINARY = 'fixture'; controller.SCOPE = 'fixture-scope'
    sources = learning._EnglishTriggerSources(runtime_runs={'actual-source-run': queries})
    sources.candidate_replay_options = options
    for index in range(4):
        sources.runtime_replay_inputs[('actual-source-run',str(index))] = {
            'causal': {'system': 'Public policy.', 'turn': index + 1, 'messages': [
                {'role': 'assistant', 'content': 'apis.amazon.move_product_from_cart_to_wish_list(access_token=t, product_id=p)'},
                {'role': 'user', 'content': 'Execution failed: 422 The product is not available in the cart.'}]},
            'instruction': 'Move the selected product.', 'execution_id': f'framework:{index+1}:office:1',
            'level': 'L3', 'messages': [], 'completed_model': completed}
    hits = learning._native_l1_profile(controller,policy,sources)
    assert calls == [queries]  # Native once state spans the complete original run.
    assert [row['count_after'] for row in sources.runtime_replay_profile['actual-source-run']] == expected
    assert hits[('actual-source-run','3')] == (set() if completed else {'phase-source'})
    assert sources.runtime_replay_profile['actual-source-run'][1]['cached_application'] is completed


def test_candidate_replay_does_not_invent_main_replan_context(candidate_replay):
    import learning
    controller, _, _, options, _, _ = candidate_replay
    packet = learning._candidate_l1_replay_packet(controller,options,
        {'system':'Public policy.','messages':[],'turn':3},'Public task.',
        'framework:3:office:1','actual-source-run','L3')
    assert packet['apply_policy'] is False
    assert 'control_id' not in packet['context']


@pytest.mark.parametrize('problem', [None, 'old_content', 'unadmitted', 'duplicate', 'already_deployed'])
def test_incremental_training_recipe_preserves_parent_and_requires_native_admission(tmp_path, monkeypatch, problem):
    import learning
    from types import SimpleNamespace
    old = {'memory_id': 'old', 'content_revision': 'old-revision', 'content': 'Original lesson.',
           'trigger_pattern': 'original method', 'target_scope': 'scope', 'match_terms': ['original','method']}
    new = dict(old, memory_id='new', content_revision='new-revision', content='Actual admitted TRAIN lesson.')
    parent = {'policy': {'snapshot': {'identity': {'run_id': 'snapshot'}, 'options': {'require_validated_evidence':True}, 'experiences':[old]},
        'options': {'approved_experiences': [{'memory_id':'old','content_revision':'old-revision'}],
                    'loop_rules':[{'source_experience':{'memory_id':'old','content_revision':'old-revision'}}]}},
        'agent': {'appworld_options': {'l1_auth_parameter_experiences':[{'memory_id':'old','content_revision':'old-revision'}]}}}
    current = dict(old,content_revision='updated-approved-revision')
    if problem == 'old_content': current['content'] = 'Changed lesson.'
    cards = [current] + ([] if problem=='unadmitted' else [new])
    snapshot = {'identity':{'run_id':'snapshot'},'options':{},'experiences':cards}
    monkeypatch.setattr(learning,'_readiness',lambda c:tmp_path/'memory.json')
    monkeypatch.setattr(learning,'_compile_seed_snapshot',lambda *args:copy.deepcopy(snapshot))
    def native(binary,root,operation,value):
        assert operation == ['runtime','compile']
        return json.dumps({'snapshot':snapshot,'options':value['options']})
    controller=SimpleNamespace(native=native,BINARY='fixture',ROOT=tmp_path)
    ids=['new','new'] if problem=='duplicate' else ['old'] if problem=='already_deployed' else ['new']
    before=copy.deepcopy(parent)
    if problem:
        with pytest.raises((ValueError,RuntimeError)):
            learning._incremental_training_artifact(controller,parent,ids)
    else:
        artifact,_=learning._incremental_training_artifact(controller,parent,ids)
        assert artifact['policy']['options']['approved_experiences']==[
            {'memory_id':'old','content_revision':'updated-approved-revision'},
            {'memory_id':'new','content_revision':'new-revision'}]
        assert artifact['policy']['options']['loop_rules'][0]['source_experience']['content_revision']=='updated-approved-revision'
        assert artifact['agent']['appworld_options']['l1_auth_parameter_experiences'][0]['content_revision']=='updated-approved-revision'
    assert parent == before


@pytest.mark.parametrize('saved_ids', [[], ['old'], ['new', 'new'], ['unadmitted']])
def test_incremental_saved_result_recipe_rejects_invalid_or_inherited_refs(saved_ids):
    import learning
    parent = {'policy': {'snapshot': {'options': {'require_validated_evidence': True},
                                     'experiences': []}, 'options': {'approved_experiences': []}}}
    with pytest.raises(ValueError, match='incremental_saved_result_sources_invalid'):
        learning._incremental_training_artifact(None, parent, ['new'], saved_result_ids=saved_ids)


def test_candidate_saved_result_phase_requires_explicit_validated_option():
    import learning
    source = {'experience': {'content': 'Save an API result and print the variable later.',
                              'trigger_pattern': 'Show saved results.'}}
    assert learning._public_phase_conditions(source) is None
    source['saved_result_qualification'] = True
    assert learning._public_phase_conditions(source) == ('saved_public_collection_result',
                                                         'undisplayed_api_assignment')


def test_source_complete_api_trigger_uses_only_exact_approved_body():
    from copy import deepcopy
    import learning
    import pytest
    experience = {'memory_id': 'schema-source', 'content_revision': 'source-r1',
                  'content': 'Use apis.simple_note.search_notes or apis.phone.search_text_messages with query and page_limit.'}
    original = {'source_experience': {key: experience[key] for key in ('memory_id', 'content_revision')},
                'content': experience['content'], 'trigger_pattern': 'page_limit search_text_messages'}
    saved = deepcopy(original)
    changed, proof = learning._source_complete_api_trigger_override(original, experience)
    assert original == saved
    assert changed['trigger_pattern'] == 'page_limit search_text_messages search_notes'
    assert changed['content'] == experience['content']
    assert changed['source_experience'] == original['source_experience']
    assert proof['added_terms'] == ['search_notes'] and proof['model_calls'] == 0
    again, proof = learning._source_complete_api_trigger_override(changed, experience)
    assert again == changed and proof['added_terms'] == []
    wrong = deepcopy(original); wrong['source_experience']['content_revision'] = 'wrong'
    with pytest.raises(ValueError, match='source_mismatch'):
        learning._source_complete_api_trigger_override(wrong, experience)
    wrong = deepcopy(original); wrong['content'] += ' invented API'
    with pytest.raises(ValueError, match='source_mismatch'):
        learning._source_complete_api_trigger_override(wrong, experience)


def test_auth_phase_derive_uses_approved_train_sources_and_preserves_policy(tmp_path):
    from types import SimpleNamespace
    import learning
    token={'memory_id':'token','content_revision':'token-r'}
    login={'memory_id':'full-login','content_revision':'login-r'}
    wrong={'memory_id':'other-app','content_revision':'other-r'}
    scope='agent:fixture/actor:default'
    cards=[{**token,'target_scope':scope,'content':'Pass access_token to later API calls.'},
           {**login,'target_scope':scope,'content':'Read show_account_passwords; call the app login; save access_token.'},
           {**wrong,'target_scope':scope,'content':'Read show_account_passwords; call apis.spotify.login and retain access_token.'}]
    policy={'snapshot':{'options':{'require_validated_evidence':True},'experiences':cards},
            'options':{'approved_experiences':[token,login,wrong],
                       'experience_overrides':[{'source_experience':r,'trigger_pattern':'authentication_needed login'} for r in [login,wrong]],
                       'max_context_bytes':2048,'loop_rules':[{'max_replans':1}]}}
    parent={'level':'L3','agent':{'appworld_options':{'l1_auth_parameter_experiences':[token]}},'policy':policy}
    memory={'entries':[{'memory_id':r['memory_id'],'candidate':{'target_scope':scope,
                 'support_evidence':[{'evidence_id':'existing-evidence','source':{'user_id':'training-owner',
                  'conversation_id':'real-source','dialogue_id':'real-training-run'},'verdict':'SEMANTIC_VERDICT_USEFUL'}]}}
                     for r in [login,wrong]]}
    c=SimpleNamespace(ROOT=ROOT,TRAINING=tmp_path,OWNER='training-owner',read=lambda p:copy.deepcopy(memory))
    before=copy.deepcopy(parent);derived,diag=learning._derive_auth_phase_artifact(c,parent)
    assert parent==before and derived['policy']==policy
    assert derived['agent']['appworld_options']['l1_auth_parameter_experiences']==[
        {**token,'phase':'parameters'},{**login,'phase':'login'}]
    assert diag['model_calls']==0 and diag['experience_text_and_english_unchanged']
    memory['entries'][0]['candidate']['support_evidence'][0]['source']['user_id']='not-training'
    with pytest.raises(RuntimeError,match='login_source_missing'):learning._derive_auth_phase_artifact(c,parent)


def test_auth_phase_source_schema_normal_recipe_preserves_bodies_and_controls(tmp_path):
    from types import SimpleNamespace
    import learning
    scope='agent:fixture/actor:default'
    token={'memory_id':'token','content_revision':'token-r'}
    login={'memory_id':'full-login','content_revision':'login-r'}
    schema={'memory_id':'schema','content_revision':'schema-r'}
    narrower={'memory_id':'quantity','content_revision':'quantity-r'}
    cards=[{**token,'target_scope':scope,'content':'Pass access_token to later API calls.'},
           {**login,'target_scope':scope,'content':'Read show_account_passwords; call the app login; save access_token.'},
           {**schema,'target_scope':scope,'content':'Use apis.simple_note.search_notes or apis.phone.search_text_messages with query and page_limit.'},
           {**narrower,'target_scope':scope,'content':'Use apis.amazon.move_product_from_cart_to_wish_list with product_id and quantity.'}]
    overrides=[{'source_experience':ref,'content':card['content'],'trigger_pattern':trigger}
               for ref,card,trigger in [(login,cards[1],'authentication_needed login'),
                                        (schema,cards[2],'page_limit search_text_messages'),
                                        (narrower,cards[3],'cart_quantity quantity')]]
    policy={'snapshot':{'options':{'require_validated_evidence':True},'experiences':cards},
            'options':{'approved_experiences':[token,login,schema,narrower],
                       'experience_overrides':overrides,'max_context_bytes':2048,
                       'action_rules':[{'max_failed_attempts':2}],'loop_rules':[{'max_replans':1}]}}
    parent={'level':'L3','agent':{'appworld_options':{'l1_auth_parameter_experiences':[token]}},'policy':policy}
    memory={'entries':[{'memory_id':ref['memory_id'],'candidate':{'target_scope':scope,'support_evidence':[
        {'evidence_id':'existing-'+ref['memory_id'],'source':{'user_id':'training-owner',
         'conversation_id':'real-source','dialogue_id':'real-run-'+ref['memory_id']},'verdict':'SEMANTIC_VERDICT_USEFUL'}]}}
         for ref in [login,schema,narrower]]}
    def native(binary,root,args,payload):
        assert args==['runtime','recompile']
        result=copy.deepcopy(payload['policy']);result['options']=copy.deepcopy(payload['options'])
        return json.dumps(result)
    controller=SimpleNamespace(ROOT=ROOT,BINARY='no-model',TRAINING=tmp_path,OWNER='training-owner',
                               read=lambda p:copy.deepcopy(memory),native=native)
    before=copy.deepcopy(parent)
    derived,diagnostic=learning._derive_auth_phase_artifact(controller,parent,parameter_schema=True)
    assert parent==before and derived['policy']['snapshot']==policy['snapshot']
    for key in ['approved_experiences','action_rules','loop_rules','max_context_bytes']:
        assert derived['policy']['options'][key]==policy['options'][key]
    refs=derived['agent']['appworld_options']['l1_auth_parameter_experiences']
    assert len(refs)==3 and all(ref['login_evidence']=='completed_prefix' for ref in refs)
    assert refs[-1]=={**schema,'phase':'parameters','parameter_evidence':'source_schema','login_evidence':'completed_prefix'}
    assert derived['policy']['options']['experience_overrides'][1]['trigger_pattern']=='page_limit search_text_messages search_notes'
    assert diagnostic['source_complete_trigger']['added_terms']==['search_notes']
    assert diagnostic['experience_text_unchanged'] and not diagnostic['experience_text_and_english_unchanged']
    assert diagnostic['model_calls']==0
    for entry in memory['entries'][1:]:entry['candidate']['support_evidence'][0]['source']['user_id']='not-training'
    with pytest.raises(RuntimeError,match='schema_source_missing'):
        learning._derive_auth_phase_artifact(controller,parent,parameter_schema=True)
