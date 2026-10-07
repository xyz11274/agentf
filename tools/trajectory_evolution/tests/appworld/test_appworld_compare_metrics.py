"""Compact results retain the canonical business metric contract; no API calls."""
from __future__ import annotations

from copy import deepcopy
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'tools/trajectory_evolution/experiments/appworld'))
import layout
import compare


def native_row(task='appworld-fixture_1', *, tokens=100, calls=2, quality=.8, verdict='passed'):
    return {'task_id': task, 'trial_id': task + '-trial', 'execution_status': 'completed',
            'verdict': verdict, 'duration_ms': 1000,
            'metrics': {'tokens': tokens, 'llm_calls': calls},
            'evidence': {'output': {'final': {'appworld_reward': 1, 'quality_score': 1,
                'tokens': 999, 'model_calls': 999, 'latency_ms': 1,
                'known_agent_tokens': tokens, 'tokens_complete': True}},
                'assessment': {'quality': quality, 'critical_failures': 0},
                'loop_review': {'status': 'complete', 'count': 0,
                                'observed_iterations': 1, 'wasted_iterations': []},
                'trace': {'identity': {'run_id': task + '-run'},
                          'attributes': {'capture_complete': True},
                          'events': [{'kind': 'run.start'}, {'kind': 'run.end'}]}}}


class CompareMetricContractTest(unittest.TestCase):
    def test_native_judge_projection_reads_capture_retry_source_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            shard = root / 'shard'
            source = shard / 'capture-retry-01/report.json'
            source.parent.mkdir(parents=True)
            source.write_text('{}')
            config = shard / 'config.json'
            current = layout.judge_projection()
            self.assertIsNone(compare._report_judge_projection(source))
            config.write_text(json.dumps({'judge': current}))
            self.assertEqual(compare._report_judge_projection(source), current)
            old = dict(current, require_state_dependency=False)
            config.write_text(json.dumps({'judge': old}))
            self.assertEqual(compare._report_judge_projection(source), old)
            self.assertNotEqual(compare._report_judge_projection(source), current)
            config.unlink()
            self.assertIsNone(compare._report_judge_projection(source))

    def test_native_judge_projection_prefers_nearest_regrade_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'shard/regrade/retry-01/report.json'
            source.parent.mkdir(parents=True)
            source.write_text('{}')
            current = layout.judge_projection()
            old = dict(current, require_state_dependency=False)
            (root / 'shard/config.json').write_text(json.dumps({'judge': old}))
            source.parent.with_name('retry-01.regrade.config.json').write_text(
                json.dumps({'judge': current}))
            self.assertEqual(compare._report_judge_projection(source), current)

    def run_parallel_baseline_fixture(self, *, update_during_capture):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dataset, output = root / 'dataset', root / 'comparison'
            dataset.mkdir()
            row = native_row('task')
            row['checks'] = [{'check_id': 'model', 'status': 'passed'}]
            (dataset / 'holdout.jsonl').write_text(json.dumps({'task_id': 'task'}))
            seed, l0 = root / 'strategy.json', root / 'L0.json'
            artifact = {'level': 'L3', 'policy': {'fixture': True}}
            seed.write_text(json.dumps(artifact))
            source = root / 'grade' / 'report.json'
            source.parent.mkdir()
            source.write_text(json.dumps({'complete': True, 'trials': [row]}))
            source.with_name('manifest.json').write_text(json.dumps({'complete': True}))
            grade_config = source.parent.with_name('grade.regrade.config.json')
            current = layout.judge_projection()
            old = dict(current, keep_dispatch_bindings_before_preview=False)

            def write_baseline(projection, quality):
                value = deepcopy(row)
                value['evidence']['assessment']['quality'] = quality
                grade_config.write_text(json.dumps({'judge': projection}))
                l0.write_text(json.dumps({'status': 'complete', 'host_version': 'fixture-host',
                    'source_report': str(source), 'judge_projection': projection,
                    'items': [compare.compact_row(value, 'L0')]}))

            write_baseline(old, .2)
            original = l0.read_bytes()
            executions = []

            def capture(dataset_path, config_path, destination, env):
                executions.append('task')
                if update_during_capture:
                    write_baseline(current, .9)
                destination.mkdir(parents=True)
                report = {'complete': True, 'trials': [deepcopy(row)]}
                (destination / 'report.json').write_text(json.dumps(report))
                (destination / 'manifest.json').write_text(json.dumps({'complete': True}))
                return report

            arguments = ['compare.py', '--seed', str(seed), '--l0-source', str(l0),
                         '--output', str(output), '--task-count', '1', '--workers', '1']
            with patch.object(compare, 'DATASET', dataset), patch.object(sys, 'argv', arguments), \
                    patch.object(layout, 'environment', return_value={}), \
                    patch.object(layout, 'host_version', return_value='fixture-host'), \
                    patch.object(layout, 'evaluation_config', side_effect=lambda ids, art, workers: {
                        'variants': [{'artifact': art}], 'judge': current}), \
                    patch.object(layout.shared, 'capture', side_effect=capture), \
                    patch.object(compare, 'summarize_existing', side_effect=AssertionError('L0 is read only')):
                self.assertEqual(compare.comparison_main(), 0)
                first = json.loads((output / 'comparison.json').read_text())
                if not update_during_capture:
                    self.assertEqual(first['status'], 'partial')
                    self.assertEqual(first['baseline_status'], 'awaiting_l0')
                    self.assertNotIn('L0_reused', first['metrics'])
                    self.assertEqual(l0.read_bytes(), original)
                    write_baseline(current, .9)
                retained = l0.read_bytes()
                self.assertEqual(compare.comparison_main(), 0)
                final = json.loads((output / 'comparison.json').read_text())
            self.assertEqual(final['status'], 'complete')
            self.assertEqual(final['metrics']['L0_reused']['quality'], .9)
            self.assertEqual(l0.read_bytes(), retained)
            self.assertEqual(executions, ['task'])

    def test_parallel_l0_refresh_uses_final_grade_without_overwriting_source(self):
        self.run_parallel_baseline_fixture(update_during_capture=True)

    def test_awaiting_l0_finalizes_through_normal_command_without_reexecution(self):
        self.run_parallel_baseline_fixture(update_during_capture=False)

    def test_final_l0_checks_actual_config_and_both_profiles(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'grade' / 'report.json'
            source.parent.mkdir()
            source.write_text('{}')
            source.with_name('manifest.json').write_text(json.dumps({'complete': True}))
            config = source.parent.with_name('grade.regrade.config.json')
            wanted = layout.judge_projection()
            changed = dict(wanted, keep_dispatch_bindings_before_preview=False)
            l0 = root / 'L0.json'
            l0.write_text(json.dumps({'host_version': 'fixture-host', 'status': 'complete',
                'source_report': str(source), 'judge_projection': wanted,
                'items': [compare.compact_row(native_row('task'), 'L0')]}))
            progress = {'shards': {'0': {'label': 'shard-00', 'judge_projection': wanted}}}
            with patch.object(layout, 'host_version', return_value='fixture-host'):
                config.write_text(json.dumps({'judge': changed}))
                self.assertEqual(compare.comparison_baseline_state(l0, ['task'], progress)[1:], (False, False))
                config.write_text(json.dumps({'judge': wanted}))
                progress['shards']['0']['judge_projection'] = changed
                self.assertEqual(compare.comparison_baseline_state(l0, ['task'], progress)[1:], (True, False))
                progress['shards']['0']['judge_projection'] = wanted
                self.assertEqual(compare.comparison_baseline_state(l0, ['task'], progress)[1:], (True, True))
                with self.assertRaisesRegex(RuntimeError, 'final_task_coverage_changed'):
                    compare.comparison_baseline_state(l0, ['other-task'], progress)

    def test_preexecution_exemption_requires_explicit_zero_observed_runs(self):
        row = {'execution_status': 'error', 'evidence': {
            'exit_code': 137, 'runner_error': 'evaluation_entry_capture_point_capture_degraded',
            'failure_capture': {'complete': False}}}
        self.assertFalse(compare._preexecution_capture_failure(row))
        row['evidence']['failure_capture']['observed_runs'] = 1
        self.assertFalse(compare._preexecution_capture_failure(row))
        row['evidence']['failure_capture']['observed_runs'] = 0
        self.assertTrue(compare._preexecution_capture_failure(row))

    def test_proven_preexecution_retry_still_records_both_attempts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            success = native_row('task')
            failed = {'task_id': 'task', 'trial_id': 'preexecution', 'execution_status': 'error',
                      'evidence': {'exit_code': 137,
                          'runner_error': 'evaluation_entry_capture_point_capture_degraded',
                          'failure_capture': {'complete': False, 'observed_runs': 0}}}
            for label, report in [('capture', {'run_id': 'first', 'trials': [failed]}),
                                  ('capture-retry-01', {'run_id': 'last', 'trials': [success],
                                      'summary': {'resume_source': {'run_id': 'first'}}})]:
                path = root / label / 'report.json'
                path.parent.mkdir()
                path.write_text(json.dumps(report))
            item = compare.execution_cost_rows([compare.compact_row(success, 'fixture')],
                        root / 'capture-retry-01/report.json', root)[0]
            self.assertEqual(item['execution_attempts'], 2)
            self.assertEqual(item['execution_cost_exemptions'], 1)
            self.assertTrue(item['execution_costs_complete'])
            self.assertEqual(compare.summarize([item], 1)['extra_execution_attempts'], 1)

    def test_partial_baseline_checkpoint_keeps_native_rows_and_rejects_changed_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stage, dataset = root / 'stage', root / 'dataset'
            folder = stage / 'capture'
            (folder / 'trials').mkdir(parents=True)
            dataset.mkdir()
            config = {'task_ids': ['one', 'two'], 'repetitions': 1, 'parallelism': 16,
                      'variants': [{'id': 'candidate', 'version': 'fixture-v1'}]}
            stage.joinpath('capture.config.json').write_text(json.dumps(config))
            dataset.joinpath('manifest.json').write_text(json.dumps(
                {'metadata': {'dataset_version': 'fixture-dataset'}}))
            started = {'complete': False, 'planned_trials': 2, 'run_id': 'native-run',
                       'dataset_version': 'fixture-dataset'}
            folder.joinpath('started.json').write_text(json.dumps(started))
            row = native_row('one')
            row.update(task_version='fixture-task', variant_id='candidate', repetition=1)
            path = folder / 'trials' / (row['trial_id'] + '.json')
            path.write_text(json.dumps(row))
            before = path.read_bytes()
            tasks = [{'task_id': task, 'task_version': 'fixture-task'} for task in ['one', 'two']]
            result = compare._partial_baseline_checkpoint(stage, ['one', 'two'], config, tasks, dataset)
            self.assertEqual(result, (folder, started, [row]))
            self.assertEqual(path.read_bytes(), before)
            larger_report = dict(config, max_report_bytes=2 * 1024 * 1024 * 1024)
            self.assertEqual(compare._partial_baseline_checkpoint(
                stage, ['one', 'two'], larger_report, tasks, dataset), result)
            changed = deepcopy(config)
            changed['parallelism'] = 8
            with self.assertRaisesRegex(RuntimeError, 'config_changed'):
                compare._partial_baseline_checkpoint(stage, ['one', 'two'], changed, tasks, dataset)
            row['task_version'] = 'changed-task'
            path.write_text(json.dumps(row))
            with self.assertRaisesRegex(RuntimeError, 'identity_mismatch'):
                compare._partial_baseline_checkpoint(stage, ['one', 'two'], config, tasks, dataset)

    def test_full_scope_report_capacity_changes_only_storage(self):
        with patch.object(layout, 'version', return_value='fixture-version'), \
                patch.object(layout, 'ids', return_value=[]):
            artifact = {'agent': {'revision': layout.host_version()}, 'level': 'L0', 'policy': None}
            config = layout.evaluation_config(['fixture'] * 500, artifact, 16)
            self.assertEqual(config['max_report_bytes'], 2 * 1024 * 1024 * 1024)
            self.assertEqual(config['parallelism'], 16)
            self.assertEqual(config['timeout_ms'], 1800000)
            self.assertEqual(config['judge']['max_model_calls'], 4000)
            self.assertEqual(layout.judge_projection(config), layout.judge_projection())

    def test_partial_capture_accepts_only_one_auxiliary_quality_ulp_without_rewriting(self):
        import math
        row = native_row('roundtrip')
        value = .9166666666666665
        row['evidence']['output']['quality_score'] = value
        row['evidence']['trace']['events'][-1]['output'] = {'quality_score': value}
        original = deepcopy(row)
        adjacent = math.nextafter(value, -math.inf)
        restored = deepcopy(row)
        restored['evidence']['output']['quality_score'] = adjacent
        restored['evidence']['trace']['events'][-1]['output']['quality_score'] = adjacent
        self.assertTrue(compare._partial_capture_roundtrip_equal([row], [restored]))
        self.assertEqual(row, original)
        for mutate in ('two_ulps', 'duration', 'tokens', 'output', 'event', 'trial_id'):
            changed = deepcopy(restored)
            if mutate == 'two_ulps':
                changed['evidence']['output']['quality_score'] = math.nextafter(adjacent, -math.inf)
            elif mutate == 'duration': changed['duration_ms'] += 1
            elif mutate == 'tokens': changed['metrics']['tokens'] += 1
            elif mutate == 'output': changed['evidence']['output']['final']['appworld_reward'] = 0
            elif mutate == 'event': changed['evidence']['trace']['events'][0]['kind'] = 'altered'
            else: changed['trial_id'] = 'changed'
            self.assertFalse(compare._partial_capture_roundtrip_equal([row], [changed]), mutate)

    def test_partial_resume_cost_lineage_preserves_saved_costs_and_marks_inflight_unknown(self):
        import hashlib
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory) / 'l0-current-stage500'
            folder, target = stage / 'capture', stage / 'capture-retry-01'
            (folder / 'trials').mkdir(parents=True)
            target.mkdir()
            saved, pending = native_row('saved'), native_row('pending')
            started = {'complete': False, 'run_id': 'partial-native', 'dataset_version': 'dataset-v1'}
            folder.joinpath('started.json').write_text(json.dumps(started))
            folder.joinpath('trials', saved['trial_id'] + '.json').write_text(json.dumps(saved))
            stage.joinpath('capture.config.json').write_text(json.dumps(
                {'task_ids': ['saved', 'pending'], 'repetitions': 1, 'variants': [{'id': 'candidate'}]}))
            in_flight = hashlib.sha256(b'partial-native:1').hexdigest()[:32]
            (folder / '.work' / in_flight).mkdir(parents=True)
            final = {'run_id': 'resumed-native', 'trials': [saved, pending],
                     'summary': {'resume_source': {'run_id': 'partial-native'}}}
            source = target / 'report.json'
            source.write_text(json.dumps(final))
            compact = [compare.compact_row(row, 'l0-baseline') for row in final['trials']]
            result = compare.execution_cost_rows(compact, source, stage)
            self.assertEqual(result[0], compact[0])
            self.assertEqual(result[1]['execution_attempts'], 2)
            self.assertFalse(result[1]['execution_costs_complete'])
            self.assertIsNone(result[1]['latency_ms'])
            self.assertIsNone(result[1]['tokens'])
            self.assertEqual(result[1]['known_execution_latency_ms'], 1000)
            self.assertEqual(result[1]['known_agent_tokens'], 100)
            self.assertFalse(result[1]['retry_loop_complete'])
            # Another interruption after all rows were saved must keep the
            # original unfinished attempt unknown through its actual parent.
            target.joinpath('started.json').write_text(json.dumps(
                {'complete': False, 'run_id': 'resumed-native'}))
            (target / 'trials').mkdir()
            for row in final['trials']:
                target.joinpath('trials', row['trial_id'] + '.json').write_text(json.dumps(row))
            stage.joinpath('capture.progress.json').write_text(json.dumps(
                {'partial_resume_sources': {'capture-retry-01': {
                    'source_dir': str(folder), 'run_id': 'partial-native'}}}))
            source.unlink()
            final['run_id'] = 'second-resume'
            final['summary']['resume_source']['run_id'] = 'resumed-native'
            second = stage / 'capture-retry-02'
            second.mkdir()
            second_source = second / 'report.json'
            second_source.write_text(json.dumps(final))
            repeated = compare.execution_cost_rows(compact, second_source, stage)
            self.assertEqual(repeated, result)

    def test_partial_declared_unrecorded_attempts_survive_removed_work_and_incomplete_scope(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory) / 'l0-current-stage500'
            folder, target = stage / 'capture', stage / 'capture-retry-01'
            (folder / 'trials').mkdir(parents=True)
            target.mkdir()
            saved, pending = native_row('saved'), native_row('pending')
            folder.joinpath('started.json').write_text(json.dumps(
                {'complete': False, 'run_id': 'partial-native'}))
            folder.joinpath('trials', saved['trial_id'] + '.json').write_text(json.dumps(saved))
            stage.joinpath('capture.config.json').write_text(json.dumps(
                {'task_ids': ['saved', 'pending'], 'repetitions': 1, 'variants': [{'id': 'candidate'}]}))
            declared = {'run_id': 'partial-native', 'source_dir': str(folder),
                        'task_ids': ['pending'], 'complete': True}
            progress = {'partial_unrecorded_attempts': {'capture': declared}}
            stage.joinpath('capture.progress.json').write_text(json.dumps(progress))
            final = {'run_id': 'resumed-native', 'trials': [saved, pending],
                     'summary': {'resume_source': {'run_id': 'partial-native'}}}
            source = target / 'report.json'
            source.write_text(json.dumps(final))
            compact = [compare.compact_row(row, 'l0-baseline') for row in final['trials']]
            result = compare.execution_cost_rows(compact, source, stage)
            self.assertEqual(result[0], compact[0])
            self.assertEqual(result[1]['execution_attempts'], 2)
            self.assertIsNone(result[1]['tokens'])
            self.assertEqual(result[1]['known_agent_tokens'], 100)
            # Unknown task assignment cannot make any run total complete;
            # known closed execution evidence and scores remain available.
            declared['complete'] = False
            stage.joinpath('capture.progress.json').write_text(json.dumps(progress))
            incomplete = compare.execution_cost_rows(compact, source, stage)
            self.assertTrue(all(row['execution_costs_complete'] is False for row in incomplete))
            self.assertTrue(all(row['tokens'] is None for row in incomplete))
            self.assertTrue(all(row['known_agent_tokens'] == 100 for row in incomplete))
            self.assertEqual([row['quality_score'] for row in incomplete],
                             [row['quality_score'] for row in compact])
            declared['run_id'] = 'different-native'
            stage.joinpath('capture.progress.json').write_text(json.dumps(progress))
            with self.assertRaisesRegex(RuntimeError, 'unrecorded_attempt_identity_mismatch'):
                compare._partial_cost_source(folder / 'started.json')

    def test_partial_resume_regrades_judge_error_without_changing_saved_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            config_path = stage / 'capture.config.json'
            config = {'splits': ['holdout'], 'repetitions': 1, 'parallelism': 16,
                      'variants': [{'id': 'candidate', 'version': 'fixture-v1'}],
                      'judge': {'enabled': True, 'assessment_mode': 'objectives'}}
            config_path.write_text(json.dumps(config))
            healthy, bad = native_row('healthy'), native_row('judge-error')
            bad.update(error_code='evaluation_judge_failed', verdict='error')
            bad['evidence']['loop_review']['status'] = 'error'
            checkpoint = (stage / 'capture', {'run_id': 'partial-native',
                'dataset_version': 'fixture-dataset'}, [healthy, bad])
            reviewed = deepcopy(bad)
            reviewed.pop('error_code')
            reviewed['verdict'] = 'passed'
            reviewed['evidence']['loop_review']['status'] = 'complete'
            calls = []
            def run(command, **kwargs):
                source = Path(command[command.index('--resume-report') + 1])
                self.assertEqual(json.loads(source.read_text())['trials'], [healthy, bad])
                calls.append(command)
                target = Path(command[command.index('--output-root') + 1])
                target.mkdir()
                target.joinpath('report.json').write_text(json.dumps(
                    {'complete': True, 'trials': [healthy, reviewed]}))
                return type('Result', (), {'returncode': 0})()
            with patch.object(compare.subprocess, 'run', side_effect=run):
                report, _ = compare._resume_partial_baseline(checkpoint, stage=stage,
                    dataset=stage / 'dataset', config_path=config_path, config=config, env={})
            self.assertEqual(report['trials'][0], healthy)
            self.assertEqual(report['trials'][1]['duration_ms'], bad['duration_ms'])
            self.assertNotIn('--resume-rerun-trial', calls[0])
            self.assertFalse(list(stage.glob('.checkpoint-*')))

    @unittest.skipUnless((ROOT / 'tools/trajectory_evolution/build-v4/trajectory_evolution').is_file(),
                         'native CLI unavailable')
    def test_partial_resume_real_native_executes_only_unsaved_tasks(self):
        import subprocess
        binary = ROOT / 'tools/trajectory_evolution/build-v4/trajectory_evolution'
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tasks = ['one', 'two', 'three']
            golden = root / 'tasks.jsonl'
            golden.write_text('\n'.join(json.dumps({'task_id': task, 'group_id': task,
                'task_input': 'Return known total for ' + task, 'expected_behavior': 'Value is 20',
                'difficulty': 'easy', 'category': 'data', 'initial_state': {'value': 20},
                'split': 'holdout', 'review_status': 'reviewed'}) for task in tasks) + '\n')
            build = json.loads((ROOT / 'tools/trajectory_evolution/config/dataset.json').read_text())
            build.update(sources=[{'kind': 'golden', 'path': str(golden)}], max_model_calls=0)
            build_path = root / 'build.json'
            build_path.write_text(json.dumps(build))
            dataset = root / 'dataset'
            env = {'PATH': os.environ['PATH'], 'NO_PROXY': '*', 'no_proxy': '*',
                   'TRAJECTORY_UNCONDITIONAL_RESUME': '1'}
            built = subprocess.run([str(binary), 'dataset', 'build', '--config', str(build_path),
                '--output-root', str(dataset)], env=env, text=True, capture_output=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            counter, host = root / 'calls.jsonl', root / 'host.py'
            host.write_text("""import json,pathlib,subprocess,sys
request=json.load(sys.stdin)
with pathlib.Path(sys.argv[1]).open('a') as stream: stream.write(request['task']['task_id']+'\\n')
response=json.loads(subprocess.check_output([sys.executable,'-B',sys.argv[2],'trace'],input=json.dumps(request).encode()))
response['metrics'].update(tokens=100,llm_calls=2)
print(json.dumps(response))
""")
            config = {'schema_version': 1, 'splits': ['holdout'], 'task_ids': tasks,
                'repetitions': 1, 'parallelism': 16, 'max_trials': 3, 'timeout_ms': 10000,
                'max_report_bytes': 67108864, 'require_trace': True,
                'variants': [{'id': 'candidate', 'version': 'fixture-v1',
                    'command': [sys.executable, '-B', str(host), str(counter),
                        str(ROOT / 'tools/trajectory_evolution/tests/evaluation_host.py')], 'pass_env': []}],
                'checks': {'*': [{'id': 'value', 'path': '/state/value', 'op': 'equals', 'value': 20}]},
                'judge': {'enabled': False}, 'artifact_retention': {'retain_trial_files': True}}
            stage = root / 'stage'
            stage.mkdir()
            config_path = stage / 'capture.config.json'
            config_path.write_text(json.dumps(config))
            original = root / 'original'
            captured = subprocess.run([str(binary), 'dataset', 'run', str(dataset), '--config',
                str(config_path), '--output-root', str(original)], env=env, text=True,
                capture_output=True, timeout=30)
            self.assertEqual(captured.returncode, 0, captured.stderr)
            native = json.loads((original / 'report.json').read_text())
            saved = native['trials'][0]
            folder = stage / 'capture'
            (folder / 'trials').mkdir(parents=True)
            folder.joinpath('started.json').write_bytes((original / 'started.json').read_bytes())
            raw = original / 'trials' / (saved['trial_id'] + '.json')
            checkpoint_path = folder / 'trials' / raw.name
            checkpoint_path.write_bytes(raw.read_bytes())
            before = checkpoint_path.read_bytes()
            built_tasks = [json.loads(line) for line in (dataset / 'holdout.jsonl').read_text().splitlines()]
            checkpoint = compare._partial_baseline_checkpoint(stage, tasks, config, built_tasks, dataset)
            with patch.object(layout, 'BINARY', binary), patch.object(layout, 'RUNTIME', root):
                resumed, path = compare._resume_partial_baseline(checkpoint, stage=stage,
                    dataset=dataset, config_path=config_path, config=config, env=env)
            calls = counter.read_text().splitlines()
            self.assertEqual(len(calls), 5)
            self.assertEqual(calls.count(saved['task_id']), 1)
            self.assertEqual(resumed['summary']['reused_trials'], 1)
            self.assertEqual(resumed['summary']['model_calls'], 0)
            self.assertEqual(next(row for row in resumed['trials'] if row['task_id'] == saved['task_id']), saved)
            self.assertEqual(checkpoint_path.read_bytes(), before)
            self.assertTrue(path.with_name('manifest.json').is_file())
            self.assertFalse(list(stage.glob('.checkpoint-*')))

    def test_l0_host_provenance_is_required_for_every_source(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'L0.json'
            with self.assertRaisesRegex(RuntimeError, 'missing:fixture-host'):
                with patch.object(layout, 'host_version', return_value='fixture-host'):
                    compare._require_current_host_version({}, source)
            with self.assertRaisesRegex(RuntimeError, 'legacy-host:fixture-host'):
                with patch.object(layout, 'host_version', return_value='fixture-host'):
                    compare._require_current_host_version(
                        {'host_version': 'legacy-host'}, source)

    def test_capture_report_variant_provenance_is_strict(self):
        expected = layout.host_version()
        self.assertEqual(compare._report_variant_version({
            'configuration': {'variants': [{'id': 'baseline', 'version': expected}]}}), expected)
        self.assertIsNone(compare._report_variant_version({
            'configuration': {'variants': [{'id': 'baseline', 'version': expected},
                                            {'id': 'candidate', 'version': 'other'}]}}))

    def test_failed_capture_costs_survive_resume_without_counting_healthy_or_judge_twice(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            healthy, recovered = native_row('healthy'), native_row('broken')
            failed = {'task_id': 'broken', 'trial_id': 'failed-trial',
                      'execution_status': 'error', 'verdict': 'error',
                      'error_code': 'evaluation_entry_capture_point_capture_degraded',
                      'duration_ms': 36531.16262,
                      'evidence': {'failure_capture': {'complete': False, 'observed_runs': 1, 'event_count': 83},
                                   'exit_code': 137, 'runner_error': 'evaluation_entry_capture_point_capture_degraded'}}
            reports = [('capture', {'run_id': 'first', 'trials': [healthy, failed]}),
                       ('capture-retry-01', {'run_id': 'retry', 'trials': [healthy, recovered],
                        'summary': {'resume_source': {'run_id': 'first'}}}),
                       ('regrade', {'run_id': 'judge', 'trials': [healthy, recovered],
                        'summary': {'resume_source': {'run_id': 'retry'}, 'agent_reexecuted': False}})]
            originals = {}
            for label, report in reports:
                path = root / label / 'report.json'
                path.parent.mkdir()
                path.write_text(json.dumps(report))
                originals[path] = path.read_bytes()
            compact = [compare.compact_row(row, 'shard-00') for row in [healthy, recovered]]
            result = compare.execution_cost_rows(compact, root / 'regrade/report.json', root)
            self.assertEqual(result[0], compact[0])
            broken = result[1]
            self.assertEqual(broken['execution_attempts'], 2)
            self.assertAlmostEqual(broken['latency_ms'], 37531.16262)
            self.assertIsNone(broken['tokens'])
            self.assertIsNone(broken['model_calls'])
            self.assertFalse(broken['tokens_complete'])
            self.assertFalse(broken['execution_costs_complete'])
            self.assertFalse(broken['retry_loop_complete'])
            self.assertEqual(broken['avg_invalid_loop'], compact[1]['avg_invalid_loop'])
            self.assertEqual(compare.execution_cost_rows(result, root / 'regrade/report.json', root), result)
            self.assertEqual(compare.summarize(result, 2)['extra_execution_attempts'], 1)
            for path, before in originals.items():
                self.assertEqual(path.read_bytes(), before)

    def test_known_failed_attempt_costs_are_added_and_missing_lineage_is_unknown(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            failed, success = native_row('task', tokens=40, calls=1), native_row('task', tokens=60, calls=2)
            failed['trial_id'] = 'first-trial'
            first = root / 'capture/report.json'
            first.parent.mkdir()
            first.write_text(json.dumps({'run_id': 'first', 'trials': [failed]}))
            last = root / 'capture-retry-01/report.json'
            last.parent.mkdir()
            native = {'run_id': 'last', 'trials': [success],
                      'summary': {'resume_source': {'run_id': 'first'}}}
            last.write_text(json.dumps(native))
            item = compare.compact_row(success, 'fixture')
            result = compare.execution_cost_rows([item], last, root)[0]
            self.assertEqual((result['latency_ms'], result['tokens'], result['model_calls']), (2000, 100, 3))
            self.assertTrue(result['tokens_complete'])
            first.unlink()
            missing = compare.execution_cost_rows([item], last, root)[0]
            self.assertFalse(missing['execution_costs_complete'])
            self.assertIsNone(missing['tokens'])
            self.assertIsNone(missing['latency_ms'])
            self.assertEqual(missing['known_execution_latency_ms'], 1000)

    def test_cost_lineage_manifest_hint_prioritizes_parent_and_keeps_fallback(self):
        for hint in ('correct', 'missing', 'wrong'):
            with self.subTest(hint=hint), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                first = native_row('task', tokens=40, calls=1)
                first['trial_id'] = 'first-trial'
                last = native_row('task', tokens=60, calls=2)
                parent = root / 'capture-99/report.json'
                unrelated = root / 'capture-00/report.json'
                final = root / 'regrade/report.json'
                for path, value in [(parent, {'run_id': 'first', 'trials': [first]}),
                    (unrelated, {'run_id': 'unrelated', 'trials': []}),
                    (final, {'run_id': 'last', 'trials': [last],
                             'summary': {'resume_source': {'run_id': 'first'}}})]:
                    path.parent.mkdir(parents=True)
                    path.write_text(json.dumps(value))
                unrelated.with_name('manifest.json').write_text(json.dumps({'run_id': 'unrelated'}))
                if hint != 'missing':
                    parent.with_name('manifest.json').write_text(json.dumps({
                        'run_id': 'first' if hint == 'correct' else 'incorrect-hint'}))
                read = layout.read
                bodies = []
                def counted(path):
                    if Path(path).name == 'report.json': bodies.append(Path(path))
                    return read(path)
                with patch.object(layout, 'read', side_effect=counted):
                    row = compare.execution_cost_rows([compare.compact_row(last, 'fixture')], final, root)[0]
                self.assertEqual((row['latency_ms'], row['tokens'], row['model_calls']), (2000, 100, 3))
                self.assertTrue(row['execution_costs_complete'])
                self.assertEqual(unrelated in bodies, hint != 'correct')

    def test_refresh_recompacts_stale_semantic_loop_label_from_formal_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = native_row('task')
            source = root / 'regrade/report.json'
            source.parent.mkdir(parents=True)
            source.write_text(json.dumps({'run_id': 'formal', 'trials': [raw]}))
            stale = compare.compact_row(raw, 'shard-00')
            stale.update({'invalid_loop_basis': 'semantic_runtime',
                          'invalid_loop_count': 0, 'avg_invalid_loop': 0})
            refreshed = compare.execution_cost_rows([stale], source, root)[0]
            self.assertEqual(refreshed['invalid_loop_basis'], 'model_review')
            self.assertEqual(refreshed['invalid_loop_count'], 0.0)
            self.assertEqual(refreshed['avg_invalid_loop'], 0.0)

    def test_summary_does_not_count_semantic_runtime_as_formal_loop(self):
        stale = compare.compact_row(native_row('semantic'), 'fixture')
        stale.update({'invalid_loop_basis': 'semantic_runtime',
                      'invalid_loop_count': 0, 'avg_invalid_loop': 0})
        summary = compare.summarize([stale], 1)
        self.assertIsNone(summary['avg_invalid_loop'])
        self.assertEqual(summary['invalid_loop_coverage'], 0)
        self.assertEqual(summary['loop_basis'], 'model_review_unavailable')

    def test_projection_change_regrades_whole_capture_and_second_resume_calls_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            retained = root / 'review-failures/shard-00'
            retained.mkdir(parents=True)
            old_profile = dict(layout.judge_projection())
            old_profile.pop('keep_dispatch_bindings')
            rows = [native_row('a', quality=.2), native_row('b', tokens=300, calls=6)]
            for row in rows:
                row['checks'] = [{'check_id': 'model', 'status': 'passed'}]
            original = {'complete': True, 'trials': rows}
            source = retained / 'report.json'
            source.write_text(json.dumps(original))
            (retained / 'manifest.json').write_text(json.dumps({'complete': True}))
            config = retained / 'capture.config.json'
            config.write_text(json.dumps({'judge': old_profile}))
            # A healthy but old projection cache cannot override new judging.
            old_grade = retained / 'regrade'
            old_grade.mkdir()
            (old_grade / 'report.json').write_text(json.dumps(original))
            (old_grade / 'manifest.json').write_text(json.dumps({'complete': True}))
            old_grade.with_name('regrade.regrade.config.json').write_text(json.dumps({'judge': old_profile}))
            artifact = {'level': 'L3', 'policy': {'fixture': True}}
            state = {'artifact': artifact, 'shards': {'0': {'complete': True,
                'judge_projection': old_profile}}, 'items': {
                row['task_id']: dict(compare.compact_row(row, 'shard-00'), review_status='complete')
                for row in rows}}
            native = deepcopy(original)
            native['trials'][0]['evidence']['assessment']['quality'] = .4
            commands = []

            def grade(command, **kwargs):
                commands.append(command)
                self.assertIn('--regrade-report', command)
                self.assertNotIn('--resume-rerun-trial', command)
                target = Path(command[command.index('--output-root') + 1])
                target.mkdir(parents=True)
                (target / 'report.json').write_text(json.dumps(native))
                (target / 'manifest.json').write_text(json.dumps({'complete': True}))
                return type('Result', (), {'returncode': 0, 'stderr': '', 'stdout': ''})()

            before = source.read_bytes()
            with patch.object(compare.subprocess, 'run', side_effect=grade), \
                    patch.object(layout.shared, 'capture', side_effect=AssertionError('no Agent rerun')):
                result = compare.recover_candidate_reviews(state, [['a', 'b']], root, artifact, {})
                compare.recover_candidate_reviews(result, [['a', 'b']], root, artifact, {})
                cached, path = compare._review_rows(rows, report_path=source,
                    dataset=root / 'dataset', config_path=config, output_root=old_grade, env={},
                    return_source=True, source_projection=old_profile)
            self.assertEqual(len(commands), 1)
            self.assertEqual(cached, native['trials'])
            self.assertEqual(path, old_grade / 'retry-01/report.json')
            self.assertEqual(result['shards']['0']['judge_projection'], layout.judge_projection())
            self.assertEqual(source.read_bytes(), before)
            self.assertEqual(rows, original['trials'])
            for prior, refreshed in zip(rows, cached):
                self.assertEqual(prior['metrics'], refreshed['metrics'])
                self.assertEqual(prior['duration_ms'], refreshed['duration_ms'])
                self.assertEqual(prior['evidence']['trace'], refreshed['evidence']['trace'])
            self.assertEqual(result['items']['a']['quality'], .4)

    def test_same_scope_baseline_keeps_native_scoring_source_and_profiles_only_completed_grade(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dataset = root / 'dataset'
            dataset.mkdir()
            row = native_row('task')
            row['checks'] = [{'check_id': 'model', 'status': 'passed'}]
            report = {'complete': True, 'host_version': layout.host_version(),
                      'configuration': {'variants': [{'id': 'baseline',
                                                     'version': layout.host_version()}]},
                      'trials': [row]}
            (dataset / 'holdout.jsonl').write_text(json.dumps({'task_id': 'task'}))
            output = root / 'l0-stage32'
            source = output / 'regrade/retry-01/report.json'
            source.parent.mkdir(parents=True)
            source.write_text(json.dumps(report))
            old_profile = dict(layout.judge_projection(), evidence_field_bytes=256)
            config = {'variants': [], 'judge': old_profile}
            (output / 'capture.config.json').write_text(json.dumps(config))
            previous_result = {'status': 'complete', 'host_version': layout.host_version(),
                               'judge_projection': old_profile}
            (output / 'result.json').write_text(json.dumps(previous_result))
            stale = output / 'capture-retry-03/report.json'
            stale.parent.mkdir()
            stale.write_text(json.dumps({'trials': []}))
            calls = []

            def review(rows, **kwargs):
                calls.append(kwargs)
                self.assertEqual(kwargs['report_path'], source)
                self.assertEqual(kwargs['source_projection'], old_profile)
                return rows, source

            arguments = ['compare.py', '--count', '32', '--output', str(output)]
            with patch.object(compare, 'DATASET', dataset), patch.object(sys, 'argv', arguments), \
                    patch.object(compare, 'l0_evaluation_service', return_value=root / 'service.json'), \
                    patch.object(compare, '_prior_baseline_source', return_value=(report, source)), \
                    patch.object(compare, '_expand_baseline', side_effect=AssertionError('same scope no execution')), \
                    patch.object(layout, 'environment', return_value={}), \
                    patch.object(layout, 'evaluation_config', return_value={
                        'variants': [], 'judge': layout.judge_projection()}), \
                    patch.object(layout.shared, 'capture', side_effect=AssertionError('no L0 rerun')), \
                    patch.object(layout, 'PUBLISHED', root):
                (root / 'L0.json').write_text(json.dumps({'level': 'L0'}))
                with patch.object(compare, '_review_rows', side_effect=RuntimeError('judge unavailable')):
                    with self.assertRaisesRegex(RuntimeError, 'judge unavailable'):
                        compare.baseline_main()
                self.assertEqual(json.loads((output / 'result.json').read_text()), previous_result)
                with patch.object(compare, '_review_rows', side_effect=review):
                    self.assertEqual(compare.baseline_main(), 0)
            self.assertEqual(len(calls), 1)
            result = json.loads((output / 'result.json').read_text())
            self.assertEqual(result['judge_projection'], layout.judge_projection())
            self.assertEqual(result['source_report'], str(source))
            self.assertEqual(result['items'][0]['tokens'], 100)
            self.assertEqual(result['items'][0]['latency_ms'], 1000)

    def test_candidate_judge_recovery_keeps_healthy_execution_and_original_costs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            retained = root / 'review-failures/shard-00'
            retained.mkdir(parents=True)
            rows = [native_row('healthy'), native_row('judge-outage')]
            for row in rows:
                row['checks'] = [{'check_id': 'model', 'status': 'passed'}]
            rows[1].update(verdict='error', error_code='evaluation_judge_failed')
            rows[1]['checks'] = [{'check_id': 'model', 'status': 'error'}]
            rows[1]['evidence']['loop_review'] = {'status': 'uncertain', 'count': None}
            source = retained / 'report.json'
            source.write_text(json.dumps({'complete': True, 'trials': rows}))
            (retained / 'manifest.json').write_text(json.dumps({'complete': True}))
            (retained / 'capture.config.json').write_text(json.dumps({'judge': layout.judge_projection()}))
            # Two historical outages must not permanently block a normal
            # resume after the provider/local projection has recovered.
            for name in ('retry-01', 'retry-02'):
                prior = retained / 'regrade' / name
                prior.mkdir(parents=True)
                (prior / 'report.json').write_text(source.read_text())
                (prior / 'manifest.json').write_text(json.dumps({'complete': True}))
                prior.with_name(name + '.regrade.config.json').write_text(
                    json.dumps({'judge': layout.judge_projection()}))
            before = source.read_bytes()
            artifact = {'level': 'L3', 'policy': {'fixture': True}}
            progress = {'artifact': artifact, 'shards': {'0': {'complete': True,
                'judge_projection': layout.judge_projection()}},
                'items': {row['task_id']: dict(compare.compact_row(row, 'shard-00'),
                    review_status=compare.review_row_status(row['evidence']['loop_review']))
                    for row in rows}}
            commands = []

            def native_resume(command, **kwargs):
                commands.append(command)
                self.assertNotIn('TRAJECTORY_UNCONDITIONAL_RESUME', kwargs['env'])
                target = Path(command[command.index('--output-root') + 1])
                target.mkdir(parents=True)
                repaired = deepcopy(rows)
                repaired[1] = native_row('judge-outage')
                repaired[1]['checks'] = [{'check_id': 'model', 'status': 'passed'}]
                (target / 'report.json').write_text(json.dumps({'complete': True, 'trials': repaired}))
                (target / 'manifest.json').write_text(json.dumps({'complete': True}))
                return type('Result', (), {'returncode': 0})()

            with patch.object(compare.subprocess, 'run', side_effect=native_resume), \
                    patch.object(layout.shared, 'capture', side_effect=AssertionError('Agent must not rerun')):
                result = compare.recover_candidate_reviews(progress, [['healthy', 'judge-outage']],
                    root, artifact, {'TRAJECTORY_UNCONDITIONAL_RESUME': '1'})
                reused = compare.recover_candidate_reviews(result, [['healthy', 'judge-outage']],
                    root, artifact, {})
            self.assertEqual(len(commands), 1)
            self.assertIn('--resume-report', commands[0])
            self.assertEqual(Path(commands[0][commands[0].index('--resume-report') + 1]).parent.name,
                             'retry-02')
            self.assertNotIn('--resume-rerun-trial', commands[0])
            self.assertEqual(result, reused)
            self.assertEqual(result['items']['healthy']['tokens'], 100)
            self.assertEqual(result['items']['judge-outage']['tokens'], 100)
            self.assertEqual(result['items']['judge-outage']['review_status'], 'complete')
            self.assertEqual(source.read_bytes(), before)

    def test_judge_context_change_uses_full_regrade_without_relaxed_resume(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'capture/report.json'
            source.parent.mkdir()
            rows = [native_row('healthy'), native_row('judge-outage')]
            rows[0]['checks'] = [{'check_id': 'model', 'status': 'passed'}]
            rows[1].update(verdict='error', error_code='evaluation_judge_failed')
            rows[1]['checks'] = [{'check_id': 'model', 'status': 'error'}]
            rows[1]['evidence']['loop_review'] = {'status': 'uncertain', 'count': None}
            source.write_text(json.dumps({'complete': True, 'trials': rows}))
            source.with_name('manifest.json').write_text(json.dumps({'complete': True}))
            config = root / 'capture.config.json'
            config.write_text(json.dumps({'judge': layout.judge_projection()}))
            corrected = deepcopy(rows)
            corrected[0]['evidence']['assessment']['quality'] = .9
            corrected[1] = native_row('judge-outage')
            corrected[1]['checks'] = [{'check_id': 'model', 'status': 'passed'}]
            commands = []

            def native(command, **kwargs):
                commands.append(command)
                self.assertNotIn('TRAJECTORY_UNCONDITIONAL_RESUME', kwargs['env'])
                if '--resume-report' in command:
                    target = Path(command[command.index('--output-root') + 1])
                    target.mkdir(parents=True)
                    kwargs['stdout'].write('evaluation_checkpoint_context_mismatch\n')
                    return type('Result', (), {'returncode': 2})()
                self.assertIn('--regrade-report', command)
                self.assertNotIn('--resume-rerun-trial', command)
                target = Path(command[command.index('--output-root') + 1])
                target.mkdir(parents=True)
                (target / 'report.json').write_text(json.dumps({'complete': True, 'trials': corrected}))
                (target / 'manifest.json').write_text(json.dumps({'complete': True}))
                return type('Result', (), {'returncode': 0, 'stderr': '', 'stdout': ''})()

            before = source.read_bytes()
            with patch.object(compare.subprocess, 'run', side_effect=native), \
                    patch.object(layout.shared, 'capture', side_effect=AssertionError('no Agent rerun')):
                result, path = compare._review_rows(rows, report_path=source,
                    dataset=root / 'dataset', config_path=config, output_root=root / 'regrade',
                    env={'TRAJECTORY_UNCONDITIONAL_RESUME': '1'}, return_source=True,
                    source_projection=layout.judge_projection())
            self.assertEqual(len(commands), 2)
            self.assertEqual(result, corrected)
            self.assertEqual(result[0]['metrics'], rows[0]['metrics'])
            self.assertEqual(result[1]['duration_ms'], rows[1]['duration_ms'])
            self.assertEqual(source.read_bytes(), before)
            self.assertEqual(path, root / 'regrade/retry-01/report.json')

    def assert_judge_retry_preserves_parallelism(self, source_workers):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'source/report.json'
            source.parent.mkdir()
            rows = [native_row('healthy'), native_row('judge-outage')]
            rows[0]['checks'] = [{'check_id': 'model', 'status': 'passed'}]
            rows[1].update(verdict='error', error_code='evaluation_judge_failed')
            rows[1]['checks'] = [{'check_id': 'model', 'status': 'error'}]
            rows[1]['evidence']['loop_review'] = {'status': 'uncertain', 'count': None}
            source.write_text(json.dumps({'complete': True, 'trials': rows}))
            source.with_name('manifest.json').write_text(json.dumps({'complete': True}))
            original = source.read_bytes()
            config = root / 'capture.config.json'
            wanted = {'parallelism': 16, 'judge': layout.judge_projection()}
            config.write_text(json.dumps(wanted))
            source.parent.with_name('source.regrade.config.json').write_text(
                json.dumps(dict(wanted, parallelism=source_workers)))
            commands = []

            def resume(command, **kwargs):
                commands.append(command)
                self.assertIn('--resume-report', command)
                self.assertNotIn('--regrade-report', command)
                self.assertNotIn('TRAJECTORY_UNCONDITIONAL_RESUME', kwargs['env'])
                self.assertEqual(kwargs['env']['TRAJECTORY_EVALUATION_PARALLELISM'], '4')
                actual = json.loads(Path(command[command.index('--config') + 1]).read_text())
                self.assertEqual(actual['parallelism'], source_workers)
                self.assertEqual(actual['judge'], wanted['judge'])
                target = Path(command[command.index('--output-root') + 1])
                target.mkdir(parents=True)
                repaired = deepcopy(rows)
                repaired[1] = native_row('judge-outage')
                repaired[1]['checks'] = [{'check_id': 'model', 'status': 'passed'}]
                (target / 'report.json').write_text(json.dumps({'complete': True, 'trials': repaired}))
                (target / 'manifest.json').write_text(json.dumps({'complete': True}))
                return type('Result', (), {'returncode': 0})()

            with patch.object(compare.subprocess, 'run', side_effect=resume), \
                    patch.object(layout, 'regrade_report', side_effect=AssertionError('healthy grades must survive')):
                result = compare._review_rows(rows, report_path=source,
                    dataset=root / 'dataset', config_path=config, output_root=root / 'regrade',
                    env={'TRAJECTORY_UNCONDITIONAL_RESUME': '1'})
            self.assertEqual(len(commands), 1)
            self.assertEqual(result[0], rows[0])
            self.assertEqual(source.read_bytes(), original)

    def test_judge_retry_uses_worker_env_without_changing_frozen_sixteen_config(self):
        self.assert_judge_retry_preserves_parallelism(16)

    def test_judge_retry_keeps_existing_four_worker_config_identity(self):
        self.assert_judge_retry_preserves_parallelism(4)

    def test_candidate_missing_judge_evidence_stops_instead_of_rerunning_agent(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            progress = {'shards': {'0': {'complete': True}},
                'items': {'task': {'task_id': 'task', 'review_status': 'uncertain'}}}
            with patch.object(layout.shared, 'capture', side_effect=AssertionError('no Agent rerun')):
                with self.assertRaisesRegex(RuntimeError, 'retained_judge_evidence_missing'):
                    compare.recover_candidate_reviews(progress, [['task']], root, {}, {})

    def test_partial_candidate_resumes_judge_once_and_always_refreshes_comparison(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dataset = root / 'dataset'
            dataset.mkdir()
            healthy = native_row('task')
            healthy['checks'] = [{'check_id': 'model', 'status': 'passed'}]
            failed = deepcopy(healthy)
            failed.update(verdict='error', error_code='evaluation_judge_failed')
            failed['checks'] = [{'check_id': 'model', 'status': 'error'}]
            failed['evidence']['loop_review'] = {'status': 'uncertain', 'count': None}
            (dataset / 'holdout.jsonl').write_text(json.dumps({'task_id': 'task'}))
            l0 = root / 'L0.json'
            l0.write_text(json.dumps({'status': 'complete', 'judge_projection': layout.judge_projection(),
                                      'host_version': layout.host_version(),
                                      'items': [compare.compact_row(healthy, 'L0')]}))
            seed = root / 'strategy.json'
            seed.write_text(json.dumps({'level': 'L3', 'policy': {'fixture': True}}))
            output = root / 'comparison'
            executions, reviews = [], []

            def capture(dataset_path, config_path, destination, env):
                executions.append('task')
                destination.mkdir(parents=True)
                report = {'complete': True, 'trials': [deepcopy(failed)]}
                (destination / 'report.json').write_text(json.dumps(report))
                (destination / 'manifest.json').write_text(json.dumps({'complete': True}))
                return report

            def review(rows, **kwargs):
                reviews.append(kwargs)
                target = kwargs['output_root'] / 'report.json'
                target.parent.mkdir(parents=True, exist_ok=True)
                # Initial run and first resume remain unavailable; the next
                # invocation repairs the same saved Agent execution.
                rows = [deepcopy(healthy if len(reviews) >= 3 else failed)]
                target.write_text(json.dumps({'complete': True, 'trials': rows}))
                target.with_name('manifest.json').write_text(json.dumps({'complete': True}))
                return rows, target

            arguments = ['compare.py', '--seed', str(seed), '--l0-source', str(l0),
                         '--output', str(output), '--task-count', '1', '--workers', '1']
            with patch.object(compare, 'DATASET', dataset), patch.object(sys, 'argv', arguments), \
                    patch.object(layout, 'environment', return_value={}), \
                    patch.object(layout, 'evaluation_config', side_effect=lambda ids, art, workers: {
                        'variants': [{'artifact': art}], 'judge': layout.judge_projection()}), \
                    patch.object(layout.shared, 'capture', side_effect=capture), \
                    patch.object(compare, '_review_rows', side_effect=review):
                for expected_reviews, expected_status in [(1, 'partial'), (2, 'partial'),
                                                          (3, 'complete'), (3, 'complete')]:
                    self.assertEqual(compare.comparison_main(), 0)
                    result = json.loads((output / 'comparison.json').read_text())
                    progress = json.loads((output / 'progress.json').read_text())
                    self.assertEqual(result['status'], expected_status)
                    self.assertEqual(len(reviews), expected_reviews)
                    self.assertEqual(executions, ['task'])
                    self.assertTrue(progress['shards']['0']['execution_complete'])
                    self.assertEqual(progress['shards']['0']['complete'], expected_status == 'complete')
                    self.assertEqual(progress['shards']['0']['scored'], int(expected_status == 'complete'))
                    self.assertEqual(result['items'][0]['tokens'], 100)

    def test_interruption_after_capture_reuses_retained_candidate_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dataset = root / 'dataset'
            dataset.mkdir()
            row = native_row('task')
            row['checks'] = [{'check_id': 'model', 'status': 'passed'}]
            (dataset / 'holdout.jsonl').write_text(json.dumps({'task_id': 'task'}))
            l0 = root / 'L0.json'
            l0.write_text(json.dumps({'status': 'complete', 'judge_projection': layout.judge_projection(),
                                      'host_version': 'fixture-host',
                                      'items': [compare.compact_row(row, 'L0')]}))
            seed = root / 'strategy.json'
            seed.write_text(json.dumps({'level': 'L3', 'policy': {'fixture': True}}))
            output = root / 'comparison'
            executions = []

            def capture(dataset_path, config_path, destination, env):
                source = destination / 'report.json'
                if source.is_file():
                    return json.loads(source.read_text())
                executions.append('task')
                destination.mkdir(parents=True)
                report = {'complete': True, 'trials': [deepcopy(row)]}
                source.write_text(json.dumps(report))
                (destination / 'manifest.json').write_text(json.dumps({'complete': True}))
                return report

            arguments = ['compare.py', '--seed', str(seed), '--l0-source', str(l0),
                '--output', str(output), '--task-count', '1', '--workers', '1']
            with patch.object(compare, 'DATASET', dataset), patch.object(sys, 'argv', arguments), \
                    patch.object(layout, 'environment', return_value={}), \
                    patch.object(layout, 'evaluation_config', side_effect=lambda ids, art, workers: {
                        'variants': [{'artifact': art}], 'judge': layout.judge_projection()}), \
                    patch.object(layout, 'host_version', return_value='fixture-host'), \
                    patch.object(layout.shared, 'capture', side_effect=capture):
                with patch.object(compare, 'recover_baseline_capture',
                        side_effect=RuntimeError('simulated_controller_interruption')):
                    with self.assertRaisesRegex(RuntimeError, 'simulated_controller_interruption'):
                        compare.comparison_main()
                source = output / 'shards/shard-00/capture/report.json'
                self.assertTrue(source.is_file())
                before = source.read_bytes()
                self.assertEqual(compare.comparison_main(), 0)
                self.assertEqual(source.read_bytes(), before)
            self.assertEqual(executions, ['task'])

    def test_relative_baseline_output_reaches_host_as_absolute_evidence_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dataset = root / 'dataset'
            dataset.mkdir()
            (dataset / 'holdout.jsonl').write_text(json.dumps({'task_id': 'fixture'}))
            relative = os.path.relpath(root / 'output')
            arguments = ['compare.py', '--count', '32', '--workers', '16', '--output', relative]

            def capture(dataset_path, config_path, destination, env):
                self.assertTrue(config_path.is_absolute())
                self.assertTrue(destination.is_absolute())
                self.assertTrue(Path(env['APPWORLD_EVIDENCE_ROOT']).is_absolute())
                self.assertEqual(Path(env['APPWORLD_EVIDENCE_ROOT']), root / 'output/official-evidence')
                self.assertEqual(env['TRAJECTORY_EVALUATION_PARALLELISM'], '16')
                raise RuntimeError('fixture_reached_host_entry')

            with patch.object(compare, 'DATASET', dataset), patch.object(sys, 'argv', arguments), \
                    patch.object(compare, 'l0_evaluation_service', return_value=root / 'service.json'), \
                    patch.object(layout, 'read', return_value={'level': 'L0', 'policy': None}), \
                    patch.object(layout, 'evaluation_config', return_value={'variants': []}), \
                    patch.object(layout, 'environment', return_value={}), \
                    patch.object(layout.shared, 'capture', side_effect=capture):
                with self.assertRaisesRegex(RuntimeError, 'fixture_reached_host_entry'):
                    compare.baseline_main()

    def test_l0_service_uses_native_learning_switch_without_unknown_field(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            original = {'learning': {'min_new_runs': 1, 'enabled': True},
                        'runtime_options': {'loop_rules': [{'rule_id': 'learned'}]},
                        'sharing': {'enabled': True}, 'control_learning': {'enabled': True}}
            with patch.object(layout, 'RUNTIME', root), patch.object(layout, 'GEPA', root / 'gepa'), \
                    patch.object(layout, 'read', return_value=deepcopy(original)):
                path = compare.l0_evaluation_service()
            service = json.loads(path.read_text())
            self.assertNotIn('enabled', service['learning'])
            self.assertFalse(service['auto_evaluate'])
            self.assertFalse(service['sharing']['enabled'])
            self.assertFalse(service['control_learning']['enabled'])
            self.assertEqual(service['instrumentation']['level'], 'L0')
            self.assertEqual(service['runtime_options']['loop_rules'], [])
            self.assertTrue(original['learning']['enabled'])

    def test_l0_capture_recovery_reruns_only_missing_execution_then_reuses(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            healthy = native_row('healthy')
            judge_error = native_row('judge-error')
            judge_error.update(error_code='evaluation_judge_failed', verdict='error')
            judge_error['checks'] = [{'check_id': 'model', 'status': 'error',
                                     'reason': 'evaluation_judge_failed'}]
            broken = {'task_id': 'broken', 'trial_id': 'broken-trial',
                      'execution_status': 'unknown', 'error_code': 'unknown_field', 'verdict': 'error'}
            unclosed = native_row('unclosed')
            unclosed['evidence']['trace']['events'].pop()
            original = {'trials': [healthy, judge_error, broken, unclosed]}
            source = root / 'capture/report.json'
            source.parent.mkdir()
            source.write_text(json.dumps(original))
            old_bytes = source.read_bytes()
            calls = []
            repaired = {'trials': [healthy, judge_error, native_row('broken'), native_row('unclosed')]}

            def native(command, **kwargs):
                calls.append(command)
                target = Path(command[command.index('--output-root') + 1])
                target.mkdir()
                (target / 'report.json').write_text(json.dumps(repaired))
                return type('Result', (), {'returncode': 3})()

            kwargs = dict(dataset=root / 'comparison500', config_path=root / 'capture.config.json',
                          output_root=root, env={'TRAJECTORY_EVALUATION_PARALLELISM': '16'})
            with patch.object(compare.subprocess, 'run', side_effect=native):
                report, path = compare.recover_baseline_capture(original, source, **kwargs)
                resumed, reused_path = compare.recover_baseline_capture(original, source, **kwargs)
            self.assertEqual(len(calls), 1)
            command = calls[0]
            self.assertEqual(command[3], str(root / 'comparison500'))
            self.assertEqual(command[command.index('--resume-report') + 1], str(source))
            self.assertEqual([command[i + 1] for i, value in enumerate(command)
                              if value == '--resume-rerun-trial'], ['broken-trial', 'unclosed-trial'])
            self.assertEqual(report['trials'][0], healthy)
            self.assertEqual(report['trials'][1], judge_error)
            self.assertEqual(report, resumed)
            self.assertEqual(path, reused_path)
            self.assertEqual(source.read_bytes(), old_bytes)

    def test_cached_failed_recoveries_do_not_block_fixed_config_next_attempt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            failed = {'trials': [{'task_id': 'broken', 'trial_id': 'broken-trial',
                                 'execution_status': 'unknown', 'error_code': 'unknown_field',
                                 'verdict': 'error'}]}
            source = root / 'capture/report.json'
            source.parent.mkdir()
            source.write_text(json.dumps(failed))
            for number in (1, 2):
                replay = root / f'capture-retry-{number:02d}/report.json'
                replay.parent.mkdir()
                replay.write_text(json.dumps(failed))
            calls = []

            def fixed_native(command, **kwargs):
                calls.append(command)
                target = Path(command[command.index('--output-root') + 1])
                target.mkdir()
                (target / 'report.json').write_text(json.dumps({'trials': [native_row('broken')]}))
                return type('Result', (), {'returncode': 0})()

            kwargs = dict(dataset=root / 'comparison500', config_path=root / 'capture.config.json',
                          output_root=root, env={})
            with patch.object(compare.subprocess, 'run', side_effect=fixed_native):
                report, path = compare.recover_baseline_capture(failed, source, **kwargs)
                compare.recover_baseline_capture(failed, source, **kwargs)
            self.assertEqual(len(calls), 1)
            command = calls[0]
            self.assertEqual(Path(command[command.index('--output-root') + 1]).name, 'capture-retry-03')
            self.assertEqual(Path(command[command.index('--resume-report') + 1]),
                             root / 'capture-retry-02/report.json')
            self.assertEqual(path, root / 'capture-retry-03/report.json')
            self.assertEqual(report['trials'][0]['execution_status'], 'completed')

    def test_each_capture_recovery_invocation_makes_at_most_two_new_requests(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            failed = {'trials': [{'task_id': 'broken', 'trial_id': 'broken-trial',
                                 'execution_status': 'unknown', 'error_code': 'unknown_field',
                                 'verdict': 'error'}]}
            source = root / 'capture/report.json'
            source.parent.mkdir()
            source.write_text(json.dumps(failed))
            calls = []

            def failing_native(command, **kwargs):
                calls.append(command)
                target = Path(command[command.index('--output-root') + 1])
                target.mkdir()
                (target / 'report.json').write_text(json.dumps(failed))
                return type('Result', (), {'returncode': 3})()

            with patch.object(compare.subprocess, 'run', side_effect=failing_native):
                for _ in range(2):
                    before = len(calls)
                    with self.assertRaisesRegex(RuntimeError, 'capture_recovery_exhausted'):
                        compare.recover_baseline_capture(failed, source, dataset=root / 'comparison500',
                            config_path=root / 'config.json', output_root=root, env={})
                    self.assertEqual(len(calls) - before, 2)
            self.assertEqual([Path(command[command.index('--output-root') + 1]).name for command in calls],
                             ['capture-retry-01', 'capture-retry-02', 'capture-retry-03', 'capture-retry-04'])

    def test_review_reuses_31_healthy_rows_and_resumes_only_failed_judge(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, regrade = root / 'capture/report.json', root / 'regrade'
            rows = [native_row(f'fixture-{index:02d}') for index in range(32)]
            for row in rows:
                row['checks'] = [{'check_id': 'model', 'status': 'passed'}]
            bad = rows[-1]
            bad.update(verdict='error', error_code='evaluation_judge_failed')
            bad['checks'] = [{'check_id': 'model', 'status': 'error',
                              'reason': 'evaluation_judge_transport_28'}]
            bad['evidence']['loop_review'] = {'status': 'uncertain', 'count': None}
            original = {'complete': True, 'trials': rows}
            repaired = deepcopy(original)
            repaired['trials'][-1] = native_row('fixture-31')
            repaired['trials'][-1]['checks'] = [{'check_id': 'model', 'status': 'passed'}]

            def pair(path, report):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(json.dumps(report))
                path.with_name('manifest.json').write_text(json.dumps({'complete': True}))

            pair(source, original)
            pair(regrade / 'report.json', original)
            original_bytes = source.read_bytes()
            regrade_bytes = (regrade / 'report.json').read_bytes()
            commands = []

            def resume(command, **kwargs):
                commands.append(command)
                target = Path(command[command.index('--output-root') + 1])
                pair(target / 'report.json', repaired)
                return type('Result', (), {'returncode': 0})()

            kwargs = dict(report_path=source, dataset=root / 'dataset',
                          config_path=root / 'capture.config.json', output_root=regrade, env={})
            with patch.object(compare.subprocess, 'run', side_effect=resume), \
                    patch.object(layout, 'regrade_report', side_effect=AssertionError('no full regrade')):
                result = compare._review_rows(rows, **kwargs)
                cached = compare._review_rows(rows, **kwargs)
            self.assertEqual(len(commands), 1)
            command = commands[0]
            self.assertEqual(command[command.index('--resume-report') + 1],
                             str(regrade / 'report.json'))
            self.assertNotIn('--resume-rerun-trial', command)
            self.assertNotIn('--regrade-report', command)
            self.assertEqual(result[:31], rows[:31])
            self.assertEqual(result, cached)
            self.assertEqual(result[-1]['metrics'], bad['metrics'])
            self.assertEqual(result[-1]['duration_ms'], bad['duration_ms'])
            self.assertEqual(source.read_bytes(), original_bytes)
            self.assertEqual((regrade / 'report.json').read_bytes(), regrade_bytes)
            self.assertIsNone(layout._trial_error_code(result[-1]))

    def test_old_failed_judge_attempts_resume_latest_without_blocking(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, regrade = root / 'capture/report.json', root / 'regrade'
            row = native_row('fixture')
            row.update(verdict='error', error_code='evaluation_judge_failed')
            row['checks'] = [{'check_id': 'model', 'status': 'error'}]
            row['evidence']['loop_review'] = {'status': 'uncertain', 'count': None}
            original = {'complete': True, 'trials': [row]}

            def pair(path, report):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(json.dumps(report))
                path.with_name('manifest.json').write_text(json.dumps({'complete': True}))

            pair(source, original)
            for path in [regrade / 'report.json', regrade / 'retry-01/report.json',
                         regrade / 'retry-02/report.json']:
                pair(path, original)
            (regrade / 'retry-03').mkdir()  # A prior initialization failure is not reusable.
            repaired = native_row('fixture')
            repaired['checks'] = [{'check_id': 'model', 'status': 'passed'}]
            commands = []

            def resume(command, **kwargs):
                commands.append(command)
                target = Path(command[command.index('--output-root') + 1])
                pair(target / 'report.json', {'complete': True, 'trials': [repaired]})
                return type('Result', (), {'returncode': 0})()

            with patch.object(compare.subprocess, 'run', side_effect=resume):
                result = compare._review_rows([row], report_path=source, dataset=root / 'dataset',
                    config_path=root / 'config.json', output_root=regrade, env={})
            self.assertEqual(len(commands), 1)
            self.assertEqual(commands[0][commands[0].index('--resume-report') + 1],
                             str(regrade / 'retry-02/report.json'))
            self.assertEqual(Path(commands[0][commands[0].index('--output-root') + 1]).name,
                             'retry-04')
            self.assertEqual(result, [repaired])

    def test_review_failure_directory_reuses_whole_retained_native_pair(self):
        import math
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'shard/capture/report.json'
            cached = root / 'shard/regrade/report.json'
            output = root / 'review-failures/shard/regrade'
            row = native_row('fixture')
            row.update(variant_id='candidate', task_version='fixed-task',
                       verdict='error', error_code='evaluation_judge_failed')
            row['evidence']['loaded_version'] = {'artifact': 'fixed-artifact'}
            row['evidence']['output']['quality_score'] = .9166666666666665
            row['evidence']['trace']['events'][-1]['output'] = {
                'quality_score': .9166666666666665}
            row['checks'] = [{'check_id': 'model', 'status': 'error',
                              'reason': 'evaluation_evidence_projection_source_mismatch'}]
            row['evidence']['loop_review'] = {'status': 'uncertain', 'count': None}
            original = {'complete': True, 'run_id': 'original-native',
                        'dataset_version': 'fixed-dataset', 'trials': [row]}
            reviewed = deepcopy(original)
            reviewed['run_id'] = 'reviewed-native'
            fixed = reviewed['trials'][0]
            fixed.update(verdict='passed', error_code=None)
            fixed['checks'] = [{'check_id': 'model', 'status': 'passed'}]
            fixed['evidence']['loop_review'] = {'status': 'complete', 'count': 0}
            adjacent = math.nextafter(.9166666666666665, -math.inf)
            fixed['evidence']['output']['quality_score'] = adjacent
            fixed['evidence']['trace']['events'][-1]['output']['quality_score'] = adjacent
            manifest = {'complete': True, 'run_id': reviewed['run_id'],
                        'dataset_version': reviewed['dataset_version'],
                        'files': {'report.json': 'existing-native-manifest-record'}}
            for path, value in [(source, original), (cached, reviewed),
                                (cached.with_name('manifest.json'), manifest)]:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(json.dumps(value))
            before = source.read_bytes(), cached.read_bytes()
            projection = layout.judge_projection()
            config = root / 'current-config.json'
            config.write_text(json.dumps({'judge': projection}))
            with patch.object(compare, '_report_judge_projection', return_value=projection), \
                    patch.object(compare.subprocess, 'run', side_effect=AssertionError('no judge API')), \
                    patch.object(layout, 'regrade_report', side_effect=AssertionError('no regrade')):
                rows, selected = compare._review_rows([row], report_path=source,
                    dataset=root / 'dataset', config_path=config,
                    output_root=output, env={}, return_source=True,
                    source_projection=projection)
            self.assertEqual(selected, cached)
            self.assertEqual(rows, reviewed['trials'])
            self.assertFalse(output.exists())
            self.assertEqual(before, (source.read_bytes(), cached.read_bytes()))

    def test_retained_review_cache_rejects_wrong_scope_cost_artifact_or_profile(self):
        for mutation in ['scope', 'trial', 'cost', 'artifact', 'manifest', 'profile']:
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source = root / 'shard/capture/report.json'
                cached = root / 'shard/regrade/report.json'
                row = native_row('fixture')
                row.update(variant_id='candidate', task_version='fixed-task',
                           verdict='error', error_code='evaluation_judge_failed')
                row['evidence']['loaded_version'] = {'artifact': 'fixed-artifact'}
                row['checks'] = [{'check_id': 'model', 'status': 'error',
                                  'reason': 'evaluation_judge_transport_28'}]
                row['evidence']['loop_review'] = {'status': 'uncertain', 'count': None}
                original = {'complete': True, 'run_id': 'original-native',
                            'dataset_version': 'fixed-dataset', 'trials': [row]}
                reviewed = deepcopy(original)
                reviewed['run_id'] = 'reviewed-native'
                fixed = reviewed['trials'][0]
                fixed.update(verdict='passed', error_code=None)
                fixed['checks'] = [{'check_id': 'model', 'status': 'passed'}]
                fixed['evidence']['loop_review'] = {'status': 'complete', 'count': 0}
                manifest = {'complete': True, 'run_id': reviewed['run_id'],
                            'dataset_version': reviewed['dataset_version'],
                            'files': {'report.json': 'existing-native-manifest-record'}}
                if mutation == 'scope': fixed['task_id'] = 'another-task'
                if mutation == 'trial': fixed['trial_id'] = 'another-trial'
                if mutation == 'cost': fixed['metrics']['tokens'] += 1
                if mutation == 'artifact': fixed['evidence']['loaded_version']['artifact'] = 'another-artifact'
                if mutation == 'manifest': manifest['complete'] = False
                for path, value in [(source, original), (cached, reviewed),
                                    (cached.with_name('manifest.json'), manifest)]:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(json.dumps(value))
                current = layout.judge_projection()
                config = root / 'current-config.json'
                config.write_text(json.dumps({'judge': current}))
                old = dict(current, keep_dispatch_bindings_before_preview=False)
                def projection(path, fallback=None):
                    return old if mutation == 'profile' and path == cached else current
                with patch.object(compare, '_report_judge_projection', side_effect=projection), \
                        patch.object(compare.subprocess, 'run', side_effect=RuntimeError('cache rejected')):
                    with self.assertRaisesRegex(RuntimeError, 'cache rejected'):
                        compare._review_rows([row], report_path=source,
                            dataset=root / 'dataset', config_path=config,
                            output_root=root / 'review-failures/shard/regrade', env={},
                            source_projection=current)

    def test_judge_recovery_remains_bounded_and_rejects_changed_costs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, regrade = root / 'capture/report.json', root / 'regrade'
            row = native_row('fixture')
            row.update(verdict='error', error_code='evaluation_judge_failed')
            row['checks'] = [{'check_id': 'model', 'status': 'error'}]
            row['evidence']['loop_review'] = {'status': 'uncertain', 'count': None}
            report = {'complete': True, 'trials': [row]}
            source.parent.mkdir()
            source.write_text(json.dumps(report))
            calls = []

            def incomplete(command, **kwargs):
                calls.append(command)
                target = Path(command[command.index('--output-root') + 1])
                target.mkdir(parents=True)
                (target / 'report.json').write_text(json.dumps(report))
                (target / 'manifest.json').write_text(json.dumps({'complete': True}))
                return type('Result', (), {'returncode': 3})()

            kwargs = dict(report_path=source, dataset=root / 'dataset',
                          config_path=root / 'config.json', output_root=regrade, env={})
            with patch.object(compare.subprocess, 'run', side_effect=incomplete):
                for _ in range(2):
                    before = len(calls)
                    result = compare._review_rows([row], **kwargs)
                    self.assertEqual(len(calls) - before, 2)
                    with self.assertRaisesRegex(RuntimeError, 'review_incomplete:1'):
                        compare._assert_review_complete(result)
            changed = deepcopy(report)
            changed['trials'][0]['metrics']['tokens'] = 999
            (regrade / 'retry-03/report.json').write_text(json.dumps(changed))
            with patch.object(compare.subprocess, 'run', side_effect=AssertionError('no unsafe retry')):
                with self.assertRaisesRegex(RuntimeError, 'changed_execution_or_scope'):
                    compare._review_rows([row], **kwargs)

    def test_prior_stage_ignores_historical_execution_contract_family(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "gepa"
            root.mkdir()
            old = root / 'l0-stage32'
            old.mkdir()
            old.joinpath('result.json').write_text(json.dumps({
                'status': 'complete', 'items': [{'task_id': 'fixture-001'}]}))
            self.assertIsNone(compare._prior_baseline_source(
                root / 'l0-current-stage100', ['fixture-001'], {},
                [{'task_id': 'fixture-001'}]))
            self.assertTrue(old.joinpath('result.json').is_file())

    def test_prior_stage_recovers_exact_native_source_without_merging_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "gepa"
            root.mkdir()
            stage = root / 'l0-current-stage32'
            source = stage / 'regrade/retry-01/report.json'
            rows = [native_row(f'fixture-{index:03d}') for index in range(32)]
            for row in rows:
                row.update(task_version='fixture-task-v1', variant_id='candidate')
                row['checks'] = [{'check_id': 'model', 'status': 'passed'}]
            native = {'run_id': 'fixture-stage32-run', 'complete': True, 'trials': rows,
                      'configuration': {'repetitions': 1, 'splits': ['holdout'],
                          'model_enabled': True, 'allow_unreviewed': False,
                          'variants': [{'id': 'candidate', 'version': 'fixture-v1'}]}}
            items = [compare.compact_row(row, 'l0-baseline') for row in rows]
            source.parent.mkdir(parents=True)
            source.write_text(json.dumps(native))
            source.with_name('manifest.json').write_text(json.dumps({'complete': True}))
            capture = stage / 'capture/report.json'
            capture.parent.mkdir()
            capture.write_text(json.dumps(native))
            capture.with_name('manifest.json').write_text(json.dumps({'complete': True}))
            stage.joinpath('result.json').write_text(json.dumps(
                {'status': 'complete', 'host_version': layout.host_version(),
                 'items': items, 'metrics': compare.summarize(items, 32)}))
            tasks = [{'task_id': f'fixture-{index:03d}', 'task_version': 'fixture-task-v1'}
                     for index in range(100)]
            config = {'task_ids': [task['task_id'] for task in tasks], 'parallelism': 16,
                      'max_trials': 100, 'repetitions': 1, 'splits': ['holdout'],
                      'judge': dict(layout.judge_projection(), enabled=True, max_model_calls=800),
                      'variants': [{'id': 'candidate', 'version': 'fixture-v1'}]}
            old = deepcopy(config)
            old.update(task_ids=config['task_ids'][:32], max_trials=32, parallelism=4)
            old['judge']['max_model_calls'] = 256
            stage.joinpath('capture.config.json').write_text(json.dumps(old))
            source.parent.with_name('retry-01.regrade.config.json').write_text(json.dumps(old))
            report, path = compare._prior_baseline_source(root / 'l0-current-stage100',
                config['task_ids'], config, tasks)
            self.assertEqual(path, source)
            self.assertEqual(report, native)
            saved = json.loads(stage.joinpath('result.json').read_text())
            self.assertEqual(saved['source_report'], str(source))
            self.assertEqual(saved['items'], items)
            # A compact score changed by hand cannot authorize native reuse.
            saved['items'][0]['tokens'] = 1
            stage.joinpath('result.json').write_text(json.dumps(saved))
            with self.assertRaisesRegex(RuntimeError, 'complete_native_source_missing'):
                compare._prior_baseline_source(root / 'l0-current-stage100', config['task_ids'], config, tasks)

    @unittest.skipUnless(os.environ.get('APPWORLD_NATIVE_CLI'), 'native CLI opt-in fixture')
    def test_native_scope_expansion_32_to_100_executes_only_68_new_tasks(self):
        """Real native recovery with a controlled host, no model or external API."""
        import subprocess
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = Path(os.environ['APPWORLD_NATIVE_CLI'])
            task_ids = [f'fixture-{index:03d}' for index in range(100)]
            cases = [{'task_id': task, 'group_id': task, 'task_input': f'Return known value for {task}.',
                'expected_behavior': 'State value is 20.', 'difficulty': 'easy', 'category': 'data',
                'initial_state': {'value': 20}, 'followup_inputs': [], 'split': 'holdout',
                'review_status': 'reviewed'} for task in task_ids]
            golden = root / 'tasks.jsonl'
            golden.write_text('\n'.join(json.dumps(task) for task in cases) + '\n')
            build_config = json.loads((ROOT / 'tools/trajectory_evolution/config/dataset.json').read_text())
            build_config.update(sources=[{'kind': 'golden', 'path': str(golden)}],
                                max_tasks=100, max_model_calls=0)
            build_path = root / 'dataset.json'
            build_path.write_text(json.dumps(build_config))
            dataset = root / 'dataset'
            result = subprocess.run([str(binary), 'dataset', 'build', '--config', str(build_path),
                '--output-root', str(dataset)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            counter, host = root / 'executions.txt', root / 'host.py'
            host.write_text("""import json,pathlib,subprocess,sys
request=json.load(sys.stdin)
with pathlib.Path(sys.argv[1]).open('a') as output: output.write(request['task']['task_id']+'\\n')
response=json.loads(subprocess.check_output([sys.executable,'-B',sys.argv[2],'trace'],input=json.dumps(request).encode()))
response['metrics'].update(tokens=100,llm_calls=2)
print(json.dumps(response))
""")
            config = {'schema_version': 1, 'splits': ['holdout'], 'task_ids': task_ids[:32],
                'repetitions': 1, 'parallelism': 16, 'max_trials': 32, 'timeout_ms': 10000,
                'require_trace': True, 'max_report_bytes': 67108864,
                'variants': [{'id': 'baseline', 'version': 'fixture-v1',
                    'command': [sys.executable, '-B', str(host), str(counter),
                                str(ROOT / 'tools/trajectory_evolution/tests/evaluation_host.py')],
                    'pass_env': []}],
                'checks': {'*': [{'id': 'value', 'path': '/state/value', 'op': 'equals', 'value': 20}]},
                'judge': {'enabled': False}}
            config_path = root / 'evaluation.json'
            config_path.write_text(json.dumps(config))
            source = root / 'stage32/report.json'
            result = subprocess.run([str(binary), 'dataset', 'run', str(dataset), '--config',
                str(config_path), '--output-root', str(source.parent)],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            old = json.loads(source.read_text())
            self.assertEqual(len(counter.read_text().splitlines()), 32)
            before = source.read_bytes()
            config.update(task_ids=task_ids, max_trials=100)
            config_path.write_text(json.dumps(config))
            with patch.object(layout, 'BINARY', binary), patch.object(layout, 'RUNTIME', root):
                expanded = compare._expand_baseline(old, source, dataset=dataset,
                    config_path=config_path, output_root=root / 'stage100', task_ids=task_ids,
                    env={'PATH': os.environ['PATH'], 'NO_PROXY': '*', 'no_proxy': '*'})
                cached = compare._expand_baseline(old, source, dataset=dataset,
                    config_path=config_path, output_root=root / 'stage100', task_ids=task_ids,
                    env={'PATH': os.environ['PATH']})
            executions = counter.read_text().splitlines()
            self.assertEqual(len(executions), 100)
            self.assertEqual(len(set(executions)), 100)
            self.assertEqual(len(executions[32:]), 68)
            self.assertEqual(expanded, cached)
            self.assertEqual(expanded['summary']['reused_trials'], 32)
            self.assertEqual(expanded['summary']['model_calls'], 0)
            by_id = {row['task_id']: row for row in expanded['trials']}
            self.assertTrue(all(by_id[row['task_id']] == row for row in old['trials']))
            self.assertEqual(source.read_bytes(), before)

    def test_same_strategy_resumes_and_changed_strategy_only_retests_candidate(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dataset = root / 'dataset'
            dataset.mkdir()
            raw = [native_row(), native_row('appworld-other_1')]
            for row in raw:
                row['checks'] = [{'check_id': 'model', 'status': 'passed'}]
            (dataset / 'holdout.jsonl').write_text('\n'.join(
                json.dumps({'task_id': row['task_id']}) for row in raw))
            l0 = root / 'L0.json'
            l0.write_text(json.dumps({'status': 'complete', 'judge_projection': layout.judge_projection(),
                                      'host_version': 'fixture-host',
                                      'items': [compare.compact_row(row, 'original-L0') for row in raw]}))
            original_l0 = l0.read_bytes()
            seed, output = root / 'strategy.json', root / 'comparison'
            artifact = {'level': 'L3', 'policy': {'fixture_quality': .6}}
            seed.write_text(json.dumps(artifact))
            calls = []

            def capture(dataset_path, config_path, destination, env):
                config = json.loads(config_path.read_text())
                current = config['variants'][0]['artifact']
                calls.append(deepcopy(current))
                rows = deepcopy(raw)
                for row in rows:
                    row['evidence']['assessment']['quality'] = current['policy']['fixture_quality']
                native = {'run_id': 'fixture-' + str(len(calls)), 'complete': True, 'trials': rows}
                destination.mkdir(parents=True)
                (destination / 'report.json').write_text(json.dumps(native))
                return native

            arguments = ['compare.py', '--seed', str(seed), '--l0-source', str(l0),
                         '--output', str(output), '--task-count', '2', '--workers', '2']
            with patch.object(compare, 'DATASET', dataset), patch.object(sys, 'argv', arguments), \
                    patch.object(layout, 'environment', return_value={}), \
                    patch.object(layout, 'evaluation_config', side_effect=lambda ids, art, workers: {
                        'variants': [{'artifact': art}], 'judge': layout.judge_projection()}), \
                    patch.object(layout, 'host_version', return_value='fixture-host'), \
                    patch.object(layout, 'version', side_effect=AssertionError('new cache needs no version check')), \
                    patch.object(layout.shared, 'capture', side_effect=capture):
                self.assertEqual(compare.comparison_main(), 0)
                self.assertEqual(compare.comparison_main(), 0)
                self.assertEqual(len(calls), 1, 'same strategy must reuse the completed candidate')
                artifact['policy']['fixture_quality'] = .9
                seed.write_text(json.dumps(artifact))
                self.assertEqual(compare.comparison_main(), 0)
            self.assertEqual(len(calls), 2)
            final = json.loads((output / 'comparison.json').read_text())
            self.assertEqual(final['metrics']['L0_reused']['quality'], .8)
            self.assertEqual(final['metrics']['seed_' + root.name.replace('-', '_')]['quality'], .9)
            self.assertEqual(l0.read_bytes(), original_l0)

    def test_legacy_evidence_recovers_or_schedules_without_blocking(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            shard_dir = root / 'shards'
            shard_dir.mkdir()
            artifact = {'level': 'L3', 'policy': {'fixture': True}}
            row = {'task_id': 'task', 'loaded_version': 'native-original-version', 'quality': 1}
            checkpoint = shard_dir / 'shard-00.json'
            checkpoint.write_text(json.dumps({'complete': True, 'items': [row]}))
            with patch.object(layout, 'version', return_value='native-original-version'):
                resumed = compare.recover_candidate_progress({}, [['task']], shard_dir, artifact)
            self.assertEqual(resumed['items']['task'], row)
            with patch.object(layout, 'version', return_value='native-different-version'):
                resumed = compare.recover_candidate_progress({}, [['task']], shard_dir, artifact)
            self.assertEqual(resumed['shards'], {})
            with patch.object(layout, 'version', side_effect=RuntimeError('legacy evidence unavailable')):
                resumed = compare.recover_candidate_progress({}, [['task']], shard_dir, artifact)
            self.assertEqual(resumed['items'], {})
            row.pop('loaded_version')
            checkpoint.write_text(json.dumps({'complete': True, 'items': [row]}))
            resumed = compare.recover_candidate_progress({}, [['task']], shard_dir, artifact)
            self.assertEqual(resumed['items'], {}, 'unknown old evidence must schedule a candidate measurement')
            config = root / 'review-failures/shard-00/capture.config.json'
            config.parent.mkdir(parents=True)
            config.write_text(json.dumps({'variants': [{'id': 'candidate', 'artifact': artifact}]}))
            resumed = compare.recover_candidate_progress({}, [['task']], shard_dir, artifact)
            self.assertEqual(resumed['items']['task'], row)
            checkpoint.write_text(json.dumps({'complete': True, 'items': [{'task_id': []}]}))
            resumed = compare.recover_candidate_progress({'items': [], 'shards': []},
                                                         [['task']], shard_dir, artifact)
            self.assertEqual(resumed['shards'], {})

    def test_native_cost_and_duration_precedence_without_changing_raw_row(self):
        raw = native_row()
        original = deepcopy(raw)
        compact = compare.compact_row(raw, 'fixture')
        self.assertEqual((compact['tokens'], compact['model_calls'], compact['latency_ms']), (100, 2, 1000))
        self.assertTrue(compact['tokens_complete'])
        self.assertEqual(raw, original)

    def test_summary_means_totals_and_formal_strict_match_layout(self):
        rows = [native_row(), native_row('appworld-other_1', tokens=300, calls=6,
                                         quality=.4, verdict='failed')]
        compact = [compare.compact_row(row, 'fixture') for row in rows]
        summary = compare.summarize(compact, 2)
        expected = layout.metrics(rows, planned=2)
        for key in ('quality', 'scored', 'strict_passed', 'official_strict_passed', 'tokens',
                    'model_calls', 'llm_calls', 'latency_ms', 'total_tokens', 'total_model_calls',
                    'quality_status', 'invalid_loop_status', 'levels'):
            self.assertEqual(summary[key], expected[key], key)
        self.assertEqual(summary['tokens'], 200)
        self.assertEqual(summary['total_tokens'], 400)
        self.assertEqual(summary['model_calls'], 4)
        self.assertEqual(summary['strict_passed'], 1)
        self.assertEqual(summary['official_strict_passed'], 2)

    def test_judge_error_keeps_official_auxiliary_and_hides_formal_scores(self):
        raw = native_row()
        raw.update(error_code='evaluation_judge_failed', verdict='error')
        raw['checks'] = [{'check_id': 'model', 'status': 'error',
                          'reason': 'evaluation_judge_transport_failed'}]
        compact = compare.compact_row(raw, 'fixture')
        summary = compare.summarize([compact], 1)
        self.assertEqual(compact['error_code'], 'evaluation_judge_failed')
        self.assertEqual(compact['reward'], 1)
        self.assertIsNone(compact['quality'])
        self.assertIsNone(compact['avg_invalid_loop'])
        self.assertEqual(summary['scored'], 0)
        self.assertEqual(summary['errored'], 1)
        self.assertIsNone(summary['strict_passed'])
        self.assertEqual(summary['official_strict_passed'], 1)
        self.assertEqual(summary['quality_status'], 'unavailable')

    def test_unfinished_planned_scope_never_claims_complete(self):
        compact = compare.compact_row(native_row(), 'fixture')
        summary = compare.summarize([compact], 2)
        self.assertEqual(summary['missing'], 1)
        self.assertEqual(summary['quality_status'], 'partial')
        self.assertEqual(summary['invalid_loop_status'], 'partial')
        self.assertFalse(summary['tokens_complete'])
        self.assertEqual(summary['levels']['L1'], 0)
        self.assertEqual(summary['tokens'], 100)

    def test_partial_planned_scope_reports_only_observed_closed_application_counts(self):
        raw = native_row()
        raw['evidence']['output']['final'].update(level='L0', framework_capture={'status': 'complete'})
        original = deepcopy(raw)
        business = layout._business_metrics._aggregate([raw], planned=90)
        expected = layout.metrics([raw], planned=90)
        compact = compare.summarize([compare.compact_row(raw, 'L0')], 90)
        self.assertEqual(expected['levels'], {level: 0 for level in ('L1', 'L2', 'L2_action', 'L2_schedule', 'L3')})
        self.assertEqual(compact['levels'], expected['levels'])
        self.assertFalse(expected['quality_complete'])
        self.assertEqual(expected['missing'], 89)
        # The business getter has no native receipt evidence in this legacy
        # shape. Its unknown remains unknown; we do not rewrite its result.
        self.assertIsNone(business['level_counts'])
        self.assertEqual(raw, original)
        for gap in ('open_trace', 'observer_incomplete'):
            missing = deepcopy(raw)
            if gap == 'open_trace':
                missing['evidence']['trace']['events'].pop()
            else:
                missing['evidence']['output']['final']['framework_capture']['status'] = 'incomplete'
            with self.subTest(gap=gap):
                measured = layout.metrics([missing], planned=90)
                reduced = compare.summarize([compare.compact_row(missing, 'L0')], 90)
                self.assertTrue(all(value is None for value in measured['levels'].values()))
                self.assertEqual(reduced['levels'], measured['levels'])

    def test_contradictory_complete_review_keeps_replay_required(self):
        for review in (
            {'status': 'complete', 'count': -1},
            {'status': 'complete', 'count': 2, 'observed_iterations': 1},
            {'status': 'complete', 'count': 1, 'observed_iterations': 1,
             'wasted_iterations': [], 'iteration_reviews': [{'decision': 'keep'}]},
        ):
            with self.subTest(review=review):
                raw = native_row()
                raw['evidence']['loop_review'] = review
                self.assertIn('loop_review:uncertain', compare.replay_reasons(raw, review_enabled=True))
                with self.assertRaisesRegex(RuntimeError, 'l0_baseline_review_incomplete'):
                    compare._assert_review_complete([raw])
                compact = compare.compact_row(raw, 'fixture')
                self.assertIsNone(compact['avg_invalid_loop'])

    def test_unknown_iteration_inventory_is_not_a_measured_zero(self):
        first, second = native_row(), native_row('appworld-other_1')
        first['evidence']['loop_review'].pop('observed_iterations')
        compact = [compare.compact_row(row, 'fixture') for row in (first, second)]
        summary = compare.summarize(compact, 2)
        expected = layout.metrics([first, second], planned=2)
        self.assertEqual(summary['avg_invalid_loop'], 0)
        self.assertIsNone(summary['loop_observed_iterations'])
        self.assertEqual(summary['loop_observed_coverage'], 1)
        for key in ('avg_invalid_loop', 'loop_observed_iterations', 'loop_observed_coverage'):
            self.assertEqual(summary[key], expected[key], key)

    def test_five_actual_application_counts_survive_compaction(self):
        raw = native_row()
        events = raw['evidence']['trace']['events']
        for i, boundary in enumerate(('llm', 'actions', 'schedule', 'loop')):
            events.insert(1, {'kind': 'advice.applied', 'event_id': str(i),
                             'attributes': {'receipt': {'boundary': boundary,
                                                       'application': {'status': 'applied'}}}})
        compact = compare.compact_row(raw, 'fixture')
        summary = compare.summarize([compact], 1)
        self.assertEqual(summary['levels'], {'L1': 1, 'L2': 2, 'L2_action': 1, 'L2_schedule': 1, 'L3': 1})
        self.assertEqual(compact['l2_action_applied'], 1)
        self.assertEqual(compact['l2_schedule_applied'], 1)

    def test_intent_micro_counts_and_absent_coverage_match_layout(self):
        rows = [native_row(), native_row('appworld-other_1')]
        for row, actual in zip(rows, ([], ['a', 'b', 'c'])):
            row['evidence']['intent_observations'] = [{'turn': 1, 'status': 'measured',
                'expected_targets': ['a', 'b'], 'actual_targets': actual,
                'matched': [], 'extra': [], 'missing': []}]
        compact = [compare.compact_row(row, 'fixture') for row in rows]
        summary = compare.summarize(compact, 2)
        expected = layout.metrics(rows, planned=2)
        self.assertEqual(summary['intent_counts'], [2, 1, 2])
        self.assertAlmostEqual(summary['intent_f1'], 4/7)
        for key in ('intent_f1', 'intent_counts', 'intent_measured', 'intent_task_measured'):
            self.assertEqual(summary[key], expected[key])
        missing = compare.summarize([compare.compact_row(native_row(), 'fixture')], 1)
        self.assertIsNone(missing['intent_f1'])
        self.assertIsNone(missing['intent_counts'])
        self.assertEqual(missing['intent_measured'], 0)


class PendingShardWorkerTest(unittest.TestCase):
    def run_worker_fixture(self, count, shard_size, retained, *, cross_stage=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dataset, output = root / 'dataset', root / 'comparison'
            dataset.mkdir()
            rows = [native_row(f'task-{index:03d}') for index in range(count)]
            for row in rows:
                row['checks'] = [{'check_id': 'model', 'status': 'passed'}]
            dataset.joinpath('holdout.jsonl').write_text('\n'.join(
                json.dumps({'task_id': row['task_id']}) for row in rows))
            artifact = {'level': 'L1', 'fixture': 'same-frozen-candidate'}
            seed, l0 = root / 'strategy.json', root / 'L0.json'
            seed.write_text(json.dumps(artifact))
            l0.write_text(json.dumps({'status': 'complete', 'judge_projection': layout.judge_projection(),
                                      'host_version': 'fixture-host',
                                     'items': [compare.compact_row(row, 'l0') for row in rows]}))
            preserved = None
            if retained:
                pilot = root / 'pilot' if cross_stage else output
                original = pilot / 'shards' / 'shard-00'
                (original / 'capture').mkdir(parents=True)
                config = original / 'config.json'
                config.write_text(json.dumps({'variants': [{'id': 'candidate', 'artifact': artifact}],
                                              'parallelism': 16, 'max_trials': retained,
                                              'judge': layout.judge_projection()}))
                native = {'complete': True, 'trials': rows[:retained]}
                source = original / 'capture' / 'report.json'
                source.write_text(json.dumps(native))
                source.with_name('manifest.json').write_text(json.dumps({'complete': True}))
                selected = [row['task_id'] for row in rows[:retained]]
                checkpoint = pilot / 'shards' / 'shard-00.json'
                checkpoint.write_text(json.dumps({'artifact': artifact, 'task_ids': selected,
                    'execution_complete': True, 'complete': True,
                    'judge_projection': layout.judge_projection(),
                    'source_report': str(source), 'review_distribution': {'complete': retained},
                    'items': [dict(compare.compact_row(row, 'shard-00'), review_status='complete')
                              for row in rows[:retained]]}))
                pilot.joinpath('progress.json').write_text(json.dumps({
                    'artifact': artifact, 'planned_tasks': retained, 'items': {}, 'shards': {}}))
                preserved = (config, config.read_bytes(), source, source.read_bytes())
                if cross_stage:
                    import reproduce
                    pilot.joinpath('comparison.json').write_text(json.dumps({
                        'status': 'complete', 'host_version': 'fixture-host',
                        'items': [compare.compact_row(row, 'shard-00') for row in rows[:retained]]}))
                    attempt = {'pilot_comparison': str(pilot / 'comparison.json'),
                               'validation_comparison': str(output / 'comparison.json')}
                    with patch.object(reproduce, '_host_version', return_value='fixture-host'):
                        self.assertTrue(reproduce._inherit_pilot_capture(
                            seed, attempt, [row['task_id'] for row in rows]))
            scheduled = []

            def capture(dataset_path, config_path, destination, env):
                config = json.loads(config_path.read_text())
                ids = config['task_ids']
                scheduled.append((ids, config['parallelism'], env['TRAJECTORY_EVALUATION_PARALLELISM']))
                native = {'complete': True, 'trials': [row for row in rows if row['task_id'] in ids]}
                destination.mkdir(parents=True, exist_ok=True)
                destination.joinpath('report.json').write_text(json.dumps(native))
                destination.joinpath('manifest.json').write_text(json.dumps({'complete': True}))
                return native

            arguments = ['compare.py', '--seed', str(seed), '--l0-source', str(l0),
                         '--output', str(output), '--task-count', str(count),
                         '--shard-size', str(shard_size), '--workers', '16']
            with patch.object(compare, 'DATASET', dataset), patch.object(sys, 'argv', arguments), \
                    patch.object(layout, 'environment', return_value={}), \
                    patch.object(layout, 'host_version', return_value='fixture-host'), \
                    patch.object(layout, 'evaluation_config', side_effect=lambda ids, art, workers: {
                        'variants': [{'id': 'candidate', 'artifact': art}],
                        'judge': layout.judge_projection()}), \
                    patch.object(layout.shared, 'capture', side_effect=capture):
                self.assertEqual(compare.comparison_main(), 0)
            if preserved:
                config, config_bytes, source, source_bytes = preserved
                self.assertEqual(config.read_bytes(), config_bytes)
                self.assertEqual(source.read_bytes(), source_bytes)
            self.assertEqual({task for ids, _, _ in scheduled for task in ids},
                             {row['task_id'] for row in rows[retained:]})
            return scheduled

    def test_closed_thirty_two_leaves_sixteen_workers_for_pending_sixty_eight(self):
        scheduled = self.run_worker_fixture(100, 100, 32)
        self.assertEqual([(len(ids), workers, env) for ids, workers, env in scheduled], [(68, 16, '16')])

    def test_two_pending_shards_share_sixteen_total_workers(self):
        scheduled = self.run_worker_fixture(64, 32, 0)
        self.assertEqual(sorted((len(ids), workers, env) for ids, workers, env in scheduled),
                         [(32, 8, '8'), (32, 8, '8')])
        self.assertLessEqual(sum(workers for _, workers, _ in scheduled), 16)

    def test_cross_stage_links_run_only_new_sixty_eight_with_sixteen_workers(self):
        scheduled = self.run_worker_fixture(100, 100, 32, cross_stage=True)
        self.assertEqual([(len(ids), workers, env) for ids, workers, env in scheduled], [(68, 16, '16')])


class CrossStageCandidateResumeTest(unittest.TestCase):
    def fixture(self, *, unknown_cost=False, changed_task=False, changed_artifact=False):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        source, target, dataset = root / 'stage100', root / 'stage500', root / 'dataset'
        dataset.mkdir()
        artifact = {'level': 'L1', 'fixture': 'frozen-candidate'}
        seed = root / 'strategy.json'
        seed.write_text(json.dumps(artifact))
        rows = [native_row(f'task-{index:03d}') for index in range(500)]
        for row in rows:
            row.update(task_version='fixture-task', variant_id='candidate')
            row['evidence']['loaded_version'] = 'fixture-version'
            row['checks'] = [{'check_id': 'model', 'status': 'passed'}]
        ids = [row['task_id'] for row in rows]
        dataset.joinpath('holdout.jsonl').write_text('\n'.join(json.dumps(
            {'task_id': task, 'task_version': 'fixture-task'}) for task in ids))
        dataset.joinpath('manifest.json').write_text(json.dumps(
            {'metadata': {'dataset_version': 'fixture-dataset'}}))
        variant = {'id': 'candidate', 'version': 'fixture-version', 'artifact': artifact,
                   'command': [str(layout.PYTHON), '-B', str(layout.HOST), '--run-entry']}
        progress = {'planned_tasks': 100, 'artifact': artifact, 'items': {}, 'shards': {}}
        source_items = []
        for index, selected in enumerate((rows[:32], rows[32:100])):
            label = f'shard-{index:02d}'
            folder = source / 'shards' / label
            (folder / 'capture').mkdir(parents=True)
            config = {'variants': [variant], 'task_ids': [r['task_id'] for r in selected],
                      'judge': layout.judge_projection(), 'parallelism': 16}
            (folder / 'config.json').write_text(json.dumps(config))
            prior = deepcopy(selected)
            for row in prior:
                row['trial_id'] += '-previous'
                row['metrics'].update(tokens=50, llm_calls=1)
            old_run = f'earlier-{index}'
            previous = folder / 'capture-previous'
            previous.mkdir()
            previous.joinpath('report.json').write_text(json.dumps(
                {'complete': True, 'run_id': old_run, 'trials': prior}))
            native = {'complete': True, 'dataset_version': 'fixture-dataset',
                      'run_id': f'capture-{index}', 'trials': deepcopy(selected),
                      'summary': {'resume_source': {'run_id': old_run}}}
            if unknown_cost:
                native['summary']['resume_source']['run_id'] = 'unrecorded-ancestor'
            if changed_task:
                native['trials'][0]['task_version'] = 'different-task'
            report = folder / 'capture/report.json'
            report.write_text(json.dumps(native))
            report.with_name('manifest.json').write_text(json.dumps({'complete': True}))
            items = compare.execution_cost_rows(
                [compare.compact_row(row, label) for row in native['trials']], report, folder)
            for item in items:
                item.update(review_status='complete', loop_review_status='complete')
            saved = {'artifact': artifact, 'task_ids': [r['task_id'] for r in selected],
                     'execution_complete': True, 'complete': True, 'items': items,
                     'source_report': str(report), 'judge_projection': layout.judge_projection(),
                     'review_distribution': {'complete': len(selected)}}
            (source / 'shards' / f'{label}.json').write_text(json.dumps(saved))
            source_items.extend(items)
        source.joinpath('progress.json').write_text(json.dumps(progress))
        comparison = source / 'comparison.json'
        comparison.write_text(json.dumps({'status': 'complete', 'host_version': 'fixture-host',
                                          'items': source_items}))
        if changed_artifact:
            progress['artifact'] = {'level': 'L1', 'fixture': 'other-candidate'}
            source.joinpath('progress.json').write_text(json.dumps(progress))
        l0 = root / 'L0.json'
        l0.write_text(json.dumps({'status': 'complete', 'host_version': 'fixture-host',
            'judge_projection': layout.judge_projection(),
            'items': [compare.compact_row(row, 'L0') for row in rows]}))
        return root, source, target, dataset, seed, l0, artifact, rows, ids, comparison

    def test_32_plus_68_closed_sources_schedule_only_400_and_preserve_all_attempt_costs(self):
        root, source, target, dataset, seed, l0, artifact, rows, ids, comparison = self.fixture()
        originals = {p: p.read_bytes() for p in source.rglob('*.json')}
        scheduled = []

        def capture(dataset_path, config_path, destination, env):
            config = json.loads(config_path.read_text())
            chosen = config['task_ids']
            scheduled.append((chosen, config['parallelism'], env['TRAJECTORY_EVALUATION_PARALLELISM']))
            native = {'complete': True, 'run_id': destination.parent.name,
                      'trials': [row for row in rows if row['task_id'] in chosen]}
            destination.mkdir(parents=True)
            destination.joinpath('report.json').write_text(json.dumps(native))
            destination.joinpath('manifest.json').write_text(json.dumps({'complete': True}))
            return native

        arguments = ['compare.py', '--seed', str(seed), '--l0-source', str(l0),
                     '--output', str(target), '--task-count', '500', '--shard-size', '100',
                     '--workers', '16', '--resume-candidate-source', str(comparison)]
        with patch.object(compare, 'DATASET', dataset), patch.object(sys, 'argv', arguments), \
                patch.object(layout, 'environment', return_value={}), \
                patch.object(layout, 'host_version', return_value='fixture-host'), \
                patch.object(layout, 'evaluation_config', side_effect=lambda chosen, art, workers: {
                    'variants': [{'id': 'candidate', 'artifact': art}],
                    'judge': layout.judge_projection()}), \
                patch.object(layout.shared, 'capture', side_effect=capture):
            self.assertEqual(compare.comparison_main(), 0)
            self.assertEqual(compare.comparison_main(), 0)  # same command: no second execution
        self.assertEqual(len(scheduled), 4)
        self.assertEqual({task for chosen, _, _ in scheduled for task in chosen}, set(ids[100:]))
        self.assertTrue(all(len(chosen) == 100 and workers == 8 and env == '8'
                            for chosen, workers, env in scheduled))
        final = json.loads(target.joinpath('comparison.json').read_text())
        self.assertEqual(len(final['items']), 500)
        for item in final['items'][:100]:
            self.assertEqual(item['tokens'], 150)
            self.assertEqual(item['model_calls'], 3)
            self.assertEqual(item['execution_attempts'], 2)
            self.assertTrue(item['execution_costs_complete'])
        for path, contents in originals.items():
            self.assertEqual(path.read_bytes(), contents)
        self.assertFalse((target / 'shards/shard-00/capture').resolve().parent == target)

    def test_unavailable_historical_attempt_stays_unknown_after_import(self):
        _, source, target, dataset, _, _, artifact, _, ids, comparison = self.fixture(unknown_cost=True)
        with patch.object(compare, 'DATASET', dataset), \
                patch.object(layout, 'host_version', return_value='fixture-host'):
            compare.inherit_candidate_source(comparison, target, ids, artifact)
        saved = json.loads(target.joinpath('shards/shard-00.json').read_text())
        self.assertTrue(saved['source_cost_roots'])
        self.assertTrue(all(item['tokens'] is None and item['model_calls'] is None
                            and item['execution_costs_complete'] is False
                            for item in saved['items']))

    def test_changed_task_or_artifact_rejected_before_target_creation(self):
        for change in ('changed_task', 'changed_artifact'):
            with self.subTest(change=change):
                _, _, target, dataset, _, _, artifact, _, ids, comparison = self.fixture(**{change: True})
                with patch.object(compare, 'DATASET', dataset), \
                        patch.object(layout, 'host_version', return_value='fixture-host'), \
                        self.assertRaisesRegex(RuntimeError, 'comparison_candidate_source_'):
                    compare.inherit_candidate_source(comparison, target, ids, artifact)
                self.assertFalse(target.exists())


if __name__ == '__main__':
    unittest.main()
