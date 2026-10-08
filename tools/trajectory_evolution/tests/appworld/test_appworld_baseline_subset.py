"""Larger scored native sources can supply unchanged smaller L0 scopes; no API."""
from copy import deepcopy
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'tools/trajectory_evolution/experiments/appworld'))
import compare
import layout
import stage_gate
from test_appworld_compare_metrics import native_row


class LargerBaselineSubsetTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.gepa = self.root / 'gepa'
        self.small = self.gepa / 'l0-current-stage32'
        self.large = self.gepa / 'l0-current-stage100'
        self.source = self.large / 'regrade/report.json'
        self.tasks = [{'task_id': f'fixture-{i:03d}', 'task_version': 'fixture-task'} for i in range(33)]
        self.ids = [row['task_id'] for row in self.tasks[:32]]
        self.config = {'task_ids': self.ids, 'max_trials': 32, 'parallelism': 16,
                       'repetitions': 1, 'splits': ['holdout'], 'timeout_ms': 1800000,
                       'variants': [{'id': 'candidate', 'version': 'fixture-v1'}],
                       'judge': dict(layout.judge_projection(), enabled=True)}
        rows = [native_row(task['task_id']) for task in self.tasks]
        for row in rows:
            row.update(task_version='fixture-task', variant_id='candidate')
            row['checks'] = [{'check_id': 'model', 'status': 'passed'}]
        self.native = {'complete': True, 'trials': rows, 'configuration': {
            'parallelism': 16, 'repetitions': 1, 'splits': ['holdout'],
            'variants': self.config['variants'], 'model_enabled': True,
            'allow_unreviewed': False, 'judge_fingerprint': 'fixture-native-judge'}}
        self.small_capture = dict(self.native, trials=deepcopy(rows[:32]))
        self.items = [compare.compact_row(row, 'l0-baseline') for row in rows]
        self.write(self.source, self.native)
        self.write(self.source.with_name('manifest.json'), {'complete': True})
        self.write(self.large / 'capture/report.json', self.native)
        self.write(self.large / 'capture/manifest.json', {'complete': True})
        self.write(self.large / 'capture.config.json', self.config)
        self.write(self.large / 'regrade.regrade.config.json', self.config)
        self.result = {'status': 'complete', 'host_version': layout.host_version(),
                       'source_report': str(self.source), 'judge_projection': layout.judge_projection(),
                       'items': self.items, 'metrics': compare.summarize(self.items, 33)}
        self.write(self.large / 'result.json', self.result)
        self.write(self.small / 'capture/report.json', self.small_capture)
        self.write(self.small / 'capture/manifest.json', {'complete': True})
        self.write(self.small / 'capture.config.json', self.config)
        (self.small / 'capture.host-version').write_text(layout.host_version() + '\n')

    @staticmethod
    def write(path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value))

    def larger(self):
        return compare._larger_baseline_source(self.small, self.ids, self.config, self.tasks)

    def own_pair(self):
        capture = deepcopy(self.small_capture)
        capture.update(run_id='own-capture', dataset_version='own-dataset')
        reviewed = deepcopy(capture)
        reviewed['run_id'] = 'own-review'
        source = self.small / 'regrade/report.json'
        for path, report in [(self.small / 'capture/report.json', capture), (source, reviewed)]:
            self.write(path, report)
            self.write(path.with_name('manifest.json'), {'complete': True,
                'run_id': report['run_id'], 'dataset_version': report['dataset_version'],
                'files': {'report.json': 'existing-native-binding'}})
        self.write(self.small / 'regrade.regrade.config.json', self.config)
        items = compare.execution_cost_rows([compare.compact_row(row, 'l0-baseline')
            for row in reviewed['trials']], source, self.small, native_report=reviewed)
        result = {'status': 'complete', 'host_version': layout.host_version(),
            'source_report': str(source), 'judge_projection': layout.judge_projection(),
            'items': items, 'metrics': compare.summarize(items, len(items))}
        self.write(self.small / 'result.json', result)
        return source, reviewed, result

    def test_own_complete_scope_precedes_larger_source_and_keeps_costs_and_identity(self):
        row = self.small_capture['trials'][0]
        row['metrics']['tokens'] = None
        row['evidence']['output']['final'].update(known_agent_tokens=None, tokens_complete=False)
        for i, boundary in enumerate(('llm', 'actions', 'schedule', 'loop')):
            row['evidence']['trace']['events'].insert(1, {'kind': 'advice.applied', 'event_id': str(i),
                'attributes': {'receipt': {'boundary': boundary, 'application': {'status': 'applied'}}}})
        source, reviewed, original_result = self.own_pair()
        original = source.read_bytes()
        dataset = self.root / 'dataset'; dataset.mkdir()
        (dataset / 'holdout.jsonl').write_text('\n'.join(json.dumps(row) for row in self.tasks) + '\n')
        self.write(self.root / 'L0.json', {'level': 'L0'})
        argv = ['compare.py', '--count', '32', '--output', str(self.small), '--workers', '16']
        with patch.object(sys, 'argv', argv), patch.object(compare, 'DATASET', dataset), \
             patch.object(layout, 'PUBLISHED', self.root), \
             patch.object(layout, 'evaluation_config', return_value=deepcopy(self.config)), \
             patch.object(layout, 'environment', return_value={}), \
             patch.object(compare, 'l0_evaluation_service', return_value=self.root / 'service.json'), \
             patch.object(compare, '_larger_baseline_source', side_effect=AssertionError('own source first')), \
             patch.object(compare, 'execution_cost_rows', wraps=compare.execution_cost_rows) as costs, \
             patch.object(compare, 'recover_baseline_capture', side_effect=AssertionError('closed own capture')), \
             patch.object(compare, '_review_rows', side_effect=AssertionError('own current healthy grades')), \
             patch.object(layout.shared, 'capture', side_effect=AssertionError('no healthy Agent rerun')), \
             patch.object(layout, 'regrade_report', side_effect=AssertionError('no healthy judge rerun')), \
             patch.object(compare.subprocess, 'run', side_effect=AssertionError('no native subprocess')):
            self.assertEqual(compare.baseline_main(), 0)
            self.assertEqual(costs.call_count, 1)
        final = json.loads((self.small / 'result.json').read_text())
        self.assertEqual(final['source_report'], str(source))
        self.assertEqual(final['items'], original_result['items'])
        self.assertIsNone(final['items'][0]['tokens'])
        self.assertFalse(final['items'][0]['tokens_complete'])
        self.assertEqual([final['items'][0][key] for key in
                          ('l1_applied', 'l2_applied', 'l2_action_applied', 'l2_schedule_applied', 'l3_applied')],
                         [1, 2, 1, 1, 1])
        self.assertEqual(source.read_bytes(), original)

    def test_own_source_rejects_changed_provenance_or_cost_and_preserves_larger_fallback(self):
        source, reviewed, result = self.own_pair()
        manifest = json.loads(source.with_name('manifest.json').read_text())
        capture = json.loads((self.small / 'capture/report.json').read_text())
        for change in ('host', 'profile', 'source_scope', 'manifest_missing', 'manifest_incomplete', 'run', 'dataset',
                       'artifact', 'task_version', 'cost', 'rerun_trial', 'missing_trace', 'open_trace'):
            with self.subTest(change=change):
                current, native, binding = deepcopy(result), deepcopy(reviewed), deepcopy(manifest)
                if change == 'host': current['host_version'] = 'other-host'
                if change == 'profile': current['judge_projection']['require_state_dependency'] = False
                if change == 'source_scope': current['source_report'] = str(self.source)
                if change == 'manifest_incomplete': binding['complete'] = False
                if change == 'run': binding['run_id'] = 'other-run'
                if change == 'dataset': binding['dataset_version'] = 'other-dataset'
                if change == 'artifact': native['configuration']['variants'][0]['version'] = 'other-artifact'
                if change == 'task_version': native['trials'][0]['task_version'] = 'other-task'
                if change == 'cost': current['items'][0]['tokens'] += 1
                if change == 'rerun_trial': native['trials'][0]['trial_id'] = 'other-execution'
                if change == 'missing_trace': native['trials'][0]['evidence'].pop('trace')
                if change == 'open_trace': native['trials'][0]['evidence']['trace']['events'].pop()
                self.write(self.small / 'result.json', current)
                self.write(source, native); self.write(source.with_name('manifest.json'), binding)
                if change == 'manifest_missing': source.with_name('manifest.json').unlink()
                self.assertIsNone(compare._own_baseline_source(self.small, self.ids, self.config,
                                                               self.tasks, capture))
        # Own-source rejection does not change the existing larger-subset API.
        self.write(source, reviewed); self.write(source.with_name('manifest.json'), manifest)
        self.write(self.small / 'result.json', result)
        native, path = self.larger()
        self.assertEqual(path, self.source)
        self.assertEqual(native, self.native)

    def test_own_source_preserves_authoritative_raw_archive_symlink(self):
        source, reviewed, result = self.own_pair()
        archive = self.root / 'raw-archive'
        source.parent.rename(archive)
        source.parent.symlink_to(archive, target_is_directory=True)
        before = (archive / 'report.json').read_bytes()
        selected = compare._own_baseline_source(self.small, self.ids, self.config, self.tasks)
        self.assertIsNotNone(selected)
        self.assertEqual(selected[1], source)
        self.assertEqual((archive / 'report.json').read_bytes(), before)

    def test_rejected_own_source_uses_existing_larger_review_and_cost_path(self):
        source, reviewed, original_result = self.own_pair()
        changed = deepcopy(original_result)
        changed['items'][0]['tokens'] += 1
        self.write(self.small / 'result.json', changed)
        dataset = self.root / 'dataset'; dataset.mkdir()
        (dataset / 'holdout.jsonl').write_text('\n'.join(json.dumps(row) for row in self.tasks) + '\n')
        self.write(self.root / 'L0.json', {'level': 'L0'})
        argv = ['compare.py', '--count', '32', '--output', str(self.small), '--workers', '16']
        with patch.object(sys, 'argv', argv), patch.object(compare, 'DATASET', dataset), \
             patch.object(layout, 'PUBLISHED', self.root), \
             patch.object(layout, 'evaluation_config', return_value=deepcopy(self.config)), \
             patch.object(layout, 'environment', return_value={}), \
             patch.object(compare, 'l0_evaluation_service', return_value=self.root / 'service.json'), \
             patch.object(compare, '_larger_baseline_source', return_value=(self.native, self.source)) as larger, \
             patch.object(compare, '_review_rows', return_value=(reviewed['trials'], source)) as review, \
             patch.object(layout.shared, 'capture', side_effect=AssertionError('no healthy Agent rerun')), \
             patch.object(compare.subprocess, 'run', side_effect=AssertionError('no native subprocess')):
            self.assertEqual(compare.baseline_main(), 0)
            larger.assert_called_once()
            review.assert_called_once()
        final = json.loads((self.small / 'result.json').read_text())
        self.assertEqual(final['items'], original_result['items'])
        self.assertEqual(final['source_report'], str(source))

    def test_reuses_whole_real_source_and_never_rewrites_large_result(self):
        before = (self.large / 'result.json').read_bytes()
        native, source = self.larger()
        self.assertEqual(native, self.native)
        self.assertEqual(source, self.source)
        self.assertEqual((self.large / 'result.json').read_bytes(), before)
        self.assertFalse((self.small / 'regrade').exists())

    def test_old_projection_does_not_supply_new_scores(self):
        self.result['judge_projection']['keep_outer_request_inputs'] = False
        self.write(self.large / 'result.json', self.result)
        self.assertIsNone(self.larger())

    def test_different_host_does_not_supply_subset(self):
        self.result['host_version'] = 'unrelated-host'
        self.write(self.large / 'result.json', self.result)
        self.assertIsNone(self.larger())

    def test_different_existing_execution_is_not_replaced_by_larger_source(self):
        for field in ['trial_id', 'duration_ms', 'metrics', 'trace']:
            changed = deepcopy(self.small_capture)
            row = changed['trials'][0]
            if field == 'trial_id': row[field] = 'other-execution'
            elif field == 'duration_ms': row[field] += 1
            elif field == 'metrics': row[field]['tokens'] += 1
            else: row['evidence']['trace']['events'][0]['kind'] = 'changed'
            self.write(self.small / 'capture/report.json', changed)
            self.assertIsNone(self.larger(), field)

    def test_changed_compact_scores_cannot_authorize_reuse(self):
        self.result['items'][0]['tokens'] += 1
        self.write(self.large / 'result.json', self.result)
        with self.assertRaisesRegex(RuntimeError, 'complete_native_source_missing'):
            self.larger()

    def test_normal_baseline_flow_writes_only_real_source_subset_without_native_calls(self):
        dataset = self.root / 'dataset'
        dataset.mkdir()
        (dataset / 'holdout.jsonl').write_text('\n'.join(json.dumps(row) for row in self.tasks) + '\n')
        self.write(self.root / 'L0.json', {'level': 'L0'})
        argv = ['compare.py', '--count', '32', '--output', str(self.small), '--workers', '16']
        with patch.object(sys, 'argv', argv), patch.object(compare, 'DATASET', dataset), \
                patch.object(layout, 'PUBLISHED', self.root), \
                patch.object(layout, 'evaluation_config', return_value=deepcopy(self.config)), \
                patch.object(layout, 'environment', return_value={}), \
                patch.object(compare, 'l0_evaluation_service', return_value=self.root / 'service.json'), \
                patch.object(layout.shared, 'capture', side_effect=AssertionError('no Agent call')), \
                patch.object(layout, 'regrade_report', side_effect=AssertionError('no judge call')), \
                patch.object(compare.subprocess, 'run', side_effect=AssertionError('no native subprocess')):
            self.assertEqual(compare.baseline_main(), 0)
        result = json.loads((self.small / 'result.json').read_text())
        self.assertEqual(result['source_report'], str(self.source))
        self.assertEqual(result['items'], self.items[:32])
        self.assertEqual(result['judge_projection'], layout.judge_projection())
        self.assertFalse((self.small / 'regrade').exists())
        gate = stage_gate.canonical_l0(self.small / 'result.json', self.ids)
        self.assertEqual(gate['task_ids'], self.ids)
        self.assertEqual(gate['scored'], 32)
        self.assertEqual(gate['source_status'], 'complete')


    def test_lower_judge_workers_preserve_native_source_reuse(self):
        graded = deepcopy(self.native)
        graded['configuration']['parallelism'] = 8
        self.write(self.source, graded)
        regrade_config = deepcopy(self.config)
        regrade_config['parallelism'] = 8
        self.write(self.large / 'regrade.regrade.config.json', regrade_config)
        self.assertEqual(self.larger(), (graded, self.source))

    def test_initial_regrade_uses_sixteen_workers_without_modifying_capture_or_env(self):
        source = self.small / 'capture/report.json'
        config_path = self.small / 'capture.config.json'
        before = config_path.read_bytes()
        env = {'TRAJECTORY_EVALUATION_PARALLELISM': '16'}
        calls = []
        def run(command, **kwargs):
            calls.append((command, kwargs))
            target = Path(command[command.index('--output-root') + 1])
            self.write(target / 'report.json', self.small_capture)
            self.write(target / 'manifest.json', {'complete': True})
            return type('Result', (), {'returncode': 0, 'stderr': '', 'stdout': ''})()
        with patch.object(layout.subprocess, 'run', side_effect=run):
            layout.regrade_report(source, self.root / 'dataset', config_path,
                                  self.root / 'new-regrade', env=env)
        command, kwargs = calls[0]
        derived = json.loads(Path(command[command.index('--config') + 1]).read_text())
        self.assertEqual(derived['parallelism'], 16)
        self.assertEqual(kwargs['env']['TRAJECTORY_EVALUATION_PARALLELISM'], '16')
        self.assertEqual(config_path.read_bytes(), before)
        self.assertEqual(env['TRAJECTORY_EVALUATION_PARALLELISM'], '16')
        self.assertIn('--regrade-report', command)

    def test_failed_judge_resume_uses_four_workers_and_bounded_quota_backoff(self):
        source = self.small / 'capture/report.json'
        config_path = self.small / 'capture.config.json'
        original = config_path.read_bytes()
        failed = deepcopy(self.small_capture)
        row = failed['trials'][0]
        row.update(verdict='error', error_code='evaluation_judge_failed')
        row['checks'] = [{'check_id': 'model', 'status': 'error',
                          'reason': 'evaluation_judge_http_429'}]
        row['evidence']['loop_review'] = {'status': 'uncertain', 'count': None}
        self.write(source, failed)
        calls = []
        def run(command, **kwargs):
            calls.append((command, kwargs))
            target = Path(command[command.index('--output-root') + 1])
            report = failed if len(calls) == 1 else self.small_capture
            self.write(target / 'report.json', report)
            self.write(target / 'manifest.json', {'complete': True})
            return type('Result', (), {'returncode': 0})()
        with patch.object(compare.subprocess, 'run', side_effect=run), \
                patch.object(compare.time, 'sleep') as sleep, \
                patch.object(layout, 'regrade_report', side_effect=AssertionError('no healthy full regrade')):
            rows = compare._review_rows(failed['trials'], report_path=source,
                dataset=self.root / 'dataset', config_path=config_path,
                output_root=self.small / 'regrade', env={'TRAJECTORY_EVALUATION_PARALLELISM': '16'},
                source_projection=layout.judge_projection())
        self.assertEqual(rows, self.small_capture['trials'])
        self.assertEqual([call.args[0] for call in sleep.call_args_list], [15, 30])
        self.assertEqual(len(calls), 2)
        for command, kwargs in calls:
            self.assertIn('--resume-report', command)
            self.assertNotIn('--regrade-report', command)
            derived = json.loads(Path(command[command.index('--config') + 1]).read_text())
            # Strict native resume retains the source configuration identity;
            # task_evaluation.cpp applies the environment override to workers.
            self.assertEqual(derived['parallelism'], json.loads(original)['parallelism'])
            self.assertEqual(kwargs['env']['TRAJECTORY_EVALUATION_PARALLELISM'], '4')
        self.assertEqual(config_path.read_bytes(), original)

    def test_healthy_judge_rows_have_no_retry_sleep_or_subprocess(self):
        with patch.object(compare.subprocess, 'run', side_effect=AssertionError('no healthy rejudge')), \
                patch.object(compare.time, 'sleep', side_effect=AssertionError('no healthy delay')):
            rows = compare._review_rows(self.small_capture['trials'],
                report_path=self.small / 'capture/report.json', dataset=self.root / 'dataset',
                config_path=self.small / 'capture.config.json', output_root=self.small / 'regrade',
                env={'TRAJECTORY_EVALUATION_PARALLELISM': '16'},
                source_projection=layout.judge_projection())
        self.assertEqual(rows, self.small_capture['trials'])


class CandidatePartitionRecoveryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.shards = Path(self.temp.name) / 'shards'
        self.shards.mkdir()
        self.artifact = {'level': 'L1', 'fixture': 'frozen-original'}
        self.ids = [f'fixture-{i:03d}' for i in range(100)]

    def checkpoint(self, selected, index=0):
        items = [compare.compact_row(native_row(task), f'shard-{index:02d}') for task in selected]
        value = {'artifact': self.artifact, 'task_ids': selected, 'items': items,
                 'execution_complete': True, 'complete': True,
                 'judge_projection': layout.judge_projection()}
        path = self.shards / f'shard-{index:02d}.json'
        path.write_text(json.dumps(value))
        return path

    def test_original_hundred_task_partition_survives_new_cli_shard_size(self):
        path = self.checkpoint(self.ids)
        before = path.read_bytes()
        # A failed CLI repartition changed only derived progress to 32 rows.
        progress = {'artifact': self.artifact, 'planned_tasks': 100, 'items': {}, 'shards': {}}
        result = compare.retained_candidate_shards(progress, self.ids, self.shards,
                                                   self.artifact, 32)
        self.assertEqual(result, [self.ids])
        self.assertEqual(path.read_bytes(), before)

    def test_saved_thirty_two_tasks_expand_only_the_sixty_eight_suffix(self):
        self.checkpoint(self.ids[:32])
        progress = {'artifact': self.artifact, 'planned_tasks': 32, 'items': {}, 'shards': {}}
        result = compare.retained_candidate_shards(progress, self.ids, self.shards,
                                                   self.artifact, 32)
        self.assertEqual([len(ids) for ids in result], [32, 32, 32, 4])
        self.assertEqual(result[0], self.ids[:32])
        self.assertEqual([task for shard in result[1:] for task in shard], self.ids[32:])

    def test_larger_saved_prefix_is_preserved_without_recapturing_it(self):
        self.checkpoint(self.ids[:64])
        progress = {'artifact': self.artifact, 'planned_tasks': 100, 'items': {}, 'shards': {}}
        result = compare.retained_candidate_shards(progress, self.ids, self.shards,
                                                   self.artifact, 32)
        self.assertEqual([len(ids) for ids in result], [64, 32, 4])

    def test_different_artifact_or_nonprefix_scope_cannot_restore_partition(self):
        self.checkpoint(self.ids)
        progress = {'artifact': {'fixture': 'different'}, 'planned_tasks': 100}
        self.assertIsNone(compare.retained_candidate_shards(progress, self.ids, self.shards,
                                                           self.artifact, 32))
        self.checkpoint(self.ids[1:33])
        progress['artifact'] = self.artifact
        self.assertIsNone(compare.retained_candidate_shards(progress, self.ids, self.shards,
                                                           self.artifact, 32))

    def test_fresh_candidate_keeps_normal_thirty_two_task_partition(self):
        progress = {'artifact': self.artifact, 'planned_tasks': 32}
        self.assertIsNone(compare.retained_candidate_shards(progress, self.ids[:32], self.shards,
                                                           self.artifact, 32))


if __name__ == '__main__':
    unittest.main()
