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
    def generated_english(controller, policy, tasks, diagnostic, **kwargs):
        assert kwargs == {'prevention_compilation': True}
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
    import learning, reproduce
    published = tmp_path / 'published'
    private = reproduce._APPWORLD_LAYOUT
    monkeypatch.setattr(reproduce, 'OUTPUT', tmp_path)
    for name, path in {'PUBLISHED': published, 'GEPA': tmp_path / 'gepa',
                       'TRAINING': tmp_path / 'training',
                       'RUNTIME': tmp_path / 'training/runtime',
                       'TRAINING_SERVICE': tmp_path / 'training/runtime/training-service.json'}.items():
        monkeypatch.setattr(private, name, path)
    # main uses a separately loaded layout and the real shared controller.
    # Its completed TRAIN prefix is fixture evidence, never production state.
    monkeypatch.setattr(reproduce._AppWorldDatasetAdapter, 'output_root', tmp_path)
    monkeypatch.setattr(reproduce._AppWorldDatasetAdapter, 'split_counts',
                        staticmethod(lambda: {'train': 134, 'val': 12, 'holdout': 500}))
    monkeypatch.setattr(learning, '_completed_training_tasks',
                        lambda controller: {f'fixture-train-{i}' for i in range(134)})
    def fixture_save(path, value):
        path = Path(path).resolve()
        assert path.is_relative_to(tmp_path.resolve()), 'CLI fixture attempted production write'
        save(path, value)
    monkeypatch.setattr(private, 'save', fixture_save)
    prepare = Mock(side_effect=AssertionError('completed fixture prefix must be reused'))
    train = Mock(side_effect=AssertionError('completed fixture TRAIN must be reused'))
    monkeypatch.setattr(reproduce, 'prepare', prepare)
    monkeypatch.setattr(reproduce, 'train_with_recovery', train)
    save(tmp_path / 'gepa/lifecycle.json', {'stage_results': {
        'prepare': {'status': 'complete'}, 'train': {'status': 'complete'}}})
    artifact = {'level': 'L3', 'policy': {'real-training': 'fixture'}}
    calls = []
    def freeze(controller, *, label):
        assert controller is private
        calls.append(label)
        fixture_save(controller.PUBLISHED / label / 'strategy.json', artifact)
        return artifact
    monkeypatch.setattr(learning, 'freeze_seed', freeze)
    candidate = published / 'candidate-next/strategy.json'
    monkeypatch.setattr(sys, 'argv', ['reproduce.py', '--seed', '--seed-artifact', str(candidate)])
    assert reproduce.main() == 0
    assert calls == ['candidate-next']
    assert reproduce.status(candidate)['seed'] == {'status': 'ready', 'artifact': str(candidate),
                                                   'label': 'candidate-next'}
    other = published / 'candidate-other/strategy.json'
    monkeypatch.setattr(sys, 'argv', ['reproduce.py', '--seed', '--seed-label', 'candidate-other'])
    assert reproduce.main() == 0
    assert calls == ['candidate-next', 'candidate-other']
    assert reproduce.status(other)['seed']['label'] == 'candidate-other'
    stage = Mock()
    monkeypatch.setattr(reproduce, 'stage', stage)
    monkeypatch.setattr(reproduce, 'verify', Mock(return_value=0))
    monkeypatch.setattr(sys, 'argv', ['reproduce.py', '--pilot32', '--seed-label', 'candidate-other'])
    assert reproduce.main() == 0
    stage.assert_called_once_with(other, 32)
    lifecycle = json.loads((tmp_path / 'gepa/lifecycle.json').read_text())
    assert lifecycle['candidate']['seed_artifact'] == str(other)
    assert lifecycle['stage_results']['pilot32']['status'] == 'complete'
    assert reproduce._USING_FALLBACK_LIFECYCLE is False
    prepare.assert_not_called()
    train.assert_not_called()
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
    assert 'Multiple learned rules in one scope/actor require explicit predicate mode' in request['messages'][0]['content']
    assert 'disjoint error_codes, replan_on_failure=true and max_replans=1' in request['messages'][0]['content']
    assert 'identical iteration/no-progress budgets' in request['messages'][0]['content']
    assert 'runtime still shares one replan across all those mechanisms' in request['messages'][0]['content']
    # This fixture has not proved native disjoint capability; keep its legacy fallback.
    assert 'No same-scope disjoint-rule compilation proof is available' in request['messages'][0]['content']
    assert 'at most one learned loop per scope/actor' in request['messages'][0]['content']
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
        read=lambda path: json.loads(Path(path).read_text()), save=save, environment=lambda: {})
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
    if unrelated_previous_budget:
        with pytest.raises(ValueError, match='incremental_source_changed'):
            learning.generate_english_triggers(c, policy, set(), diagnostic)
        assert policy == before and not submitted
        assert diagnostic['english_trigger_learning']['model_calls'] == 3
        return
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
        if recovery == 'interrupted_after_response' and drafts and sum(bool(rows) for rows in native_calls) == 1:
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
    positive = query('positive', task, marker + ' spotify_downloaded_subset_state_ready show_playlist_library')
    negative = query('negative', 'Count artists in my playlists.', 'task_object_playlist show_playlist_library')
    sources = learning._EnglishTriggerSources({'auth-experience': {'experience': card,
        'window_mode':'observed_phase_v3',
        'evidence': [{'run_id': 'positive-run', 'query_id': 'positive', 'task': task}]}},
        runtime_runs={'positive-run': [positive], 'negative-run': [negative]})
    sources.runtime_codes = {('positive-run', 'positive'): 'apis.spotify.show_playlist_library()',
                             ('negative-run', 'negative'): 'apis.spotify.show_playlist_library()'}
    before = copy.deepcopy(policy)
    result = learning._specific_english_pairs(c, policy, sources, [], {},
        fixed_conditions={'auth-experience': 'task_object_playlist show_playlist_library'}, incremental_ids=set())
    rewritten = result['options']['experience_overrides'][0]
    assert marker in rewritten['trigger_pattern'] and 'spotify_downloaded_subset_state_ready' in rewritten['trigger_pattern']
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


def test_private_purchase_isolation_phase_binds_approved_wishlist_scope():
    import learning
    card = {'memory_id':'purchase-source', 'content_revision':'source-revision',
        'target_scope':'agent:fixture-agent/actor:default',
        'trigger_pattern':'只下单愿望清单中的目标商品并保留原购物车',
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


def test_general_purchase_source_does_not_inherit_conditional_wishlist_scope():
    import learning
    card = {'memory_id':'purchase-source', 'content_revision':'source-r',
        'trigger_pattern':'执行Amazon购物车下单任务，仅下单目标商品并保留原购物车',
        'observed_problem':'源训练题没有移除愿望清单中的已购买商品。',
        'content':'记录原购物车数量，clear_cart后add_product_to_cart完整目标数量，place_order后恢复原购物车；若任务要求，delete_product_from_wish_list。'}
    before = copy.deepcopy(card)
    assert learning._experience_cart_purchase_isolation(card)
    assert not learning._experience_wishlist_purchase_only(card)
    assert learning._public_phase_conditions({'experience':card}) is None
    assert learning._experience_task_compatible(card, 'Buy the listed products from Amazon.')
    assert not learning._experience_task_compatible(card, 'Move my Amazon cart products to the wish list.')
    _, rewrite = learning._english_trigger_item({'memory_id':card['memory_id'],
        'trigger_pattern':'add_product_to_cart amazon'}, {card['memory_id']:{'experience':card}}, set(), {})
    assert rewrite['content'] == before['content'] and card == before


def test_parameter_guidance_requires_approved_token_parameter_and_schema_sources():
    import learning
    token = {'memory_id':'token', 'content_revision':'r1', 'content':'将token作为access_token参数传给后续API。'}
    schema = {'memory_id':'docs', 'content_revision':'r2', 'content':'先读取show_api_doc返回的参数契约，再构造调用。'}
    policy = {'snapshot':{'experiences':[token,schema]}, 'options':{'approved_experiences':[
        {k:card[k] for k in ('memory_id','content_revision')} for card in [token,schema]]}}
    before = copy.deepcopy(policy)
    guide = learning._seed_parameter_guidance(policy)
    assert 'exact access_token keyword' in guide and 'separate call' in guide
    assert 'changed call' in guide and policy == before
    policy['options']['approved_experiences'].pop()
    assert learning._seed_parameter_guidance(policy) == ''
    policy['options']['approved_experiences'].append({'memory_id':'docs','content_revision':'wrong'})
    assert learning._seed_parameter_guidance(policy) == ''


@pytest.mark.parametrize('spent', [1, 2])
def test_partial_control_budget_resume_uses_remaining_calls_only(tmp_path, monkeypatch, spent):
    learning, c, rows, policy, _, _ = _fallback_fixture(tmp_path)
    attempts = [{'attempt':i+1,'status':'format_invalid'} for i in range(spent)]
    diagnostic = {'control_learning':{'model_calls':spent,'model_attempts':spent,'attempts':attempts}}
    seen=[]
    def reply(controller, packet, reason, transport):
        seen.append(transport['model_calls'])
        transport['model_calls'] += 1
        raise ValueError('invalid complete draft')
    monkeypatch.setattr(learning, '_control_model_reply', reply)
    result = learning.generate_seed_controls(c, policy, {row['task_id'] for row in rows}, diagnostic)
    status = diagnostic['control_learning']
    assert seen == list(range(spent,3)) and status['model_calls'] == status['model_attempts'] == 3
    assert status['attempts'][:spent] == attempts and status['evidence_fallback']['model_calls'] == 0
    assert result['snapshot'] == policy['snapshot']


def test_healthy_control_draft_is_natively_recompiled_after_english_interruption(tmp_path, monkeypatch):
    learning,c,rows,policy,feedback,_=_fallback_fixture(tmp_path)
    draft,_=learning._evidence_bounded_control_draft(c,policy,feedback)
    original_input=learning.seed_control_input
    def complete_mechanism_input(*args):
        packet,current_feedback,controls=original_input(*args)
        # Isolate the transport checkpoint: no extra aggregate mechanism is
        # requested. The cited native source/queries remain exact and real.
        packet['training_statistics']['failure_mechanisms']={}
        return packet,current_feedback,controls
    monkeypatch.setattr(learning,'seed_control_input',complete_mechanism_input)
    def reply(controller,packet,reason,transport):
        transport['model_calls']+=1
        return copy.deepcopy(draft),{}
    model=Mock(side_effect=reply);monkeypatch.setattr(learning,'_control_model_reply',model)
    diagnostic={}
    first=learning.generate_seed_controls(c,policy,{row['task_id'] for row in rows},diagnostic)
    assert diagnostic['control_learning']['model_calls']==1
    assert diagnostic['control_learning']['validated_model_draft']==draft
    paid_attempts=copy.deepcopy(diagnostic['control_learning']['attempts'])
    # The English phase's interrupted transport consumes its own paid call;
    # resuming the normal candidate must not regenerate healthy controls.
    diagnostic['english_trigger_learning']={'model_calls':1,'status':'failed'}
    model.side_effect=AssertionError('healthy control model draft must not be bought again')
    second=learning.generate_seed_controls(c,policy,{row['task_id'] for row in rows},diagnostic)
    assert second==first and model.call_count==1
    assert diagnostic['control_learning']['model_calls']==1
    assert diagnostic['control_learning']['attempts']==paid_attempts
    assert diagnostic['control_learning']['draft_reuse']=='native_recompiled'
    assert diagnostic['english_trigger_learning']['model_calls']==1


@pytest.mark.parametrize('spent',[1,2])
def test_partial_english_transport_budget_is_kept_without_a_saved_draft(tmp_path,monkeypatch,spent):
    learning,c,policy,feedback,_=_english_trigger_fixture(tmp_path)
    monkeypatch.setattr(learning,'seed_control_input',lambda *args:({'memories':[{'memory_id':'auth-experience'}]},feedback,{}))
    monkeypatch.setattr(learning,'_historical_english_inheritance',lambda *args:None)
    seen=[]
    def reply(controller,packet,reason,transport):
        seen.append(transport['model_calls']);transport['model_calls']+=1
        return {'triggers':[{'memory_id':'auth-experience','trigger_pattern':'login access_token'}]},{}
    monkeypatch.setattr(learning,'_english_trigger_model_reply',reply)
    diagnostic={'english_trigger_learning':{'model_calls':spent,'attempts':[],'status':'failed'}}
    result=learning.generate_english_triggers(c,policy,set(),diagnostic)
    assert seen==[spent] and diagnostic['english_trigger_learning']['model_calls']==spent+1
    assert result['snapshot']==policy['snapshot']


@pytest.mark.parametrize('executed_order', [False, True])
def test_seed_prevention_marker_requires_actual_native_pre_purchase_application(tmp_path, monkeypatch, executed_order):
    import learning
    content = ('下单前记录原购物车数量，clear_cart，再add_product_to_cart目标完整数量，'
               'place_order后恢复原购物车；若任务要求，delete_product_from_wish_list。')
    learning, c, policy, query, _ = _native_experience_rendering_fixture(tmp_path,
        content=content, trigger='执行Amazon购物车下单，place_order会购买整个购物车，须只下单目标并保留原购物车')
    card = policy['snapshot']['experiences'][0]; ref = {k:card[k] for k in ('memory_id','content_revision')}
    options = copy.deepcopy(policy['options'])
    options['experience_overrides'] = [{'source_experience':ref,'content':content,
                                       'trigger_pattern':'add_product_to_cart amazon'}]
    policy = json.loads(c.native(c.BINARY,c.ROOT,['runtime','recompile'],{'policy':policy,'options':options}))
    artifact = {'level':'L3','agent':{'appworld_options':{}},'policy':policy}
    before = copy.deepcopy(artifact)
    instruction='Buy the listed products from Amazon.'
    code='apis.amazon.add_product_to_cart(product_id=1, quantity=2, access_token=saved_token)'
    if executed_order:
        code += '\napis.amazon.place_order(address_id=1, payment_card_id=1, access_token=saved_token)'
    messages=[{'role':'assistant','content':code}, {'role':'user','content':'{"message":"success"}'}]
    causal={'system':'Generate Python code for the current task.', 'messages':messages,'turn':2}
    c.SCOPE=card['target_scope']
    monkeypatch.setattr(learning,'seed_control_input',lambda *args:({}, {}, {}))
    def sources_for_candidate(controller, current, feedback, *, candidate_artifact):
        assert candidate_artifact['policy']==current
        replay_options=learning._candidate_l1_replay_options(c,current,candidate_artifact)
        assert replay_options['appworld_prevention_sources'][0]['ref']==ref
        packet=learning._candidate_l1_replay_packet(c,replay_options,causal,instruction,
            'fixture-model:2','fixture-run','L3',messages=messages)
        assert packet['apply_policy'] is True
        q=copy.deepcopy(query);q['execution_id']='fixture-model:2'
        q['advice_query'].update(task=instruction,state=json.dumps({'retrieval_state':json.dumps(packet['state']),
            'runtime_experience_allowlist':packet['extensions']['runtime_experience_allowlist']}))
        point={'run_id':'fixture-run','query_id':q['advice_query']['query_id'],'task':instruction}
        key=(point['run_id'],point['query_id'])
        sources=learning._EnglishTriggerSources({card['memory_id']:{'experience':card,'evidence':[point]}},
            runtime_runs={'fixture-run':[q]})
        sources.candidate_replay_options=replay_options
        sources.runtime_replay_inputs={key:{'causal':causal,'instruction':instruction,'execution_id':'fixture-model:2',
            'level':'L3','messages':messages,'completed_model':True}}
        return sources
    monkeypatch.setattr(learning,'_english_trigger_sources',sources_for_candidate)
    diagnostic={'english_trigger_learning':{'specific_conditions':{'accepted_triggers':[{'memory_id':card['memory_id']}]}}}
    result=learning._compile_seed_prevention_sources(c,artifact,set(),diagnostic)
    assert artifact==before and result['policy']['snapshot']==policy['snapshot']
    assert result['policy']['options'].get('l1_prevention_sources',[])==([] if executed_order else [card['memory_id']])
    assert diagnostic['seed_prevention_learning']['model_calls']==0
    assert result['policy']['options']['experience_overrides']==policy['options']['experience_overrides']
    # No accepted real-source trigger means no deployment branch is added.
    assert learning._compile_seed_prevention_sources(c,artifact,set(),{}) is artifact


def _native_experience_rendering_fixture(tmp_path, *, content=None, trigger='gmail_collection authenticated_inbox'):
    import learning
    from types import SimpleNamespace
    actual_root = Path(training.__file__).resolve().parents[4]
    binary = actual_root / 'tools/trajectory_evolution/build-v4/trajectory_evolution'
    if not binary.is_file():
        pytest.skip('native binary unavailable')
    scope = 'agent:fixture-agent/actor:default'
    content = content or ('Read show_inbox_threads with page_index and page_limit. '
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
            'trigger_pattern': trigger,
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


def test_seed_parameter_template_renders_approved_guidance_inside_native_budget(tmp_path):
    content='调用前先读取show_api_doc返回的参数契约；将token作为access_token参数传给需要认证的API。'
    learning,c,policy,query,_=_native_experience_rendering_fixture(tmp_path,content=content)
    before=copy.deepcopy(policy)
    compiled=learning._compile_appworld_experience_template(c,policy)
    decision=json.loads(c.native(c.BINARY,c.ROOT,['runtime','query'],{'policy':compiled,'query':query}))
    assert len(decision['experiences'])==1 and content in decision['context']
    assert 'exact access_token keyword' in decision['context']
    assert 'contract is unverified' in decision['context'] and 'separate call' in decision['context']
    assert len(decision['context'].encode())<=2048
    assert compiled['snapshot']==policy['snapshot'] and policy==before
    assert compiled['options']['approved_experiences']==policy['options']['approved_experiences']


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


@pytest.mark.parametrize('generation',['fresh_model','local_fallback'])
def test_normal_english_native_acceptance_is_saved_for_projection_and_prevention(tmp_path, monkeypatch, generation):
    learning, c, policy, query, _ = _native_experience_rendering_fixture(tmp_path)
    c.TRAINING=tmp_path; c.save=save; c.environment=lambda:{}
    card=policy['snapshot']['experiences'][0];c.SCOPE=card['target_scope']
    query['advice_query']['task']='Archive every read, non-starred thread in my Gmail inbox.'
    query['advice_query']['state']='task_gmail_inbox_complete_enumeration gmail_authenticated_state_ready'
    sources=learning._EnglishTriggerSources({card['memory_id']:{'experience':card,
        'requires_observed_failure':False,'evidence':[{'run_id':'fixture-run',
        'query_id':query['advice_query']['query_id'],'task':query['advice_query']['task']}]}},
        runtime_runs={'fixture-run':[query]})
    monkeypatch.setattr(learning,'seed_control_input',lambda *args:({'memories':[]}, {'scanned_tasks':1},{}))
    monkeypatch.setattr(learning,'_english_trigger_sources',lambda *args,**kwargs:sources)
    monkeypatch.setattr(learning,'_historical_english_inheritance',lambda *args:None)
    calls=[]
    def model(controller,packet,repair_reason,transport):
        calls.append(repair_reason);transport['model_calls']+=1
        if generation=='local_fallback':
            return {'triggers':[]},{}
        return learning._parse_complete_model_json(json.dumps({'triggers':[{
            'memory_id':card['memory_id'],'trigger_pattern':'task_gmail_inbox_complete_enumeration gmail_authenticated_state_ready'}]})),{}
    monkeypatch.setattr(learning,'_english_trigger_model_reply',model)
    diagnostic={};result=learning.generate_english_triggers(c,policy,set(),diagnostic)
    accepted=diagnostic['english_trigger_learning']['specific_conditions']['accepted_triggers']
    expected_calls=1 if generation=='fresh_model' else 3
    assert len(calls)==expected_calls and diagnostic['english_trigger_learning']['model_calls']==expected_calls
    assert len(accepted)==1 and accepted[0]['memory_id']==card['memory_id']
    origin=diagnostic['english_trigger_learning']['attempts'][0]['filtering'] if generation=='fresh_model' else diagnostic['english_trigger_learning']['evidence_fallback']
    assert accepted==origin['accepted_triggers']
    assert c.save and json.loads((tmp_path/'seed-compile-status.json').read_text())==diagnostic
    assert result['snapshot']==policy['snapshot']
    projected=learning._prune_appworld_runtime_experiences(c,result,diagnostic)
    assert diagnostic['runtime_experience_projection']['accepted_trigger_count']==1
    assert projected['options']['approved_experiences']==policy['options']['approved_experiences']
    origin['accepted_triggers'].clear()
    assert len(diagnostic['english_trigger_learning']['specific_conditions']['accepted_triggers'])==1


@pytest.mark.parametrize('mismatch',[None,'revision','scope','owner','not_closed','not_imported',
    'evaluation_error','identity','citation','instruction','public_field','public_type','not_current_task'])
def test_historical_train_trigger_windows_require_exact_public_input_and_approved_binding(tmp_path,mismatch):
    learning,c,policy,feedback,memory=_english_trigger_fixture(tmp_path)
    policy['snapshot']['identity']['agent_id']='fixture-agent'
    current=feedback['local_source_evidence']['local-run'];current['task_id']='admitted-task'
    path=Path(current['source_report']);report=c.read(path);trace=report['trials'][0]['evidence']['trace']
    trace['identity']['agent_id']='fixture-agent';current['identity']=copy.deepcopy(trace['identity'])
    start=next(event for event in trace['events'] if event['kind']=='run.start')
    start['input']['benchmark']='appworld';start['input']['native_view_redacted']=True
    save(path,report)
    dataset=tmp_path/'datasets/native/train.jsonl';dataset.parent.mkdir(parents=True)
    dataset.write_text(json.dumps({'task_id':'admitted-task','task_input':start['input']['instruction']})+'\n')
    old=copy.deepcopy(trace);old['identity'].update(run_id='approved-old-run',conversation_id='approved-old-conversation')
    old['evaluation']={'quality_score':0.123,'invalid_loop_count':9}
    candidate=memory['entries'][0]['candidate'];support=candidate['support_evidence'][0]
    support['source'].update(dialogue_id='approved-old-run',conversation_id='approved-old-conversation')
    metadata={'identity':copy.deepcopy(old['identity']),'closed':True,'imported':True,
        'evaluation_status':'completed','evaluation_error':'','training_source':{
        'kind':'baseline_training','task_id':'admitted-task','task_version':'original-historical-task-version'}}
    if mismatch=='revision':policy['options']['approved_experiences'][0]['content_revision']='other'
    elif mismatch=='scope':candidate['target_scope']='other'
    elif mismatch=='owner':support['source']['user_id']='other'
    elif mismatch=='not_closed':metadata['closed']=False
    elif mismatch=='not_imported':metadata['imported']=False
    elif mismatch=='evaluation_error':metadata['evaluation_error']='failed'
    elif mismatch=='identity':old['identity']['agent_id']='other'
    elif mismatch=='citation':support['evidence_event_ids']=['absent-event']
    elif mismatch in ('instruction','public_field','public_type'):
        input=next(event for event in old['events'] if event['kind']=='run.start')['input']
        input[{'instruction':'instruction','public_field':'benchmark','public_type':'native_view_redacted'}[mismatch]]=1 if mismatch=='public_type' else 'changed'
    elif mismatch=='not_current_task':current['task_id']='not-current'
    save(tmp_path/'evolution/by-user/fixture/memory/evolution-memory.json',memory)
    save(tmp_path/'evolution/runs/approved-old-run.json',metadata)
    save(tmp_path/'evolution/by-user/fixture/traces/approved-old-conversation/approved-old-run.trace.json',old)
    before=copy.deepcopy(feedback);result=learning._approved_historical_trigger_views(c,policy,feedback)
    assert feedback==before
    if mismatch:
        assert result=={}
    else:
        view=result['approved-old-run'];assert view['identity']==old['identity']
        assert view['historical_task_version']=='original-historical-task-version'
        assert view['verdict'] is None
        assert not {'evaluation','quality','quality_score','invalid_loop_count','metrics','cost'} & set(view)
        assert view['events']==old['events']


def _native_template_basis_projection_fixture(tmp_path):
    learning,c,original,query=_native_runtime_projection_fixture(tmp_path)
    memory=tmp_path/'memory.json';store=json.loads(memory.read_text())
    for mid,trigger,content in (
        ('fixture-token-basis','required access_token parameter',
         'Token basis lesson: pass access_token parameter only when the documented API requires it.'),
        ('fixture-schema-basis','required parameter schema',
         'Schema basis lesson: read show_api_doc parameter contract before an unverified API call.')):
        row=copy.deepcopy(store['entries'][0]);row['memory_id']=mid
        row['candidate'].update(candidate_id='candidate-'+mid,trigger_pattern=trigger,proposed_change=content)
        store['entries'].append(row)
    save(memory,store)
    identity=original['snapshot']['identity'];scope=original['snapshot']['experiences'][0]['target_scope']
    shadow={'target_scopes':[scope],'max_policy_entries':64,'max_advice_items':4,
        'max_advice_bytes':4096,'max_query_bytes':32768,'max_memory_bytes':16777216,
        'max_snapshot_bytes':262144,'max_match_terms':128,'min_matched_terms':1,'require_validated_evidence':True}
    snapshot=json.loads(c.native(c.BINARY,c.ROOT,['shadow','compile'],
        {'memory_path':str(memory),'identity':identity,'options':shadow}))
    options=copy.deepcopy(original['options']);options['approved_experiences']=[
        {key:card[key] for key in ('memory_id','content_revision')} for card in snapshot['experiences']]
    policy=json.loads(c.native(c.BINARY,c.ROOT,['runtime','compile'],{
        'memory_path':str(memory),'identity':identity,'shadow_options':shadow,'options':options}))
    return learning,c,policy,query


def test_native_template_basis_projection_is_conditional_disabled_and_exactly_approved(tmp_path):
    learning,c,policy,query=_native_template_basis_projection_fixture(tmp_path)
    original=copy.deepcopy(policy);cards={card['memory_id']:card for card in policy['snapshot']['experiences']}
    diagnostic=_projection_diagnostic('fixture-accepted',cards['fixture-accepted']['content_revision'])
    projected=learning._prune_appworld_runtime_experiences(c,policy,diagnostic)
    basis=diagnostic['runtime_experience_projection']['template_basis_sources']
    assert len(basis)==2 and {ref['memory_id'] for ref in basis}=={'fixture-token-basis','fixture-schema-basis'}
    assert all(ref=={key:cards[ref['memory_id']][key] for key in ('memory_id','content_revision')} for ref in basis)
    assert all(ref in projected['options']['disabled_injections'] for ref in basis)
    assert diagnostic['runtime_experience_projection']['template_only_source_count']==2
    compiled=learning._compile_appworld_experience_template(c,projected)
    assert compiled['snapshot']==original['snapshot'] and policy==original
    assert compiled['options']['loop_rules']==original['options']['loop_rules']
    assert compiled['options']['max_context_bytes']==original['options']['max_context_bytes']
    template=compiled['options']['experience_template']
    assert 'If the current API documentation explicitly requires access_token' in template
    assert 'exact access_token keyword' in template and 'separate call' in template
    assert 'spotify' not in template.lower()
    query['advice_query']['state']+=' required access_token parameter schema'
    before=json.loads(c.native(c.BINARY,c.ROOT,['runtime','query'],{'policy':policy,'query':query}))
    after=json.loads(c.native(c.BINARY,c.ROOT,['runtime','query'],{'policy':compiled,'query':query}))
    assert {ref['memory_id'] for ref in before['experiences']} & {ref['memory_id'] for ref in basis}
    assert after['experiences']==[{key:cards['fixture-accepted'][key] for key in ('memory_id','content_revision')}]
    assert all(cards[ref['memory_id']]['content'] not in after['context'] for ref in basis)
    assert len(after['context'].encode())<=compiled['options']['max_context_bytes']
    invalid=copy.deepcopy(compiled['options']);invalid['loop_rules'][0]['source_experience']['content_revision']='not-approved'
    with pytest.raises(RuntimeError):
        c.native(c.BINARY,c.ROOT,['runtime','recompile'],{'policy':compiled,'options':invalid})


@pytest.mark.parametrize('missing',['token','schema','both'])
def test_template_basis_does_not_create_unapproved_parameter_authority(tmp_path,missing):
    learning,c,policy,query=_native_template_basis_projection_fixture(tmp_path)
    excluded={'fixture-token-basis'} if missing=='token' else {'fixture-schema-basis'}
    if missing=='both':excluded={'fixture-token-basis','fixture-schema-basis'}
    options=copy.deepcopy(policy['options']);options['approved_experiences']=[ref
        for ref in options['approved_experiences'] if ref['memory_id'] not in excluded]
    unapproved=json.loads(c.native(c.BINARY,c.ROOT,['runtime','recompile'],{'policy':policy,'options':options}))
    assert learning._seed_parameter_guidance_sources(unapproved)==[]
    assert learning._seed_parameter_guidance(unapproved)==''
    card=next(card for card in policy['snapshot']['experiences'] if card['memory_id']=='fixture-accepted')
    diagnostic=_projection_diagnostic(card['memory_id'],card['content_revision'])
    projected=learning._prune_appworld_runtime_experiences(c,unapproved,diagnostic)
    assert diagnostic['runtime_experience_projection']['template_basis_sources']==[]
    assert not {ref['memory_id'] for ref in projected['options']['approved_experiences']} & {'fixture-token-basis','fixture-schema-basis'}


def _validation_control_fixture(tmp_path, monkeypatch):
    """Use the existing cited-failure rows with a freshly native-approved generic card."""
    learning, native, original, _, _ = _native_experience_rendering_fixture(tmp_path/'native',
        content='Inspect the real validation error and documented API contract before one corrected retry.',
        trigger='repeated validation API schema')
    memory = tmp_path/'native/memory.json'
    store = json.loads(memory.read_text())
    store['entries'][0]['memory_id'] = 'experience-a'
    save(memory, store)
    scope = original['snapshot']['experiences'][0]['target_scope']
    options = copy.deepcopy(original['options'])
    options['approved_experiences'] = []
    shadow = {'target_scopes':[scope], 'max_policy_entries':64, 'max_advice_items':2,
        'max_advice_bytes':2048, 'max_query_bytes':32768, 'max_memory_bytes':16777216,
        'max_snapshot_bytes':262144, 'max_match_terms':128, 'min_matched_terms':2,
        'require_validated_evidence':True}
    identity = original['snapshot']['identity']
    snapshot = json.loads(native.native(native.BINARY, native.ROOT, ['shadow','compile'],
        {'memory_path':str(memory), 'identity':identity, 'options':shadow}))
    options['approved_experiences'] = [{k:card[k] for k in ('memory_id','content_revision')}
                                      for card in snapshot['experiences']]
    options['loop_rules'] = [{'rule_id':'validation-base', 'target_scope':scope,
        'max_iterations':30, 'max_no_progress':3, 'max_replans':1,
        'no_progress_decision':'replan', 'replan_on_failure':False,
        'reason':'Keep the existing host correction budget.',
        'replan_context':'Inspect the previous real tool failure and correct the request.',
        'error_codes':['appworld_validation_422']}]
    policy = json.loads(native.native(native.BINARY, native.ROOT, ['runtime','compile'],
        {'memory_path':str(memory), 'identity':identity, 'shadow_options':shadow, 'options':options}))
    def current_native_policy(controller):
        controller.BINARY = native.BINARY
        controller.native = training.controller.native
        return policy
    monkeypatch.setattr(sys.modules[__name__], '_control_native_policy', current_native_policy)
    learning, c, rows, _, _, memory_path = _fallback_fixture(tmp_path/'training')
    for event in rows[2]['evidence']['trace']['events']:
        if event.get('error',{}).get('code') == 'appworld_auth_401':
            event['error']['code'] = 'appworld_validation_422'
    save(c.TRAINING/'full-captures/batch/report.json', {'complete':True, 'trials':rows})
    return learning, c, rows, policy, memory_path


def _validation_loop_query(policy, *, actor='unspecified', repeated=1, iteration=3):
    return {'advice_query':{'identity':policy['snapshot']['identity'],
        'query_id':f'validation-query-{iteration}', 'decision_id':f'validation-decision-{iteration}',
        'target_scope':policy['options']['loop_rules'][0]['target_scope'],
        'task':'Use the documented operation.'},
        'execution_id':'validation-execution', 'boundary':'loop', 'actor_id':actor,
        'iteration':iteration, 'failed_actions':2, 'repeated_failure_steps':repeated,
        'error_codes':['appworld_validation_422'], 'state_fingerprint':f'validation-state-{iteration}'}


def test_cited_validation_control_replans_once_through_native_runtime(tmp_path, monkeypatch):
    learning, c, rows, policy, _ = _validation_control_fixture(tmp_path, monkeypatch)
    before = copy.deepcopy(policy)
    draft = {'action_rules':[], 'schedule_rules':[], 'loop_rules':[{
        'memory_id':'experience-a', 'max_iterations':30, 'max_no_progress':3,
        'max_replans':1, 'replan_on_failure':True, 'error_codes':['appworld_validation_422'],
        'reason':'Use the existing bounded correction after a cited repeated validation failure.'}]}
    def model(controller, packet, reason, transport):
        transport['model_calls'] += 1
        return copy.deepcopy(draft), {}
    monkeypatch.setattr(learning, '_control_model_reply', model)
    diagnostic = {}
    compiled = learning.generate_seed_controls(c, policy, {row['task_id'] for row in rows}, diagnostic)
    assert policy == before and compiled['snapshot'] == before['snapshot']
    assert diagnostic['control_learning']['model_calls'] == 1
    learned = [rule for rule in compiled['options']['loop_rules'] if rule.get('source_experience')]
    assert len(learned) == 1 and learned[0]['max_replans'] == 1
    assert learned[0]['replan_context'] == policy['options']['loop_rules'][0]['replan_context']
    parent = json.loads(c.native(c.BINARY, c.ROOT, ['runtime','query'],
        {'policy':policy, 'query':_validation_loop_query(policy)}))
    first_failure = json.loads(c.native(c.BINARY, c.ROOT, ['runtime','query'],
        {'policy':compiled, 'query':_validation_loop_query(compiled, repeated=0)}))
    recovered = json.loads(c.native(c.BINARY, c.ROOT, ['runtime','query'],
        {'policy':compiled, 'query':_validation_loop_query(compiled)}))
    following = _validation_loop_query(compiled, iteration=4)
    following['previous'] = recovered['progress']
    after_recovery = json.loads(c.native(c.BINARY, c.ROOT, ['runtime','query'],
        {'policy':compiled, 'query':following}))
    decisions = [recovered, after_recovery]
    assert parent['progress']['decision'] == first_failure['progress']['decision'] == 'continue'
    assert [decision['progress']['decision'] for decision in decisions] == ['replan','continue']
    assert [decision['progress']['replans'] for decision in decisions] == [1,1]


def test_validation_coverage_rejects_wrong_actor_and_unbound_citation(tmp_path, monkeypatch):
    learning, c, rows, policy, memory_path = _validation_control_fixture(tmp_path, monkeypatch)
    packet, feedback, controls = learning.seed_control_input(c, policy, {r['task_id'] for r in rows})
    draft, _ = learning._evidence_bounded_control_draft(c, policy, feedback, dominant_cited_mechanism=True)
    draft['loop_rules'][0]['actor_id'] = 'other-actor'
    dormant = learning.compile_seed_controls(c, policy, controls, draft, {})
    decision = json.loads(c.native(c.BINARY, c.ROOT, ['runtime','query'],
        {'policy':dormant, 'query':_validation_loop_query(dormant)}))
    assert decision['progress']['decision'] == 'continue'
    assert learning._seed_controls_require_recovery(packet, dormant, feedback)
    memory = c.read(memory_path)
    memory['entries'][0]['candidate']['support_evidence'][0]['source']['conversation_id'] = 'different-capture'
    save(memory_path, memory)
    _, unbound, _ = learning.seed_control_input(c, policy, {r['task_id'] for r in rows})
    with pytest.raises(RuntimeError, match='evidence_recovery_no_cited_failure_pair'):
        learning._evidence_bounded_control_draft(c, policy, unbound, dominant_cited_mechanism=True)
    no_approval = copy.deepcopy(policy['options'])
    no_approval['approved_experiences'] = []
    unavailable = json.loads(c.native(c.BINARY, c.ROOT, ['runtime','recompile'],
        {'policy':policy, 'options':no_approval}))
    with pytest.raises(RuntimeError) as rejected:
        learning.compile_seed_controls(c, unavailable, controls, draft, {})
    assert rejected.value.native_reason == 'control_source_not_approved'


def test_validation_omission_repairs_once_then_fallback_preserves_paid_budget(tmp_path, monkeypatch):
    learning, c, rows, policy, _ = _validation_control_fixture(tmp_path, monkeypatch)
    original = copy.deepcopy(policy)
    missing = {'action_rules':[], 'schedule_rules':[], 'loop_rules':[{
        'memory_id':'experience-a', 'max_iterations':30, 'max_no_progress':2,
        'max_replans':1, 'replan_on_failure':False, 'error_codes':['appworld_validation_422'],
        'reason':'A native-valid draft which omits repeated-failure recovery.'}]}
    reasons = []
    def model(controller, packet, reason, transport):
        public = json.dumps(learning.approved_control_packet(packet))
        assert 'LOCAL_PRIVATE' not in public and 'LOCAL_EVENT' not in public and str(tmp_path) not in public
        reasons.append(reason)
        transport['model_calls'] += 1
        return copy.deepcopy(missing), {}
    monkeypatch.setattr(learning, '_control_model_reply', model)
    diagnostic = {'control_learning':{'model_calls':1, 'model_attempts':1,
        'attempts':[{'attempt':1, 'status':'native_contract_rejected'}]}}
    compiled = learning.generate_seed_controls(c, policy, {r['task_id'] for r in rows}, diagnostic)
    assert reasons == [None,'missing_cited_validation_recovery']
    assert diagnostic['control_learning']['model_calls'] == 3
    decision = json.loads(c.native(c.BINARY, c.ROOT, ['runtime','query'],
        {'policy':compiled, 'query':_validation_loop_query(compiled)}))
    assert decision['progress']['decision'] == 'replan'
    assert policy == original and compiled['snapshot'] == original['snapshot']
    paid = copy.deepcopy(diagnostic['control_learning']['attempts'])
    no_fourth_call = Mock(side_effect=AssertionError('The original paid budget is exhausted.'))
    monkeypatch.setattr(learning, '_control_model_reply', no_fourth_call)
    resumed = learning.generate_seed_controls(c, policy, {r['task_id'] for r in rows}, diagnostic)
    no_fourth_call.assert_not_called()
    assert diagnostic['control_learning']['model_calls'] == 3
    assert diagnostic['control_learning']['attempts'] == paid
    assert resumed == compiled

def _native_prevention_fixedpoint_fixture(tmp_path,monkeypatch):
    """Synthetic approved cards and real Host/native once/cache decisions."""
    content=('Record the original Amazon cart, clear_cart, add_product_to_cart only the requested products, '
             'then place_order and preserve the requested original cart state.')
    learning,c,base,query,_=_native_experience_rendering_fixture(tmp_path,content=content,
        trigger='Amazon cart purchase isolation')
    memory=tmp_path/'memory.json';store=json.loads(memory.read_text())
    definitions=[('fixture-file','Inspect file creation metadata',
        'Read apis.file_system.show_file and its created_at field for the requested files.'),
        ('fixture-login-422','Correct login validation failure',
         'After a login HTTP 422, inspect the documented parameter contract and correct the call.'),
        ('fixture-auth','Authenticate the requested service',
         'Run login and use the returned access_token for the requested authenticated API.')]
    for mid,trigger,text in definitions:
        row=copy.deepcopy(store['entries'][0]);row['memory_id']=mid
        row['candidate'].update(candidate_id='candidate-'+mid,trigger_pattern=trigger,proposed_change=text)
        store['entries'].append(row)
    save(memory,store);identity=base['snapshot']['identity'];scope=base['snapshot']['experiences'][0]['target_scope']
    shadow={'target_scopes':[scope],'max_policy_entries':64,'max_advice_items':2,'max_advice_bytes':2048,
        'max_query_bytes':32768,'max_memory_bytes':16777216,'max_snapshot_bytes':262144,
        'max_match_terms':128,'min_matched_terms':2,'require_validated_evidence':True}
    snapshot=json.loads(c.native(c.BINARY,c.ROOT,['shadow','compile'],{'memory_path':str(memory),'identity':identity,'options':shadow}))
    options=copy.deepcopy(base['options']);options['approved_experiences']=[{k:card[k] for k in ('memory_id','content_revision')} for card in snapshot['experiences']]
    triggers={'fixture-gmail-pagination':'add_product_to_cart amazon','fixture-file':'created_at show_file',
              'fixture-login-422':'422 login','fixture-auth':'authentication_needed login'}
    options['experience_overrides']=[{'source_experience':{k:card[k] for k in ('memory_id','content_revision')},
        'content':card['content'],'trigger_pattern':triggers[card['memory_id']]} for card in snapshot['experiences']]
    policy=json.loads(c.native(c.BINARY,c.ROOT,['runtime','compile'],{
        'memory_path':str(memory),'identity':identity,'shadow_options':shadow,'options':options}))
    artifact={'level':'L3','agent':{'appworld_options':{'l1_window_mode':'observed_phase_v3'}},'policy':policy};c.SCOPE=scope
    file_task='Inspect files created in the requested directory.';cart_task='Buy the requested products from Amazon and preserve the original cart.'
    docs="print(apis.supervisor.show_account_passwords())\nprint(apis.api_docs.show_api_doc(app_name='file_system', api_name='login'))"
    login="token = apis.file_system.login(username=profile.email, password=creds)['access_token']\nprint(apis.file_system.show_directory(path='/public', access_token=token))"
    show="print(apis.file_system.show_file(file_path='/public/example.txt', access_token=token)['created_at'])"
    steps=[(docs,'{"file_system":{"password":"<redacted_password>"},"login_schema":{"required_parameters":["username","password"]}}'),
        (login,'{"children":["example.txt"]}'),(login,'{"children":["example.txt"]}'),
        (show,'{"created_at":"public_date","path":"/public/example.txt"}')]
    cases={};history=[]
    for i,(code,observation) in enumerate(steps,1):
        history+= [{'role':'assistant','content':code},{'role':'user','content':observation}]
        cases.setdefault('file-run',[]).append((f'file-{i}',file_task,{'system':'Generate Python code for the current task.','messages':copy.deepcopy(history),'turn':i}))
    messages=[{'role':'assistant','content':'apis.amazon.add_product_to_cart(product_id=1, quantity=2, access_token=saved_token)'},{'role':'user','content':'{"message":"success"}'}]
    cases['cart-run']=[('cart-1',cart_task,{'system':'Generate Python code for the current task.','messages':messages,'turn':1})]
    cards={card['memory_id']:card for card in policy['snapshot']['experiences']};captured=[]
    def sources_for_candidate(controller,current,feedback,*,candidate_artifact):
        replay_options=learning._candidate_l1_replay_options(c,current,candidate_artifact)
        sources=learning._EnglishTriggerSources();sources.candidate_replay_options=replay_options
        for run,steps in cases.items():
            if feedback and run not in feedback['local_source_evidence']:continue
            sources.runtime_runs[run]=[];sources.runtime_source_evidence[run]={'identity':{'run_id':run}}
            points=[]
            for qid,task,causal in steps:
                execution='fixture-model:'+str(causal['turn']);messages=causal['messages'];packet=learning._candidate_l1_replay_packet(
                    c,replay_options,causal,task,execution,run,'L3',messages=messages)
                if not packet or not packet.get('apply_policy'):continue
                state=learning._l1_bound(learning._l1_bound(packet['state'],{}),{});refs=packet['extensions'].get('runtime_experience_allowlist')
                if refs is not None:state=json.dumps({'retrieval_state':state,'runtime_experience_allowlist':refs})
                q=copy.deepcopy(query);q['execution_id']=execution;q['advice_query'].update(query_id=qid,decision_id=qid,task=task,state=state)
                sources.runtime_runs[run].append(q);point={'run_id':run,'query_id':qid,'task':task};points.append(point)
                sources.runtime_replay_inputs[(run,qid)]={'causal':causal,'instruction':task,'execution_id':execution,
                    'level':'L3','messages':messages,'completed_model':True}
            for mid,card in cards.items():
                if (run=='cart-run')==(mid=='fixture-gmail-pagination'):
                    sources[mid]={'experience':card,'evidence':points}
        captured.append(sources);return sources
    monkeypatch.setattr(learning,'seed_control_input',lambda *args:({}, {}, {}))
    monkeypatch.setattr(learning,'_english_trigger_sources',sources_for_candidate)
    diagnostic={'english_trigger_learning':{'specific_conditions':{'accepted_triggers':[{'memory_id':mid} for mid in cards]}}}
    return learning,c,artifact,diagnostic,captured


def test_prevention_fixedpoint_rejects_unstable_file_but_retains_cart_with_native_cache_budget(tmp_path,monkeypatch):
    learning,c,artifact,diagnostic,captured=_native_prevention_fixedpoint_fixture(tmp_path,monkeypatch)
    original=copy.deepcopy(artifact);result=learning._compile_seed_prevention_sources(c,artifact,set(),diagnostic)
    assert artifact==original and result['policy']['snapshot']==original['policy']['snapshot']
    assert result['policy']['options']['l1_prevention_sources']==['fixture-gmail-pagination']
    assert [len(row['kept_sources']) for row in diagnostic['seed_prevention_learning']['subset_iterations']]==[2,1,1]
    first=captured[0].runtime_replay_profile['file-run'];second=captured[1].runtime_replay_profile['file-run']
    assert [row['count_after'] for row in first]==[0,0,0,1]
    assert [row['count_after'] for row in second]==[1,2,3,3]
    assert second[1]['cached_application'] and second[2]['cached_application'] and second[3]['budget_exhausted']
    assert first[3]['selected_memory_ids']==['fixture-file']
    assert second[3]['selected_memory_ids']==[]
    assert len(captured)==3 and set(captured[-1].runtime_runs)=={'cart-run'}
    for key in ('experience_overrides','approved_experiences','max_context_bytes','loop_rules','action_rules','schedule_rules'):
        assert result['policy']['options'].get(key)==original['policy']['options'].get(key)


@pytest.mark.parametrize('failure',['no_windows','native_failure'])
def test_prevention_fixedpoint_failure_never_certifies_unreplayed_sources(tmp_path,monkeypatch,failure):
    learning,c,artifact,diagnostic,_=_native_prevention_fixedpoint_fixture(tmp_path,monkeypatch)
    source_builder=learning._english_trigger_sources;calls=[]
    def fail_later(*args,**kwargs):
        calls.append(True)
        if len(calls)>1:raise RuntimeError('appworld_english_trigger_runtime_windows_missing' if failure=='no_windows' else 'native_runtime_query_failed')
        return source_builder(*args,**kwargs)
    monkeypatch.setattr(learning,'_english_trigger_sources',fail_later)
    if failure=='no_windows':
        assert learning._compile_seed_prevention_sources(c,artifact,set(),diagnostic) is artifact
        assert diagnostic['seed_prevention_learning']['qualified_sources']==[]
    else:
        with pytest.raises(RuntimeError,match='native_runtime_query_failed'):
            learning._compile_seed_prevention_sources(c,artifact,set(),diagnostic)
    assert artifact['policy']['options'].get('l1_prevention_sources',[])==[]


@pytest.mark.parametrize('source_guard', ['valid', 'citation', 'approval'])
def test_last_empty_paid_control_reply_uses_current_native_fallback_and_same_budget(tmp_path, monkeypatch, source_guard):
    """Replay A5's paid sequence; only current approved cited evidence can recover."""
    learning, c, rows, policy, memory_path = _validation_control_fixture(tmp_path, monkeypatch)
    original = copy.deepcopy(policy)
    missing = {'action_rules': [], 'schedule_rules': [], 'loop_rules': [{
        'memory_id': 'experience-a', 'max_iterations': 30, 'max_no_progress': 2,
        'max_replans': 1, 'replan_on_failure': False,
        'error_codes': ['appworld_validation_422'],
        'reason': 'A native-valid paid draft lacking repeated-failure recovery.'}]}
    invalid = copy.deepcopy(missing)
    invalid['loop_rules'][0]['match_mode'] = 'invalid_fixture_match_mode'
    drafts = iter((missing, invalid, {'action_rules': [], 'loop_rules': [], 'schedule_rules': []}))
    reasons = []
    def reply(controller, packet, reason, transport):
        reasons.append(reason)
        transport['model_calls'] += 1
        if len(reasons) == 3:
            if source_guard == 'citation':
                memory = c.read(memory_path)
                memory['entries'][0]['candidate']['support_evidence'][0]['source']['conversation_id'] = 'unbound'
                save(memory_path, memory)
            elif source_guard == 'approval':
                options = copy.deepcopy(policy['options'])
                options['approved_experiences'] = []
                unavailable = json.loads(c.native(c.BINARY, c.ROOT, ['runtime', 'recompile'],
                    {'policy': policy, 'options': options}))
                policy.clear()
                policy.update(unavailable)
        return copy.deepcopy(next(drafts)), {'total_tokens': 7}
    model = Mock(side_effect=reply)
    monkeypatch.setattr(learning, '_control_model_reply', model)
    diagnostic = {}
    if source_guard == 'valid':
        compiled = learning.generate_seed_controls(c, policy, {row['task_id'] for row in rows}, diagnostic)
        assert policy == original and compiled['snapshot'] == original['snapshot']
        assert diagnostic['control_learning']['evidence_fallback']['status'] == 'native_compiled'
        learned = [rule for rule in compiled['options']['loop_rules'] if rule.get('source_experience')]
        assert len(learned) == 1 and learned[0]['max_replans'] == 1
        assert learned[0]['source_experience']['memory_id'] == 'experience-a'
        assert learned[0]['source_experience']['content_revision'] == original['options']['approved_experiences'][0]['content_revision']
        assert learned[0]['error_codes'] == ['appworld_validation_422']
        first = json.loads(c.native(c.BINARY, c.ROOT, ['runtime', 'query'],
            {'policy': compiled, 'query': _validation_loop_query(compiled)}))
        following = _validation_loop_query(compiled, iteration=4)
        following['previous'] = first['progress']
        after = json.loads(c.native(c.BINARY, c.ROOT, ['runtime', 'query'],
            {'policy': compiled, 'query': following}))
        assert first['progress']['decision'] == 'replan' and first['progress']['replans'] == 1
        assert after['progress']['decision'] == 'continue' and after['progress']['replans'] == 1
    else:
        with pytest.raises(RuntimeError, match='evidence_recovery_no_cited_failure_pair'):
            learning.generate_seed_controls(c, policy, {row['task_id'] for row in rows}, diagnostic)
        assert diagnostic['control_learning']['evidence_fallback']['status'] == 'insufficient_evidence'
    status = diagnostic['control_learning']
    assert reasons == [None, 'missing_cited_validation_recovery', 'native_contract_rejected:runtime_rule_match_mode_invalid']
    assert model.call_count == status['model_calls'] == status['model_attempts'] == 3
    assert [attempt['status'] for attempt in status['attempts']] == ['generated', 'native_contract_rejected', 'no_controls']
    assert status['validated_model_draft'] == missing
    paid = copy.deepcopy(status['attempts'])
    no_fourth = Mock(side_effect=AssertionError('A fourth paid call is forbidden.'))
    monkeypatch.setattr(learning, '_control_model_reply', no_fourth)
    if source_guard == 'valid':
        resumed = learning.generate_seed_controls(c, policy, {row['task_id'] for row in rows}, diagnostic)
        assert resumed == compiled
    elif source_guard == 'approval':
        with pytest.raises(RuntimeError, match='evidence_recovery_no_cited_failure_pair'):
            learning.generate_seed_controls(c, policy, {row['task_id'] for row in rows}, diagnostic)
    else:
        # A native-valid paid draft remains reusable, but a withdrawn local
        # citation cannot turn its dormant rule into failure recovery.
        resumed = learning.generate_seed_controls(c, policy, {row['task_id'] for row in rows}, diagnostic)
        assert not any(rule.get('source_experience') and rule.get('replan_on_failure')
                       for rule in resumed['options'].get('loop_rules', []))
        dormant = json.loads(c.native(c.BINARY, c.ROOT, ['runtime', 'query'],
            {'policy': resumed, 'query': _validation_loop_query(resumed)}))
        assert dormant['progress']['decision'] == 'continue'
    no_fourth.assert_not_called()
    assert diagnostic['control_learning']['model_calls'] == 3
    assert diagnostic['control_learning']['attempts'] == paid

def _native_detail_mechanism_fixture(tmp_path):
    content = ('先调用 show_album_library 获取专辑库中的歌曲 ID，'
               '再对每个歌曲 ID 调用 show_song 获取 play_count，比较得出最高播放次数的歌曲。')
    learning, c, policy, query, _ = _native_experience_rendering_fixture(tmp_path,
        content=content, trigger='需要获取歌曲播放次数等详情时')
    card = policy['snapshot']['experiences'][0]
    c.SCOPE = card['target_scope']
    adapter = str(c.ROOT / 'examples/agent/business-simulator/appworld')
    if adapter not in sys.path:
        sys.path.insert(0, adapter)
    from appworld_skill import office_retrieval_state
    task = 'Find the album song with the highest playback count in my Spotify library.'
    code = "print(apis.api_docs.show_api_doc(app_name='spotify', api_name='show_song'))"
    messages = [{'role':'assistant','content':code}, {'role':'user','content':
        '{"api_name":"show_song","response_fields":["play_count"]}'}]
    projected = office_retrieval_state('Generate Python code.', messages, task,
        l1_window_mode='observed_phase_v3')
    state = learning._l1_bound(learning._l1_bound(projected, {}), {})
    assert 'task_object_album' in learning._l1_terms(state)
    positive = copy.deepcopy(query)
    positive['advice_query'].update(query_id='detail-doc', decision_id='detail-doc', task=task, state=state)
    offline = copy.deepcopy(query)
    offline['advice_query'].update(query_id='offline-album', decision_id='offline-album',
        task='Play an album for an offline drive.', state='task_object_album show_album_library')
    sources = learning._EnglishTriggerSources({card['memory_id']:{'experience':card,
        'evidence':[{'run_id':'fixture-run','query_id':'detail-doc','task':task}]}},
        runtime_runs={'fixture-run':[positive], 'offline-run':[offline]})
    sources.runtime_codes = {('fixture-run','detail-doc'):code,
        ('offline-run','offline-album'):'apis.spotify.show_album_library()'}
    sources.runtime_observations = {('fixture-run','detail-doc'):'show_song play_count'}
    return learning, c, policy, card, sources, positive, offline


@pytest.mark.parametrize('trigger',['task_object_album show_album_library','album show_album_library'])
def test_native_english_detail_source_rejects_ambient_upstream_condition(tmp_path,trigger):
    learning,c,policy,card,sources,_,_ = _native_detail_mechanism_fixture(tmp_path)
    before = copy.deepcopy(policy)
    assert learning._experience_mechanism_anchors(card) == {'play_count','show_song'}
    with pytest.raises(ValueError, match='source_mechanism_missing'):
        learning._compile_english_triggers(c,policy,{'triggers':[{
            'memory_id':card['memory_id'],'trigger_pattern':trigger}]},sources)
    assert policy == before


@pytest.mark.parametrize('trigger',['task_object_album show_song','show_song play_count'])
def test_native_english_detail_translation_matches_executed_public_doc_before_detail_call(tmp_path,trigger):
    learning,c,policy,card,sources,positive,offline = _native_detail_mechanism_fixture(tmp_path)
    before = copy.deepcopy(policy)
    assert 'play_count' in positive['advice_query']['state']
    assert 'show_song' in sources.runtime_codes[('fixture-run','detail-doc')]
    assert 'apis.spotify.show_song' not in sources.runtime_codes[('fixture-run','detail-doc')]
    compiled = learning._compile_english_triggers(c,policy,{'triggers':[{
        'memory_id':card['memory_id'],'trigger_pattern':trigger}]},sources)
    profile = learning._native_l1_profile(c,compiled,sources)
    assert profile[('fixture-run','detail-doc')] == {card['memory_id']}
    assert profile[('offline-run','offline-album')] == set()
    assert policy == before and compiled['snapshot'] == before['snapshot']
    assert compiled['options']['experience_overrides'][0]['content'] == card['content']
    assert compiled['options']['approved_experiences'] == before['options']['approved_experiences']


def test_native_local_english_selector_regenerates_broad_inheritance_with_source_detail(tmp_path):
    learning,c,policy,card,sources,_,_ = _native_detail_mechanism_fixture(tmp_path)
    diagnostic = {}
    before = copy.deepcopy(policy)
    compiled = learning._specific_english_pairs(c,policy,sources,[],diagnostic,
        fixed_conditions={card['memory_id']:'task_object_album show_album_library'},incremental_ids=set())
    trigger = compiled['options']['experience_overrides'][0]['trigger_pattern']
    assert set(learning._l1_terms(trigger)) & {'show_song','play_count'}
    assert trigger != 'task_object_album show_album_library'
    assert compiled['snapshot'] == before['snapshot'] and policy == before
    assert learning._native_l1_profile(c,compiled,sources)[('offline-run','offline-album')] == set()


def test_native_english_all_chinese_source_can_keep_faithful_observable_translation(tmp_path):
    learning,c,policy,query,_ = _native_experience_rendering_fixture(tmp_path,
        content='调用需要认证的接口前先登录，再使用返回的令牌继续调用。',trigger='调用认证接口前')
    card = policy['snapshot']['experiences'][0]
    c.SCOPE = card['target_scope']
    assert learning._experience_mechanism_anchors(card) == set()
    query['advice_query'].update(task='Authenticate the requested service.',state='authentication_needed login')
    sources = learning._EnglishTriggerSources({card['memory_id']:{'experience':card,
        'evidence':[{'run_id':'fixture-run','query_id':query['advice_query']['query_id']}]}},
        runtime_runs={'fixture-run':[query]})
    compiled = learning._compile_english_triggers(c,policy,{'triggers':[{
        'memory_id':card['memory_id'],'trigger_pattern':'authentication_needed login'}]},sources)
    assert learning._native_l1_profile(c,compiled,sources)[('fixture-run',query['advice_query']['query_id'])] == {card['memory_id']}
    assert compiled['snapshot'] == policy['snapshot']


def test_native_three_term_detail_condition_cannot_match_only_ambient_terms(tmp_path):
    learning,c,policy,card,sources,positive,offline = _native_detail_mechanism_fixture(tmp_path)
    trigger = 'task_object_album show_album_library play_count'
    options = copy.deepcopy(policy['options'])
    options['experience_overrides'] = [{'source_experience':{key:card[key] for key in ('memory_id','content_revision')},
        'content':card['content'],'trigger_pattern':trigger}]
    unsafe = json.loads(c.native(c.BINARY,c.ROOT,['runtime','recompile'],{'policy':policy,'options':options}))
    actual = json.loads(c.native(c.BINARY,c.ROOT,['runtime','query'],{'policy':unsafe,'query':offline}))
    assert {ref['memory_id'] for ref in actual['experiences']} == {card['memory_id']}
    with pytest.raises(ValueError,match='native_mechanism_not_required'):
        learning._compile_english_triggers(c,policy,{'triggers':[{
            'memory_id':card['memory_id'],'trigger_pattern':trigger}]},sources)


def test_native_single_documented_detail_api_remains_eligible_before_execution(tmp_path):
    learning,c,policy,query,_ = _native_experience_rendering_fixture(tmp_path,
        content='Read apis.file_system.show_file and its created_at field for the requested files.',
        trigger='Inspect file creation metadata')
    card = policy['snapshot']['experiences'][0];c.SCOPE=card['target_scope']
    assert learning._experience_mechanism_anchors(card) == {'created_at','show_file'}
    query['advice_query'].update(task='Inspect the requested files.', state='file_system show_file')
    sources = learning._EnglishTriggerSources({card['memory_id']:{'experience':card,
        'evidence':[{'run_id':'fixture-run','query_id':query['advice_query']['query_id']}]}},
        runtime_runs={'fixture-run':[query]})
    sources.runtime_codes = {('fixture-run',query['advice_query']['query_id']):
        "apis.api_docs.show_api_doc(app_name='file_system', api_name='show_file')"}
    compiled = learning._compile_english_triggers(c,policy,{'triggers':[{
        'memory_id':card['memory_id'],'trigger_pattern':'file_system show_file'}]},sources)
    assert learning._native_l1_profile(c,compiled,sources)[('fixture-run',query['advice_query']['query_id'])] == {card['memory_id']}
    assert compiled['snapshot'] == policy['snapshot']

def _disjoint_control_fixture(tmp_path, monkeypatch, binary=None):
    learning,c,rows,policy,memory_path=_validation_control_fixture(tmp_path,monkeypatch)
    import os
    new_cli=Path(os.environ.get('APPWORLD_DISJOINT_TEST_BINARY',
        str(ROOT/'tools/trajectory_evolution/build-v4/trajectory_evolution')))
    if not new_cli.is_file():
        pytest.skip('disjoint native test compiler unavailable')
    c.BINARY=binary or new_cli
    native_memory=tmp_path/'native/memory.json'
    store=json.loads(native_memory.read_text())
    auth=copy.deepcopy(store['entries'][0]);auth['memory_id']='experience-auth'
    auth['candidate'].update(candidate_id='candidate-auth',trigger_pattern='repeated authentication HTTP 401',
        proposed_change='Inspect the actual authentication failure before one corrected retry.')
    store['entries'].append(auth);save(native_memory,store)
    shadow={'target_scopes':[policy['options']['loop_rules'][0]['target_scope']],
        'max_policy_entries':64,'max_advice_items':2,'max_advice_bytes':2048,
        'max_query_bytes':32768,'max_memory_bytes':16777216,'max_snapshot_bytes':262144,
        'max_match_terms':128,'min_matched_terms':2,'require_validated_evidence':True}
    snapshot=json.loads(c.native(c.BINARY,c.ROOT,['shadow','compile'],{
        'memory_path':str(native_memory),'identity':policy['snapshot']['identity'],'options':shadow}))
    options=copy.deepcopy(policy['options'])
    options['approved_experiences']=[{k:card[k] for k in ('memory_id','content_revision')}
                                      for card in snapshot['experiences']]
    options['loop_rules'][0]['error_codes']=['appworld_auth_401','appworld_validation_422']
    policy=json.loads(c.native(c.BINARY,c.ROOT,['runtime','compile'],{
        'memory_path':str(native_memory),'identity':policy['snapshot']['identity'],
        'shadow_options':shadow,'options':options}))
    auth_row=copy.deepcopy(rows[2]);auth_row['task_id']='LOCAL_PRIVATE_AUTH_TASK_1'
    auth_row['trial_id']='LOCAL_PRIVATE_AUTH_TRIAL'
    trace=auth_row['evidence']['trace'];trace['identity'].update(
        conversation_id='LOCAL_AUTH_CONVERSATION',run_id='LOCAL_AUTH_RUN')
    for i,event in enumerate(trace['events']):
        event['event_id']='LOCAL_AUTH_EVENT_'+str(i)
        if event.get('error',{}).get('code')=='appworld_validation_422':
            event['error']['code']='appworld_auth_401'
    rows.append(auth_row);save(c.TRAINING/'full-captures/batch/report.json',{'complete':True,'trials':rows})
    local=c.read(memory_path);citation=copy.deepcopy(local['entries'][0])
    citation['memory_id']='experience-auth'
    ends=[e for e in trace['events'] if e['kind']=='action.end']
    evidence=citation['candidate']['support_evidence'][0]
    evidence['source'].update(conversation_id='LOCAL_AUTH_CONVERSATION',dialogue_id='LOCAL_AUTH_RUN')
    evidence['evidence_event_ids']=[e['event_id'] for e in ends[:2]]
    local['entries'].append(citation);save(memory_path,local)
    return learning,c,rows,policy,memory_path


def test_disjoint_fallback_uses_two_exact_sources_and_one_shared_native_replan(tmp_path,monkeypatch):
    learning,c,rows,policy,_=_disjoint_control_fixture(tmp_path,monkeypatch)
    before=copy.deepcopy(policy)
    packet,feedback,controls=learning.seed_control_input(c,policy,{r['task_id'] for r in rows})
    learning._seed_disjoint_recovery_probe(c,policy,controls,feedback)
    assert c._seed_disjoint_loop_capable is True
    status={};compiled=learning._compile_evidence_recovery(c,policy,controls,feedback,status)
    learned=[r for r in compiled['options']['loop_rules'] if r.get('source_experience')]
    assert len(learned)==2
    assert {r['source_experience']['memory_id']:r['error_codes'] for r in learned}=={
        'experience-auth':['appworld_auth_401'],'experience-a':['appworld_validation_422']}
    assert all(r['match_mode']=='predicate' and r['max_replans']==1 for r in learned)
    assert all((r['max_iterations'],r['max_no_progress'])==(30,3) for r in learned)
    assert all(r['source_experience'] in policy['options']['approved_experiences'] for r in learned)
    assert not learning._seed_controls_require_recovery(packet,compiled,feedback)
    query=_validation_loop_query(compiled)
    first=json.loads(c.native(c.BINARY,c.ROOT,['runtime','query'],{'policy':compiled,'query':query}))
    switched=_validation_loop_query(compiled,iteration=4)
    switched['error_codes']=['appworld_auth_401'];switched['previous']=first['progress']
    second=json.loads(c.native(c.BINARY,c.ROOT,['runtime','query'],{'policy':compiled,'query':switched}))
    assert first['progress']['decision']=='replan' and first['progress']['replans']==1
    assert second['progress']['decision']=='continue' and second['progress']['replans']==1
    assert policy==before and compiled['snapshot']==policy['snapshot']


def test_disjoint_generation_requires_each_cited_mechanism_without_fourth_paid_call(tmp_path,monkeypatch):
    learning,c,rows,policy,_=_disjoint_control_fixture(tmp_path,monkeypatch)
    reasons=[]
    def reply(controller,packet,reason,transport):
        assert controller._seed_disjoint_loop_capable is True
        public=json.dumps(learning.approved_control_packet(packet))
        assert 'LOCAL_PRIVATE' not in public and 'LOCAL_EVENT' not in public
        reasons.append(reason);transport['model_calls']+=1
        return {'action_rules':[],'loop_rules':[],'schedule_rules':[]},{}
    monkeypatch.setattr(learning,'_control_model_reply',reply)
    diagnostic={}
    compiled=learning.generate_seed_controls(c,policy,{r['task_id'] for r in rows},diagnostic)
    assert len(reasons)==2 and diagnostic['control_learning']['model_calls']==2
    assert len([r for r in compiled['options']['loop_rules'] if r.get('source_experience')])==2
    paid=copy.deepcopy(diagnostic['control_learning']['attempts'])
    diagnostic['control_learning']['model_calls']=3
    monkeypatch.setattr(learning,'_control_model_reply',Mock(side_effect=AssertionError('no fourth paid call')))
    restored=learning.generate_seed_controls(c,policy,{r['task_id'] for r in rows},diagnostic)
    assert restored==compiled and diagnostic['control_learning']['model_calls']==3
    assert diagnostic['control_learning']['attempts']==paid


def test_disjoint_duplicate_scope_downgrade_and_current_approval_revalidation(tmp_path,monkeypatch):
    learning,c,rows,policy,memory_path=_disjoint_control_fixture(tmp_path,monkeypatch)
    packet,feedback,controls=learning.seed_control_input(c,policy,{r['task_id'] for r in rows})
    original_native=c.native
    def old_contract(binary,root,operation,value):
        if operation==['runtime','control-compile'] and len(value['draft']['loop_rules'])>1:
            error=RuntimeError('native_runtime_control-compile_failed')
            error.native_reason='control_duplicate_loop_scope'
            raise error
        return original_native(binary,root,operation,value)
    c.native=old_contract;c._seed_disjoint_loop_capable=True
    learning._seed_disjoint_recovery_probe(c,policy,controls,feedback)
    assert c._seed_disjoint_loop_capable is False and 'cited_disjoint_recovery' not in feedback
    single=learning._compile_evidence_recovery(c,policy,controls,feedback,{})
    assert len([r for r in single['options']['loop_rules'] if r.get('source_experience')])==1
    c.native=original_native
    controls['max_rules']=1
    learning._seed_disjoint_recovery_probe(c,policy,controls,feedback)
    assert c._seed_disjoint_loop_capable is False
    bounded=learning._compile_evidence_recovery(c,policy,controls,feedback,{})
    limited=[r for r in bounded['options']['loop_rules'] if r.get('source_experience')]
    assert len(limited)==1 and limited[0]['error_codes']==['appworld_validation_422']
    controls['max_rules']=16
    unapproved=copy.deepcopy(policy['options']);unapproved['approved_experiences']=[]
    removed=json.loads(c.native(c.BINARY,c.ROOT,['runtime','recompile'],{'policy':policy,'options':unapproved}))
    learning._seed_disjoint_recovery_probe(c,removed,controls,feedback)
    assert c._seed_disjoint_loop_capable is False
    draft,_=learning._evidence_bounded_control_draft(c,policy,feedback,multi_mechanism=True)
    with pytest.raises(RuntimeError) as exc:
        learning.compile_seed_controls(c,removed,controls,draft,{})
    assert exc.value.native_reason=='control_source_not_approved'
    memory=c.read(memory_path)
    for row in memory['entries']:
        row['candidate']['support_evidence'][0]['source']['conversation_id']='unbound'
    save(memory_path,memory)
    _,uncited,_=learning.seed_control_input(c,policy,{r['task_id'] for r in rows})
    learning._seed_disjoint_recovery_probe(c,policy,controls,uncited)
    assert c._seed_disjoint_loop_capable is False
    with pytest.raises(RuntimeError,match='no_cited_failure_pair'):
        learning._evidence_bounded_control_draft(c,policy,uncited,multi_mechanism=True)


def test_disjoint_coverage_rejects_extra_uncited_codes_and_other_probe_failures(tmp_path,monkeypatch):
    learning,c,rows,policy,_=_disjoint_control_fixture(tmp_path,monkeypatch)
    options=copy.deepcopy(policy['options'])
    options['loop_rules'][0]['error_codes'].append('appworld_code_error')
    policy=json.loads(c.native(c.BINARY,c.ROOT,['runtime','recompile'],{'policy':policy,'options':options}))
    packet,feedback,controls=learning.seed_control_input(c,policy,{r['task_id'] for r in rows})
    learning._seed_disjoint_recovery_probe(c,policy,controls,feedback)
    draft,_=learning._evidence_bounded_control_draft(c,policy,feedback,multi_mechanism=True)
    draft['loop_rules'][0]['error_codes'].append('appworld_code_error')
    changed=learning.compile_seed_controls(c,policy,controls,draft,{})
    assert learning._seed_controls_require_recovery(packet,changed,feedback)
    original_native=c.native
    def denied(binary,root,operation,value):
        if operation==['runtime','control-compile']:
            error=RuntimeError('native_runtime_control-compile_failed')
            error.native_reason='control_source_not_approved'
            raise error
        return original_native(binary,root,operation,value)
    c.native=denied
    with pytest.raises(RuntimeError) as exc:
        learning._seed_disjoint_recovery_probe(c,policy,controls,feedback)
    assert exc.value.native_reason=='control_source_not_approved'
    assert c._seed_disjoint_loop_capable is False
    assert 'cited_disjoint_recovery' not in feedback

def _native_precorrective_fixture(tmp_path, *, content=None):
    content = content or ('下单前记录原购物车及数量，使用clear_cart清空购物车，'
        'add_product_to_cart添加完整数量，place_order下单后恢复原购物车。')
    learning,c,policy,query,_ = _native_experience_rendering_fixture(tmp_path,
        content=content,trigger='Amazon购物车下单，仅购买目标并保留原购物车')
    card=policy['snapshot']['experiences'][0];c.SCOPE=card['target_scope']
    before=copy.deepcopy(query);before['advice_query'].update(query_id='before-write',
        decision_id='before-write',task='Order only the requested Amazon products.',state='apis.amazon.show_cart()')
    after=copy.deepcopy(before);after['advice_query'].update(query_id='after-write',
        decision_id='after-write',state='apis.amazon.clear_cart() apis.amazon.add_product_to_cart(quantity=2)')
    trace={'events':[
        {'event_id':'before-write','kind':'action.start','timestamp_us':'10','input':{'messages':[]}},
        {'event_id':'first-write','kind':'action.start','timestamp_us':'20',
            'input':{'code':'apis.amazon.clear_cart(access_token=token)'}},
        {'event_id':'after-write','kind':'action.start','timestamp_us':'30','input':{'messages':[]}},
        {'event_id':'order-write','kind':'action.start','timestamp_us':'40',
            'input':{'code':'apis.amazon.place_order(access_token=token)'}}]}
    early=learning._source_pre_action_queries(card,trace,[before,after])
    assert early=={'before-write'}
    sources=learning._EnglishTriggerSources({card['memory_id']:{'experience':card,
        'evidence':[{'run_id':'fixture-run','query_id':row['advice_query']['query_id'],
                    'task':row['advice_query']['task'],'pre_action_eligible':row['advice_query']['query_id'] in early}
                    for row in [before,after]]}},runtime_runs={'fixture-run':[before,after]})
    sources.runtime_codes={('fixture-run','before-write'):'apis.amazon.show_cart()',
        ('fixture-run','after-write'):'apis.amazon.clear_cart() apis.amazon.add_product_to_cart(quantity=2)'}
    return learning,c,policy,card,sources,before,after,trace


def test_native_precorrective_condition_rejects_only_after_correction_even_before_order(tmp_path):
    learning,c,policy,card,sources,before,after,trace=_native_precorrective_fixture(tmp_path)
    original=copy.deepcopy(policy)
    with pytest.raises(ValueError,match='native_pre_action_not_matched'):
        learning._compile_english_triggers(c,policy,{'triggers':[{
            'memory_id':card['memory_id'],'trigger_pattern':'clear_cart add_product_to_cart'}]},sources)
    assert policy==original
    assert trace['events'][-1]['event_id']=='order-write'


def test_native_precorrective_read_condition_beats_local_corrective_pair(tmp_path):
    learning,c,policy,card,sources,before,after,trace=_native_precorrective_fixture(tmp_path)
    diagnostic={};mid=card['memory_id']
    compiled=learning._specific_english_pairs(c,policy,sources,[[{'memory_id':mid,
        'trigger_pattern':'amazon show_cart','approved_source':True}]],diagnostic)
    assert diagnostic['accepted_triggers']==[{'memory_id':mid,'trigger_pattern':'amazon show_cart'}]
    assert learning._native_l1_profile(c,compiled,sources)[('fixture-run','before-write')]=={mid}
    assert compiled['snapshot']==policy['snapshot']
    assert compiled['options']['experience_overrides'][0]['content']==card['content']
    contrast=copy.deepcopy(before);contrast['advice_query'].update(query_id='namespace-only',
        decision_id='namespace-only',state='amazon cart quantity',task='Inspect my Amazon cart.')
    result=json.loads(c.native(c.BINARY,c.ROOT,['runtime','query'],{'policy':compiled,'query':contrast}))
    assert result['experiences']==[]
    assert not learning._experience_task_compatible(card,'Move my cart to my wish list.')


def test_precorrective_quantity_boundary_uses_source_schema_before_transfer_not_order(tmp_path):
    learning,c,policy,query,_=_native_experience_rendering_fixture(tmp_path,
        content='先记录原购物车product_id与quantity，目标移入后place_order，最后恢复原购物车数量。',
        trigger='仅订购愿望清单目标商品并保留原购物车')
    card=policy['snapshot']['experiences'][0]
    queries=[{'advice_query':{'query_id':name}} for name in ('before-transfer','after-transfer')]
    trace={'events':[
        {'event_id':'before-transfer','kind':'action.start','timestamp_us':10,'input':{}},
        {'event_id':'transfer','kind':'action.start','timestamp_us':20,'input':{
            'code':'apis.amazon.move_product_from_wish_list_to_cart(product_id=selected, quantity=quantity)'}},
        {'event_id':'after-transfer','kind':'action.start','timestamp_us':30,'input':{}},
        {'event_id':'order','kind':'action.start','timestamp_us':40,'input':{
            'code':'apis.amazon.place_order(access_token=token)'}}]}
    assert learning._source_pre_action_queries(card,trace,queries)=={'before-transfer'}
    assert 'show_wish_list' in learning._experience_pre_action_operations(card)
    assert not learning._experience_task_compatible(card,'Inspect my wish list quantities.')
    assert learning._experience_task_compatible(card,'Order all mouse pads in my Amazon wish list.')
    missing=copy.deepcopy(trace);missing['events'][0].pop('timestamp_us')
    assert learning._source_pre_action_queries(card,missing,queries)==set()
    # A skipped function definition cannot establish a real action boundary.
    unused=copy.deepcopy(trace);unused['events'][1]['input']['code']='def unused():\n    apis.amazon.move_product_from_wish_list_to_cart(quantity=2)'
    assert learning._source_pre_action_queries(card,unused,queries)=={'before-transfer','after-transfer'}


def test_precorrective_saved_paid_draft_uses_only_remaining_one_repair(tmp_path,monkeypatch):
    learning,c,policy,card,sources,before,after,trace=_native_precorrective_fixture(tmp_path)
    mid=card['memory_id'];c.TRAINING=tmp_path;c.environment=lambda:{}
    c.save=save;c.read=lambda path:json.loads(Path(path).read_text())
    packet={'memories':[{key:card.get(key) for key in ('memory_id','trigger_pattern','observed_problem','content','target_scope')}],
        'training_statistics':{}}
    feedback={'scanned_tasks':['training-fixture']}
    monkeypatch.setattr(learning,'seed_control_input',lambda *args:(packet,feedback,{}))
    monkeypatch.setattr(learning,'_english_trigger_sources',lambda *args,**kwargs:sources)
    monkeypatch.setattr(learning,'_historical_english_inheritance',lambda *args:None)
    calls=[]
    def repaired(controller,outgoing,reason=None,transport=None):
        calls.append(reason)
        assert transport['model_calls']==2
        transport['model_calls']+=1
        return {'triggers':[{'memory_id':mid,'trigger_pattern':'amazon show_cart'}]}, {'output_tokens':10}
    monkeypatch.setattr(learning,'_english_trigger_model_reply',repaired)
    diagnostic={'english_trigger_learning':{'status':'compiled','model_calls':2,'attempts':[
        {'status':'rejected','reason':'appworld_control_model_output_truncated'},
        {'status':'compiled','draft':[{'memory_id':mid,'approved_source':True,
            'trigger_pattern':'clear_cart add_product_to_cart'}]}],
        'specific_conditions':{'accepted_triggers':[{'memory_id':mid,'trigger_pattern':'clear_cart add_product_to_cart'}]}}}
    result=learning.generate_english_triggers(c,policy,{'training-fixture'},diagnostic,prevention_compilation=True)
    assert len(calls)==1 and calls[0]=='appworld_english_trigger_native_pre_action_not_matched'
    assert diagnostic['english_trigger_learning']['model_calls']==3
    assert diagnostic['english_trigger_learning']['draft_reuse']['status']=='rejected'
    assert diagnostic['english_trigger_learning']['specific_conditions']['accepted_triggers']==[
        {'memory_id':mid,'trigger_pattern':'amazon show_cart'}]
    assert result['snapshot']==policy['snapshot']


def test_precorrective_incremental_expansion_cannot_reset_spent_paid_budget(tmp_path,monkeypatch):
    learning,c,policy,card,sources,before,after,trace=_native_precorrective_fixture(tmp_path)
    c.TRAINING=tmp_path;c.save=save
    mid=card['memory_id']
    status={'model_calls':3,'specific_conditions':{'accepted_triggers':[]}}
    diagnostic={'english_trigger_learning':status}
    paid=Mock(side_effect=AssertionError('a fourth paid call is forbidden'))
    monkeypatch.setattr(learning,'_english_trigger_model_reply',paid)
    result=learning._expand_english_trigger_subset(c,policy,policy,sources,
        {'memories':[card],'training_statistics':{}},diagnostic,{})
    assert result==policy and status['model_calls']==3
    assert status['incremental_generation']['unsubmitted_sources']==1
    paid.assert_not_called()


def test_precorrective_guard_preserves_exact_cited_reactive_failure_window(tmp_path):
    learning,c,policy,feedback,memory=_english_trigger_fixture(tmp_path)
    card=policy['snapshot']['experiences'][0]
    card.update(trigger_pattern='参数校验失败后需要修正重试',
        content='After appworld_validation_422 retry apis.amazon.add_product_to_cart with documented parameters.')
    view=feedback['local_source_evidence']['local-run']
    view.update(task_id='training-fixture',verdict='passed')
    view['event_mechanisms']['LOCAL_AUTH_EVENT']='appworld_validation_422'
    report=c.read(view['source_report']);trace=report['trials'][0]['evidence']['trace']
    trace['events'].insert(0,{'event_id':'first-failed-write','kind':'action.start',
        'timestamp_us':1,'input':{'code':'apis.amazon.add_product_to_cart(product_id=target)'}})
    save(Path(view['source_report']),report)
    sources=learning._english_trigger_sources(c,policy,feedback)
    assert sources[card['memory_id']]['requires_observed_failure'] is True
    assert all(point['pre_action_eligible'] for point in sources[card['memory_id']]['evidence'])


def test_precorrective_partial_paid_reuse_keeps_healthy_draft_and_repairs_missing_source_once(tmp_path,monkeypatch):
    learning,c,policy,card,sources,before,after,trace=_native_precorrective_fixture(tmp_path)
    memory=tmp_path/'memory.json';store=json.loads(memory.read_text())
    second=copy.deepcopy(store['entries'][0]);second['memory_id']='fixture-healthy-note'
    second['candidate'].update(candidate_id='fixture-note-candidate',
        trigger_pattern='simple_note show_note',proposed_change='Read the requested note using show_note.')
    store['entries'].append(second);save(memory,store)
    identity=policy['snapshot']['identity'];shadow=policy['snapshot']['options']
    snapshot=json.loads(c.native(c.BINARY,c.ROOT,['shadow','compile'],
        {'memory_path':str(memory),'identity':identity,'options':shadow}))
    options=copy.deepcopy(policy['options']);options['approved_experiences']=[
        {key:row[key] for key in ('memory_id','content_revision')} for row in snapshot['experiences']]
    policy=json.loads(c.native(c.BINARY,c.ROOT,['runtime','compile'],
        {'memory_path':str(memory),'identity':identity,'shadow_options':shadow,'options':options}))
    cards={row['memory_id']:row for row in policy['snapshot']['experiences']}
    mid=card['memory_id'];healthy='fixture-healthy-note';sources[mid]['experience']=cards[mid]
    before['advice_query']['state']+=' apis.simple_note.show_note()'
    sources.runtime_codes[('fixture-run','before-write')]+=' apis.simple_note.show_note()'
    sources[healthy]={'experience':cards[healthy],'evidence':[{
        'run_id':'fixture-run','query_id':'before-write','task':before['advice_query']['task'],
        'pre_action_eligible':True}]}
    c.TRAINING=tmp_path;c.save=save;c.environment=lambda:{}
    packet={'memories':[cards[mid],cards[healthy]],'training_statistics':{}}
    monkeypatch.setattr(learning,'seed_control_input',lambda *args:(packet,{'scanned_tasks':['training-fixture']},{}))
    monkeypatch.setattr(learning,'_english_trigger_sources',lambda *args,**kwargs:sources)
    monkeypatch.setattr(learning,'_historical_english_inheritance',lambda *args:None)
    calls=[]
    def repair(controller,outgoing,reason=None,transport=None):
        assert [row['memory_id'] for row in outgoing['memories']]==[mid]
        assert transport['model_calls']==0
        transport['model_calls']+=1;calls.append(reason)
        return {'triggers':[{'memory_id':mid,'trigger_pattern':'amazon show_cart'}]}, {'output_tokens':10}
    monkeypatch.setattr(learning,'_english_trigger_model_reply',repair)
    original_draft=[{'memory_id':healthy,'approved_source':True,'trigger_pattern':'simple_note show_note'}]
    diagnostic={'english_trigger_learning':{'status':'compiled','model_calls':2,
        'attempts':[{'status':'rejected'},{'status':'compiled','draft':copy.deepcopy(original_draft)}],
        'specific_conditions':{'accepted_triggers':[{'memory_id':mid,'trigger_pattern':'clear_cart add_product_to_cart'},
            {'memory_id':healthy,'trigger_pattern':'simple_note show_note'}]}}}
    result=learning.generate_english_triggers(c,policy,{'training-fixture'},diagnostic,prevention_compilation=True)
    status=diagnostic['english_trigger_learning']
    assert len(calls)==1 and status['model_calls']==3
    assert status['attempts'][1]['draft']==original_draft
    assert status['draft_reuse']['pre_action_repair_sources']==[mid]
    assert {row['memory_id'] for row in status['specific_conditions']['accepted_triggers']}=={mid,healthy}
    assert result['snapshot']==policy['snapshot']


def _duplicate_l1_native_fixture(tmp_path):
    import learning
    _,c,policy,query=_native_runtime_projection_fixture(tmp_path/'native')
    cards={card['memory_id']:card for card in policy['snapshot']['experiences']}
    refs={mid:{k:card[k] for k in ('memory_id','content_revision')} for mid,card in cards.items()}
    options=copy.deepcopy(policy['options'])
    options['loop_rules'][0]['source_experience']=refs['fixture-accepted']
    options['experience_overrides']=[{'source_experience':refs['fixture-accepted'],
        'content':cards['fixture-accepted']['content'],'trigger_pattern':'gmail collection'},
        {'source_experience':refs['fixture-excluded'],'content':cards['fixture-excluded']['content'],
         'trigger_pattern':'unmatched card'}]
    options['l1_prevention_sources']=['fixture-accepted']
    policy=json.loads(c.native(c.BINARY,c.ROOT,['runtime','recompile'],{'policy':policy,'options':options}))
    c.version=lambda artifact:c.native(c.BINARY,c.ROOT,['artifact-version'],artifact)
    c.read=lambda path:json.loads(Path(path).read_text());c.save=save
    c.TRAINING=tmp_path/'training';c.GEPA=tmp_path/'gepa';c.PUBLISHED=tmp_path/'published'
    c.RUNTIME=c.TRAINING/'runtime';c.TRAINING_SERVICE=c.RUNTIME/'training-service.json'
    c.OWNER=policy['snapshot']['identity']['user_id'];c.AGENT=policy['snapshot']['identity']['agent_id']
    c.SCOPE=cards['fixture-accepted']['target_scope'];c.environment=lambda:{}
    c.ids=lambda split:['local-training'] if split=='train' else ['fixture-one','fixture-two']
    c.evaluation_config=lambda tasks,artifact,workers:{'tasks':tasks,'artifact':artifact,'workers':workers}
    base={'schema_version':1,'level':'L3','user_id':c.OWNER,'policy':policy,
        'agent':{'name':c.AGENT,'revision':'fixture-v1',
                 'appworld_options':{'l1_window_mode':'observed_phase_v3'}}}
    counts={(refs[mid]['memory_id'],refs[mid]['content_revision']):{
        'regressed_tasks':int(mid=='fixture-accepted'),'improved_tasks':0,'failed_tasks':1,'passed_tasks':0}
            for mid in ('fixture-accepted','fixture-excluded')}
    before=json.loads(c.native(c.BINARY,c.ROOT,['runtime','query'],{'policy':policy,'query':query}))
    assert {ref['memory_id'] for ref in before['experiences']}=={'fixture-accepted','fixture-excluded'}
    return c,base,query,counts

def _duplicate_l1_test_reservation(c,base,query,counts):
    duplicate=c.PUBLISHED/'duplicate/strategy.json';prior=c.PUBLISHED/'parent/strategy.json'
    target=c.PUBLISHED/'next/strategy.json'
    for path in (duplicate,prior):
        save(path,base);save(path.with_name('manifest.json'),{'version':c.version(base)})
    comparison=c.GEPA/'parent-stage32/comparison.json';baseline=c.GEPA/'l0/result.json'
    version=c.version(base)
    items=[]
    for i,pair in enumerate(counts):
        task=f'fixture-{i}';trial=f'trial-{i}'
        items.append({'task_id':task,'trial_id':trial,'source_shard':'shard-00',
            'strict_passed':0,'execution_status':'completed','loaded_version':version})
        row={'task_id':task,'trial_id':trial,'evidence':{'loaded_version':version,'trace':{'events':[
            {'kind':'advice.applied','application':{'status':'applied'},'attributes':{'receipt':{
              'boundary':'llm','experiences':[dict(zip(('memory_id','content_revision'),pair))]}}}]}}}
        save(comparison.parent/'shards/shard-00/capture/trials'/f'{trial}.json',row)
    save(baseline,{'status':'complete','host_version':'fixture-host','items':[
        {'task_id':r['task_id'],'strict_passed':int(i==0)} for i,r in enumerate(items)]})
    save(comparison,{'status':'complete','host_version':'fixture-host','source_l0':str(baseline),'items':items})
    save(comparison.with_name('progress.json'),{'artifact':base})
    search={'round':1,'current_seed':str(target),'attempts':[{'seed':str(prior),'pilot_comparison':str(comparison)}],
        'automatic_seed_generation':{'round':1,'status':'pending','seed_label':'next','seed':str(target),
            'source_seed':str(prior),'method':'approved_training_native_seed',
            'duplicate_feedback':{'candidate':str(duplicate),'duplicate_of':str(prior)}},
        'generation_history':[{'status':'duplicate','seed':str(duplicate),'duplicate_of':str(prior),
            'seed_generation_diagnostic':{'control_learning':{'model_calls':3,'status':'generated','durable_draft':[]},
                'english_trigger_learning':{'model_calls':3,'status':'compiled','durable_draft':[]}}}]}
    save(c.GEPA/'status.json',{'seed_search':search})
    return target,search

def test_native_l1_disable_retains_another_l1_and_same_source_loop_authority(tmp_path):
    import learning
    c,base,query,counts=_duplicate_l1_native_fixture(tmp_path);before=copy.deepcopy(base)
    result,disabled=learning._compile_duplicate_l1_candidate(c,base,base['policy']['snapshot'],counts,{c.version(base)})
    assert disabled[0]['memory_id']=='fixture-accepted'
    assert result['policy']['options']['l1_prevention_sources']==[]
    actual=json.loads(c.native(c.BINARY,c.ROOT,['runtime','query'],{'policy':result['policy'],'query':query}))
    assert [r['memory_id'] for r in actual['experiences']]==['fixture-excluded']
    loop=copy.deepcopy(query);loop.update(boundary='loop',iteration=31)
    old=json.loads(c.native(c.BINARY,c.ROOT,['runtime','query'],{'policy':base['policy'],'query':loop}))
    new=json.loads(c.native(c.BINARY,c.ROOT,['runtime','query'],{'policy':result['policy'],'query':loop}))
    assert old['progress']['decision']==new['progress']['decision']=='stop'
    assert new['progress']['rule_id']==old['progress']['rule_id']=='fixture-loop'
    assert base==before and c.version(base)!=c.version(result)
    for field in ('snapshot','options'):
        if field=='snapshot':assert result['policy'][field]==base['policy'][field]
    changes={key for key in base['policy']['options'] if base['policy']['options'][key]!=result['policy']['options'][key]}
    assert changes=={'disabled_injections','l1_prevention_sources'}

def test_seen_native_versions_skipped_and_final_active_source_never_disabled(tmp_path):
    import learning
    c,base,_,counts=_duplicate_l1_native_fixture(tmp_path);seen={c.version(base)}
    first,_=learning._compile_duplicate_l1_candidate(c,base,base['policy']['snapshot'],counts,seen)
    seen.add(c.version(first))
    second,disabled=learning._compile_duplicate_l1_candidate(c,base,base['policy']['snapshot'],counts,seen)
    assert disabled[0]['memory_id']=='fixture-excluded';seen.add(c.version(second))
    with pytest.raises(RuntimeError,match='exploration_exhausted'):
        learning._compile_duplicate_l1_candidate(c,base,base['policy']['snapshot'],counts,seen)
    assert len(seen)==3

@pytest.mark.parametrize('problem',['revoked','revision','body'])
def test_current_native_authority_must_still_bind_every_frozen_source(tmp_path,problem):
    import learning
    c,base,_,counts=_duplicate_l1_native_fixture(tmp_path);authority=copy.deepcopy(base['policy']['snapshot'])
    if problem=='revoked':authority['experiences'].pop()
    elif problem=='revision':authority['experiences'][0]['content_revision']='wrong'
    else:authority['experiences'][0]['content']='changed text'
    with pytest.raises(RuntimeError,match='no_longer_approved'):
        learning._compile_duplicate_l1_candidate(c,base,authority,counts,set())

def test_normal_freeze_consumes_duplicate_receipt_and_preserves_three_paid_calls(tmp_path,monkeypatch):
    import learning
    c,base,query,counts=_duplicate_l1_native_fixture(tmp_path);target,search=_duplicate_l1_test_reservation(c,base,query,counts)
    actual_native=c.native
    def native(binary,root,operation,packet):
        if operation==['learn','--config',str(c.TRAINING_SERVICE),'--refresh-evidence']:
            return json.dumps({'status':'completed','model_calls':0})
        return actual_native(binary,root,operation,packet)
    c.native=Mock(side_effect=native)
    monkeypatch.setattr(learning,'_completed_training_tasks',lambda controller:{'local-training'})
    monkeypatch.setattr(learning,'_readiness',lambda controller:tmp_path/'native/memory.json')
    control=Mock(side_effect=AssertionError('duplicate must not add paid control calls'))
    english=Mock(side_effect=AssertionError('duplicate must not add paid translation calls'))
    monkeypatch.setattr(learning,'generate_seed_controls',control)
    monkeypatch.setattr(learning,'generate_english_triggers',english)
    before=copy.deepcopy(base)
    result=learning.freeze_seed(c,label='next')
    assert control.call_count==english.call_count==0 and c.version(result)!=c.version(base)
    assert c.read(target)==result and c.read(c.PUBLISHED/'duplicate/strategy.json')==before
    diagnostic=c.read(c.TRAINING/'seed-compile-status.json')
    assert diagnostic['control_learning']['model_calls']==diagnostic['english_trigger_learning']['model_calls']==3
    assert diagnostic['duplicate_l1_exploration']['model_calls']==0
    assert diagnostic['duplicate_l1_exploration']['causal_effect']=='unmeasured_requires_pilot'
    assert diagnostic['seed_label']=='next' and diagnostic['published'] is True
    assert any(call.args[2]==['shadow','compile'] for call in c.native.call_args_list)

@pytest.mark.parametrize('problem',['no_receipt','wrong_label','not_duplicate','unapplied','wrong_boundary'])
def test_missing_or_invalid_receipts_do_not_create_ablation_authority(tmp_path,problem):
    import learning
    c,base,query,counts=_duplicate_l1_native_fixture(tmp_path);target,search=_duplicate_l1_test_reservation(c,base,query,counts)
    if problem=='no_receipt':
        search['automatic_seed_generation'].pop('duplicate_feedback')
        save(c.GEPA/'status.json',{'seed_search':search})
        assert learning._duplicate_l1_generation(c,target,'next') is None;return
    if problem=='wrong_label':search['automatic_seed_generation']['seed_label']='other'
    elif problem=='not_duplicate':search['generation_history'][0]['status']='frozen'
    else:
        trial=c.GEPA/'parent-stage32/shards/shard-00/capture/trials/trial-0.json'
        row=c.read(trial);event=row['evidence']['trace']['events'][0]
        if problem=='unapplied':event['application']['status']='rejected'
        else:event['attributes']['receipt']['boundary']='loop'
        save(trial,row)
    save(c.GEPA/'status.json',{'seed_search':search})
    if problem in ('wrong_label','not_duplicate'):
        with pytest.raises(RuntimeError,match='receipt_invalid'):
            learning._duplicate_l1_generation(c,target,'next')
    else:
        observed,_=learning._duplicate_l1_applications(c,search,base)
        with pytest.raises(RuntimeError,match='exploration_exhausted'):
            learning._compile_duplicate_l1_candidate(c,base,base['policy']['snapshot'],observed,set())



def test_duplicate_ablation_skips_retained_native_version_after_publish_pointer_changes(tmp_path,monkeypatch):
    import learning
    c,base,query,counts=_duplicate_l1_native_fixture(tmp_path)
    target,search=_duplicate_l1_test_reservation(c,base,query,counts)
    already_seen,_=learning._compile_duplicate_l1_candidate(c,base,base['policy']['snapshot'],counts,{c.version(base)})
    mutable=c.PUBLISHED/'strategy/strategy.json'
    save(mutable,already_seen)
    comparison=Path(search['attempts'][0]['pilot_comparison'])
    save(comparison.with_name('progress.json'),{'artifact':already_seen})
    search['attempts'][0]['seed']=str(mutable)
    # A later publication moves the pointer; the original native artifact
    # remains bound to the retained progress record, not its new manifest.
    save(mutable,base)
    save(mutable.with_name('manifest.json'),{'version':c.version(base)})
    save(c.GEPA/'status.json',{'seed_search':search})
    reservation=learning._duplicate_l1_generation(c,target,'next')
    # This test isolates the seen-version index. Formal paired feedback is
    # independently bound in the normal-entry test; it never borrows scores
    # from the differently versioned retained artifact.
    monkeypatch.setattr(learning,'_duplicate_l1_applications',lambda *args:(counts,comparison))
    result=learning._freeze_duplicate_l1_candidate(c,target,'next',base['policy']['snapshot'],{'seed_label':'next'},reservation)
    assert c.version(result) not in {c.version(base),c.version(already_seen)}
    assert [ref['memory_id'] for ref in result['policy']['options']['disabled_injections']]==['fixture-excluded']
    assert result['policy']['options']['loop_rules']==base['policy']['options']['loop_rules']

def _incremental_directory_fixture(tmp_path, monkeypatch):
    learning,c,policy,feedback,_=_english_trigger_fixture(tmp_path)
    base=learning._english_trigger_sources(c,policy,feedback)
    template=copy.deepcopy(policy['snapshot']['experiences'][0])
    ids=['healthy','late-z-cart','late-z-quantity']+[f'new-{i:02d}' for i in range(9)]
    cards=[dict(template,memory_id=mid,content_revision='revision-'+mid) for mid in ids]
    policy['snapshot']['experiences']=cards
    policy['options']['approved_experiences']=[{k:r[k] for k in ('memory_id','content_revision')} for r in cards]
    sources=learning._EnglishTriggerSources(runtime_runs=base.runtime_runs)
    for card in cards:
        sources[card['memory_id']]=dict(copy.deepcopy(base['auth-experience']),experience=card)
    packet={'memories':[{'memory_id':r['memory_id'],'scope':c.SCOPE,'condition':r['trigger_pattern'],
        'problem':'A reviewed API parameter failure.','experience':r['content']} for r in cards],
        'training_statistics':{'count':12}}
    monkeypatch.setattr(learning,'seed_control_input',lambda *args:(packet,feedback,{}))
    monkeypatch.setattr(learning,'_english_trigger_sources',lambda *args,**kwargs:sources)
    return learning,c,policy,sources,packet


def test_incremental_directory_requeues_native_dropped_inheritance_before_other_cards(tmp_path,monkeypatch):
    learning,c,policy,sources,packet=_incremental_directory_fixture(tmp_path,monkeypatch)
    fixed={'healthy':'login access_token','late-z-cart':'login token','late-z-quantity':'login token'}
    monkeypatch.setattr(learning,'_historical_english_inheritance',lambda *args:{
        'fixed_conditions':fixed,'incremental_ids':set(sources)-set(fixed),
        'diagnostic':{'eligible_conditions':3,'incremental_sources':9}})
    submitted=[]
    def select(controller,original,current_sources,drafts,status,**options):
        rows=dict(options['fixed_conditions'])
        rows.update({r['memory_id']:r['trigger_pattern'] for d in drafts for r in d})
        if submitted:
            for mid in ('late-z-cart','late-z-quantity'):
                if rows.get(mid)=='login token':
                    rows.pop(mid);status.setdefault('rejected',[]).append({'memory_id':mid,
                        'reason':'appworld_english_trigger_native_pre_action_not_matched'})
        accepted=[{'memory_id':mid,'trigger_pattern':trigger} for mid,trigger in sorted(rows.items())]
        status.update(accepted_triggers=accepted,accepted_count=len(accepted))
        current_sources.last_profile={'validated_condition_count':len(accepted)}
        result=copy.deepcopy(original)
        result['options']['experience_overrides']=[{'source_experience':{k:current_sources[r['memory_id']]['experience'][k]
            for k in ('memory_id','content_revision')},'content':current_sources[r['memory_id']]['experience']['content'],
            'trigger_pattern':r['trigger_pattern']} for r in accepted]
        return result
    monkeypatch.setattr(learning,'_specific_english_pairs',select)
    def reply(controller,outgoing,reason,transport):
        ids=[r['memory_id'] for r in outgoing['memories']]
        assert all(set(r)=={'memory_id','scope','condition','problem','experience'} for r in outgoing['memories'])
        assert 'healthy' not in ids
        if len(submitted)==1:
            assert ids[:2]==['late-z-cart','late-z-quantity']
            assert reason=={'reason':'appworld_english_trigger_native_pre_action_not_matched','memory_id':'late-z-cart'}
        submitted.append(ids);transport['model_calls']+=1
        return {'triggers':[{'memory_id':mid,'trigger_pattern':'login access_token'} for mid in ids]}, {'total_tokens':10}
    paid=Mock(side_effect=reply);monkeypatch.setattr(learning,'_english_trigger_model_reply',paid)
    diagnostic={};before=copy.deepcopy(policy)
    result=learning.generate_english_triggers(c,policy,set(),diagnostic)
    status=diagnostic['english_trigger_learning'];generation=status['incremental_generation']
    assert len(submitted)==status['model_calls']==generation['model_calls']==generation['model_attempts']==3
    assert {'late-z-cart','late-z-quantity'}<=set(submitted[1])
    assert set(sources)=={r['memory_id'] for r in status['specific_conditions']['accepted_triggers']}
    assert generation['batches'][1]['repair_memory_ids']==['late-z-cart','late-z-quantity']
    assert generation['repair_sources']==[] and generation['unsubmitted_sources']==0
    assert policy==before and result['snapshot']==policy['snapshot']
    old_drafts=copy.deepcopy([r['draft'] for r in generation['batches']])
    restored=learning.generate_english_triggers(c,policy,set(),diagnostic)
    assert paid.call_count==3 and restored==result
    assert [r['draft'] for r in diagnostic['english_trigger_learning']['incremental_generation']['batches']]==old_drafts


@pytest.mark.parametrize('change',['withdrawn','revision'])
def test_incremental_directory_rejects_saved_source_withdrawal_without_paid_retry(tmp_path,monkeypatch,change):
    learning,c,policy,sources,packet=_incremental_directory_fixture(tmp_path,monkeypatch)
    ref=dict(policy['options']['approved_experiences'][0])
    previous={'model_calls':2,'incremental_generation':{'source_experiences':[ref],
        'model_calls':2,'model_attempts':2,'batches':[]}}
    if change=='withdrawn':policy['options']['approved_experiences'].remove(ref)
    else:previous['incremental_generation']['source_experiences'][0]['content_revision']='old-revision'
    status={'model_calls':2,'specific_conditions':{'accepted_triggers':[]}}
    paid=Mock(side_effect=AssertionError('invalid source cannot grant a new call'))
    monkeypatch.setattr(learning,'_english_trigger_model_reply',paid)
    with pytest.raises(ValueError,match='incremental_source_changed'):
        learning._expand_english_trigger_subset(c,policy,policy,sources,packet,
            {'english_trigger_learning':status},previous)
    assert status['model_calls']==2
    paid.assert_not_called()


def test_incremental_directory_changed_missing_subset_keeps_three_spent_calls(tmp_path,monkeypatch):
    learning,c,policy,sources,packet=_incremental_directory_fixture(tmp_path,monkeypatch)
    old_binding=[r for r in policy['options']['approved_experiences'] if r['memory_id'].startswith('new-')]
    previous={'model_calls':3,'incremental_generation':{'source_experiences':old_binding,
        'model_calls':3,'model_attempts':3,'batches':[{'attempt':i,'status':'rejected','memory_ids':[]} for i in (1,2,3)]}}
    status={'model_calls':3,'specific_conditions':{'accepted_triggers':[{'memory_id':'healthy','trigger_pattern':'login access_token'}]}}
    paid=Mock(side_effect=AssertionError('changed directory cannot grant a fourth call'))
    monkeypatch.setattr(learning,'_english_trigger_model_reply',paid)
    learning._expand_english_trigger_subset(c,policy,policy,sources,packet,{'english_trigger_learning':status},previous)
    generation=status['incremental_generation']
    assert status['model_calls']==generation['model_calls']==generation['model_attempts']==3
    assert len(generation['source_experiences'])==len(sources)
    assert generation['unsubmitted_sources']==len(sources)-1
    paid.assert_not_called()


def test_incremental_directory_cold_native_generation_repairs_precorrective_source(tmp_path,monkeypatch):
    learning,c,policy,card,sources,before,after,trace=_native_precorrective_fixture(tmp_path)
    c.TRAINING=tmp_path;c.save=save;c.environment=lambda:{}
    # Compile a separate healthy approved source into the same native snapshot.
    memory=tmp_path/'memory.json';store=json.loads(memory.read_text())
    healthy_entry=copy.deepcopy(store['entries'][0]);healthy_entry['memory_id']='fixture-healthy-note'
    healthy_entry['candidate'].update(candidate_id='fixture-note-candidate',
        trigger_pattern='simple_note show_note',proposed_change='Read the requested note using show_note.')
    store['entries'].append(healthy_entry);save(memory,store)
    shadow=policy['snapshot']['options'];identity=policy['snapshot']['identity']
    snapshot=json.loads(c.native(c.BINARY,c.ROOT,['shadow','compile'],{'memory_path':str(memory),'identity':identity,'options':shadow}))
    options=copy.deepcopy(policy['options']);options['approved_experiences']=[{k:r[k] for k in ('memory_id','content_revision')} for r in snapshot['experiences']]
    policy=json.loads(c.native(c.BINARY,c.ROOT,['runtime','compile'],{'memory_path':str(memory),'identity':identity,'shadow_options':shadow,'options':options}))
    cards={r['memory_id']:r for r in policy['snapshot']['experiences']};mid=card['memory_id'];healthy='fixture-healthy-note'
    sources[mid]['experience']=cards[mid]
    before['advice_query']['state']+=' apis.simple_note.show_note()'
    sources.runtime_codes[('fixture-run','before-write')]+=' apis.simple_note.show_note()'
    sources[healthy]={'experience':cards[healthy],'evidence':[{'run_id':'fixture-run','query_id':'before-write',
        'task':before['advice_query']['task'],'pre_action_eligible':True}]}
    packet={'memories':[{'memory_id':r['memory_id'],'scope':r['target_scope'],'condition':r['trigger_pattern'],
        'problem':'A reviewed correction was unavailable before a write.','experience':r['content']} for r in cards.values()],
        'training_statistics':{'count':2}}
    monkeypatch.setattr(learning,'seed_control_input',lambda *args:(packet,{'scanned_tasks':['training-fixture']},{}))
    monkeypatch.setattr(learning,'_english_trigger_sources',lambda *args,**kwargs:sources)
    monkeypatch.setattr(learning,'_historical_english_inheritance',lambda *args:{
        'fixed_conditions':{mid:'clear_cart add_product_to_cart',healthy:'simple_note show_note'},
        'incremental_ids':set(),'diagnostic':{'eligible_conditions':2,'incremental_sources':0}})
    calls=[]
    def repair(controller,outgoing,reason=None,transport=None):
        assert [r['memory_id'] for r in outgoing['memories']]==[mid]
        assert reason=={'reason':'appworld_english_trigger_no_specific_supported_pair','memory_id':mid}
        assert set(outgoing['memories'][0])=={'memory_id','scope','condition','problem','experience'}
        transport['model_calls']+=1;calls.append(reason)
        return {'triggers':[{'memory_id':mid,'trigger_pattern':'amazon show_cart'}]}, {'total_tokens':10}
    monkeypatch.setattr(learning,'_english_trigger_model_reply',repair)
    original=copy.deepcopy(policy);diagnostic={}
    result=learning.generate_english_triggers(c,policy,{'training-fixture'},diagnostic,prevention_compilation=True)
    status=diagnostic['english_trigger_learning']
    assert len(calls)==status['model_calls']==1
    assert {r['memory_id'] for r in status['specific_conditions']['accepted_triggers']}=={mid,healthy}
    assert learning._native_l1_profile(c,result,sources)[('fixture-run','before-write')]=={mid,healthy}
    assert policy==original and result['snapshot']==policy['snapshot']
    assert all(r['content']==cards[r['source_experience']['memory_id']]['content'] for r in result['options']['experience_overrides'])
    paid=Mock(side_effect=AssertionError('healthy saved conditions need no retransmission'))
    monkeypatch.setattr(learning,'_english_trigger_model_reply',paid)
    restored=learning.generate_english_triggers(c,policy,{'training-fixture'},diagnostic,prevention_compilation=True)
    assert restored==result and diagnostic['english_trigger_learning']['model_calls']==1
    paid.assert_not_called()


def test_incremental_directory_saved_global_paid_count_is_not_added_twice(tmp_path,monkeypatch):
    learning,c,policy,sources,packet=_incremental_directory_fixture(tmp_path,monkeypatch)
    bindings=copy.deepcopy(policy['options']['approved_experiences'])
    previous={'model_calls':2,'incremental_generation':{'source_experiences':bindings,
        'model_calls':1,'model_attempts':2,'batches':[
            {'attempt':i,'status':'compiled','memory_ids':[],'usage':{'total_tokens':10},'draft':[]} for i in (1,2)]}}
    status={'model_calls':2,'specific_conditions':{'accepted_triggers':[{'memory_id':'healthy','trigger_pattern':'login access_token'}]}}
    calls=[]
    def select(controller,original,current_sources,drafts,filtering,**options):
        rows=dict(options['fixed_conditions'])
        rows.update({r['memory_id']:r['trigger_pattern'] for d in drafts for r in d})
        filtering.update(accepted_triggers=[{'memory_id':mid,'trigger_pattern':pattern} for mid,pattern in rows.items()],
            accepted_count=len(rows))
        current_sources.last_profile={'validated_condition_count':len(rows)}
        return copy.deepcopy(original)
    monkeypatch.setattr(learning,'_specific_english_pairs',select)
    def reply(controller,outgoing,reason,transport):
        assert status['model_calls']==2
        assert status['incremental_generation']['model_calls']==2
        calls.append([r['memory_id'] for r in outgoing['memories']]);transport['model_calls']+=1
        return {'triggers':[{'memory_id':mid,'trigger_pattern':'login access_token'} for mid in calls[-1]]},{'total_tokens':10}
    monkeypatch.setattr(learning,'_english_trigger_model_reply',reply)
    learning._expand_english_trigger_subset(c,policy,policy,sources,packet,{'english_trigger_learning':status},previous)
    assert len(calls)==1
    generation=status['incremental_generation']
    assert status['model_calls']==generation['model_calls']==generation['model_attempts']==3


@pytest.mark.parametrize('bad_ref',[
    {'memory_id':'unknown-source','content_revision':'unknown-revision'},
    {'memory_id':['healthy'],'content_revision':'revision-healthy'}])
def test_incremental_directory_rejects_unknown_or_nonstring_receipt_before_transport(tmp_path,monkeypatch,bad_ref):
    learning,c,policy,sources,packet=_incremental_directory_fixture(tmp_path,monkeypatch)
    previous={'model_calls':2,'incremental_generation':{'source_experiences':[bad_ref],
        'model_calls':2,'model_attempts':2,'batches':[]}}
    status={'model_calls':2,'specific_conditions':{'accepted_triggers':[]}}
    paid=Mock(side_effect=AssertionError('unknown receipt cannot authorize a call'))
    monkeypatch.setattr(learning,'_english_trigger_model_reply',paid)
    with pytest.raises(ValueError,match='incremental_source_changed'):
        learning._expand_english_trigger_subset(c,policy,policy,sources,packet,{'english_trigger_learning':status},previous)
    assert status['model_calls']==2
    paid.assert_not_called()


def test_public_pre_action_get_contracts_parse_once_and_keep_source_namespace(tmp_path, monkeypatch):
    import learning
    from types import SimpleNamespace
    docs = tmp_path/'examples/agent/business-simulator/appworld/runtime/world/data/api_docs/standard'
    docs.mkdir(parents=True)
    (docs/'alpha.json').write_text(json.dumps({
        'update_cart': {'method':'POST','parameters':[{'name':'quantity'}]},
        'show_cart': {'method':'get','response_schemas':{'success':{'quantity':0}}},
        'delete_cart': {'method':'DELETE','parameters':[{'name':'quantity'}]}}))
    (docs/'beta.json').write_text(json.dumps({
        'update_report': {'method':'POST','parameters':[{'name':'amount'}]},
        'show_report': {'method':'GET','response_schemas':{'success':{'amount':0}}}}))
    monkeypatch.setattr(learning,'__file__',str(tmp_path/'a/b/c/d/learning.py'))
    monkeypatch.setattr(learning._public_l1_detail_contracts,'cached',None,raising=False)
    monkeypatch.setattr(learning._public_l1_detail_contracts,'pre_action_reads',None,raising=False)
    actual_read = Path.read_text
    parsed = []
    def read(path,*args,**kwargs):
        if path.parent == docs: parsed.append(path.name)
        return actual_read(path,*args,**kwargs)
    monkeypatch.setattr(Path,'read_text',read)
    cards=[{'trigger_pattern':'apis.alpha.update_cart','content':'Check quantity before write'},
           {'trigger_pattern':'apis.beta.update_report','content':'Check amount before write'},
           {'trigger_pattern':'unlisted_action','content':'quantity is present but no documented source API'}]
    before=copy.deepcopy(cards)
    for _ in range(5):
        assert learning._experience_pre_action_operations(cards[0])=={'update_cart','show_cart'}
        assert learning._experience_pre_action_operations(cards[1])=={'update_report','show_report'}
        assert learning._experience_pre_action_operations(cards[2])==set()
    assert sorted(parsed)==['alpha.json','beta.json']
    assert cards==before


@pytest.mark.parametrize('order',['chronological','unknown','equal'])
def test_candidate_model_end_index_preserves_first_status_and_original_query_order(monkeypatch,order):
    import learning
    from types import SimpleNamespace
    monkeypatch.setattr(learning,'_observed_phase_causal_input',lambda event,by_id:{'public_fixture':True})
    monkeypatch.setattr(learning,'_candidate_l1_replay_packet',lambda *args,**kwargs:
        {'apply_policy':True,'state':{'observation':'same public evidence'}})
    events=[{'event_id':'run','kind':'run.start','timestamp_us':1,'input':{'instruction':'public fixture'}}]
    for index in range(3):
        step='same-step';execution=None if index==2 else 'execution-'+str(index)
        events.extend([
            {'event_id':'step-'+str(index),'step_id':step,'kind':'step.start',
             'timestamp_us':index*10+2,'attributes':{'apply_policy':True}},
            {'event_id':'query-'+str(index),'step_id':step,'execution_id':execution,'actor_id':'skill_executor',
             'timestamp_us':index*10+3,'kind':'action.start','attributes':{'semantic':'model',
                 'request_binding':{'mode':'system_message','pointer':'/messages'}},
             'input':{'messages':[{'role':'user','content':'current window'}]}},
            {'event_id':'tool-'+str(index),'step_id':step,'execution_id':execution,'kind':'action.end',
             'timestamp_us':index*10+4,'attributes':{'semantic':'tool'},'status':'completed'},
            {'event_id':'first-'+str(index),'step_id':step,'execution_id':execution,'kind':'action.end',
             'timestamp_us':index*10+5,'attributes':{'semantic':'model'},
             'status':'failed' if index==0 else 'completed'},
            {'event_id':'second-'+str(index),'step_id':step,'execution_id':execution,'kind':'action.end',
             'timestamp_us':index*10+6,'attributes':{'semantic':'model'},'status':'failed'}])
    if order=='chronological':events.reverse()
    elif order=='unknown':events[0].pop('timestamp_us');events.reverse()
    else:
        for event in events:event['timestamp_us']=1
    trace={'identity':{'run_id':'fixture-run'},'events':events};before=copy.deepcopy(trace)
    replay={};policy={'snapshot':{'identity':{'user_id':'fixture'}}}
    c=SimpleNamespace(ROOT=Path('/unused-fixture-root'),SCOPE='agent:fixture/actor:default')
    queries=learning._l1_runtime_queries(c,policy,trace,{},candidate_options={},replay_inputs=replay)
    ordered=events
    if order=='chronological':ordered=sorted(events,key=lambda event:event['timestamp_us'])
    expected=[event['event_id'] for event in ordered if event['kind']=='action.start']
    assert [query['advice_query']['query_id'] for query in queries]==expected
    for event in (row for row in ordered if row['kind']=='action.start'):
        first=next(row for row in ordered if row['kind']=='action.end'
            and row.get('step_id')==event.get('step_id')
            and row.get('execution_id')==event.get('execution_id')
            and row['attributes']['semantic']=='model')
        assert replay[('fixture-run',event['event_id'])]['completed_model']==(first['status']=='completed')
    assert trace==before


from copy import deepcopy
from types import SimpleNamespace
import learning


def _generation_receipt_controller(tmp_path):
    def read(path):
        path = Path(path).resolve()
        assert path.is_relative_to(tmp_path.resolve()), 'read outside private fixture'
        return json.loads(path.read_text())
    def save(path, value):
        path = Path(path).resolve()
        assert path.is_relative_to(tmp_path.resolve()), 'write outside private fixture'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value))
    c = SimpleNamespace(TRAINING=tmp_path/'training', PUBLISHED=tmp_path/'published',
        RUNTIME=tmp_path/'runtime', ROOT=tmp_path, OWNER='fixture-owner', AGENT='fixture-agent',
        SCOPE='fixture-scope', BINARY=tmp_path/'native-not-launched', read=read, save=save,
        environment=lambda:{}, version=lambda artifact:'fixture-native-version',
        ids=lambda split:['train-fixture'] if split=='train' else ['dev-fixture'],
        evaluation_config=lambda *args:{})
    c.TRAINING_SERVICE=c.RUNTIME/'training-service.json'
    return c


def _generation_receipt_artifact(c):
    cards=[{'memory_id':mid,'content_revision':'revision-'+mid,'target_scope':c.SCOPE,
        'trigger_pattern':'login access_token','content':'Use the documented keyword.'} for mid in ('healthy','dropped')]
    ref={k:cards[0][k] for k in ('memory_id','content_revision')}
    return {'schema_version':1,'level':'L3','user_id':c.OWNER,'agent':{'name':c.AGENT},
        'policy':{'snapshot':{'experiences':cards,'options':{'require_validated_evidence':True}},
            'options':{'approved_experiences':[{k:card[k] for k in ref} for card in cards],
                'loop_rules':[{'source_experience':ref}]}}}


def _generation_receipt_diagnostic(label='candidate-receipt'):
    submitted=[{'memory_id':mid,'trigger_pattern':'login access_token'} for mid in ('healthy','dropped')]
    return {'seed_label':label,'control_learning':{'status':'generated','model_calls':2,'model_attempts':2,
        'attempts':[{'attempt':1,'status':'rejected','usage':None},
                    {'attempt':2,'status':'generated','usage':{'total_tokens':11,'unrelated_field':'private'}}],
        'validated_model_draft':{'action_rules':[],'loop_rules':[{'memory_id':'healthy',
            'max_iterations':30,'max_no_progress':3,'max_replans':1,'replan_on_failure':True}], 'schedule_rules':[]},
        'full_feedback':{'raw_trace':'not a receipt'}},
        'english_trigger_learning':{'status':'compiled','model_calls':3,'attempts':[],
            'specific_conditions':{'accepted_triggers':submitted,'cards':['large diagnosis']},
            'incremental_generation':{'status':'complete','model_calls':3,'model_attempts':3,
                'max_model_calls':3,'source_experiences':[{'memory_id':mid,'content_revision':'revision-'+mid}
                    for mid in ('healthy','dropped')],
                'batches':[{'attempt':3,'status':'compiled','draft':submitted,'accepted_count':1,
                    'memory_ids':['healthy','dropped'],'usage':{'total_tokens':17},
                    'filtering':{'runtime_profile':['large windows']}}]},
            'runtime_profile':{'full_windows':['never copied']}}}


def test_generation_receipt_normal_freeze_and_resume_without_paid_transport(tmp_path,monkeypatch):
    c=_generation_receipt_controller(tmp_path);artifact=_generation_receipt_artifact(c);original=_generation_receipt_diagnostic()
    memory=c.TRAINING/'memory.json';c.save(memory,{'entries':[]})
    c.save(c.RUNTIME/'service.json',{'runtime_options':{}})
    c.save(c.PUBLISHED/'L0.json',dict(artifact,level='L0',policy=None))
    template=c.ROOT/'tools/trajectory_evolution/config/prompts/runtime-experience.txt'
    template.parent.mkdir(parents=True);template.write_text('Approved advice')
    monkeypatch.setattr(learning,'_completed_training_tasks',lambda *args:{'train-fixture'})
    monkeypatch.setattr(learning,'_readiness',lambda *args:memory)
    monkeypatch.setattr(learning,'_compile_seed_snapshot',lambda *args:deepcopy(artifact['policy']['snapshot']))
    monkeypatch.setattr(learning,'_duplicate_l1_generation',lambda *args:None)
    c.native=Mock(side_effect=[json.dumps({'status':'completed','model_calls':0}),json.dumps(artifact['policy'])])
    def controls(controller,policy,tasks,diagnostic):
        diagnostic['control_learning']=deepcopy(original['control_learning']);return policy
    def english(controller,policy,tasks,diagnostic,**kwargs):
        diagnostic['english_trigger_learning']=deepcopy(original['english_trigger_learning']);return policy
    monkeypatch.setattr(learning,'generate_seed_controls',controls)
    monkeypatch.setattr(learning,'generate_english_triggers',english)
    monkeypatch.setattr(learning,'_prune_appworld_runtime_experiences',lambda c,p,d:p)
    monkeypatch.setattr(learning,'_compile_appworld_experience_template',lambda c,p:p)
    monkeypatch.setattr(learning,'_seed_l1_window_options',lambda c,a:a)
    monkeypatch.setattr(learning,'_compile_seed_prevention_sources',lambda c,a,t,d:a)
    frozen=learning.freeze_seed(c,label=original['seed_label'])
    target=c.PUBLISHED/original['seed_label']/'strategy.json';before=target.read_bytes()
    manifest=c.read(target.with_name('manifest.json'));receipt=manifest['generation_receipt']
    assert set(receipt)=={'schema_version','seed_label','available_detail','control_learning','english_trigger_learning'}
    assert 'version' not in receipt and manifest['version']=='fixture-native-version'
    encoded=json.dumps(receipt)
    assert all(word not in encoded for word in ('raw_trace','full_feedback','runtime_profile','accepted_triggers','filtering','snapshot'))
    assert receipt['control_learning']['attempts'][0]['usage'] is None
    assert receipt['control_learning']['attempts'][1]['usage']=={'total_tokens':11}
    assert receipt['control_learning']['validated_model_draft']==original['control_learning']['validated_model_draft']
    assert len(receipt['english_trigger_learning']['incremental_generation']['batches'][0]['draft'])==2
    # Another candidate replaces the single mutable diagnostic; no model or native compile is repeated.
    c.save(c.TRAINING/'seed-compile-status.json',{'seed_label':'other-label','control_learning':{'model_calls':0}})
    forbidden=Mock(side_effect=AssertionError('resume cannot invoke model/control compilation'))
    monkeypatch.setattr(learning,'generate_seed_controls',forbidden);monkeypatch.setattr(learning,'generate_english_triggers',forbidden)
    c.native=forbidden
    assert learning.freeze_seed(c,label=original['seed_label'])==frozen
    restored=c.read(c.TRAINING/'seed-compile-status.json')
    assert restored['control_learning']['model_calls']==2 and restored['english_trigger_learning']['model_calls']==3
    assert restored['resume']['model_calls']==0
    assert restored['generation_receipt_recovery']['runtime_evidence_available'] is False
    assert target.read_bytes()==before
    forbidden.assert_not_called()


@pytest.mark.parametrize('change',['version','label','calls'])
def test_generation_receipt_mismatch_never_restores_or_writes(tmp_path,change):
    c=_generation_receipt_controller(tmp_path);artifact=_generation_receipt_artifact(c);diagnostic=_generation_receipt_diagnostic()
    target=c.PUBLISHED/diagnostic['seed_label']/'strategy.json';c.save(target,artifact)
    receipt=learning._compact_seed_generation_receipt(c,artifact['policy'],diagnostic)
    manifest={'version':c.version(artifact),'validated_evidence_required':True,'generation_receipt':receipt}
    if change=='version':manifest['version']='different-native-version'
    if change=='label':receipt['seed_label']='other-label'
    if change=='calls':receipt['control_learning']['model_calls']=None
    c.save(target.with_name('manifest.json'),manifest)
    with pytest.raises(RuntimeError,match='generation_receipt_mismatch'):
        learning._resume_frozen_seed(c,target,diagnostic['seed_label'])
    assert not (c.TRAINING/'seed-compile-status.json').exists()


def test_generation_receipt_incomplete_legacy_diagnostic_stays_unknown(tmp_path):
    c=_generation_receipt_controller(tmp_path);artifact=_generation_receipt_artifact(c)
    partial={'seed_label':'candidate-partial','status':'diagnostic_partially_recovered',
        'control_learning':{'status':'generated_diagnostic_incomplete','model_calls':2,'draft_available':False},
        'english_trigger_learning':{'status':'compiled_diagnostic_incomplete','model_calls':3,'usage_complete':False}}
    assert learning._compact_seed_generation_receipt(c,artifact['policy'],partial) is None
    target=c.PUBLISHED/partial['seed_label']/'strategy.json';c.save(target,artifact)
    c.save(target.with_name('manifest.json'),{'version':c.version(artifact),'validated_evidence_required':True})
    c.save(c.TRAINING/'seed-compile-status.json',partial)
    learning._resume_frozen_seed(c,target,partial['seed_label'])
    restored=c.read(c.TRAINING/'seed-compile-status.json')
    assert restored['control_learning']==partial['control_learning']
    assert restored['english_trigger_learning']==partial['english_trigger_learning']
    assert 'generation_receipt_recovery' not in restored


def test_generation_receipt_expand_retains_failed_submitted_sibling_without_fourth_call(tmp_path,monkeypatch):
    c=_generation_receipt_controller(tmp_path);policy=_generation_receipt_artifact(c)['policy']
    class Sources(dict):pass
    sources=Sources({row['memory_id']:{'experience':row} for row in policy['snapshot']['experiences']})
    sources.last_profile={}
    packet={'memories':[{'memory_id':mid} for mid in sources]}
    previous={'model_calls':2,'attempts':[]}
    status={'model_calls':2,'specific_conditions':{'accepted_triggers':[]}}
    submitted=[{'memory_id':mid,'trigger_pattern':'login access_token'} for mid in sources]
    def reply(controller,outgoing,reason,transport):
        transport['model_calls']+=1;return {'triggers':deepcopy(submitted)},{'total_tokens':10}
    paid=Mock(side_effect=reply)
    monkeypatch.setattr(learning,'_english_trigger_model_reply',paid)
    monkeypatch.setattr(learning,'_english_trigger_item',lambda item,*args:(item['memory_id'],item['trigger_pattern']))
    def filtered(controller,original,current,drafts,filtering,**kwargs):
        filtering.update(accepted_triggers=[submitted[0]],accepted_count=1)
        return deepcopy(original)
    native_recheck=Mock(side_effect=filtered)
    monkeypatch.setattr(learning,'_specific_english_pairs',native_recheck)
    diagnostic={'english_trigger_learning':status}
    learning._expand_english_trigger_subset(c,policy,policy,sources,packet,diagnostic,previous)
    row=status['incremental_generation']['batches'][0]
    assert row['accepted_count']==1 and [r['memory_id'] for r in row['draft']]==list(sources)
    assert status['model_calls']==3 and paid.call_count==1
    restored=deepcopy(status);status2={'model_calls':3,'specific_conditions':{'accepted_triggers':[]}}
    learning._expand_english_trigger_subset(c,policy,policy,sources,packet,{'english_trigger_learning':status2},restored)
    assert paid.call_count==1 and status2['model_calls']==3
    assert len(status2['incremental_generation']['batches'][0]['draft'])==2
    assert native_recheck.call_count>=2




def test_generation_receipt_orphan_strategy_recovers_only_original_native_identical_diagnostic(tmp_path):
    for mode in ('same','changed','native_rejected','partial','wrong_label','wrong_scope'):
        c=_generation_receipt_controller(tmp_path/mode);artifact=_generation_receipt_artifact(c);diagnostic=_generation_receipt_diagnostic()
        diagnostic.update(scope=c.SCOPE,validated_evidence_required=True)
        target=c.PUBLISHED/diagnostic['seed_label']/'strategy.json';c.save(target,artifact)
        before=target.read_bytes();manifest=target.with_name('manifest.json')
        if mode=='partial':diagnostic['status']='diagnostic_partially_recovered'
        if mode=='wrong_label':diagnostic['seed_label']='different-label'
        if mode=='wrong_scope':diagnostic['scope']='different-scope'
        c.save(c.TRAINING/'seed-compile-status.json',diagnostic)
        def local_native(binary,root,arguments,packet):
            assert arguments==['runtime','recompile'] and packet['policy']==artifact['policy']
            assert packet['options']==artifact['policy']['options']
            if mode=='native_rejected':raise RuntimeError('native_runtime_recompile_failed')
            result=deepcopy(artifact['policy'])
            if mode=='changed':result['options']['changed']=True
            return json.dumps(result)
        c.native=Mock(side_effect=local_native)
        if mode in ('changed','native_rejected'):
            with pytest.raises(RuntimeError,match='recompile'):
                learning._resume_frozen_seed(c,target,target.parent.name)
            assert not manifest.exists() and c.native.call_count==1
        else:
            assert learning._resume_frozen_seed(c,target,target.parent.name)==artifact
            if mode=='same':
                value=c.read(manifest);receipt=value['generation_receipt']
                assert value['version']==c.version(artifact)
                assert 'training_input_tasks' not in value and 'planned_training_count' not in value
                assert receipt['control_learning']['model_calls']==2
                assert receipt['english_trigger_learning']['model_calls']==3
                assert receipt['control_learning']['attempts'][0]['usage'] is None
                assert c.native.call_count==1
                c.save(c.TRAINING/'seed-compile-status.json',{'seed_label':'later-label'})
                c.native=Mock(side_effect=AssertionError('recovered manifest must not repeat native compilation'))
                learning._resume_frozen_seed(c,target,target.parent.name)
                restored=c.read(c.TRAINING/'seed-compile-status.json')
                assert restored['control_learning']['model_calls']==2
                assert restored['english_trigger_learning']['model_calls']==3
                c.native.assert_not_called()
            else:
                assert not manifest.exists();c.native.assert_not_called()
        assert target.read_bytes()==before


@pytest.mark.parametrize('reason',[
    'appworld_english_trigger_no_valid_subset',
    'appworld_english_trigger_no_specific_supported_pair',
    'appworld_english_trigger_native_pre_action_not_matched'])
def test_spent_saved_english_draft_uses_native_source_fallback_without_new_model(tmp_path,monkeypatch,reason):
    learning,c,policy,feedback,_=_english_trigger_fixture(tmp_path)
    packet={'memories':copy.deepcopy(policy['snapshot']['experiences']), 'training_statistics':{}}
    monkeypatch.setattr(learning,'seed_control_input',lambda *args:(packet,feedback,{}))
    real_specific=learning._specific_english_pairs;calls=[]
    def reject_saved_then_compile(*args,**kwargs):
        calls.append(copy.deepcopy(args[3]))
        if len(calls)==1:raise RuntimeError(reason)
        return real_specific(*args,**kwargs)
    monkeypatch.setattr(learning,'_specific_english_pairs',reject_saved_then_compile)
    paid=Mock(side_effect=AssertionError('old three model calls stay spent'))
    monkeypatch.setattr(learning,'_english_trigger_model_reply',paid)
    attempts=[{'attempt':i,'status':'rejected','usage':{'total_tokens':10*i},
               'draft':[{'memory_id':'auth-experience','trigger_pattern':'login access_token'}]}
              for i in (1,2,3)]
    diagnostic={'seed_label':'fixture-candidate','english_trigger_learning':{
        'model_calls':3,'attempts':copy.deepcopy(attempts)}}
    result=learning.generate_english_triggers(c,policy,set(),diagnostic)
    assert calls[1]==[] and len(calls)==2
    paid.assert_not_called()
    status=diagnostic['english_trigger_learning']
    assert status['model_calls']==3 and status['attempts']==attempts
    assert status['draft_reuse']['recovery']=='source_fallback_no_paid_calls'
    assert status['evidence_fallback']['status']=='compiled'
    assert status['evidence_fallback']['model_calls']==0
    assert result['snapshot']==policy['snapshot']
    assert result['options']['approved_experiences']==policy['options']['approved_experiences']
    assert result['options']['loop_rules']==policy['options']['loop_rules']
    assert result['options']['experience_overrides'][0]['source_experience']['memory_id']=='auth-experience'
    assert json.loads((tmp_path/'seed-compile-status.json').read_text())['english_trigger_learning']['model_calls']==3


def test_spent_saved_english_draft_does_not_downgrade_source_or_receipt_failure(tmp_path,monkeypatch):
    learning,c,policy,feedback,_=_english_trigger_fixture(tmp_path)
    packet={'memories':copy.deepcopy(policy['snapshot']['experiences']), 'training_statistics':{}}
    monkeypatch.setattr(learning,'seed_control_input',lambda *args:(packet,feedback,{}))
    fault=RuntimeError('native_runtime_recompile_failed');fault.native_reason='control_source_not_approved'
    monkeypatch.setattr(learning,'_specific_english_pairs',Mock(side_effect=fault))
    fallback=Mock(side_effect=AssertionError('source failure must not be relabeled unsupported'))
    monkeypatch.setattr(learning,'_derive_english_trigger_fallback',fallback)
    diagnostic={'seed_label':'fixture-candidate','english_trigger_learning':{'model_calls':3,
        'attempts':[{'draft':[{'memory_id':'auth-experience','trigger_pattern':'login access_token'}]}]}}
    with pytest.raises(RuntimeError,match='native_runtime_recompile_failed'):
        learning.generate_english_triggers(c,policy,set(),diagnostic)
    fallback.assert_not_called()
    assert diagnostic['english_trigger_learning']['model_calls']==3

def test_native_early_pair_directory_repairs_written_only_model_draft_without_paid_call(tmp_path,monkeypatch):
    learning,c,policy,card,sources,before,after,trace=_native_precorrective_fixture(tmp_path)
    original=copy.deepcopy(policy);mid=card['memory_id'];diagnostic={}
    paid=Mock(side_effect=AssertionError('local candidate selection must not call a model'))
    monkeypatch.setattr(learning,'_english_trigger_model_reply',paid)
    compiled=learning._specific_english_pairs(c,policy,sources,[[{'memory_id':mid,
        'trigger_pattern':'clear_cart add_product_to_cart place_order','approved_source':True}]],diagnostic)
    pattern=diagnostic['accepted_triggers'][0]['trigger_pattern']
    assert 'show_cart' in pattern and 'clear_cart' not in pattern
    assert learning._native_l1_profile(c,compiled,sources)[('fixture-run','before-write')]=={mid}
    assert compiled['snapshot']==original['snapshot'] and policy==original
    assert compiled['options']['approved_experiences']==original['options']['approved_experiences']
    assert compiled['options']['experience_overrides'][0]['content']==card['content']
    paid.assert_not_called()


def test_native_early_pairs_keep_all_negative_tasks_and_namespace_scope(tmp_path):
    learning,c,policy,card,sources,before,after,trace=_native_precorrective_fixture(tmp_path)
    negative=copy.deepcopy(before);negative['advice_query'].update(query_id='unrelated-cart-read',
        decision_id='unrelated-cart-read',task='Move my Amazon cart to my wish list.',state='apis.amazon.show_cart()')
    sources.runtime_runs['negative-run']=[negative]
    sources.runtime_codes[('negative-run','unrelated-cart-read')]='apis.amazon.show_cart()'
    diagnostic={};mid=card['memory_id']
    original=copy.deepcopy(policy)
    with pytest.raises(RuntimeError,match='appworld_english_trigger_no_specific_supported_pair'):
        learning._specific_english_pairs(c,policy,sources,[[{'memory_id':mid,
            'trigger_pattern':'clear_cart place_order','approved_source':True}]],diagnostic)
    assert diagnostic['task_negative_count']==2
    assert all(row.get('trigger_pattern')!='order show_cart' for row in diagnostic['cards'])
    assert policy==original


def test_early_pair_candidate_does_not_authorize_unrelated_documented_read(tmp_path):
    learning,c,policy,card,sources,before,after,trace=_native_precorrective_fixture(tmp_path)
    before['advice_query'].update(task='Order the requested Amazon products.',state='apis.gmail.show_inbox_threads(page_index=0)')
    sources.runtime_codes[('fixture-run','before-write')]='apis.gmail.show_inbox_threads(page_index=0)'
    original=copy.deepcopy(policy)
    with pytest.raises(RuntimeError,match='no_specific_supported_pair'):
        learning._specific_english_pairs(c,policy,sources,[[{'memory_id':card['memory_id'],
            'trigger_pattern':'gmail show_inbox_threads','approved_source':True}]],{})
    assert policy==original


def test_native_reactive_failure_keeps_observed_error_condition_and_late_application(tmp_path):
    learning,c,policy,card,sources,before,after,trace=_native_precorrective_fixture(tmp_path)
    mid=card['memory_id'];sources[mid]['requires_observed_failure']=True
    for point in sources[mid]['evidence']:point['pre_action_eligible']=True
    sources.runtime_observations={('fixture-run','after-write'):'The clear_cart validation call failed.'}
    diagnostic={}
    compiled=learning._specific_english_pairs(c,policy,sources,[[{'memory_id':mid,
        'trigger_pattern':'clear_cart add_product_to_cart','approved_source':True}]],diagnostic)
    assert diagnostic['accepted_triggers']==[{'memory_id':mid,'trigger_pattern':'add_product_to_cart clear_cart'}]
    profile=learning._native_l1_profile(c,compiled,sources)
    assert profile[('fixture-run','before-write')]==set() and profile[('fixture-run','after-write')]=={mid}
    assert 'local_pair_recovery' not in diagnostic


def test_same_batch_early_alternative_uses_full_native_compiler_and_keeps_three_paid(tmp_path,monkeypatch):
    learning,c,policy,card,sources,before,after,trace=_native_precorrective_fixture(tmp_path,
        content='下单前记录原购物车quantity，clear_cart清空购物车，add_product_to_cart添加目标后place_order，恢复原购物车。')
    before['advice_query']['state']='apis.amazon.show_cart() quantity'
    mid=card['memory_id'];original=copy.deepcopy(policy);native_compile=learning._compile_english_triggers
    calls=[];paid_status={'model_calls':3}
    def failed_primary(controller,current,draft,current_sources,status=None):
        calls.append(copy.deepcopy(draft))
        if len(calls)==1:
            status.setdefault('rejected',[]).append({'memory_id':mid,
                'reason':'appworld_english_trigger_native_pre_action_not_matched'})
            raise ValueError('appworld_english_trigger_no_valid_subset')
        return native_compile(controller,current,draft,current_sources,status)
    monkeypatch.setattr(learning,'_compile_english_triggers',failed_primary)
    paid=Mock(side_effect=AssertionError('paid budget has already been spent'))
    monkeypatch.setattr(learning,'_english_trigger_model_reply',paid)
    diagnostic={}
    compiled=learning._specific_english_pairs(c,policy,sources,[[{'memory_id':mid,
        'trigger_pattern':'clear_cart place_order','approved_source':True}]],diagnostic)
    assert len(calls)==2 and calls[0]['triggers']!=calls[1]['triggers']
    assert diagnostic['local_pair_recovery']['status']=='compiled'
    assert learning._native_l1_profile(c,compiled,sources)[('fixture-run','before-write')]=={mid}
    assert paid_status=={'model_calls':3};paid.assert_not_called()
    assert policy==original and compiled['snapshot']==original['snapshot']


def test_same_batch_early_fallback_never_retries_source_permission_errors(tmp_path,monkeypatch):
    learning,c,policy,card,sources,before,after,trace=_native_precorrective_fixture(tmp_path)
    calls=[]
    def denied(*args,**kwargs):
        calls.append(1);raise RuntimeError('control_source_not_approved')
    monkeypatch.setattr(learning,'_compile_english_triggers',denied)
    with pytest.raises(RuntimeError,match='control_source_not_approved'):
        learning._specific_english_pairs(c,policy,sources,[[{'memory_id':card['memory_id'],
            'trigger_pattern':'clear_cart place_order','approved_source':True}]],{})
    assert calls==[1]


def test_native_early_pair_incremental_support_does_not_count_cross_paragraph_window(tmp_path):
    learning,c,policy,card,sources,before,after,trace=_native_precorrective_fixture(tmp_path)
    mid=card['memory_id'];second=copy.deepcopy(before)
    second['advice_query'].update(query_id='cross-paragraph',decision_id='cross-paragraph',state='show_cart()')
    sources.runtime_runs['fixture-run'].insert(1,second)
    sources.runtime_codes[('fixture-run','cross-paragraph')]='apis.amazon.show_cart()'
    sources[mid]['evidence'].append({'run_id':'fixture-run','query_id':'cross-paragraph',
        'task':second['advice_query']['task'],'pre_action_eligible':True})
    diagnostic={};original=copy.deepcopy(policy)
    with pytest.raises(RuntimeError,match='appworld_english_trigger_no_specific_supported_pair'):
        learning._specific_english_pairs(c,policy,sources,[[{'memory_id':mid,
            'trigger_pattern':'amazon show_cart','approved_source':True}]],diagnostic,
            incremental_ids={mid},min_incremental_support=2)
    assert any(row.get('status')=='low_support_incremental' and row.get('positive_windows')==1
               for row in diagnostic['cards'])
    assert policy==original


@pytest.mark.parametrize('boundary',['header','refresh-failed'])
def test_same_seed_initial_header_or_refresh_failure_keeps_complete_spent_drafts(tmp_path,monkeypatch,boundary):
    c=_generation_receipt_controller(tmp_path);artifact=_generation_receipt_artifact(c);previous=_generation_receipt_diagnostic()
    previous['control_learning']['model_calls']=3;previous['english_trigger_learning']['model_calls']=3
    label=previous['seed_label'];saved={key:deepcopy(previous[key]) for key in ('control_learning','english_trigger_learning')}
    memory=c.TRAINING/'memory.json';c.save(memory,{'entries':[]});c.save(c.RUNTIME/'service.json',{'runtime_options':{}})
    template=c.ROOT/'tools/trajectory_evolution/config/prompts/runtime-experience.txt';template.parent.mkdir(parents=True);template.write_text('Approved advice')
    c.save(c.TRAINING/'seed-compile-status.json',previous)
    monkeypatch.setattr(learning,'_completed_training_tasks',lambda *args:{'train-fixture'})
    monkeypatch.setattr(learning,'_readiness',lambda *args:memory)
    monkeypatch.setattr(learning,'_compile_seed_snapshot',lambda *args:deepcopy(artifact['policy']['snapshot']))
    monkeypatch.setattr(learning,'_duplicate_l1_generation',lambda *args:None)
    calls=[]
    def native(binary,root,operation,payload):
        calls.append(list(operation))
        if operation[0]=='learn':
            if boundary=='refresh-failed':raise RuntimeError('fixture_native_refresh_failed')
            return json.dumps({'status':'completed','model_calls':0})
        assert operation==['runtime','compile']
        current=c.read(c.TRAINING/'seed-compile-status.json')
        assert all(current[key]==value for key,value in saved.items())
        assert current['seed_label']==label and current['memory_source']==str(memory)
        assert current['validated_evidence_required'] is True
        raise KeyboardInterrupt('controlled header interruption')
    c.native=native
    if boundary=='refresh-failed':
        with pytest.raises(RuntimeError,match='fixture_native_refresh_failed'):learning.freeze_seed(c,label=label)
    else:
        with pytest.raises(KeyboardInterrupt,match='controlled header interruption'):learning.freeze_seed(c,label=label)
    current=c.read(c.TRAINING/'seed-compile-status.json')
    assert all(current[key]==value for key,value in saved.items())
    assert current['seed_label']==label
    assert len(calls)==(1 if boundary=='refresh-failed' else 2)
    assert not (c.PUBLISHED/label/'strategy.json').exists()
def _development_feedback_fixture(tmp_path, monkeypatch):
    """Synthetic formal32 captures, real source-native versions and compiler."""
    import learning, stage_gate
    c, anchor, query, _ = _duplicate_l1_native_fixture(tmp_path)
    c._development_query=query
    c._trial_error_code=lambda row:row.get('error_code')
    c.framework_capture_complete=lambda row:row.get('execution_status')=='completed'
    c._has_complete_loop_review=lambda row:(row.get('evidence') or {}).get('loop_review',{}).get('status')=='complete'
    authority=anchor['policy']['snapshot']
    latest=copy.deepcopy(anchor)
    options=copy.deepcopy(latest['policy']['options']); options['l1_prevention_sources']=[]
    latest['policy']=json.loads(c.native(c.BINARY,c.ROOT,['runtime','recompile'],{'policy':latest['policy'],'options':options}))
    refs=anchor['policy']['options']['approved_experiences']
    mid=next(r['memory_id'] for r in refs if r['memory_id']=='fixture-accepted')
    healthy=next(r['memory_id'] for r in refs if r['memory_id']=='fixture-excluded')
    baseline=c.GEPA/'l0-current-stage32/result.json'
    save(baseline,{'status':'complete','host_version':'local-host','judge_projection':{'model':'fixed'},
        'items':[{'task_id':f'dev-{i}','strict_passed':int(i<24)} for i in range(32)]})
    attempts=[]
    def captured(artifact, index, failed):
        seed=c.PUBLISHED/f'candidate-{index}/strategy.json'; save(seed,artifact)
        version=c.version(artifact);save(seed.with_name('manifest.json'),{'version':version})
        path=c.GEPA/f'seed-search-{index}-stage32/comparison.json'
        rows=[];items=[]
        for i in range(32):
            task=f'dev-{i}';tid=f'trial-{index}-{i}';passed=i not in failed
            source=mid if i in (0,1) else healthy if i in (3,4) else None
            events=[]
            identity={'user_id':'fixture-user','agent_id':'fixture-agent','run_id':tid}
            if source:
                ref=next(r for r in refs if r['memory_id']==source)
                application={'status':'applied','reason':'host_boundary_verified','policy_id':artifact['policy']['policy_id']+':scoped',
                             'query_id':tid+'-query','decision_id':tid+'-decision'}
                events.append({'kind':'custom','attributes':{'type':'policy.query','decision':{
                    'status':'ready','policy_id':application['policy_id'],'experiences':[ref],
                    'query':{'boundary':'llm','execution_id':tid+'-execution','advice_query':{
                        'identity':identity,'query_id':application['query_id'],'decision_id':application['decision_id']}}}}})
                events.append({'kind':'advice.applied','application':application,'attributes':{'receipt':{
                    'boundary':'llm','identity':identity,'execution_id':tid+'-execution','application':application,'experiences':[ref]}}})
            events += [{'kind':'action.start','action_id':'login','loop_id':'main','operation':'appworld.execute','attributes':{'semantic':'action'},'input':{'code':"apis.amazon.login(username='PRIVATE_EMAIL', password='PRIVATE_PASSWORD')"}},
                {'kind':'action.end','action_id':'login','loop_id':'main','status':'completed','attributes':{'semantic':'action','executed':True},'output':{'observation':'PRIVATE_OBSERVATION'}},
                {'kind':'action.start','action_id':'read','loop_id':'main','operation':'appworld.execute','attributes':{'semantic':'action'},'input':{'code':"apis.amazon.show_wish_list()"}},
                {'kind':'action.end','action_id':'read','loop_id':'main','attributes':{'semantic':'action','executed':True},'output':{'observation':'PRIVATE_OBSERVATION'},'status':'completed' if passed else 'failed',
                 'error':{} if passed else {'code':'appworld_auth_401','message':'PRIVATE_SECRET_TOKEN'}},
                {'kind':'run.end','output':{'task_completed':passed,'termination_reason':'completed' if passed else 'bounded_recovery_exhausted',
                                          'response':'PRIVATE_BUSINESS_ANSWER'}}]
            rows.append({'task_id':task,'trial_id':tid,'execution_status':'completed','verdict':'passed' if passed else 'failed',
                'evidence':{'loaded_version':version,'loop_review':{'status':'complete','count':0},'trace':{'identity':identity,'events':events}}})
            items.append({'task_id':task,'trial_id':tid,'loaded_version':version,'execution_status':'completed',
                'quality':int(passed),'strict_passed':int(passed),'avg_invalid_loop':0})
        native=path.parent/'shards/shard-00/capture/report.json';save(native,{'complete':True,'trials':rows});save(native.with_name('manifest.json'),{'complete':True,'summary':{'planned_trials':32}})
        save(path,{'status':'complete','host_version':'local-host','judge_projection':{'model':'fixed'},
                   'source_l0':str(baseline),'seed':str(seed),'items':items})
        save(path.with_name('progress.json'),{'artifact':artifact,'shards':{'shard-00':{'source_report':str(native)}}})
        attempts.append({'attempt':index,'round':1,'seed':str(seed),'status':'failed','last_stage':32,'pilot_comparison':str(path)})
        return seed, path, native
    anchor_path,_,_=captured(anchor,5,{0,1,27,28,29,30,31})
    latest_path,latest_comparison,native=captured(latest,8,{0,1,2,3,26,27,28,29,30,31})
    target=c.PUBLISHED/'candidate-next/strategy.json'
    search={'round':1,'attempts':attempts,'automatic_seed_generation':{'round':1,'status':'pending',
        'seed':str(target),'seed_label':'candidate-next','source_seed':str(latest_path),'source_attempt':8,
        'method':'approved_training_native_seed'}}
    save(c.GEPA/'status.json',{'seed_search':search})
    monkeypatch.setattr(stage_gate,'verify_stage',lambda *args:(False,{'coverage_pass':True,'seed_match':True,'status':'fail'}))
    return c,anchor,latest,authority,target,search,latest_comparison,native,mid,healthy


@pytest.mark.parametrize('anchor_round',[1,0])
def test_ordinary_failed32_uses_exact_actual_receipts_and_preserves_best_anchor(tmp_path,monkeypatch,anchor_round):
    import learning
    c,anchor,latest,authority,target,search,path,native,mid,healthy=_development_feedback_fixture(tmp_path,monkeypatch)
    query=c._development_query
    search['attempts'][0]['round']=anchor_round
    save(c.GEPA/'status.json',{'seed_search':search})
    before={p:p.read_bytes() for p in (path,native,c.GEPA/'status.json')}
    feedback=learning._development_seed_feedback(c,target,'candidate-next',authority)
    assert feedback['latest']['artifact']==latest and feedback['anchor']['artifact']==anchor
    assert feedback['anchor_counts'][(mid,next(r['content_revision'] for r in anchor['policy']['options']['approved_experiences'] if r['memory_id']==mid))]['regressed_tasks']==2
    candidate,pair=learning._compile_development_l1_candidate(c,feedback,authority)
    assert pair[0]==mid
    changes={key for key in anchor['policy']['options'] if anchor['policy']['options'][key]!=candidate['policy']['options'][key]}
    assert changes=={'disabled_injections','l1_prevention_sources'}
    assert candidate['policy']['snapshot']==anchor['policy']['snapshot']
    for field in ('action_rules','loop_rules','schedule_rules','experience_overrides'):
        assert candidate['policy']['options'].get(field)==anchor['policy']['options'].get(field)
    assert c.version(candidate) not in {c.version(anchor),c.version(latest)}
    # Real native L1 excludes the ablated source while its same-source L3 stays authorized.
    response=json.loads(c.native(c.BINARY,c.ROOT,['runtime','query'],{'policy':candidate['policy'],'query':query}))
    assert mid not in {r['memory_id'] for r in response.get('experiences',[])}
    loop=copy.deepcopy(query);loop.update(boundary='loop',iteration=31)
    old=json.loads(c.native(c.BINARY,c.ROOT,['runtime','query'],{'policy':anchor['policy'],'query':loop}))
    new=json.loads(c.native(c.BINARY,c.ROOT,['runtime','query'],{'policy':candidate['policy'],'query':loop}))
    assert old['progress']['decision']==new['progress']['decision']=='stop'
    assert old['progress']['rule_id']==new['progress']['rule_id']
    assert all(p.read_bytes()==b for p,b in before.items())


@pytest.mark.parametrize('problem',['partial','wrong_source_attempt','legacy_source_attempt','wrong_revision','wrong_policy_receipt','holdout'])
def test_development_feedback_refuses_incomplete_foreign_or_unapplied_sources(tmp_path,monkeypatch,problem):
    import learning,stage_gate
    c,anchor,latest,authority,target,search,path,native,mid,_=_development_feedback_fixture(tmp_path,monkeypatch)
    if problem=='partial':
        monkeypatch.setattr(stage_gate,'verify_stage',lambda *args:(False,{'coverage_pass':False,'seed_match':True}))
        with pytest.raises(RuntimeError,match='incomplete'):learning._development_seed_feedback(c,target,'candidate-next',authority)
    elif problem=='wrong_source_attempt':
        search['automatic_seed_generation']['source_attempt']=999;save(c.GEPA/'status.json',{'seed_search':search})
        with pytest.raises(RuntimeError,match='binding_invalid'):
            learning._development_seed_feedback(c,target,'candidate-next',authority)
    elif problem=='holdout':
        outside=c.PUBLISHED/'comparison500.json';save(outside,c.read(path));search['attempts'][-1]['pilot_comparison']=str(outside)
        save(c.GEPA/'status.json',{'seed_search':search})
        with pytest.raises(RuntimeError,match='binding_invalid'):learning._development_seed_feedback(c,target,'candidate-next',authority)
    elif problem=='legacy_source_attempt':
        search['automatic_seed_generation']['source_attempt']=None;save(c.GEPA/'status.json',{'seed_search':search})
        assert learning._development_seed_feedback(c,target,'candidate-next',authority) is None
    elif problem=='wrong_revision':
        wrong=copy.deepcopy(authority);wrong['experiences'][0]['content_revision']='foreign'
        local=learning._development_seed_feedback(c,target,'candidate-next',authority)
        with pytest.raises(RuntimeError,match='no_longer_approved'):learning._compile_development_l1_candidate(c,local,wrong)
    else:
        value=c.read(native)
        for row in value['trials']:
            for event in row['evidence']['trace']['events']:
                if event['kind']=='advice.applied':event['application']['policy_id']='foreign-policy'
        save(native,value)
        local=learning._development_seed_feedback(c,target,'candidate-next',authority)
        assert local['source_counts']=={}  # A foreign receipt cannot authorize suppression or attributed guidance.


def test_development_model_feedback_is_bounded_machine_evidence_not_answers(tmp_path,monkeypatch):
    import learning
    c,anchor,latest,authority,target,search,path,native,mid,_=_development_feedback_fixture(tmp_path,monkeypatch)
    local=learning._development_seed_feedback(c,target,'candidate-next',authority)
    support=[{'memory_id':mid,'failure_support':{'appworld_auth_401':{'validated_supports':1}}}]
    packet=learning._development_public_feedback(local,latest['policy'],support)
    wire=json.dumps(packet)
    assert len(wire.encode())<=4096 and packet['tasks']==32
    assert all(secret not in wire for secret in ('PRIVATE_EMAIL','PRIVATE_PASSWORD','PRIVATE_SECRET_TOKEN','PRIVATE_BUSINESS_ANSWER','dev-0','trial-8-0'))
    assert any(step['requested_api']=='apis.amazon.login' and step['status']=='code_block_completed' for row in packet['failure_examples'] for step in row['steps'])
    assert any(step['requested_api']=='apis.amazon.show_wish_list' and step['parameter_names']==[] and step['error_code']=='appworld_auth_401'
               and mid in step['applied_before'] for row in packet['failure_examples'] for step in row['steps'])
    assert any(row['termination_reason']=='bounded_recovery_exhausted' and not row['goal_completed'] for row in packet['failure_examples'])
    bad=copy.deepcopy(packet);bad['failure_examples'][0]['answer']='PRIVATE_BUSINESS_ANSWER'
    with pytest.raises(ValueError,match='step_invalid'):learning._approved_development_feedback(bad,{r['memory_id'] for r in latest['policy']['options']['approved_experiences']})


def test_ordinary_freeze_single_ablation_is_zero_paid_and_uses_nonduplicate_feedback(tmp_path,monkeypatch):
    import learning
    c,anchor,latest,authority,target,search,path,native,mid,_=_development_feedback_fixture(tmp_path,monkeypatch)
    memory=tmp_path/'native/memory.json';assert memory.is_file()
    original=c.native
    c.native=lambda binary,root,operation,packet:json.dumps({'status':'completed','model_calls':0}) if operation[0]=='learn' else original(binary,root,operation,packet)
    monkeypatch.setattr(learning,'_completed_training_tasks',lambda controller:{'canonical-training-only'})
    monkeypatch.setattr(learning,'_readiness',lambda controller:memory)
    monkeypatch.setattr(learning,'_compile_seed_snapshot',lambda *args:copy.deepcopy(authority))
    control=Mock(side_effect=AssertionError('ablation adds no paid control model call'))
    english=Mock(side_effect=AssertionError('ablation adds no paid English model call'))
    monkeypatch.setattr(learning,'generate_seed_controls',control);monkeypatch.setattr(learning,'generate_english_triggers',english)
    result=learning.freeze_seed(c,label='candidate-next')
    assert result==c.read(target) and c.version(result)!=c.version(anchor)
    assert control.call_count==english.call_count==0
    diagnostic=c.read(c.TRAINING/'seed-compile-status.json')
    assert diagnostic['development_feedback']['anchor_seed'].endswith('candidate-5/strategy.json')
    assert diagnostic['development_feedback']['association']=='paired_regression_not_proven_causality'
    assert diagnostic['control_learning']['model_calls']==diagnostic['english_trigger_learning']['model_calls']==0
    assert diagnostic['development_feedback']['requires_new_formal_gate'] is True
    assert c.read(c.PUBLISHED/'candidate-5/strategy.json')==anchor


def test_development_packet_guides_existing_control_and_english_generators(tmp_path,monkeypatch):
    import learning
    _,c,rows,policy,_,_=_fallback_fixture(tmp_path)
    mid='experience-a'
    c._seed_development_feedback={'source_counts':{(mid,policy['snapshot']['experiences'][0]['content_revision']):
        {'regressed_tasks':0,'improved_tasks':0,'failed_tasks':2,'passed_tasks':0}},'failure_examples':[
        {'was_l0_success':False,'goal_completed':False,'termination_reason':'bounded_recovery_exhausted','applied_source_ids':[mid],
         'steps':[{'requested_api':'apis.amazon.login','parameter_names':['username','password'],'status':'code_block_completed','evidence_basis':'requested_code','error_code':None,'applied_before':[mid]},
                  {'requested_api':'apis.amazon.show_wish_list','parameter_names':[],'status':'code_block_failed','evidence_basis':'requested_code','error_code':'appworld_auth_401','applied_before':[mid]}]}]}
    c.BINARY=ROOT/'tools/trajectory_evolution/build-v4/trajectory_evolution'
    packet,local,controls=learning.seed_control_input(c,policy,{r['task_id'] for r in rows})
    assert packet['development_feedback']['source_summary'][0]['memory_id']==mid
    assert local['development_preferred_failure_codes']==['appworld_auth_401']
    approved=learning.approved_control_packet(packet)
    assert approved['development_feedback']==packet['development_feedback']
    outgoing=[]
    def transport(controller,payload,instructions,budget):
        outgoing.append((copy.deepcopy(payload),instructions))
        if 'triggers' in instructions:return json.dumps({'triggers':[]}),{}
        return json.dumps({'action_rules':[],'loop_rules':[],'schedule_rules':[]}),{}
    monkeypatch.setattr(learning,'_generic_card_response',transport)
    model=c.RUNTIME/'runtime.json';save(model,{'model':{'api_key_env':'FAKE'}})
    c.environment=lambda:{'FAKE':'local-fixture-only'}
    (c.RUNTIME/'existing-native-control-prompt.txt').write_text('Bounded source controls only.')
    learning._control_model_reply(c,packet)
    learning._english_trigger_model_reply(c,packet)
    assert len(outgoing)==2 and all(payload['development_feedback']==approved['development_feedback'] for payload,_ in outgoing)
    assert all('development32' in instructions for _,instructions in outgoing)
    assert all('PRIVATE_' not in json.dumps(payload) for payload,_ in outgoing)
    # The original canonical TRAIN rows are still the only cited native control authority.
    draft,provenance=learning._evidence_bounded_control_draft(c,policy,local)
    compiled=learning.compile_seed_controls(c,policy,controls,draft,{})
    assert any(r.get('source_experience',{}).get('memory_id')==mid for r in compiled['options']['loop_rules'])




def test_development_feedback_excludes_dispatch_completion_and_keeps_unfinished_tail(tmp_path,monkeypatch):
    import learning
    c,anchor,latest,authority,target,search,path,native,mid,_=_development_feedback_fixture(tmp_path,monkeypatch)
    data=c.read(native)
    for row in data['trials']:
        if row['task_id']=='dev-0':
            events=row['evidence']['trace']['events']
            events.insert(1,{'kind':'action.start','action_id':'dispatch','loop_id':'framework:dispatch',
                'actor_id':'skill_executor','operation':'appworld_submit_code','attributes':{'semantic':'action'},
                'input':{'code':"apis.amazon.place_order(address_id='PRIVATE_ANSWER')"}})
            events.insert(2,{'kind':'action.end','action_id':'dispatch','loop_id':'framework:dispatch','actor_id':'skill_executor',
                'status':'completed','attributes':{'semantic':'action','executed':True},'output':{'status':'success','tool_result':'submitted'}})
            events[-1]['output']['termination_reason']='max_steps'
    save(native,data)
    local=learning._development_seed_feedback(c,target,'candidate-next',authority)
    public=learning._development_public_feedback(local,latest['policy'],[])
    assert any(row['termination_reason']=='max_steps' and not row['goal_completed'] for row in public['failure_examples'])
    assert all(step['requested_api']!='apis.amazon.place_order' for row in public['failure_examples'] for step in row['steps'])
    assert 'PRIVATE_ANSWER' not in json.dumps(public)


def test_development_anchor_never_replaces_successful_l1_with_unmeasured_retranslation(tmp_path,monkeypatch):
    import learning
    c,anchor,latest,authority,target,search,path,native,mid,_=_development_feedback_fixture(tmp_path,monkeypatch)
    local=learning._development_seed_feedback(c,target,'candidate-next',authority)
    fresh=copy.deepcopy(latest)
    options=copy.deepcopy(fresh['policy']['options']);options['experience_overrides'][0]['trigger_pattern']='wrong new lexical variant'
    fresh['policy']=json.loads(c.native(c.BINARY,c.ROOT,['runtime','recompile'],{'policy':fresh['policy'],'options':options}))
    result=learning._development_anchor_candidate(c,fresh,local)
    assert result==anchor and c.version(result)==c.version(anchor)
    assert fresh['policy']['options']['experience_overrides'][0]['trigger_pattern']=='wrong new lexical variant'


def test_late_l1_receipt_is_not_a_regression_ablation_candidate(tmp_path,monkeypatch):
    import learning
    c,anchor,latest,authority,target,search,path,native,mid,_=_development_feedback_fixture(tmp_path,monkeypatch)
    original=c.read(native)
    for row in original['trials']:
        events=row['evidence']['trace']['events']
        receipts=[event for event in events if event['kind']=='advice.applied']
        events[:]=[event for event in events if event['kind']!='advice.applied']
        events[-1:-1]=receipts
    save(native,original)
    local=learning._development_seed_feedback(c,target,'candidate-next',authority)
    assert local['source_counts']=={}


def test_development_no_new_control_keeps_one_native_validated_l1_change_and_anchor_siblings(tmp_path,monkeypatch):
    import learning
    c,anchor,latest,authority,target,search,path,native,mid,_=_development_feedback_fixture(tmp_path,monkeypatch)
    local=learning._development_seed_feedback(c,target,'candidate-next',authority)
    local['observed_context_sources']={mid}
    fresh=copy.deepcopy(anchor);options=copy.deepcopy(anchor['policy']['options'])
    override=next(row for row in options['experience_overrides'] if row['source_experience']['memory_id']==mid)
    override['trigger_pattern']='gmail inbox'
    fresh['policy']=json.loads(c.native(c.BINARY,c.ROOT,['runtime','recompile'],{'policy':fresh['policy'],'options':options}))
    card=next(row for row in authority['experiences'] if row['memory_id']==mid)
    sources=learning._EnglishTriggerSources()
    positive=copy.deepcopy(c._development_query)
    positive['advice_query'].update(query_id='positive',decision_id='positive',task='gmail inbox collection',state='')
    negative=copy.deepcopy(positive)
    negative['advice_query'].update(query_id='negative',decision_id='negative',task='Compute geometric area',state='')
    sources.runtime_runs={'positive-run':[positive],'negative-run':[negative]}
    sources[mid]={'experience':card,'requires_observed_failure':True,'window_mode':None,
        'evidence':[{'run_id':'positive-run','query_id':'positive','pre_action_eligible':True}]}
    c._seed_development_training_feedback={'local_source_evidence':{'positive-run':{'identity':positive['advice_query']['identity']}}}
    source_calls=[]
    def exact_sources(controller,policy,feedback,**kwargs):
        assert kwargs['candidate_artifact']['policy']==policy and kwargs['prevention_compilation'] is True
        source_calls.append(True);return sources
    monkeypatch.setattr(learning,'_english_trigger_sources',exact_sources)
    result=learning._development_anchor_candidate(c,fresh,local)
    assert source_calls==[True] and c.version(result)!=c.version(anchor)
    assert result['policy']['options']['experience_overrides']==fresh['policy']['options']['experience_overrides']
    for name in ('action_rules','loop_rules','schedule_rules','approved_experiences','disabled_injections','l1_prevention_sources'):
        assert result['policy']['options'].get(name)==anchor['policy']['options'].get(name)
    assert sources.last_profile['after']['training_runs']==2
    actual=json.loads(c.native(c.BINARY,c.ROOT,['runtime','query'],{'policy':result['policy'],'query':positive}))
    assert mid in {ref['memory_id'] for ref in actual.get('experiences',[])}
    actual=json.loads(c.native(c.BINARY,c.ROOT,['runtime','query'],{'policy':result['policy'],'query':negative}))
    assert mid not in {ref['memory_id'] for ref in actual.get('experiences',[])}


def test_development_snapshot_metadata_only_is_abstention_and_larger_source_never_read(tmp_path,monkeypatch):
    import learning
    c,anchor,latest,authority,target,search,path,native,mid,_=_development_feedback_fixture(tmp_path,monkeypatch)
    local=learning._development_seed_feedback(c,target,'candidate-next',authority)
    memory=tmp_path/'native/memory.json';value=c.read(memory);value['revision']='2';save(memory,value)
    shadow={'target_scopes':[c.SCOPE],'max_policy_entries':64,'max_advice_items':4,'max_advice_bytes':4096,
        'max_query_bytes':32768,'max_memory_bytes':16777216,'max_snapshot_bytes':262144,'max_match_terms':128,
        'min_matched_terms':1,'require_validated_evidence':True}
    fresh=copy.deepcopy(anchor)
    fresh['policy']=json.loads(c.native(c.BINARY,c.ROOT,['runtime','compile'],{'memory_path':str(memory),
        'identity':anchor['policy']['snapshot']['identity'],'shadow_options':shadow,'options':anchor['policy']['options']}))
    assert fresh['policy']['options']==anchor['policy']['options']
    assert fresh['policy']['snapshot']!=anchor['policy']['snapshot']
    assert c.version(fresh)==c.version(anchor)  # Native canonical versions already ignore this metadata.
    result=learning._development_anchor_candidate(c,fresh,local)
    assert result==anchor and c.version(result) in local['seen_versions']
    save(native.with_name('manifest.json'),{'complete':True,'summary':{'planned_trials':500}})
    read=c.read
    def no_holdout_body(path):
        if Path(path)==native:raise AssertionError('500 native body must never be read')
        return read(path)
    c.read=no_holdout_body
    with pytest.raises(RuntimeError,match='native_scope_invalid'):
        learning._development_trial_rows(c,local['latest'],{})


def test_development_bare_api_context_uses_exact_public_namespace():
    import learning
    card={'content':'下单前 clear_cart 清空购物车，place_order 下单；按 quantity 保留。','trigger_pattern':'Amazon目标下单'}
    assert learning._development_source_api_context(card,'apis.amazon.place_order')
    assert learning._development_source_api_context(card,'apis.amazon.clear_cart')
    assert not learning._development_source_api_context(card,'apis.other.place_order')
    assert not learning._development_source_api_context(card,'apis.amazon.replace_order')
    assert not learning._development_source_api_context({'content':'replace_order place_order_extra','trigger_pattern':''},'apis.amazon.place_order')
    assert not learning._development_source_api_context({'content':'apis.other.place_order','trigger_pattern':''},'apis.amazon.place_order')
    assert not learning._development_source_api_context({'content':'login','trigger_pattern':''},'apis.amazon.login')


def test_development_bare_cart_sources_enter_only_approved_proposal_guidance():
    import learning
    cards=[{'memory_id':mid,'content_revision':'r-'+mid,'content':body,'trigger_pattern':'仅目标下单'}
        for mid,body in [('cart-isolation','clear_cart清空购物车后place_order，恢复原购物车。'),
                         ('cart-quantity','记录quantity后place_order，保留非目标商品。'),
                         ('unapproved','place_order')]]
    policy={'snapshot':{'experiences':cards},'options':{'approved_experiences':[
        {k:card[k] for k in ('memory_id','content_revision')} for card in cards[:2]]}}
    local={'source_counts':{},'failure_examples':[{'was_l0_success':True,'goal_completed':True,
        'termination_reason':'task_completed','applied_source_ids':[],'steps':[{
            'requested_api':'apis.amazon.place_order','parameter_names':['access_token'],
            'status':'code_block_completed','evidence_basis':'requested_code','error_code':None,'applied_before':[]}]}]}
    original=copy.deepcopy(policy)
    public=learning._development_public_feedback(local,policy,[])
    assert local['observed_context_sources']=={'cart-isolation','cart-quantity'}
    assert {row['memory_id'] for row in public['source_summary']}==local['observed_context_sources']
    assert all(row['observed_failed_api_context'] for row in public['source_summary'])
    assert public['failure_examples'][0]['steps'][0]['evidence_basis']=='requested_code'
    assert policy==original and 'unapproved' not in json.dumps(public)


def test_development_bare_cart_guidance_reaches_native_checked_anchor_change(tmp_path,monkeypatch):
    learning,c,policy,card,sources,before,after,trace=_native_precorrective_fixture(tmp_path)
    mid=card['memory_id'];c.version=lambda artifact: artifact['policy']['policy_id']
    anchor={'level':'L3','policy':copy.deepcopy(policy)}
    local={'anchor':{'artifact':anchor},'source_counts':{},'seen_versions':{c.version(anchor)},
        'failure_examples':[{'was_l0_success':True,'goal_completed':True,'termination_reason':'task_completed',
            'applied_source_ids':[],'steps':[{'requested_api':'apis.amazon.place_order',
            'parameter_names':['access_token'],'status':'code_block_completed','evidence_basis':'requested_code',
            'error_code':None,'applied_before':[]}]}]}
    learning._development_public_feedback(local,policy,[])
    assert mid in local['observed_context_sources']
    compiled=learning._specific_english_pairs(c,policy,sources,[],{})
    fresh={'level':'L3','policy':compiled};c._seed_development_training_feedback={'local_source_evidence':{}}
    def exact_sources(controller,current,feedback,**kwargs):
        assert kwargs['candidate_artifact']['policy']==current and kwargs['prevention_compilation'] is True
        return sources
    monkeypatch.setattr(learning,'_english_trigger_sources',exact_sources)
    result=learning._development_anchor_candidate(c,fresh,local)
    assert c.version(result)!=c.version(anchor)
    assert result['policy']['snapshot']==policy['snapshot']
    assert result['policy']['options']['approved_experiences']==policy['options']['approved_experiences']
    assert result['policy']['options']['experience_overrides'][0]['content']==card['content']
    assert learning._native_l1_profile(c,result['policy'],sources)[('fixture-run','before-write')]=={mid}


def test_development_anchor_regression_precedes_four_l0_regressions(tmp_path, monkeypatch):
    import learning
    c, anchor, latest, authority, target, search, path, native, mid, healthy = _development_feedback_fixture(tmp_path, monkeypatch)
    # Four L0 regressions also passed the anchor; the fifth case is an anchor-only gain.
    anchor_path = Path(search['attempts'][0]['pilot_comparison'])
    anchor_compact = c.read(anchor_path)
    for item in anchor_compact['items']:
        if item['task_id'] in ('dev-0', 'dev-1'):
            item.update(strict_passed=1, quality=1)
    save(anchor_path, anchor_compact)
    anchor_capture = Path(next(iter(c.read(anchor_path.with_name('progress.json'))['shards'].values()))['source_report'])
    anchor_native = c.read(anchor_capture)
    for row in anchor_native['trials']:
        if row['task_id'] in ('dev-0', 'dev-1'):
            row['verdict'] = 'passed'
            events = row['evidence']['trace']['events']
            events[-2].update(status='completed', error={})
            events[-1]['output'].update(task_completed=True, termination_reason='completed')
    save(anchor_capture, anchor_native)
    captured = c.read(native)
    cart = next(row for row in captured['trials'] if row['task_id'] == 'dev-26')
    start = next(event for event in cart['evidence']['trace']['events'] if event.get('action_id') == 'read' and event['kind'] == 'action.start')
    start['input']['code'] = 'apis.amazon.place_order(access_token=token)'
    save(native, captured)
    protected = {p: p.read_bytes() for p in (path, native, c.GEPA / 'status.json')}
    local = learning._development_seed_feedback(c, target, 'candidate-next', authority)
    assert local['anchor']['artifact'] == anchor
    assert local['latest']['baseline']['dev-26']['strict_passed'] == 0
    assert local['anchor']['current']['dev-26']['strict_passed'] == 1
    assert local['latest']['current']['dev-26']['strict_passed'] == 0
    assert sum(row['was_l0_success'] for row in local['failure_examples']) == 4
    assert all(row['_anchor_success'] for row in local['failure_examples'] if row['was_l0_success'])
    cart_local = next(row for row in local['failure_examples'] if any(step['requested_api'] == 'apis.amazon.place_order' for step in row['steps']))
    assert cart_local['_anchor_success'] is True
    packet = learning._development_public_feedback(local, latest['policy'], [])
    assert len(packet['failure_examples']) == 4
    selected = [row for row in packet['failure_examples'] if any(step['requested_api'] == 'apis.amazon.place_order' for step in row['steps'])]
    assert len(selected) == 1 and selected[0]['was_l0_success'] is False
    assert packet['failure_examples'][0] == selected[0]
    supplied = {row['memory_id'] for row in packet['source_summary']}
    approved = {ref['memory_id'] for ref in latest['policy']['options']['approved_experiences']}
    assert supplied <= approved
    assert all(set(row['applied_source_ids']) <= supplied for row in packet['failure_examples'])
    assert all(set(step['applied_before']) <= supplied for row in packet['failure_examples'] for step in row['steps'])
    assert learning._approved_development_feedback(packet, approved) == packet
    encoded = json.dumps(packet)
    assert len(encoded.encode()) <= 4096 and '_anchor_success' not in encoded and 'dev-' not in encoded
    assert cart_local['_anchor_success'] is True
    assert all(p.read_bytes() == value for p, value in protected.items())


def test_development_foreign_anchor_cannot_prioritize_current_failures(tmp_path, monkeypatch):
    import learning
    c, anchor, latest, authority, target, search, path, native, mid, healthy = _development_feedback_fixture(tmp_path, monkeypatch)
    anchor_path = Path(search['attempts'][0]['pilot_comparison'])
    foreign = c.read(anchor_path)
    foreign['items'][0]['task_id'] = 'foreign-holdout-task'
    save(anchor_path, foreign)
    local = learning._development_seed_feedback(c, target, 'candidate-next', authority)
    assert local['anchor']['artifact'] == latest
    assert all(row['_anchor_success'] is False for row in local['failure_examples'])
    packet = learning._development_public_feedback(local, latest['policy'], [])
    assert all(row['was_l0_success'] for row in packet['failure_examples'])
    assert 'foreign-holdout-task' not in json.dumps(packet)
def _development100_fixture(tmp_path, monkeypatch):
    import learning, stage_gate
    c,anchor,latest,authority,target,search,path,native,mid,healthy=_development_feedback_fixture(tmp_path,monkeypatch)
    source=json.loads(native.read_text());report=json.loads(path.read_text());base=json.loads(Path(report['source_l0']).read_text())
    source_rows,items,originals=[],[],[]
    for i in range(100):
        row=copy.deepcopy(source['trials'][i%32]);old=copy.deepcopy(report['items'][i%32]);origin=copy.deepcopy(base['items'][i%32])
        task=f'validation-{i}';row['task_id']=old['task_id']=origin['task_id']=task
        row['trial_id']=old['trial_id']=f'validation-trial-{i}'
        # The public list must contain this successful-but-wasted-loop case;
        # it retains the truth: completed goal, completed requested code block.
        passed=i not in (0,1,2,3,4)
        old.update(strict_passed=int(passed),quality=int(passed),avg_invalid_loop=9 if i==99 else 5 if i==0 else 0)
        row['verdict']='passed' if passed else 'failed'
        row['evidence']['loop_review']['count']=old['avg_invalid_loop']
        row['evidence']['trace']['events'][-1]['output'].update(task_completed=passed,
            termination_reason='completed' if passed else 'bounded_recovery_exhausted')
        if i==99:
            events=row['evidence']['trace']['events']
            for event in events:
                if event.get('kind')=='action.start':
                    event['input']['code']='apis.gmail.show_api_doc(api_name="PRIVATE_VALUE")'
        if i==0:
            events=row['evidence']['trace']['events']
            for event in events:
                if event.get('kind')=='action.end' and event.get('action_id')=='read':
                    event.update(status='failed',error={'code':'appworld_code_error'})
        source_rows.append(row);items.append(old);originals.append(origin)
    folder=c.GEPA/'seed-search-8-stage100';comparison=folder/'comparison.json';native100=folder/'shards/shard-00/capture/report.json'
    base100=c.GEPA/'l0-current-stage100/result.json';save(base100,dict(base,items=originals))
    save(comparison,dict(report,items=items,source_l0=str(base100)))
    save(native100,dict(source,trials=source_rows));save(native100.with_name('manifest.json'),{'complete':True,'summary':{'planned_trials':100}})
    save(folder/'progress.json',{'artifact':latest,'shards':{'shard-00':{'source_report':str(native100)}}})
    search['attempts'][-1].update(validation_comparison=str(comparison),last_stage=100,status='failed')
    save(c.GEPA/'status.json',{'seed_search':search})
    return c,authority,target,search,comparison,native100,mid,latest


def test_complete_failed100_guides_next_seed_and_preserves_successful_loop_counterexample(tmp_path,monkeypatch):
    import learning
    c,authority,target,search,comparison,native,mid,latest=_development100_fixture(tmp_path,monkeypatch)
    protected={p:p.read_bytes() for p in (comparison,native,c.GEPA/'status.json')}
    feedback=learning._development_seed_feedback(c,target,'candidate-next',authority)
    assert feedback['latest']['tasks']==100 and feedback['latest']['comparison']==comparison
    assert len(feedback['latest']['current'])==100 and feedback['anchor']['tasks']==100
    assert all(task.startswith('validation-') for task in feedback['latest']['current'])
    packet=learning._development_public_feedback(feedback,latest['policy'],[])
    assert packet['tasks']==100 and packet['scope']=='development100_proposal_guidance_only'
    assert any(example['goal_completed'] and example['steps'][0]['requested_api']=='apis.gmail.show_api_doc'
               for example in packet['failure_examples'])
    assert any(step['error_code']=='appworld_code_error' for ex in packet['failure_examples'] for step in ex['steps'])
    assert len(packet['failure_examples'])<=4 and len(json.dumps(packet).encode())<=4096
    assert set(packet)=={'scope','tasks','source_summary','failure_examples'}
    assert not any(value in json.dumps(packet) for value in ('PRIVATE','validation-','trial-','_reviewed','_strict','_anchor'))
    for p,before in protected.items():assert p.read_bytes()==before


@pytest.mark.parametrize('problem',['partial','foreign','wrong_version','scope500','unreviewed','wrong_host'])
def test_development100_prefers_only_complete_bound_native_same_scope(tmp_path,monkeypatch,problem):
    import learning
    c,authority,target,search,path,native,mid,latest=_development100_fixture(tmp_path,monkeypatch)
    if problem=='partial':
        value=c.read(path);value['status']='pending';save(path,value)
        feedback=learning._development_seed_feedback(c,target,'candidate-next',authority)
        assert feedback['latest']['tasks']==32 and feedback['latest']['comparison']!=path
        return
    if problem=='foreign':
        elsewhere=tmp_path/'foreign-comparison.json';save(elsewhere,c.read(path));search['attempts'][-1]['validation_comparison']=str(elsewhere)
        save(c.GEPA/'status.json',{'seed_search':search})
    elif problem=='scope500':
        save(native.with_name('manifest.json'),{'complete':True,'summary':{'planned_trials':500}})
        original=c.read
        def read(p):
            if Path(p)==native:raise AssertionError('500 body must not be read')
            return original(p)
        c.read=read
    else:
        value=c.read(path)
        if problem=='wrong_version':value['items'][0]['loaded_version']='foreign'
        elif problem=='wrong_host':value['host_version']='foreign'
        else:value['items'][0]['avg_invalid_loop']=None
        save(path,value)
    with pytest.raises(RuntimeError,match='development_feedback'):
        learning._development_seed_feedback(c,target,'candidate-next',authority)


def test_duplicate_feedback_uses_the_same_complete100_native_scope(tmp_path,monkeypatch):
    import learning
    c,authority,target,search,path,native,mid,latest=_development100_fixture(tmp_path,monkeypatch)
    # The existing duplicate application reader owns retained per-trial paths.
    # Keep the exact actual source trial bytes at its standard location.
    compact=c.read(path)
    for row,item in zip(c.read(native)['trials'],compact['items']):
        item['source_shard']='shard-00'
        save(path.parent/'shards/shard-00/capture/trials'/f"{item['trial_id']}.json",row)
    save(path,compact)
    counts,source=learning._duplicate_l1_applications(c,search,latest)
    assert source==path and counts
    assert sum(value['passed_tasks']+value['failed_tasks'] for value in counts.values())==15  # All 100 rows, not the pilot's four applications.


def test_code_error_fallback_requires_exact_training_pair_and_keeps_shared_budget(tmp_path,monkeypatch):
    import learning
    learning,c,rows,policy,feedback,memory=_fallback_fixture(tmp_path)
    c.BINARY=ROOT/'tools/trajectory_evolution/build-v4/trajectory_evolution'
    options=copy.deepcopy(policy['options'])
    for rule in options['loop_rules']:
        if not rule.get('source_experience'):rule['error_codes']=['appworld_code_error']
    policy=json.loads(c.native(c.BINARY,c.ROOT,['runtime','recompile'],{'policy':policy,'options':options}))
    for event in rows[2]['evidence']['trace']['events']:
        if event.get('error',{}).get('code')=='appworld_auth_401':event['error']['code']='appworld_code_error'
    save(c.TRAINING/'full-captures/batch/report.json',{'complete':True,'trials':rows})
    feedback=learning.seed_control_feedback(c,{row['task_id'] for row in rows})
    draft,provenance=learning._evidence_bounded_control_draft(c,policy,feedback,preferred_failure_codes=['appworld_code_error'])
    assert len(draft['loop_rules'])==1 and draft['loop_rules'][0]['error_codes']==['appworld_code_error']
    controls=c.read(c.RUNTIME/'service.json')['control_learning'];controls['enabled']=True
    try:
        compiled=learning.compile_seed_controls(c,policy,controls,draft,{})
    except RuntimeError as error:
        raise AssertionError(getattr(error,'native_reason',str(error))) from None
    learned=[r for r in compiled['options']['loop_rules'] if r.get('source_experience')]
    assert len(learned)==1 and learned[0]['max_replans']==1 and compiled['snapshot']==policy['snapshot']
    # Same broad error family, but absent exact TRAIN citation, grants nothing.
    m=c.read(memory);m['entries'][0]['candidate']['support_evidence']=[];save(memory,m)
    with pytest.raises(RuntimeError):learning._evidence_bounded_control_draft(c,policy,feedback,preferred_failure_codes=['appworld_code_error'])

@pytest.mark.parametrize('same_request', [True, False])
def test_development100_packet_keeps_real_requested_repeats_under_budget(tmp_path, monkeypatch, same_request):
    import learning
    c,authority,target,search,path,native,mid,latest=_development100_fixture(tmp_path,monkeypatch)
    report=c.read(native);row=report['trials'][-1];events=row['evidence']['trace']['events'];terminal=events[-1]
    start=next(e for e in events if e.get('kind')=='action.start' and e.get('loop_id')=='main')
    end=next(e for e in events if e.get('kind')=='action.end' and e.get('loop_id')=='main')
    actual=[]
    names=['argument_'+str(i)+'_'+'x'*40 for i in range(16)]
    for i in range(9):
        begin,finish=copy.deepcopy(start),copy.deepcopy(end);key='doc-'+str(i)
        begin['action_id']=finish['action_id']=key
        value='PRIVATE_REQUEST' if same_request else 'PRIVATE_REQUEST_'+str(i)
        begin['input']['code']='apis.api_docs.show_api_doc('+','.join(name+'='+repr(value) for name in names)+')'
        finish.update(status='completed',output={'observation':'public documentation'})
        finish.pop('error',None);finish['attributes'].update(executed=True,semantic='action')
        actual.extend([begin,finish])
    for i in range(7):
        begin,finish=copy.deepcopy(start),copy.deepcopy(end);key='tail-'+str(i)
        begin['action_id']=finish['action_id']=key;begin['input']['code']='apis.amazon.show_order(order_id='+str(i)+')'
        finish.update(status='completed',output={'observation':'public result'})
        finish.pop('error',None);finish['attributes'].update(executed=True,semantic='action')
        actual.extend([begin,finish])
    row['evidence']['trace']['events']=actual+[terminal];save(native,report)
    local=learning._development_seed_feedback(c,target,'candidate-next',authority)
    loop=next(ex for ex in local['failure_examples'] if ex.get('_reviewed_loops')==9)
    assert loop['_repeat_requested_steps']==(2 if same_request else 0)
    if same_request:assert [step['requested_api'] for step in loop['steps'][:2]]==['apis.api_docs.show_api_doc']*2
    packet=learning._development_public_feedback(local,latest['policy'],[])
    assert len(json.dumps(packet).encode())<=4096 and len(packet['source_summary'])<=8
    assert all(ex['steps'] for ex in packet['failure_examples'])
    docs=[ex for ex in packet['failure_examples'] if sum(step['requested_api']=='apis.api_docs.show_api_doc' for step in ex['steps'])>=2]
    assert bool(docs)==same_request
    assert not any(value in json.dumps(packet) for value in ('PRIVATE_REQUEST','_local_request_key','_repeat_requested_steps','validation-','trial-'))
    assert set(packet)=={'scope','tasks','source_summary','failure_examples'}
    supplied={ref['memory_id'] for ref in packet['source_summary']}
    assert all(set(ex['applied_source_ids'])<=supplied and all(set(step['applied_before'])<=supplied for step in ex['steps']) for ex in packet['failure_examples'])


def _saved_result_training_case(tmp_path):
    import learning
    from types import SimpleNamespace
    c = SimpleNamespace(ROOT=ROOT, WORLD=ROOT/'examples/agent/business-simulator/appworld/runtime/world',
                        read=lambda path: json.loads(Path(path).read_text()))
    card = {'trigger_pattern':'需要展示已保存的API返回结果时',
            'content':'保存结果变量，后续print变量及type(...).__name__，无stdout不等于失败。'}
    def event(eid,stamp,code,observation):
        return {'event_id':eid,'kind':'action.end','timestamp_us':stamp,'status':'completed',
            'actor_id':'main','execution_id':'same-execution','loop_id':'main',
            'step_id':eid+'-step','action_id':eid+'-action','input':{'code':code},
            'attributes':{'semantic':'action','executed':True},'output':{'observation':observation}}
    saved=event('saved',3,'saved = apis.amazon.show_wish_list(access_token=token)','Execution successful.')
    displayed=event('displayed',5,'print(saved)\nprint(type(saved).__name__)','[]\nlist')
    def start(end):
        result=copy.deepcopy(end);result.update(kind='action.start',timestamp_us=end['timestamp_us']-1,operation='appworld.execute')
        result.pop('output');return result
    trace={'events':[{'kind':'run.start','timestamp_us':1},start(saved),saved,start(displayed),displayed,
        {'kind':'run.end','timestamp_us':6}]}
    return learning,c,card,trace


def test_saved_result_normal_training_requires_cited_simple_completed_collection(tmp_path):
    learning,c,card,trace=_saved_result_training_case(tmp_path)
    original=copy.deepcopy(trace)
    assert learning._saved_result_training_support(c,card,trace,{'saved','displayed'})
    assert trace==original
    assert not learning._saved_result_training_support(c,card,trace,{'saved'})
    assert not learning._saved_result_training_support(c,{'content':'General print advice.'},trace,{'saved','displayed'})


@pytest.mark.parametrize('fault',['conditional','docs','write','failed','unclosed','rebound','type_only','wrong_actor','no_world_start','wrapper','empty_identity','prior_api_rebind'])
def test_saved_result_normal_training_rejects_unproven_or_wrong_phase(tmp_path,fault):
    learning,c,card,trace=_saved_result_training_case(tmp_path)
    saved=next(e for e in trace['events'] if e.get('event_id')=='saved' and e['kind']=='action.end')
    display=next(e for e in trace['events'] if e.get('event_id')=='displayed' and e['kind']=='action.end')
    if fault=='conditional': saved['input']['code']='if False:\n saved=apis.amazon.show_wish_list(access_token=token)'
    elif fault=='docs': saved['input']['code']='saved=apis.api_docs.show_api_doc(app_name="amazon",api_name="show_wish_list")'
    elif fault=='write': saved['input']['code']='saved=apis.amazon.clear_cart(access_token=token)'
    elif fault=='failed': saved['status']='error'
    elif fault=='unclosed': trace['events'].pop()
    elif fault=='rebound': display['input']['code']='saved=[]\nprint(saved)\nprint(type(saved).__name__)'
    elif fault=='type_only': display['input']['code']='print(type(saved).__name__)'
    elif fault=='wrong_actor': display['actor_id']='different'
    elif fault=='no_world_start': trace['events']=[e for e in trace['events'] if e['kind']!='action.start']
    elif fault=='wrapper':
        for e in trace['events']:
            if e['kind']=='action.start': e['operation']='run_skill'
    elif fault=='empty_identity': saved['execution_id']=''
    else:
        early=copy.deepcopy(saved);early.update(event_id='rebind',timestamp_us=2,action_id='rebind-action',step_id='rebind-step')
        early['input']['code']='apis=other_object';early['output']['observation']='Execution successful.'
        start=copy.deepcopy(early);start.update(kind='action.start',operation='appworld.execute',timestamp_us=1)

        for event in trace['events']: event['timestamp_us'] += 3
        start['timestamp_us']=2;early['timestamp_us']=3
        trace['events'][0]['timestamp_us']=1
        trace['events'][1:1]=[start,early]
    assert not learning._saved_result_training_support(c,card,trace,{'saved','displayed'})


def test_saved_result_normal_final_artifact_no_unapproved_or_disabled_refs(tmp_path,monkeypatch):
    learning,c,card,trace=_saved_result_training_case(tmp_path)
    assert learning._saved_result_training_support(c,card,trace,{'saved','displayed'})
    # The existing native/Host qualification remains the authority after the
    # normal-training helper proposes a phase; no source identity shortcut.
    from appworld_skill import appworld_saved_result_experiences
    ref={'memory_id':'approved-saved-result','content_revision':'exact-revision'}
    artifact={'level':'L3','agent':{'appworld_options':{'l1_saved_result_experiences':[ref]}},
        'policy':{'options':{'approved_experiences':[ref]},
                  'snapshot':{'experiences':[{**ref,'content':'Original approved result-display lesson.'}]}}}
    assert appworld_saved_result_experiences(artifact)['refs']==[ref]
    changed=copy.deepcopy(artifact);changed['policy']['options']['approved_experiences']=[]
    with pytest.raises(ValueError,match='source_invalid'):appworld_saved_result_experiences(changed)
    changed=copy.deepcopy(artifact);changed['agent']['appworld_options']['l1_saved_result_experiences'][0]['content_revision']='wrong'
    with pytest.raises(ValueError,match='source_invalid'):appworld_saved_result_experiences(changed)
