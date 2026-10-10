#!/usr/bin/env python3
"""Model-free regression tests for the staged AppWorld gate."""
from __future__ import annotations

import json
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from contextlib import contextmanager, ExitStack, redirect_stdout
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[4]
EXPERIMENT = ROOT / "tools/trajectory_evolution/experiments/appworld"
sys.path.insert(0, str(EXPERIMENT))
import stage_gate as gate
import offline_gepa
import reproduce
_GEPA_REAL_SAVE = offline_gepa.save


@contextmanager
def _isolated_gepa_outputs(output, *, gepa=None, training=None, published=None, dataset=None):
    """Bind every derived output path and refuse writes outside the fixture."""
    output = Path(output).resolve()
    gepa = Path(gepa) if gepa is not None else output / 'gepa'
    training = Path(training) if training is not None else output / 'training'
    published = Path(published) if published is not None else output / 'published'
    paths = dict(OUTPUT=output, GEPA=gepa, TRAINING=training, PUBLISHED=published,
                 CONFIG=gepa / 'runtime', WORKSPACE=gepa / 'workspace',
                 PLAN=gepa / 'plan.json', STATUS=gepa / 'status.json',
                 DATASET=Path(dataset) if dataset is not None else training / 'datasets/gepa')
    def save_in_fixture(path, value):
        if not Path(path).resolve().is_relative_to(output):
            raise AssertionError('GEPA test attempted a write outside its temporary fixture')
        return _GEPA_REAL_SAVE(path, value)

    with ExitStack() as stack:
        for name, value in paths.items():
            if not value.resolve().is_relative_to(output):
                raise AssertionError('GEPA output fixture path is not temporary')
            stack.enter_context(patch.object(offline_gepa, name, value))
        stack.enter_context(patch.object(offline_gepa.controller, 'GEPA', gepa))
        stack.enter_context(patch.object(offline_gepa.controller, 'OUTPUT', output))
        stack.enter_context(patch.object(offline_gepa, 'save', side_effect=save_in_fixture))
        yield paths


def _native_grade_source(path: Path) -> Path:
    import layout
    folder = path.parent / (path.stem + '-native-grade')
    folder.mkdir(exist_ok=True)
    source = folder / 'report.json'
    source.write_text(json.dumps({'complete': True}))
    source.with_name('manifest.json').write_text(json.dumps({'complete': True}))
    folder.with_name(folder.name + '.regrade.config.json').write_text(json.dumps(
        {'judge': layout.judge_projection()}))
    return source


def _l0(path: Path, *, loop: float = 0.2,
        loop_basis: str = "model_review", count: int = 32) -> list[str]:
    ids = [f"task-{i:03d}" for i in range(count)]
    items = []
    for i, task_id in enumerate(ids):
        items.append({
            "task_id": task_id,
            "reward": 1 if i < 5 else 0,
            "verdict": "passed" if i < 5 else "failed",
            "tokens_complete": True,
            "metrics": {
                "quality": 0.4,
                "auxiliary_test_fraction": 0.4,
                "avg_invalid_loop": loop,
                "loop_basis": loop_basis,
                "latency_ms": 100.0,
                "tokens": 100.0,
                "model_calls": 10.0,
            },
        })
    import layout
    source = _native_grade_source(path)
    path.write_text(json.dumps({
        "status": "complete", "items": items,
        "source_report": str(source), "judge_projection": layout.judge_projection(),
        "metrics": {"loop_basis": loop_basis, "invalid_loop_coverage": count},
    }), encoding="utf-8")
    return ids


def _candidate(path: Path, ids: list[str], *, strict: int = 6,
               quality: float = 0.42, loop: float = 0.1,
               missing: str | None = None) -> None:
    items = []
    for i, task_id in enumerate(ids):
        row = {
            "task_id": task_id,
            "reward": 1 if i < strict else 0,
            "verdict": "passed" if i < strict else "failed",
            "quality": quality,
            "quality_score": quality,
            "avg_invalid_loop": loop,
            "latency_ms": 90.0,
            "tokens": 90.0,
            "model_calls": 9.0,
            "tokens_complete": True,
            "error_code": None,
            "invalid_loop_basis": "model_review",
        }
        if missing:
            row.pop(missing, None)
        items.append(row)
    import layout
    source = _native_grade_source(path)
    progress = path.with_name(path.stem + '-progress.json')
    progress.write_text(json.dumps({'shards': {'0': {
        'execution_complete': True, 'complete': True,
        'judge_projection': layout.judge_projection(), 'source_report': str(source)}}}))
    path.write_text(json.dumps({
        "status": "complete", "items": items,
        "progress": str(progress), "judge_projection": layout.judge_projection(),
        "metrics": {"candidate": {"loop_basis": "model_review"}},
    }), encoding="utf-8")


class CurrentNativeJudgeProfileTests(unittest.TestCase):
    def fixture(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        l0, candidate = root / 'l0.json', root / 'candidate.json'
        _candidate(candidate, _l0(l0))
        return l0, candidate, json.loads(candidate.read_text())

    def test_current_native_sources_pass_the_existing_core_gate(self):
        l0, candidate, value = self.fixture()
        passed, result = gate.verify_stage('32', candidate, l0)
        self.assertTrue(passed)
        self.assertTrue(result['goal_passed'])

    def test_matching_compact_cannot_hide_old_native_grade_configuration(self):
        for side in ('baseline', 'candidate'):
            with self.subTest(side=side):
                l0, candidate, value = self.fixture()
                if side == 'baseline':
                    source = Path(json.loads(l0.read_text())['source_report'])
                else:
                    progress = json.loads(Path(value['progress']).read_text())
                    source = Path(progress['shards']['0']['source_report'])
                config = source.parent.with_name(source.parent.name + '.regrade.config.json')
                settings = json.loads(config.read_text())
                settings['judge']['require_state_dependency'] = False
                config.write_text(json.dumps(settings))
                passed, result = gate.verify_stage('32', candidate, l0)
                self.assertFalse(passed)
                self.assertEqual(result['status'], 'pending')
                self.assertEqual(result['reason'], 'current_native_judge_profile_pending')

    def test_incomplete_native_manifest_and_stale_compact_remain_pending(self):
        l0, candidate, value = self.fixture()
        source = Path(json.loads(l0.read_text())['source_report'])
        source.with_name('manifest.json').write_text(json.dumps({'complete': False}))
        passed, result = gate.verify_stage('32', candidate, l0)
        self.assertFalse(passed)
        self.assertEqual(result['status'], 'pending')
        source.with_name('manifest.json').write_text(json.dumps({'complete': True}))
        value['judge_projection']['require_state_dependency'] = False
        candidate.write_text(json.dumps(value))
        passed, result = gate.verify_stage('32', candidate, l0)
        self.assertFalse(passed)
        self.assertEqual(result['status'], 'pending')


class StageGateRegressionTest(unittest.TestCase):
    def test_comparison500_cli_uses_full_scope_core_gate_and_same_validated_seed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            seed = root / 'strategy.json'
            artifact = {'level': 'L3', 'policy': {'frozen': True}}
            seed.write_text(json.dumps(artifact))
            for count in (100, 500):
                baseline, comparison = root / f'L0-{count}.json', root / f'candidate-{count}.json'
                _candidate(comparison, _l0(baseline, count=count))
                value = json.loads(comparison.read_text())
                value.update(seed=str(seed), artifact=artifact)
                comparison.write_text(json.dumps(value))
                self.assertTrue(gate.verify_stage(str(count) if count == 100 else 'comparison500',
                                                 comparison, baseline, seed)[0])
            output = io.StringIO()
            with patch.object(sys, 'argv', ['stage_gate.py', 'verify', '--stage', 'comparison500',
                    '--comparison', str(comparison), '--l0-source', str(baseline), '--seed', str(seed)]), \
                    patch.object(sys, 'stdout', output):
                self.assertEqual(gate.main(), 0)
            result = json.loads(output.getvalue())
            self.assertEqual(result['stage'], 'comparison500')
            self.assertTrue(result['seed_match'])
            self.assertTrue(result['goal_passed'])
            self.assertEqual(result['completed'], 500)

    def test_comparison500_cli_does_not_waive_scope_profile_identity_or_core_gains(self):
        for cause in ('incomplete', 'stale_profile', 'changed_seed', 'loop_tie', 'score_tie'):
            with self.subTest(cause=cause), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                seed, baseline, comparison = root / 'strategy.json', root / 'L0.json', root / 'candidate.json'
                artifact = {'level': 'L3', 'policy': {'frozen': True}}
                seed.write_text(json.dumps(artifact))
                _candidate(comparison, _l0(baseline, count=500))
                value = json.loads(comparison.read_text())
                value.update(seed=str(seed), artifact=artifact)
                if cause == 'incomplete':
                    value['items'].pop()
                elif cause == 'stale_profile':
                    progress = json.loads(Path(value['progress']).read_text())
                    source = Path(progress['shards']['0']['source_report'])
                    config = source.parent.with_name(source.parent.name + '.regrade.config.json')
                    settings = json.loads(config.read_text())
                    settings['judge']['require_state_dependency'] = False
                    config.write_text(json.dumps(settings))
                elif cause == 'changed_seed':
                    seed.write_text(json.dumps({'level': 'L3', 'policy': {'frozen': False}}))
                elif cause == 'loop_tie':
                    for row in value['items']:
                        row['avg_invalid_loop'] = 0.2
                elif cause == 'score_tie':
                    value['items'][5].update(reward=0, verdict='failed')
                comparison.write_text(json.dumps(value))
                output = io.StringIO()
                with patch.object(sys, 'argv', ['stage_gate.py', 'verify', '--stage', 'comparison500',
                        '--comparison', str(comparison), '--l0-source', str(baseline), '--seed', str(seed)]), \
                        patch.object(sys, 'stdout', output):
                    self.assertEqual(gate.main(), 1)
                result = json.loads(output.getvalue())
                self.assertFalse(result['goal_passed'])
                if cause == 'stale_profile':
                    self.assertEqual(result['status'], 'pending')
                    self.assertEqual(result['reason'], 'current_native_judge_profile_pending')

    def test_completed_pilot_with_no_pending_workers_retains_registered_paths(self):
        with tempfile.TemporaryDirectory(dir='/tmp') as directory:
            root = Path(directory)
            seed = root / 'published/current/strategy.json'
            seed.parent.mkdir(parents=True)
            artifact = {'level': 'L3', 'policy': {'current': True}}
            seed.write_text(json.dumps(artifact))
            pilot = root / 'gepa/seed-search-7-stage32/comparison.json'
            pilot.parent.mkdir(parents=True)
            pilot.write_text(json.dumps({'status': 'complete'}))
            progress = {'artifact': artifact, 'planned_tasks': 32,
                'items': {str(i): {} for i in range(32)},
                'shards': {'0': {'complete': True}},
                'total_parallelism': 0, 'workers_per_shard': 16}
            pilot.with_name('progress.json').write_text(json.dumps(progress))
            attempt = {'attempt': 7, 'round': 1, 'status': 'pilot_passed',
                'seed': str(seed), 'pilot_comparison': str(pilot),
                'validation_comparison': str(root / 'gepa/seed-search-7-stage100/comparison.json')}
            search = {'round': 1, 'attempts': [attempt]}
            (root / 'gepa/status.json').write_text(json.dumps({'seed_search': search}))
            before = pilot.with_name('progress.json').read_bytes()
            with patch.object(reproduce, 'OUTPUT', root):
                self.assertEqual(reproduce._matching_attempt(seed, search), attempt)
                self.assertEqual(reproduce.stage_path(seed, 32), pilot)
                self.assertEqual(reproduce.stage_path(seed, 100), Path(attempt['validation_comparison']))
            self.assertEqual(pilot.with_name('progress.json').read_bytes(), before)

    def test_zero_pending_worker_pilot_requires_complete_original_batch(self):
        with tempfile.TemporaryDirectory(dir='/tmp') as directory:
            root = Path(directory)
            seed = root / 'published/current/strategy.json'
            seed.parent.mkdir(parents=True)
            artifact = {'level': 'L3', 'policy': {'current': True}}
            seed.write_text(json.dumps(artifact))
            pilot = root / 'gepa/seed-search-7-stage32/comparison.json'
            pilot.parent.mkdir(parents=True)
            attempt = {'attempt': 7, 'round': 1, 'seed': str(seed),
                'pilot_comparison': str(pilot), 'validation_comparison': str(root / '100.json')}
            search = {'round': 1, 'attempts': [attempt]}
            for status, shard_complete, workers in (
                    ('partial', True, 16), ('complete', False, 16), ('complete', True, 8)):
                with self.subTest(status=status, shard_complete=shard_complete, workers=workers):
                    pilot.write_text(json.dumps({'status': status}))
                    pilot.with_name('progress.json').write_text(json.dumps({
                        'artifact': artifact, 'planned_tasks': 32,
                        'items': {str(i): {} for i in range(32)},
                        'shards': {'0': {'complete': shard_complete}},
                        'total_parallelism': 0, 'workers_per_shard': workers}))
                    with patch.object(reproduce, 'OUTPUT', root):
                        self.assertIsNone(reproduce._matching_attempt(seed, search))

    def test_final_three_metric_contract_reports_unknown_cost_without_blocking(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            l0, candidate = root / 'l0.json', root / 'candidate.json'
            ids = _l0(l0, count=500)
            _candidate(candidate, ids)
            baseline = json.loads(l0.read_text())
            baseline['items'][0]['tokens_complete'] = False
            l0.write_text(json.dumps(baseline))
            value = json.loads(candidate.read_text())
            for row in value['items']:
                row['tokens'] = 110
            passed, result = gate._verify_against_l0('comparison500', value, value['items'], l0)
            self.assertTrue(passed)
            self.assertFalse(result['resources_required'])
            self.assertFalse(result['goal_checks']['resources_not_increased'])
            self.assertEqual(result['exploration_gain']['resource_state'], 'blocked_unverifiable')
            self.assertFalse(result['baseline']['tokens_complete'])

    def test_final_three_metric_contract_does_not_waive_missing_loop(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            l0, candidate = root / 'l0.json', root / 'candidate.json'
            ids = _l0(l0, count=500)
            _candidate(candidate, ids)
            value = json.loads(candidate.read_text())
            value['items'][0].pop('avg_invalid_loop')
            passed, result = gate._verify_against_l0('comparison500', value, value['items'], l0)
            self.assertFalse(passed)
            self.assertFalse(result['goal_checks']['invalid_loop_reduction_15pct'])

    def test_started_current_attempt_resumes_its_own_paths_when_others_are_occupied(self):
        with tempfile.TemporaryDirectory(dir='/tmp') as directory:
            root = Path(directory)
            seed = root/'published/current/strategy.json'
            seed.parent.mkdir(parents=True)
            artifact = {'level': 'L3', 'policy': {'current': True}}
            seed.write_text(json.dumps(artifact))
            with patch.object(reproduce, 'OUTPUT', root):
                attempt = reproduce.begin_seed_attempt(seed)
                progress = Path(attempt['pilot_comparison']).with_name('progress.json')
                progress.parent.mkdir(parents=True)
                progress.write_text(json.dumps({'artifact': artifact, 'items': {},
                                               'total_parallelism': 16, 'workers_per_shard': 16}))
                occupied = root/'gepa/seed-search-2-stage100/comparison.json'
                occupied.parent.mkdir(parents=True)
                occupied.write_text('{"historical":true}')
                self.assertEqual(reproduce.begin_seed_attempt(seed), attempt)
                self.assertEqual(reproduce.stage_path(seed, 100), Path(attempt['validation_comparison']))
                self.assertEqual(len(reproduce.seed_search()['attempts']), 1)

    def test_new_attempt_skips_unregistered_stage_evidence_and_preserves_it(self):
        with tempfile.TemporaryDirectory(dir='/tmp') as directory:
            root = Path(directory)
            seed = root/'published/new/strategy.json'
            seed.parent.mkdir(parents=True)
            seed.write_text(json.dumps({'level': 'L3', 'policy': {'new': True}}))
            old_files = []
            for index, count, filename in ((1, 32, 'comparison.json'), (2, 100, 'progress.json'),
                                          (3, 32, 'shards/config.json')):
                path = root/f'gepa/seed-search-{index}-stage{count}'/filename
                path.parent.mkdir(parents=True)
                path.write_text(json.dumps({'owner': 'historical', 'index': index}))
                old_files.append(path)
            before = {path: path.read_bytes() for path in old_files}
            # Empty directories have no old capture or score to preserve.
            (root/'gepa/seed-search-4-stage32').mkdir()
            with patch.object(reproduce, 'OUTPUT', root):
                attempt = reproduce.begin_seed_attempt(seed)
                self.assertEqual(attempt['attempt'], 4)
                self.assertIn('seed-search-4-stage32', attempt['pilot_comparison'])
                self.assertEqual(len(reproduce.seed_search()['attempts']), 1)
                self.assertEqual(reproduce.begin_seed_attempt(seed)['attempt'], 4)
            self.assertTrue(all(path.read_bytes() == raw for path, raw in before.items()))

    def test_pending_attempt_with_retained_report_is_not_reassigned(self):
        with tempfile.TemporaryDirectory(dir='/tmp') as directory:
            root = Path(directory)
            seed = root/'published/new/strategy.json'
            seed.parent.mkdir(parents=True)
            seed.write_text(json.dumps({'level': 'L3', 'policy': {'new': True}}))
            comparison = root/'gepa/seed-search-9-stage32/comparison.json'
            comparison.parent.mkdir(parents=True)
            comparison.write_text(json.dumps({'status': 'complete', 'seed': 'historical'}))
            old = {'attempt': 9, 'round': 1, 'status': 'pending', 'seed': 'historical',
                   'pilot_comparison': str(comparison),
                   'validation_comparison': str(root/'gepa/seed-search-9-stage100/comparison.json')}
            state = root/'gepa/status.json'
            state.write_text(json.dumps({'seed_search': {'round': 1, 'status': 'searching', 'attempts': [old]}}))
            before = comparison.read_bytes()
            with patch.object(reproduce, 'OUTPUT', root):
                self.assertEqual(reproduce.begin_seed_attempt(seed)['attempt'], 10)
                self.assertEqual(reproduce.seed_search()['attempts'][0], old)
            self.assertEqual(comparison.read_bytes(), before)

    def test_common_resume_refreshes_dataset_counts_and_clears_old_error_without_waiving_gate(self):
        with tempfile.TemporaryDirectory(dir='/tmp') as directory:
            root = Path(directory)
            seed = root/'published/fixture/strategy.json'
            identity = {'seed_label': 'fixture', 'seed_artifact': str(seed.resolve())}
            path = root/'gepa/lifecycle.json'
            path.parent.mkdir(parents=True)
            receipts = {'prepare': {'status': 'complete'}, 'train': {'status': 'complete'},
                        'seed': {'status': 'complete'},
                        'pilot32': {'status': 'complete', 'gate_passed': False}}
            path.write_text(json.dumps({'candidate': identity, 'status': 'blocked',
                'phase': 'seed', 'last_error': 'obsolete_seed_error',
                'dataset': {'name': 'appworld', 'split_counts': {'train': 105}},
                'stage_results': receipts}))
            with patch.object(reproduce, 'OUTPUT', root), \
                 patch.object(reproduce._AppWorldDatasetAdapter, 'output_root', root), \
                 patch.object(reproduce._AppWorldDatasetAdapter, 'split_counts',
                              return_value={'train': 111, 'val': 12}), \
                 patch.object(reproduce, '_candidate_identity', return_value=identity), \
                 patch.object(reproduce, 'seed_search', return_value={'attempts': []}), \
                 patch('learning._completed_training_tasks', return_value={'appworld-native-fixture'}), \
                 patch.object(reproduce, 'run', side_effect=AssertionError('receipts need no execution')):
                result = reproduce._run_common_all(seed, 'fixture', stop_at='train')
                self.assertEqual(result['dataset']['split_counts'], {'train': 111, 'val': 12})
                self.assertNotIn('last_error', result)
                self.assertEqual(result['stage_results'], receipts)
                with self.assertRaisesRegex(RuntimeError, 'stage_gate_not_passed:pilot32'):
                    reproduce._run_common_all(seed, 'fixture', stop_at='validation100')
            blocked = json.loads(path.read_text())
            self.assertEqual(blocked['last_error'], 'stage_gate_not_passed:pilot32')
            self.assertFalse(blocked['stage_results']['pilot32']['gate_passed'])

    def test_training_status_counts_durable_native_learning_not_capture_or_resume_cursor(self):
        import dataset
        import layout
        with tempfile.TemporaryDirectory(dir='/tmp') as directory:
            root = Path(directory)
            batch = root / 'training/full-captures/train-full-07'
            batch.mkdir(parents=True)
            source = batch / 'report.json'
            tasks = ['appworld-old', 'appworld-durable', 'appworld-captured-only']
            source.write_text(json.dumps({'trials': [{'task_id': task,
                'evidence': {'trace': {'identity': {'run_id': task + '-run'}}}} for task in tasks]}))
            source.with_name('manifest.json').write_text('{}')
            checkpoint = {'native_import_progressed': True, 'imported': 2, 'shared_status': 'idle',
                'appworld_input': {'original_report': str(source), 'admitted_training_tasks': tasks},
                'appworld_successful_model_analysis_runs': [tasks[0] + '-run', tasks[1] + '-run']}
            path = batch / 'learning-status.json'
            path.write_text(json.dumps(checkpoint))
            with patch.object(reproduce, 'OUTPUT', root), patch.object(dataset, 'rows',
                    return_value=[{'task_id': task} for task in tasks]), patch.object(layout, 'metrics', return_value={}):
                result = reproduce.training_status({'status': 'running', 'completed_tasks': 1})
                self.assertEqual(result['learning_cursor_tasks'], 1)
                self.assertEqual(result['learned_tasks'], 2)
                self.assertEqual(result['captured_tasks'], 3)
                checkpoint['appworld_successful_model_analysis_runs'] = ['foreign-run']
                path.write_text(json.dumps(checkpoint))
                self.assertEqual(reproduce.training_status({'completed_tasks': 1})['learned_tasks'], 1)
                checkpoint['appworld_successful_model_analysis_runs'] = [tasks[1] + '-run']
                checkpoint['native_import_progressed'] = False
                path.write_text(json.dumps(checkpoint))
                self.assertEqual(reproduce.training_status({'completed_tasks': 1})['learned_tasks'], 1)

    def test_new_review_round_does_not_reuse_old_pending_attempt(self):
        with tempfile.TemporaryDirectory(dir='/tmp') as directory:
            root = Path(directory)
            seed = root / 'published/changed/strategy.json'
            seed.parent.mkdir(parents=True)
            seed.write_text(json.dumps({'level': 'L3', 'policy': {'changed': True}}))
            path = root / 'gepa/status.json'
            path.parent.mkdir()
            old = {'attempt': 1, 'round': 1, 'status': 'pending', 'seed': 'old',
                   'pilot_comparison': str(root / 'gepa/old32/comparison.json'),
                   'validation_comparison': str(root / 'gepa/old100/comparison.json')}
            path.write_text(json.dumps({'seed_search': {'round': 2, 'status': 'searching',
                                                       'consecutive_failures': 0, 'attempts': [old]}}))
            with patch.object(reproduce, 'OUTPUT', root):
                attempt = reproduce.begin_seed_attempt(seed)
                self.assertEqual(attempt['round'], 2)
                self.assertEqual(attempt['attempt'], 2)
                self.assertEqual(reproduce.seed_search()['attempts'][0], old)

    def test_review_round_rejects_unchanged_failed_seed_but_history_remains_readable(self):
        with tempfile.TemporaryDirectory(dir='/tmp') as directory:
            root = Path(directory)
            seed = root / 'published/unchanged/strategy.json'
            seed.parent.mkdir(parents=True)
            artifact = {'level': 'L3', 'policy': {'same': True}}
            seed.write_text(json.dumps(artifact))
            comparison = root / 'gepa/old32/comparison.json'
            comparison.parent.mkdir(parents=True)
            comparison.write_text(json.dumps({'status': 'complete'}))
            comparison.with_name('progress.json').write_text(json.dumps({'artifact': artifact, 'total_parallelism': 16, 'workers_per_shard': 16}))
            old = {'attempt': 1, 'round': 1, 'status': 'failed', 'seed': str(seed),
                   'gate_policy': reproduce.INTERIM_GATE_POLICY,
                   'pilot_comparison': str(comparison),
                   'validation_comparison': str(root / 'gepa/old100/comparison.json')}
            path = root / 'gepa/status.json'
            search = {'round': 2, 'status': 'searching', 'consecutive_failures': 0, 'attempts': [old]}
            path.write_text(json.dumps({'seed_search': search}))
            with patch.object(reproduce, 'OUTPUT', root):
                self.assertIsNone(reproduce._matching_attempt(seed, search, current_round_only=True))
                self.assertEqual(reproduce.stage_path(seed, 32), comparison)
                with self.assertRaisesRegex(RuntimeError, 'seed_unchanged_after_review'):
                    reproduce.begin_seed_attempt(seed)
                self.assertEqual(reproduce.seed_search(), search)

    def test_historical_complete_reuse_does_not_count_as_new_seed_failure(self):
        with tempfile.TemporaryDirectory(dir='/tmp') as directory:
            root = Path(directory)
            seed = root / 'published/reused/strategy.json'
            seed.parent.mkdir(parents=True)
            artifact = {'level': 'L3', 'policy': {'snapshot': {'experiences': []}}}
            seed.write_text(json.dumps(artifact))
            comparison = root / 'gepa/old32/comparison.json'
            comparison.parent.mkdir(parents=True)
            comparison.write_text(json.dumps({'status': 'complete', 'items': []}))
            comparison.with_name('progress.json').write_text(json.dumps({'artifact': artifact, 'total_parallelism': 16, 'workers_per_shard': 16}))
            old = {'attempt': 1, 'round': 1, 'status': 'pilot_passed', 'seed': str(seed),
                   'pilot_comparison': str(comparison),
                   'validation_comparison': str(root / 'gepa/old100/comparison.json')}
            path = root / 'gepa/status.json'
            path.write_text(json.dumps({'seed_search': {'round': 2, 'status': 'searching',
                'consecutive_failures': 0, 'attempts': [old]}}))
            with patch.object(reproduce, 'OUTPUT', root):
                attempt = reproduce.begin_seed_attempt(seed)
                self.assertEqual(attempt['round'], 2)
                self.assertEqual(attempt['reused_from_attempt'], 1)
                self.assertEqual(attempt['pilot_comparison'], str(comparison))
                reproduce.record_seed_gate(seed, 32, comparison, {
                    'seed_match': True, 'goal_passed': False, 'coverage_pass': True,
                    'score_coverage_pass': True, 'status': 'fail',
                    'exploration_gain': {'strict_relative_gain': 0, 'quality_relative_gain': 0}})
                search = reproduce.seed_search()
                self.assertEqual(search['attempts'][0], old)
                self.assertEqual(search['attempts'][1]['status'], 'reused_failed')
                self.assertEqual(search['consecutive_failures'], 0)
                self.assertEqual(search['status'], 'searching')

    def test_reviewed_round_preserves_failures_and_cannot_bypass_the_gate(self):
        from copy import deepcopy
        with tempfile.TemporaryDirectory(dir='/tmp') as directory:
            output = Path(directory)
            search = {'round': 1, 'status': 'review_required', 'consecutive_failures': 5,
                      'attempts': [{'attempt': i, 'round': 1, 'status': 'failed'}
                                   for i in range(1, 6)]}
            path = output / 'gepa/status.json'
            path.parent.mkdir()
            path.write_text(json.dumps({'seed_search': search}))
            original = deepcopy(search['attempts'])
            with patch.object(reproduce, 'OUTPUT', output):
                with self.assertRaisesRegex(ValueError, 'review_summary_required'):
                    reproduce.restart_search_after_review('')
                self.assertEqual(reproduce.seed_search(), search)
                reproduce.restart_search_after_review(
                    'Reviewed real failed traces; private capture recovery and semantic trigger fixes verified.')
                updated = reproduce.seed_search()
                self.assertEqual(updated['attempts'], original)
                self.assertEqual(updated['round'], 2)
                self.assertEqual(updated['consecutive_failures'], 0)
                self.assertEqual(updated['reviews'][0]['failed_attempts'], [1, 2, 3, 4, 5])
                self.assertNotEqual(updated['status'], 'found')
                self.assertEqual(reproduce.stage_path(output / 'published/new/strategy.json', 32).name,
                                 'comparison.json')
                with self.assertRaisesRegex(RuntimeError, 'review_not_required'):
                    reproduce.restart_search_after_review('Cannot reset an already active search round.')

    def test_unknown_failed_attempt_cost_is_reported_and_preserves_final_loop(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            l0, candidate = root / 'l0.json', root / 'candidate.json'
            ids = _l0(l0)
            _candidate(candidate, ids)
            value = gate.load(candidate)
            row = value['items'][0]
            row.update(execution_attempts=2, execution_costs_complete=False,
                       retry_loop_complete=False, tokens=None, model_calls=None,
                       tokens_complete=False, latency_ms=36621.16262)
            candidate.write_text(json.dumps(value))
            passed, result = gate.verify_stage('32', candidate, l0)
            self.assertTrue(passed)
            self.assertEqual(result['status'], 'pass')
            self.assertEqual(result['exploration_gain']['resource_state'], 'blocked_unverifiable')
            self.assertEqual(result['candidate']['extra_execution_attempts'], 1)
            self.assertFalse(result['candidate']['retry_loop_complete'])
            self.assertAlmostEqual(result['candidate']['avg_invalid_loop'], .1)

    def test_five_distinct_failed_seeds_stop_sixth_and_repeat_resume_does_not_count(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            seed = root / 'published/current/strategy.json'
            seed.parent.mkdir(parents=True)
            baseline = root / 'gepa/l0-stage32/result.json'; baseline.parent.mkdir(parents=True)
            ids = _l0(baseline)
            with patch.object(reproduce, 'OUTPUT', root), patch.object(offline_gepa, 'GEPA', root / 'gepa'):
                for index in range(5):
                    artifact = {'level': 'L3', 'policy': {'snapshot': {'experiences': []},
                        'candidate': index}}
                    seed.write_text(json.dumps(artifact))
                    attempt = reproduce.begin_seed_attempt(seed)
                    self.assertEqual(attempt['attempt'], index + 1)
                    comparison = Path(attempt['pilot_comparison']); comparison.parent.mkdir(parents=True)
                    _candidate(comparison, ids, quality=.4)
                    value = json.loads(comparison.read_text())
                    for row in value['items']:
                        row.update(l1_applied=0)
                    progress = comparison.with_name('progress.json')
                    value.update(seed=str(seed), progress=str(progress))
                    comparison.write_text(json.dumps(value))
                    progress.write_text(json.dumps({'artifact': artifact, 'total_parallelism': 16, 'workers_per_shard': 16,
                        'items': {row['task_id']: row for row in value['items']}}))
                    passed, result = gate.verify_stage('32', comparison, baseline, seed)
                    self.assertFalse(passed); self.assertTrue(result['coverage_pass'])
                    reproduce.record_seed_gate(seed, 32, comparison, result)
                    reproduce.record_seed_gate(seed, 32, comparison, result)
                    state = reproduce.seed_search()
                    self.assertEqual(state['consecutive_failures'], index + 1)
                    self.assertEqual(len(state['attempts']), index + 1)
                    self.assertIn('quality_delta', state['attempts'][-1]['diagnostics']['metric_deltas'])
                    self.assertEqual(state['attempts'][-1]['diagnostics']['memory_applied_task_coverage'], 0)
                    self.assertNotIn('unclassified', state['attempts'][-1]['diagnostics']['failed_task_groups'])
                    if index < 4:
                        self.assertEqual(reproduce.begin_seed_attempt(seed)['attempt'], index + 1)
                self.assertEqual(state['status'], 'review_required')
                seed.write_text(json.dumps({'level': 'L3', 'policy': {'candidate': 5}}))
                with self.assertRaisesRegex(RuntimeError, 'five_failed_seeds_review_required'):
                    reproduce.begin_seed_attempt(seed)
                with self.assertRaisesRegex(RuntimeError, 'five_failed_seeds_review_required'):
                    offline_gepa.require_seed_gates(seed)
                self.assertEqual(len(reproduce.seed_search()['attempts']), 5)
                # There is no touch-file automatic clearing command.
                self.assertFalse(hasattr(reproduce, 'complete_seed_review'))

    def test_known_quality_failure_counts_once_when_retry_costs_are_unknown(self):
        with tempfile.TemporaryDirectory(dir='/tmp') as directory:
            root = Path(directory)
            seed = root / 'published/repair/strategy.json'
            seed.parent.mkdir(parents=True)
            artifact = {'level': 'L3', 'policy': {'snapshot': {'experiences': []}}}
            seed.write_text(json.dumps(artifact))
            baseline = root / 'gepa/l0.json'
            baseline.parent.mkdir()
            ids = _l0(baseline)
            with patch.object(reproduce, 'OUTPUT', root):
                attempt = reproduce.begin_seed_attempt(seed)
                comparison = Path(attempt['pilot_comparison'])
                comparison.parent.mkdir()
                _candidate(comparison, ids, strict=5, quality=.4)
                value = gate.load(comparison)
                value.update(seed=str(seed), progress=str(comparison.with_name('progress.json')))
                value['items'][0].update(tokens=None, model_calls=None, tokens_complete=False,
                                         execution_costs_complete=False, execution_attempts=2)
                comparison.write_text(json.dumps(value))
                comparison.with_name('progress.json').write_text(json.dumps({
                    'artifact': artifact, 'total_parallelism': 16, 'workers_per_shard': 16,
                    'items': {row['task_id']: row for row in value['items']}}))
                passed, result = gate.verify_stage('32', comparison, baseline, seed)
                self.assertFalse(passed)
                self.assertTrue(result['coverage_pass'])
                self.assertTrue(result['score_coverage_pass'])
                self.assertEqual(result['status'], 'fail')
                reproduce.record_seed_gate(seed, 32, comparison, result)
                reproduce.record_seed_gate(seed, 32, comparison, result)
                search = reproduce.seed_search()
                self.assertEqual(search['consecutive_failures'], 1)
                self.assertEqual(search['attempts'][0]['status'], 'failed')

    def test_known_resource_overrun_remains_diagnostic_without_counting_seed_failure(self):
        """Resource overruns stay visible but do not count as core gate failures."""
        with tempfile.TemporaryDirectory(dir='/tmp') as directory:
            root = Path(directory)
            seed = root / 'published/resource-overrun/strategy.json'
            seed.parent.mkdir(parents=True)
            artifact = {'level': 'L3', 'policy': {'snapshot': {'experiences': []}}}
            seed.write_text(json.dumps(artifact))
            baseline = root / 'gepa/l0.json'
            baseline.parent.mkdir()
            ids = _l0(baseline)
            with patch.object(reproduce, 'OUTPUT', root):
                attempt = reproduce.begin_seed_attempt(seed)
                comparison = Path(attempt['pilot_comparison'])
                comparison.parent.mkdir()
                _candidate(comparison, ids, strict=8, quality=.5)
                value = json.loads(comparison.read_text())
                progress = comparison.with_name('progress.json')
                value.update(seed=str(seed), progress=str(progress))
                comparison.write_text(json.dumps(value))
                progress.write_text(json.dumps({
                    'artifact': artifact, 'total_parallelism': 16, 'workers_per_shard': 16,
                    'items': {row['task_id']: row for row in value['items']}}))
                result = {
                    'seed_match': True, 'coverage_pass': False,
                    'score_coverage_pass': True, 'goal_passed': False,
                    'status': 'blocked',
                    'exploration_gain': {
                        'strict_relative_gain': .2,
                        'quality_relative_gain': .2,
                        'resource_state': 'blocked_unverifiable',
                        'resources': {
                            'latency_ms': {'ok': False, 'candidate': 110.0,
                                           'baseline': 100.0, 'delta': 10.0},
                            'tokens': {'ok': False, 'candidate': None,
                                       'baseline': 100.0, 'delta': None},
                            'model_calls': {'ok': True, 'candidate': 10.0,
                                            'baseline': 10.0, 'delta': 0.0},
                            'all': False,
                        },
                    },
                    'candidate': {'metric_coverage': {
                        'quality': 32, 'latency_ms': 32, 'tokens': 31,
                        'model_calls': 32}},
                    'baseline': {'metric_coverage': {
                        'quality': 32, 'latency_ms': 32, 'tokens': 32,
                        'model_calls': 32}},
                }
                reproduce.record_seed_gate(seed, 32, comparison, result)
                search = reproduce.seed_search()
                self.assertEqual(search['consecutive_failures'], 0)
                self.assertEqual(search['attempts'][0]['status'], 'pending')
                self.assertTrue(search['attempts'][0]['diagnostics']['resource_lower_bound_failure'])
                self.assertNotIn('failure_basis', search['attempts'][0]['diagnostics'])

    def test_unknown_resource_gap_stays_pending_when_no_lower_bound_is_proven(self):
        with tempfile.TemporaryDirectory(dir='/tmp') as directory:
            root = Path(directory)
            seed = root / 'published/resource-unknown/strategy.json'
            seed.parent.mkdir(parents=True)
            artifact = {'level': 'L3', 'policy': {'snapshot': {'experiences': []}}}
            seed.write_text(json.dumps(artifact))
            baseline = root / 'gepa/l0.json'
            baseline.parent.mkdir()
            ids = _l0(baseline)
            with patch.object(reproduce, 'OUTPUT', root):
                attempt = reproduce.begin_seed_attempt(seed)
                comparison = Path(attempt['pilot_comparison'])
                comparison.parent.mkdir()
                _candidate(comparison, ids, strict=8, quality=.5)
                value = json.loads(comparison.read_text())
                progress = comparison.with_name('progress.json')
                value.update(seed=str(seed), progress=str(progress))
                comparison.write_text(json.dumps(value))
                progress.write_text(json.dumps({
                    'artifact': artifact, 'total_parallelism': 16, 'workers_per_shard': 16,
                    'items': {row['task_id']: row for row in value['items']}}))
                result = {
                    'seed_match': True, 'coverage_pass': False,
                    'score_coverage_pass': True, 'goal_passed': False,
                    'status': 'blocked',
                    'exploration_gain': {
                        'strict_relative_gain': .2,
                        'quality_relative_gain': .2,
                        'resource_state': 'blocked_unverifiable',
                        'resources': {
                            'latency_ms': {'ok': False, 'candidate': 101.0,
                                           'baseline': 100.0, 'delta': 1.0},
                            'tokens': {'ok': False, 'candidate': None,
                                       'baseline': 100.0, 'delta': None},
                            'model_calls': {'ok': True, 'candidate': 10.0,
                                            'baseline': 10.0, 'delta': 0.0},
                            'all': False,
                        },
                    },
                    # 31 known candidate rows at 101 are not enough to prove
                    # that its 32-row total exceeds the baseline's 3200.
                    'candidate': {'metric_coverage': {
                        'quality': 32, 'latency_ms': 31, 'tokens': 31,
                        'model_calls': 32}},
                    'baseline': {'metric_coverage': {
                        'quality': 32, 'latency_ms': 32, 'tokens': 32,
                        'model_calls': 32}},
                }
                reproduce.record_seed_gate(seed, 32, comparison, result)
                search = reproduce.seed_search()
                self.assertEqual(search['consecutive_failures'], 0)
                self.assertEqual(search['attempts'][0]['status'], 'pending')

    def test_same_artifact_new_candidate_label_does_not_count_as_new_seed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = root / 'published/first/strategy.json'; first.parent.mkdir(parents=True)
            artifact = {'level': 'L3', 'policy': {'candidate': 'same'}}
            first.write_text(json.dumps(artifact))
            with patch.object(reproduce, 'OUTPUT', root):
                attempt = reproduce.begin_seed_attempt(first)
                progress = Path(attempt['pilot_comparison']).with_name('progress.json')
                progress.parent.mkdir(parents=True)
                progress.write_text(json.dumps({'artifact': artifact, 'total_parallelism': 16, 'workers_per_shard': 16, 'items': {}}))
                second = root / 'published/second/strategy.json'; second.parent.mkdir(parents=True)
                second.write_text(json.dumps(artifact))
                repeated = reproduce.begin_seed_attempt(second)
                self.assertEqual(repeated['attempt'], attempt['attempt'])
                self.assertEqual(len(reproduce.seed_search()['attempts']), 1)
                self.assertEqual(reproduce.seed_search()['current_seed'], str(second))
                # Matching an already-started artifact updates the active
                # cursor, while preserving the original candidate label and
                # its comparison paths for auditability.
                self.assertEqual(repeated['seed'], str(first.resolve()))
                self.assertEqual(repeated['pilot_comparison'], attempt['pilot_comparison'])

    def test_missing_judge_or_coverage_does_not_count_as_failed_seed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            seed = root / 'published/current/strategy.json'; seed.parent.mkdir(parents=True)
            artifact = {'level': 'L3', 'policy': {'candidate': 1}}
            seed.write_text(json.dumps(artifact))
            with patch.object(reproduce, 'OUTPUT', root):
                attempt = reproduce.begin_seed_attempt(seed)
                comparison = Path(attempt['pilot_comparison']); comparison.parent.mkdir(parents=True)
                _candidate(comparison, ['task-' + str(i) for i in range(32)], missing='tokens')
                progress = comparison.with_name('progress.json')
                value = json.loads(comparison.read_text()); value.update(seed=str(seed), progress=str(progress))
                comparison.write_text(json.dumps(value))
                progress.write_text(json.dumps({'artifact': artifact,
                    'items': {row['task_id']: row for row in value['items']}}))
                result = {'seed_match': True, 'coverage_pass': False, 'goal_passed': False,
                          'status': 'fail', 'goal_checks': {'coverage': False}}
                reproduce.record_seed_gate(seed, 32, comparison, result)
                state = reproduce.seed_search()
                self.assertEqual(state['consecutive_failures'], 0)
                self.assertEqual(state['attempts'][0]['status'], 'pending')

    def test_same_seed_32_pass_then_100_failure_counts_one_attempt(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            seed = root / 'published/current/strategy.json'; seed.parent.mkdir(parents=True)
            artifact = {'level': 'L3', 'policy': {'candidate': 1}}
            seed.write_text(json.dumps(artifact))
            with patch.object(reproduce, 'OUTPUT', root):
                attempt = reproduce.begin_seed_attempt(seed)
                for count in (32, 100):
                    baseline = root / f'gepa/l0-stage{count}/result.json'; baseline.parent.mkdir(parents=True)
                    ids = _l0(baseline, count=count)
                    comparison = Path(attempt['pilot_comparison' if count == 32 else 'validation_comparison'])
                    comparison.parent.mkdir(parents=True)
                    _candidate(comparison, ids, quality=.42 if count == 32 else .4)
                    value = json.loads(comparison.read_text()); progress = comparison.with_name('progress.json')
                    value.update(seed=str(seed), progress=str(progress)); comparison.write_text(json.dumps(value))
                    progress.write_text(json.dumps({'artifact': artifact, 'total_parallelism': 16, 'workers_per_shard': 16,
                        'items': {row['task_id']: row for row in value['items']}}))
                    passed, result = gate.verify_stage(str(count), comparison, baseline, seed)
                    self.assertEqual(passed, count == 32)
                    reproduce.record_seed_gate(seed, count, comparison, result)
                reproduce.record_seed_gate(seed, 100, comparison, result)
                state = reproduce.seed_search()
                self.assertEqual(state['consecutive_failures'], 1)
                self.assertEqual(len(state['attempts']), 1)
                self.assertEqual(state['attempts'][0]['last_stage'], 100)

    def test_same_path_changed_artifact_and_inflight_checkpoint_cannot_reuse_pass(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            seed, baseline, comparison, progress = [root / name for name in
                ('strategy.json', 'l0.json', 'comparison.json', 'progress.json')]
            artifact = {'level': 'L3', 'policy': {'advice': 'original'}}
            seed.write_text(json.dumps(artifact))
            ids = _l0(baseline); _candidate(comparison, ids)
            value = json.loads(comparison.read_text())
            value.update(seed=str(seed), progress=str(progress))
            comparison.write_text(json.dumps(value))
            checkpoint = {'artifact': artifact,
                          'items': {row['task_id']: row for row in value['items']}}
            progress.write_text(json.dumps(checkpoint))
            self.assertTrue(gate.verify_stage('32', comparison, baseline, seed)[0])
            replacement = {'level': 'L3', 'policy': {'advice': 'replacement'}}
            seed.write_text(json.dumps(replacement))
            self.assertFalse(gate.verify_stage('32', comparison, baseline, seed)[0])
            # A replacement in flight cannot put a new artifact next to old rows.
            checkpoint.update(artifact=replacement, items={})
            progress.write_text(json.dumps(checkpoint))
            self.assertFalse(gate.verify_stage('32', comparison, baseline, seed)[0])

    def test_direct_gepa_entries_require_same_seed_complete_100_targets(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "gepa"
            root.mkdir()
            seed = root / 'published/current/strategy.json'
            seed.parent.mkdir(parents=True)
            artifact = {'level': 'L3', 'policy': {'advice': 'original'}}
            seed.write_text(json.dumps(artifact))
            for count in (32, 100):
                folder = root / f'current-stage{count}'; folder.mkdir()
                baseline = root / f'l0-current-stage{count}/result.json'; baseline.parent.mkdir()
                ids = _l0(baseline, count=count)
                comparison, progress = folder / 'comparison.json', folder / 'progress.json'
                _candidate(comparison, ids)
                value = json.loads(comparison.read_text())
                native_progress = json.loads(Path(value['progress']).read_text())
                value.update(seed=str(seed), progress=str(progress))
                comparison.write_text(json.dumps(value))
                progress.write_text(json.dumps(dict(native_progress, artifact=artifact,
                    items={row['task_id']: row for row in value['items']})))
            # Derived paths are bound when offline_gepa is imported. Patching
            # GEPA alone leaves CONFIG/PLAN/STATUS pointed at real experiments
            # if a negative gate assertion unexpectedly reaches prepare().
            with patch.object(gate, 'GEPA', root), \
                    _isolated_gepa_outputs(root.parent, gepa=root,
                                           training=root / 'training', published=root / 'published'):
                self.assertEqual(offline_gepa.require_seed_gates(seed), artifact)
                target = root / 'current-stage100/comparison.json'
                original = json.loads(target.read_text())
                progress = root / 'current-stage100/progress.json'
                # Each target remains a real same-task complete 100-row gate.
                for field, failure in [('quality', .4), ('verdict', 'failed'),
                    ('avg_invalid_loop', .2)]:
                    with self.subTest(field=field):
                        value = json.loads(json.dumps(original))
                        for row in value['items']:
                            row[field] = failure
                        target.write_text(json.dumps(value))
                        progress.write_text(json.dumps(dict(native_progress, artifact=artifact,
                            items={row['task_id']: row for row in value['items']})))
                        with self.assertRaisesRegex(RuntimeError, 'stage100_goal_gate_not_passed'):
                            offline_gepa.require_seed_gates(seed)
                # Direct --prepare/--run must stop before training setup or serve.
                with patch.object(offline_gepa, '_training_ready',
                        side_effect=AssertionError('failed gate reached training setup')) as ready:
                    with self.assertRaisesRegex(RuntimeError, 'stage100_goal_gate_not_passed'):
                        offline_gepa.prepare(seed)
                    ready.assert_not_called()
                with patch.object(offline_gepa, 'read', wraps=offline_gepa.read) as read, \
                     patch.object(offline_gepa, 'PLAN', root / 'plan.json'), \
                     patch.object(offline_gepa.subprocess, 'Popen') as process:
                    (root / 'plan.json').write_text(json.dumps({'seed': str(seed)}))
                    with self.assertRaisesRegex(RuntimeError, 'stage100_goal_gate_not_passed'):
                        offline_gepa.run(seed)
                    process.assert_not_called()

    def test_gate_summary_preserves_measured_intent_turns(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            l0, candidate = root / 'l0.json', root / 'candidate.json'
            ids = _l0(l0)
            _candidate(candidate, ids)
            for path in (l0, candidate):
                value = json.loads(path.read_text())
                value['items'][0].update(intent_counts=[2, 0, 0], intent_measured=1,
                                         intent_turn_keys=[ids[0] + '#1'])
                path.write_text(json.dumps(value))
            passed, result = gate.verify_stage('32', candidate, l0)
            self.assertTrue(passed, result)
            for role in ('baseline', 'candidate'):
                self.assertEqual(result[role]['intent_turn_keys'], [ids[0] + '#1'])

    def test_v8_reports_exploration_but_not_goal(self) -> None:
        comparison = gate.GEPA / "seed0-structured-v8-stage32" / "comparison.json"
        if not comparison.is_file() or not gate.DEFAULT_L0.is_file():
            self.skipTest("v8 32-task fixture is not present")
        passed, result = gate.verify_stage("32", comparison, gate.DEFAULT_L0)
        self.assertFalse(passed)
        # Historical v8 rows predate the canonical business quality field and
        # use native loop lower-bound evidence; they remain exploratory only.
        self.assertFalse(result["coverage_pass"])
        self.assertFalse(result["goal_passed"])
        self.assertTrue(result["goal_checks"]["strict_relative_gain_5pct"])
        self.assertFalse(result["goal_checks"]["quality_relative_gain_5pct"])
        self.assertEqual(result["exploration_gain"]["invalid_loop_state"],
                         "blocked_unverifiable")
        self.assertFalse(result["goal_checks"]["resources_not_increased"])

    def test_complete_same_task_evidence_can_pass(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            l0 = root / "l0.json"
            candidate = root / "candidate.json"
            ids = _l0(l0)
            _candidate(candidate, ids)
            passed, result = gate.verify_stage("32", candidate, l0)
            self.assertTrue(passed, result)
            self.assertTrue(result["coverage_pass"])
            self.assertTrue(result["goal_passed"])
            self.assertEqual(result["status"], "pass")
            self.assertEqual(result["exploration_gain"]["invalid_loop_state"], "pass")

    def test_missing_resource_field_is_unknown_without_blocking_core_gate(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            l0 = root / "l0.json"
            candidate = root / "candidate.json"
            ids = _l0(l0)
            _candidate(candidate, ids, missing="tokens")
            passed, result = gate.verify_stage("32", candidate, l0)
            self.assertTrue(passed)
            self.assertTrue(result["coverage_pass"])
            self.assertTrue(result["goal_passed"])
            self.assertFalse(result["goal_checks"]["resources_not_increased"])
            self.assertTrue(result["errors"])

    def test_zero_loop_is_blocked_even_when_other_goals_pass(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            l0 = root / "l0.json"
            candidate = root / "candidate.json"
            ids = _l0(l0, loop=0.0)
            _candidate(candidate, ids, loop=0.0)
            passed, result = gate.verify_stage("32", candidate, l0)
            self.assertFalse(passed)
            self.assertEqual(result["status"], "blocked")
            self.assertEqual(result["exploration_gain"]["invalid_loop_state"],
                             "blocked_unverifiable")
            self.assertFalse(result["goal_checks"]["invalid_loop_reduction_15pct"])

    def test_semantic_runtime_is_diagnostic_only_for_stage_loop_gate(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            l0 = root / "l0.json"
            ids = _l0(l0, loop_basis="semantic_runtime")
            baseline = gate.canonical_l0(l0, ids)
            self.assertIsNone(baseline["avg_invalid_loop"])
            self.assertIsNone(baseline["loop_basis"])
            self.assertEqual(baseline["loop_review_coverage"], 0)
            self.assertIsNone(gate._formal_loop_basis("model_review+semantic_runtime"))
            # A native lower bound cannot satisfy the formal loop gate even
            # when the candidate has complete model-review evidence.
            candidate = root / "candidate.json"
            _candidate(candidate, ids)
            passed, result = gate.verify_stage("32", candidate, l0)
            self.assertFalse(passed)
            self.assertEqual(result["exploration_gain"]["invalid_loop_state"],
                             "blocked_unverifiable")

    def test_partial_loop_coverage_is_not_a_goal_pass(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            l0 = root / "l0.json"
            candidate = root / "candidate.json"
            ids = _l0(l0)
            _candidate(candidate, ids)
            value = json.loads(candidate.read_text())
            value["items"][0].pop("invalid_loop_basis")
            value["items"][0].pop("avg_invalid_loop")
            candidate.write_text(json.dumps(value), encoding="utf-8")
            passed, result = gate.verify_stage("32", candidate, l0)
            self.assertFalse(passed)
            self.assertFalse(result["goal_checks"]["coverage"])
            self.assertFalse(result["goal_checks"]["invalid_loop_reduction_15pct"])
            self.assertFalse(result["exploration_gain"]["loop_complete_coverage"])
            self.assertEqual(result["exploration_gain"]["paired_loop_coverage"], 31)

    def test_quality_score_alias_cannot_pass_quality_gate(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            l0 = root / "l0.json"
            candidate = root / "candidate.json"
            ids = _l0(l0)
            _candidate(candidate, ids)
            value = json.loads(candidate.read_text())
            for row in value["items"]:
                row.pop("quality", None)
            candidate.write_text(json.dumps(value), encoding="utf-8")
            passed, result = gate.verify_stage("32", candidate, l0)
            self.assertFalse(passed)
            self.assertEqual(result["status"], "blocked")
            self.assertIsNone(result["candidate"]["quality"])
            self.assertFalse(result["goal_checks"]["quality_relative_gain_5pct"])

    def test_official_reward_cannot_override_failed_native_verdict(self) -> None:
        row = {"task_id": "fixture", "reward": 1, "verdict": "failed",
               "quality": 0, "strict_passed": 1}
        result = gate.aggregate([row])
        self.assertEqual(result["strict_passed"], 0)
        self.assertEqual(result["official_strict_passed"], 1)
        self.assertEqual(result["scored"], 1)
        row.pop("quality")
        result = gate.aggregate([row])
        self.assertEqual(result["scored"], 0)
        self.assertIsNone(result["strict_passed"])

    def test_per_trial_means_and_five_actual_level_counts_are_preserved(self) -> None:
        rows = []
        for i in range(2):
            rows.append({"task_id": f"fixture-{i}", "verdict": "passed", "quality": 1,
                         "tokens": 10 + 20 * i, "model_calls": 1 + 2 * i,
                         "duration_ms": 100 + 200 * i, "latency_ms": 9999,
                         "l1_applied": 1, "l2_applied": 3,
                         "l2_action_applied": 2, "l2_schedule_applied": 1,
                         "l3_applied": 4})
        result = gate.aggregate(rows)
        self.assertEqual(result["tokens"], 20)
        self.assertEqual(result["total_tokens"], 40)
        self.assertEqual(result["model_calls"], 2)
        self.assertEqual(result["total_model_calls"], 4)
        self.assertEqual(result["latency_ms"], 200)
        self.assertEqual(result["levels"], {"L1": 2, "L2": 6,
                                          "L2_action": 4, "L2_schedule": 2, "L3": 8})

    def test_baseline_token_coverage_is_reported_exactly_like_candidate(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            l0, candidate = root / "l0.json", root / "candidate.json"
            ids = _l0(l0)
            _candidate(candidate, ids)
            value = json.loads(l0.read_text())
            value["items"][0]["tokens_complete"] = False
            l0.write_text(json.dumps(value))
            passed, result = gate.verify_stage("32", candidate, l0)
            self.assertTrue(passed)
            self.assertTrue(result["coverage_pass"])
            self.assertFalse(result["goal_checks"]["resources_not_increased"])
            self.assertFalse(result["baseline"]["tokens_complete"])

    def test_intent_uses_pooled_counts_and_error_rows_cannot_improve_f1(self) -> None:
        rows = [{"task_id": "first", "quality": 1, "verdict": "passed",
                 "intent_counts": [1, 0, 1], "intent_measured": 1},
                {"task_id": "second", "quality": 0, "verdict": "failed",
                 "intent_counts": [1, 2, 0], "intent_measured": 2},
                {"task_id": "error", "quality": 1, "verdict": "error",
                 "error_code": "evaluation_judge_failed", "intent_counts": [50, 0, 0],
                 "intent_measured": 20}]
        result = gate.aggregate(rows)
        self.assertAlmostEqual(result["intent_f1"], 4 / 7)
        self.assertEqual(result["intent_measured"], 3)
        self.assertEqual(result["intent_task_measured"], 2)
        self.assertEqual(gate.aggregate([])["intent_measured"], 0)
        self.assertIsNone(gate.aggregate([])["intent_f1"])

    @contextmanager
    def native_publish_fixture(self, root, count=3):
        import compare
        import hashlib
        gepa, published, dataset = root / 'gepa', root / 'published', root / 'dataset'
        search = gepa / 'workspace/agent-evaluation/fixture-job/search'
        versions = search / 'versions'
        versions.mkdir(parents=True)
        dataset.mkdir()
        dataset_version = 'dataset-v1:fixture'
        (dataset / 'manifest.json').write_text(json.dumps({
            'metadata': {'dataset_version': dataset_version}, 'summary': {'splits': {'holdout': 500}}}))
        (dataset / 'holdout.jsonl').write_text(''.join(
            json.dumps({'task_id': f'task-{i:03d}'}) + '\n' for i in range(500)))
        runtime = gepa / 'runtime'
        runtime.mkdir()
        model = runtime / 'model.json'
        model.write_text(json.dumps({'model': {'endpoint': 'fixture', 'model_id': 'fixture'}}))
        prompt = runtime / 'judge.txt'
        prompt.write_text('frozen real judge prompt')
        service = runtime / 'service.json'
        service.write_text('{}')
        config = {'variants': [{'instrumentation_config': str(service)}],
                  'judge': dict(offline_gepa.controller.judge_projection(), enabled=True,
                                runtime_config=str(model), prompt=str(prompt), assessment_mode='objectives')}
        config_path = runtime / 'evaluation.json'
        config_path.write_text(json.dumps(config))
        env = {'OPENAI_BASE_URL': 'fixture-endpoint', 'OPENAI_MODEL': 'fixture-model'}
        def native_rows(variant):
            baseline = variant == 'baseline'
            correct = 200 if baseline else 210
            tokens, calls, duration, loops = (10, 2, 100, 2) if baseline else (9, 1, 90, 1)
            return [{'task_id': f'task-{i:03d}', 'trial_id': f'{variant}-{i}',
                     'split': 'holdout', 'variant_id': variant, 'execution_status': 'completed',
                     'duration_ms': duration, 'verdict': 'passed' if i < correct else 'failed',
                     'metrics': {'tokens': tokens, 'llm_calls': calls},
                     'evidence': {'output': {'final': {
                         'appworld_reward': int(i < correct), 'strict_passed': i < correct,
                         'quality_score': int(i < correct), 'tokens_complete': True,
                         'tokens': tokens, 'model_calls': calls, 'latency_ms': 9999,
                         'framework_capture': {'status': 'complete'}}},
                         'assessment': {'version': 'goal-assessment-v1', 'quality': int(i < correct),
                                        'quality_upper': int(i < correct), 'coverage': 1, 'critical_failures': 0},
                         'loop_review': {'status': 'complete', 'count': loops, 'observed_iterations': 3},
                         'trace': {'identity': {'run_id': f'{variant}-{i}'},
                                   'attributes': {'capture_complete': True},
                                   'events': [{'kind': 'run.start'}, {'kind': 'run.end'}]}}} for i in range(500)]
        baseline = native_rows('baseline')
        baseline_source = gepa / 'l0-current-stage500/capture/report.json'
        baseline_source.parent.mkdir(parents=True)
        baseline_source.parent.joinpath('config.json').write_text(json.dumps(config))
        baseline_source.write_text(json.dumps({'complete': True,
            'configuration': {'judge_fingerprint': 'frozen-judge'}, 'trials': baseline}))
        baseline_source.with_name('manifest.json').write_text(json.dumps({'complete': True}))
        gepa.joinpath('l0-current-stage500/result.json').write_text(json.dumps({
            'status': 'complete', 'source_report': str(baseline_source),
            'judge_projection': offline_gepa.controller.judge_projection(),
            'items': [compare.compact_row(row, 'original-L0') for row in baseline]}))
        with _isolated_gepa_outputs(root, gepa=gepa, published=published, dataset=dataset), \
             patch.object(offline_gepa.controller, 'environment', return_value=env), \
             patch.object(offline_gepa, 'progress', return_value={'job_id': 'fixture-job', 'job_status': 'passed'}), \
             patch.object(offline_gepa.controller, 'version', side_effect=lambda a: a['agent']['revision']):
            context = compare._native_gepa_evaluation_context(config_path, dataset_version)
            tree = {'status': 'completed', 'dataset_version': dataset_version,
                    'baseline_version': 'baseline-version',
                    'evaluation_context': context, 'candidate_evaluation_context': 'frozen-execution',
                    'nodes': [], 'final_holdout': {'status': 'completed', 'requested_top_k': 3,
                                                 'candidates': []}, 'evaluations': {}}
            reports = []
            for node in range(1, count + 1):
                version = f'version-{node}'
                report = search / f'evaluations/eval-{node}/attempt-1/report.json'
                report.parent.mkdir(parents=True)
                run_id = f'evaluation-{node}'
                report.write_text(json.dumps({'complete': True, 'run_id': run_id,
                    'dataset_version': dataset_version,
                    'configuration': {'allow_unreviewed': False, 'model_enabled': True,
                                      'judge_fingerprint': 'frozen-judge',
                                      'variants': [{'id': 'baseline', 'version': 'baseline-version'},
                                                   {'id': 'candidate', 'version': version}]},
                    'trials': baseline + native_rows('candidate')}))
                report_hash = hashlib.sha256(report.read_bytes()).hexdigest()
                report.with_name('manifest.json').write_text(json.dumps({'complete': True,
                    'run_id': run_id, 'dataset_version': dataset_version,
                    'files': {'report.json': report_hash}}))
                tree['nodes'].append({'id': float(node), 'version': version})
                tree['final_holdout']['candidates'].append({'node': float(node), 'version': version,
                                                          'rank': node, 'status': 'complete', 'trials': 1000})
                tree['evaluations'][f'eval-{node}'] = {'split': 'holdout', 'status': 'complete',
                    'version': version, 'report': str(report.relative_to(search)), 'report_hash': report_hash,
                    'evaluation_context': context, 'candidate_evaluation_context': 'frozen-execution'}
                (versions / f'{node}.json').write_text(json.dumps(
                    {'level': 'L3', 'policy': {}, 'agent': {'revision': version}}))
                reports.append(report)
            tree_path = search / 'tree.json'
            tree_path.write_text(json.dumps(tree))
            yield {'tree': tree, 'tree_path': tree_path, 'reports': reports,
                   'config_path': config_path, 'model': model, 'prompt': prompt,
                   'env': env, 'gepa': gepa, 'published': published, 'service': service}

    def test_native_top3_publish_uses_per_trial_means_without_double_averaging(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            with self.native_publish_fixture(Path(directory)):
                result = offline_gepa.publish()
            self.assertTrue(result['goal_passed'])
            self.assertEqual(result['release_status'], 'published_goal_passed')
            proof = result['candidate_goal_checks'][0]['gate']
            self.assertEqual(proof['baseline']['tokens'], 10)
            self.assertEqual(proof['candidate']['tokens'], 9)
            self.assertEqual(proof['candidate']['total_tokens'], 4500)
            self.assertEqual(proof['candidate']['model_calls'], 1)
            self.assertEqual(proof['candidate']['total_model_calls'], 500)
            self.assertEqual(proof['candidate']['latency_ms'], 90)
            self.assertEqual(proof['candidate']['strict_passed'], 210)
            self.assertEqual(proof['candidate']['levels']['L2_action'], 0)
            self.assertEqual(proof['candidate']['levels']['L2_schedule'], 0)

    def test_native_publish_one_or_two_real_candidates_without_duplicating_scores(self):
        for count in (1, 2):
            with self.subTest(count=count), tempfile.TemporaryDirectory() as directory:
                with self.native_publish_fixture(Path(directory), count=count) as fixture:
                    before = [source.read_bytes() for source in fixture['reports']]
                    result = offline_gepa.publish()
                    self.assertTrue(result['goal_passed'])
                    self.assertEqual(result['evaluated_candidate_count'], count)
                    self.assertEqual(len(result['candidate_goal_checks']), count)
                    self.assertEqual([source.read_bytes() for source in fixture['reports']], before)
                    self.assertTrue((fixture['published'] / 'gepa-final/strategy.json').is_file())

    def test_fresh_native_paired_l0_proof_without_historical_500_file(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.native_publish_fixture(Path(directory), count=1) as fixture:
                (fixture['gepa'] / 'l0-current-stage500/result.json').unlink()
                result = offline_gepa.publish()
                self.assertTrue(result['goal_passed'])
                baseline = json.loads((fixture['gepa'] / 'final-comparison/L0.json').read_text())
                self.assertEqual(baseline['source_report'], str(fixture['reports'][0]))

    def test_native_final_proof_keeps_failed_goal_and_does_not_publish(self):
        import hashlib
        with tempfile.TemporaryDirectory() as directory:
            with self.native_publish_fixture(Path(directory), count=1) as fixture:
                report = fixture['reports'][0]
                value = json.loads(report.read_text())
                for row in value['trials']:
                    if row['variant_id'] == 'candidate' and 200 <= int(row['task_id'][-3:]) < 210:
                        row['verdict'] = 'failed'
                        row['evidence']['assessment'].update(quality=0, quality_upper=0)
                        row['evidence']['output']['final'].update(appworld_reward=0, strict_passed=False,
                                                                   quality_score=0)
                report.write_text(json.dumps(value))
                report_hash = hashlib.sha256(report.read_bytes()).hexdigest()
                manifest = json.loads(report.with_name('manifest.json').read_text())
                manifest['files']['report.json'] = report_hash
                report.with_name('manifest.json').write_text(json.dumps(manifest))
                fixture['tree']['evaluations']['eval-1']['report_hash'] = report_hash
                fixture['tree_path'].write_text(json.dumps(fixture['tree']))
                result = offline_gepa.publish(proof_only=True)
                self.assertFalse(result['goal_passed'])
                self.assertEqual(result['candidate_goal_checks'][0]['gate']['candidate']['strict_passed'], 200)
                self.assertFalse((fixture['published'] / 'gepa-final/strategy.json').exists())

    def test_native_publish_refuses_empty_excess_or_duplicate_finalists(self):
        for count, mutation in ((0, None), (4, None), (2, 'duplicate'), (2, 'above-requested')):
            with self.subTest(count=count, mutation=mutation), tempfile.TemporaryDirectory() as directory:
                with self.native_publish_fixture(Path(directory), count=count) as fixture:
                    tree = fixture['tree']
                    if mutation == 'duplicate':
                        tree['final_holdout']['candidates'][1] = dict(tree['final_holdout']['candidates'][0])
                    if mutation == 'above-requested':
                        tree['final_holdout']['requested_top_k'] = 1
                    fixture['tree_path'].write_text(json.dumps(tree))
                    with self.assertRaisesRegex(RuntimeError, 'top3_holdout_incomplete'):
                        offline_gepa.publish()
                    self.assertFalse((fixture['published'] / 'gepa-final/strategy.json').exists())

    def test_native_publish_rejects_changed_configuration_and_native_bindings(self):
        cases = ('runtime', 'model-environment', 'evaluation-config', 'prompt', 'instrumentation',
                 'judge-fingerprint', 'manifest-run', 'manifest-dataset', 'report-version',
                 'artifact-version', 'report-bytes', 'entry-context', 'manifest-missing')
        for case in cases:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as directory:
                with self.native_publish_fixture(Path(directory), count=1) as fixture:
                    report = fixture['reports'][0]
                    if case == 'runtime':
                        fixture['model'].write_text('{"changed": true}')
                    elif case == 'model-environment':
                        fixture['env']['OPENAI_MODEL'] = 'different-model'
                    elif case == 'evaluation-config':
                        config = json.loads(fixture['config_path'].read_text())
                        config['judge']['require_state_dependency'] = False
                        fixture['config_path'].write_text(json.dumps(config))
                    elif case == 'prompt':
                        fixture['prompt'].write_text('different judge prompt')
                    elif case == 'instrumentation':
                        fixture['service'].write_text('{"different": true}')
                    elif case == 'manifest-missing':
                        report.with_name('manifest.json').unlink()
                    elif case in ('manifest-run', 'manifest-dataset'):
                        manifest = json.loads(report.with_name('manifest.json').read_text())
                        manifest['run_id' if case == 'manifest-run' else 'dataset_version'] = 'other-source'
                        report.with_name('manifest.json').write_text(json.dumps(manifest))
                    elif case == 'artifact-version':
                        artifact = report.parents[3] / 'versions/1.json'
                        value = json.loads(artifact.read_text()); value['agent']['revision'] = 'other-version'
                        artifact.write_text(json.dumps(value))
                    elif case == 'entry-context':
                        fixture['tree']['evaluations']['eval-1']['evaluation_context'] = 'other-context'
                        fixture['tree_path'].write_text(json.dumps(fixture['tree']))
                    else:
                        value = json.loads(report.read_text())
                        if case == 'judge-fingerprint':
                            value['configuration']['judge_fingerprint'] = 'other-judge'
                        elif case == 'report-version':
                            value['configuration']['variants'][1]['version'] = 'other-version'
                        else:
                            value['trials'][0]['verdict'] = 'failed'
                        report.write_text(json.dumps(value))
                    with self.assertRaisesRegex(RuntimeError, 'native_judge_profile_invalid'):
                        offline_gepa.publish()
                    self.assertFalse((fixture['published'] / 'gepa-final/strategy.json').exists())

    def test_cli_no_resource_bypass_option(self) -> None:
        command = [sys.executable, str(Path(gate.__file__)), "verify", "--help"]
        output = subprocess.run(command, check=True, capture_output=True, text=True).stdout
        self.assertNotIn("--strict-resources", output)
        self.assertNotIn("--resource-tolerance", output)


class ControllerStatusTests(unittest.TestCase):
    def test_only_training_child_is_detached_from_controller_session(self):
        with patch.object(reproduce.subprocess, 'run') as run:
            run.return_value.returncode = 0
            reproduce.run('training.py', '--workers', '16')
            reproduce.run('compare.py', '--workers', '16')
        self.assertTrue(run.call_args_list[0].kwargs['start_new_session'])
        self.assertFalse(run.call_args_list[1].kwargs['start_new_session'])

    def test_real_training_child_survives_controller_process_group_hangup(self):
        import os
        import signal
        import time
        with tempfile.TemporaryDirectory(prefix='appworld-session-', dir='/tmp') as directory:
            root = Path(directory)
            healthy = root / 'healthy.json'
            healthy.write_text(json.dumps({'executions': 1}))
            original = healthy.read_bytes()
            (root / 'training.py').write_text('''import json, os, time
from pathlib import Path
root = Path(__file__).parent
(root / 'started.json').write_text(json.dumps({'pid': os.getpid(), 'sid': os.getsid(0)}))
while not (root / 'finish').exists():
    time.sleep(.02)
assert json.loads((root / 'healthy.json').read_text())['executions'] == 1
(root / 'completed.json').write_text(json.dumps({'reused_healthy': 1, 'new_executions': 1}))
''')
            code = ('import sys; from pathlib import Path; '
                    f'sys.path.insert(0, {str(EXPERIMENT)!r}); import reproduce; '
                    f'reproduce.HERE = reproduce.ROOT = Path({str(root)!r}); '
                    'reproduce.run("training.py")')
            controller = subprocess.Popen([sys.executable, '-B', '-c', code],
                start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            child = None
            try:
                deadline = time.monotonic() + 5
                while not (root / 'started.json').exists() and time.monotonic() < deadline:
                    time.sleep(.02)
                self.assertTrue((root / 'started.json').is_file())
                started = json.loads((root / 'started.json').read_text())
                child = started['pid']
                self.assertEqual(started['sid'], child)
                self.assertNotEqual(started['sid'], controller.pid)
                os.killpg(controller.pid, signal.SIGHUP)
                self.assertEqual(controller.wait(timeout=3), -signal.SIGHUP)
                os.kill(child, 0)
                (root / 'finish').touch()
                deadline = time.monotonic() + 3
                while not (root / 'completed.json').exists() and time.monotonic() < deadline:
                    time.sleep(.02)
                self.assertEqual(json.loads((root / 'completed.json').read_text()),
                                 {'reused_healthy': 1, 'new_executions': 1})
                self.assertEqual(healthy.read_bytes(), original)
            finally:
                if controller.poll() is None:
                    controller.kill()
                controller.wait(timeout=3)
                if child:
                    try:
                        os.kill(child, signal.SIGTERM)
                    except ProcessLookupError:
                        pass

    def test_interrupt_and_resume_use_real_main_without_changing_seed_attempts(self):
        import os
        import shutil
        import signal
        import time
        with tempfile.TemporaryDirectory(prefix='appworld-status-', dir='/tmp') as directory:
            root = Path(directory)
            here = root / 'tools/trajectory_evolution/experiments/appworld'
            here.mkdir(parents=True)
            shutil.copyfile(Path(reproduce.__file__), here / 'reproduce.py')
            (here / 'layout.py').write_text('''import json
from pathlib import Path
def save(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value))
''')
            (here / 'learning.py').write_text('def validate_seed_label(label):\n    return label\n')
            state_path = root / 'trace_output/appworld/gepa/status.json'
            state_path.parent.mkdir(parents=True)
            search = {'status': 'searching', 'round': 1, 'consecutive_failures': 0,
                      'attempts': [{'attempt': 1, 'status': 'pending'}]}
            state_path.write_text(json.dumps({'seed_search': search}))
            wrapper = root / 'fixture.py'
            wrapper.write_text('''import sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent / 'tools/trajectory_evolution/experiments/appworld'))
import reproduce
reproduce.prepare = lambda: None
def train():
    marker = reproduce.OUTPUT / 'training/complete'
    marker.parent.mkdir(parents=True, exist_ok=True)
    if not marker.exists():
        while True: time.sleep(.05)
reproduce.train_with_recovery = train
reproduce.seed_with_recovery = lambda **kwargs: {'level': 'L3'}
reproduce.stage = lambda *args: None
reproduce.verify = lambda *args, **kwargs: 0
reproduce.run = lambda *args, **kwargs: 0
raise SystemExit(reproduce.main())
''')
            process = subprocess.Popen([sys.executable, '-B', str(wrapper), '--all'],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                       start_new_session=True)
            try:
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    state = json.loads(state_path.read_text())
                    if (state.get('experiment') or {}).get('phase') == 'training':
                        break
                    time.sleep(.02)
                self.assertEqual(state['experiment']['status'], 'running')
                self.assertEqual(state['experiment']['phase'], 'training')
                process.send_signal(signal.SIGINT)
                self.assertEqual(process.wait(timeout=3), 130)
                interrupted = json.loads(state_path.read_text())
                self.assertEqual(interrupted['experiment']['status'], 'interrupted')
                self.assertEqual(interrupted['experiment']['phase'], 'training')
                self.assertEqual(interrupted['seed_search'], search)
                marker = root / 'trace_output/appworld/training/complete'
                marker.write_text('complete')
                resumed = subprocess.run([sys.executable, '-B', str(wrapper), '--all'],
                    capture_output=True, text=True, timeout=5)
                self.assertEqual(resumed.returncode, 0, resumed.stderr)
                final = json.loads(state_path.read_text())
                self.assertEqual(final['experiment']['status'], 'complete')
                self.assertEqual(final['experiment']['phase'], 'publish')
                self.assertEqual(final['seed_search']['attempts'], search['attempts'])
                self.assertEqual(final['seed_search']['consecutive_failures'], 0)
                self.assertEqual(marker.read_text(), 'complete')
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=3)
                if process.stderr:
                    process.stderr.close()

    def test_gate_failure_records_stage_failed_and_keeps_pending_attempt(self):
        with tempfile.TemporaryDirectory() as directory, \
             patch.object(reproduce, 'OUTPUT', Path(directory)), \
             patch.object(reproduce, 'verify', return_value=1), \
             patch.object(sys, 'argv', ['reproduce.py', '--gate32']):
            path = Path(directory) / 'gepa/status.json'
            path.parent.mkdir(parents=True)
            search = {'status': 'searching', 'consecutive_failures': 0,
                      'attempts': [{'attempt': 1, 'status': 'pending'}]}
            path.write_text(json.dumps({'seed_search': search}))
            self.assertEqual(reproduce.main(), 1)
            result = json.loads(path.read_text())
            self.assertEqual(result['experiment']['status'], 'stage_failed')
            self.assertEqual(result['experiment']['phase'], 'gate32')
            self.assertEqual(result['seed_search'], search)

    def test_untrusted_exception_prose_is_not_saved_or_reported_as_success(self):
        with tempfile.TemporaryDirectory() as directory, \
             patch.object(reproduce, 'OUTPUT', Path(directory)), \
             patch.object(reproduce, 'prepare', side_effect=RuntimeError('private model credential body')), \
             patch.object(sys, 'argv', ['reproduce.py', '--prepare']):
            self.assertEqual(reproduce.main(), 1)
            state = json.loads((Path(directory) / 'gepa/status.json').read_text())
            self.assertEqual(state['experiment']['reason'], 'appworld_stage_failed')
            self.assertNotIn('private model', json.dumps(state))

    def test_normal_failure_emits_safe_reason_class_phase_and_exit(self):
        from contextlib import redirect_stderr
        class GateFailed(RuntimeError):
            pass
        class Incomplete(RuntimeError):
            pass
        cases = [(RuntimeError('stage_gate_not_passed:validation100'),
                  'stage_gate_not_passed:validation100'),
                 (RuntimeError('stage_dependency_incomplete:validation100:pilot32'),
                  'stage_dependency_incomplete:validation100:pilot32'),
                 (GateFailed('private credential response'), 'appworld_stage_gate_not_passed'),
                 (Incomplete('private credential response'), 'appworld_stage_incomplete'),
                 (ValueError('private credential response'), 'appworld_stage_failed')]
        for error, reason in cases:
            with self.subTest(error_class=type(error).__name__), tempfile.TemporaryDirectory() as directory:
                def fail(*args, **kwargs):
                    reproduce._experiment_status('running', 'validation100')
                    raise error
                stderr = io.StringIO()
                with patch.object(reproduce, 'OUTPUT', Path(directory)), \
                     patch.object(reproduce, '_run_common_all', side_effect=fail), \
                     patch.object(sys, 'argv', ['reproduce.py', '--validation100']), redirect_stderr(stderr):
                    self.assertEqual(reproduce.main(), 1)
                reported = json.loads(stderr.getvalue())
                self.assertEqual(reported, {'status': 'stage_failed', 'reason': reason,
                    'error_class': type(error).__name__, 'phase': 'validation100', 'exit_code': 1})
                state = json.loads((Path(directory) / 'gepa/status.json').read_text())
                self.assertEqual(state['experiment']['status'], 'stage_failed')
                self.assertEqual(state['experiment']['reason'], reason)
                self.assertNotIn('private credential', stderr.getvalue() + json.dumps(state))

    def test_failure_diagnostic_does_not_echo_untrusted_lifecycle_phase(self):
        from contextlib import redirect_stderr
        with tempfile.TemporaryDirectory() as directory:
            def fail(*args, **kwargs):
                reproduce._experiment_status('running', 'private credential phase')
                raise RuntimeError('stage_gate_not_passed:private_credential')
            stderr = io.StringIO()
            with patch.object(reproduce, 'OUTPUT', Path(directory)), \
                 patch.object(reproduce, '_run_common_all', side_effect=fail), \
                 patch.object(sys, 'argv', ['reproduce.py', '--validation100']), redirect_stderr(stderr):
                self.assertEqual(reproduce.main(), 1)
            reported = json.loads(stderr.getvalue())
            self.assertEqual(reported['phase'], 'unknown')
            self.assertEqual(reported['reason'], 'appworld_stage_failed')
            self.assertNotIn('private credential', stderr.getvalue())


class UnifiedL0SourceTests(unittest.TestCase):
    def fixture(self):
        from copy import deepcopy
        import compare
        from test_appworld_compare_metrics import native_row
        temporary = tempfile.TemporaryDirectory(prefix='appworld-unified-l0-', dir='/tmp')
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        full = root / 'gepa/l0-current-stage500/result.json'
        source = full.parent / 'regrade/retry-11/report.json'
        source.parent.mkdir(parents=True)
        published = root / 'published'
        published.mkdir()
        (published / 'L0.json').write_text(json.dumps({'agent': {'revision': 'fixture-revision'}}))
        ids = [f'task-{i:03d}' for i in range(500)]
        rows = []
        for i, task in enumerate(ids):
            passed = i < 22 or 32 <= i < 77 or 100 <= i < 355
            row = native_row(task, quality=float(passed), verdict='passed' if passed else 'failed')
            row['checks'] = [{'check_id': 'model', 'status': 'passed' if passed else 'failed'}]
            if i < 50 or 100 <= i < 240:
                row['evidence']['loop_review'].update(count=1, wasted_iterations=[{'step': 1}])
            rows.append(row)
        items = [compare.compact_row(row, 'l0-baseline') for row in rows]
        for row in items:
            row['loaded_version'] = 'fixture-native-version'
        items[0].update(tokens=None, latency_ms=None, model_calls=None,
                        tokens_complete=False, execution_costs_complete=False)
        configuration = {'variants': [{'id': 'candidate', 'version': 'fixture-revision'}],
                         'model_enabled': True, 'allow_unreviewed': False,
                         'judge_fingerprint': 'fixture-existing-judge'}
        native = {'complete': True, 'configuration': configuration,
                  'dataset_version': 'fixture-dataset', 'run_id': 'fixture-review', 'trials': rows}
        source.write_text(json.dumps(native))
        metrics = compare.summarize(items, 500)
        manifest = {'complete': True, 'dataset_version': 'fixture-dataset',
                    'run_id': 'fixture-review', 'files': {'report.json': 'fixture-existing-binding'},
                    'summary': {'trials': 500, 'planned_trials': 500, 'scored': 500,
                                'errors': 0, 'uncertain': 0, 'not_started_trials': 0,
                                'passed': metrics['strict_passed'],
                                'invalid_loop_assessment': {'method': 'model_review',
                                    'observations': 500, 'mean': metrics['avg_invalid_loop']}}}
        source.with_name('manifest.json').write_text(json.dumps(manifest))
        source.parent.with_name('retry-11.regrade.config.json').write_text(
            json.dumps({'judge': reproduce._APPWORLD_LAYOUT.judge_projection()}))
        result = {'status': 'complete', 'host_version': 'fixture-host', 'source_report': str(source),
                  'judge_projection': reproduce._APPWORLD_LAYOUT.judge_projection(),
                  'items': items, 'metrics': metrics}
        full.write_text(json.dumps(result))
        dataset = root / 'training/datasets/comparison500'
        dataset.mkdir(parents=True)
        (dataset / 'holdout.jsonl').write_text('\n'.join(json.dumps({'task_id': task}) for task in ids))
        (dataset / 'manifest.json').write_text(json.dumps({'metadata': {'dataset_version': 'fixture-dataset'}}))
        stack = ExitStack()
        stack.enter_context(patch.object(reproduce, 'OUTPUT', root))
        stack.enter_context(patch.object(reproduce._APPWORLD_LAYOUT, 'PUBLISHED', published))
        stack.enter_context(patch.object(reproduce, '_host_version', return_value='fixture-host'))
        self.addCleanup(stack.close)
        return root, full, source, ids, deepcopy(result), deepcopy(native), deepcopy(manifest)

    def test_new_scopes_and_dataset_adapter_use_complete_500_with_unknown_costs(self):
        root, full, source, ids, result, _, _ = self.fixture()
        before = source.read_bytes()
        for count in (32, 100):
            self.assertEqual(reproduce._stage_l0_source(count), full)
            self.assertEqual(reproduce._AppWorldDatasetAdapter.baseline(str(count))['path'], str(full))
        self.assertEqual(source.read_bytes(), before)
        self.assertFalse(result['items'][0]['execution_costs_complete'])
        self.assertIsNone(result['items'][0]['tokens'])

    def test_100_stage_reuses_500_and_inherits_only_first_100_ids(self):
        import dataset
        root, full, source, ids, _, _, _ = self.fixture()
        seed = root / 'published/new/strategy.json'
        seed.parent.mkdir()
        seed.write_text('{}')
        attempt = {'pilot_comparison': str(root / 'gepa/new32/comparison.json'),
                   'validation_comparison': str(root / 'gepa/new100/comparison.json')}
        with patch.object(dataset, 'materialize'), patch.object(reproduce, 'ensure_search_allowed'), \
             patch.object(reproduce, 'begin_seed_attempt', return_value=attempt), \
             patch.object(reproduce, 'verify', return_value=0), \
             patch.object(reproduce, '_experiment_status'), \
             patch.object(reproduce, '_inherit_pilot_capture') as inherit, \
             patch.object(reproduce, '_stage_compare_with_recovery') as compare:
            reproduce.stage(seed, 100)
        inherit.assert_called_once_with(seed, attempt, ids[:100])
        self.assertEqual(compare.call_count, 1)
        arguments = compare.call_args.args[0]
        self.assertEqual(arguments[arguments.index('--l0-source') + 1], full)
        self.assertEqual(arguments[arguments.index('--task-count') + 1], 100)
        self.assertNotIn('--baseline', arguments)

    def test_same_task_32_and_100_metrics_ignore_tail_and_full_summary(self):
        from copy import deepcopy
        root, full, source, ids, result, _, _ = self.fixture()
        for count in (32, 100):
            expected = gate.canonical_l0(full, ids[:count])
            changed = deepcopy(result)
            changed['metrics'].update(strict_passed=499, quality=.99, avg_invalid_loop=.001)
            for row in changed['items'][count:]:
                row.update(strict_passed=False, verdict='failed', quality=0,
                           avg_invalid_loop=999, invalid_loop_count=999)
            full.write_text(json.dumps(changed))
            actual = gate.canonical_l0(full, ids[:count])
            for key in ('strict_passed', 'quality', 'avg_invalid_loop', 'task_ids'):
                self.assertEqual(actual[key], expected[key])
            self.assertEqual(actual['quality_coverage'], count)
            full.write_text(json.dumps(result))

    def test_historical_verify_keeps_its_bound_32_and_100_baselines(self):
        root, full, source, ids, _, _, _ = self.fixture()
        seed = root / 'published/history/strategy.json'
        seed.parent.mkdir()
        seed.write_text('{}')
        pilot, validation = root / 'history32.json', root / 'history100.json'
        old32, old100 = root / 'old32.json', root / 'old100.json'
        _l0(old32)
        _l0(old100, count=100)
        pilot.write_text(json.dumps({'source_l0': str(old32)}))
        validation.write_text(json.dumps({'source_l0': str(old100)}))
        before = {path: path.read_bytes() for path in (pilot, validation, old32, old100)}
        def path_for(seed, count):
            return pilot if count == 32 else validation
        with patch.object(reproduce, 'stage_path', side_effect=path_for), \
             patch.object(reproduce, 'record_seed_gate'), \
             patch.object(gate, 'verify_stage', return_value=(True, {'goal_passed': True})) as verify, \
             redirect_stdout(io.StringIO()):
            self.assertEqual(reproduce.verify(seed, 100, validation), 0)
        self.assertEqual([call.args[2] for call in verify.call_args_list], [old32, old100])
        for path, body in before.items():
            self.assertEqual(path.read_bytes(), body)

    def test_invalid_500_falls_back_without_masking_existing_scope(self):
        from copy import deepcopy
        root, full, source, ids, result, native, manifest = self.fixture()
        config = source.parent.with_name('retry-11.regrade.config.json')
        original_config = json.loads(config.read_text())
        for failure in ('partial', 'host', 'profile', 'native_profile', 'coverage',
                        'judge_error', 'manifest', 'run', 'dataset', 'artifact', 'metric', 'loop'):
            with self.subTest(failure=failure):
                value, report, binding, settings = map(deepcopy, (result, native, manifest, original_config))
                if failure == 'partial': value['status'] = 'partial'
                if failure == 'host': value['host_version'] = 'other-host'
                if failure == 'profile': value['judge_projection']['require_state_dependency'] = False
                if failure == 'native_profile': settings['judge']['require_state_dependency'] = False
                if failure == 'coverage': value['items'].pop()
                if failure == 'judge_error': value['items'][0]['judge_error_code'] = 'fixture-error'
                if failure == 'manifest': binding['complete'] = False
                if failure == 'run': binding['run_id'] = 'other-run'
                if failure == 'dataset': report['dataset_version'] = 'other-dataset'
                if failure == 'artifact': report['configuration']['variants'][0]['version'] = 'other-revision'
                if failure == 'metric': value['metrics']['quality'] = .99
                if failure == 'loop': binding['summary']['invalid_loop_assessment']['observations'] = 499
                full.write_text(json.dumps(value))
                source.write_text(json.dumps(report))
                source.with_name('manifest.json').write_text(json.dumps(binding))
                config.write_text(json.dumps(settings))
                for count in (32, 100):
                    self.assertEqual(reproduce._stage_l0_source(count),
                        reproduce._APPWORLD_LAYOUT.baseline_result(count, output_root=root))


class JudgeProjectionTests(unittest.TestCase):
    def test_english_trigger_flag_reaches_seed_freeze_in_the_current_process(self):
        import os
        observed = []
        def freeze(*, label):
            observed.append((label, os.environ.get('APPWORLD_ENGLISH_TRIGGERS'),
                             os.environ.get('APPWORLD_ENGLISH_TRIGGERS_FROM')))
            return {'level': 'L3'}
        with tempfile.TemporaryDirectory() as directory, \
             patch.object(reproduce, 'OUTPUT', Path(directory)), \
             patch.object(reproduce, 'prepare'), \
             patch.object(reproduce, 'train_with_recovery'), \
             patch.object(reproduce, 'ensure_search_allowed'), \
             patch.object(reproduce, 'seed_with_recovery', side_effect=freeze), \
             patch.dict(os.environ, {}, clear=True), \
             patch.object(sys, 'argv', ['reproduce.py', '--seed', '--seed-label', 'strategy',
                                       '--english-triggers-from', 'seed0']):
            self.assertEqual(reproduce.main(), 0)
        self.assertEqual(observed, [('strategy', '1', 'seed0')])

    def test_real_shell_resume_quotes_and_forwards_candidate_flags_to_worker(self):
        import os
        import shutil
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / 'tools/trajectory_evolution/experiments/appworld/reproduce.sh'
            script.parent.mkdir(parents=True)
            shutil.copy2(EXPERIMENT / 'reproduce.sh', script)
            binaries = root / 'fixture-bin'
            binaries.mkdir()
            tmux = binaries / 'tmux'
            tmux.write_text('#!/usr/bin/env bash\ncase "$1" in\n'
                            'has-session) exit 1 ;;\n'
                            'new-session) printf "%s\\0" "${@:5}" > "$APPWORLD_FIXTURE_COMMAND" ;;\n'
                            'set-window-option) exit 0 ;;\n'
                            '*) exit 3 ;;\nesac\n')
            interpreter = binaries / 'python-fixture'
            interpreter.write_text('#!/usr/bin/env bash\nprintf "%s\\0" "$@" > "$APPWORLD_FIXTURE_ARGUMENTS"\n')
            tmux.chmod(0o755)
            interpreter.chmod(0o755)
            env = dict(os.environ, PATH=str(binaries) + os.pathsep + os.environ['PATH'],
                       AGENTFLOW_PYTHON=str(interpreter), APPWORLD_SESSION='fixture-session',
                       APPWORLD_FIXTURE_COMMAND=str(root / 'command.txt'),
                       APPWORLD_FIXTURE_ARGUMENTS=str(root / 'arguments.bin'))
            flags = ['--seed-label', 'strategy', '--english-triggers-from', 'seed0',
                     '--comparison', str(root / 'a file.json'), '--gepa-max-model-calls', '1024']
            launch = subprocess.run(['bash', str(script), '--resume', *flags],
                                    env=env, text=True, capture_output=True, timeout=5)
            self.assertEqual(launch.returncode, 0, launch.stderr)
            command = (root / 'command.txt').read_bytes().decode().split('\0')[:-1]
            self.assertEqual(command[0], 'env')
            self.assertEqual(command[3:5], ['--noprofile', '--norc'])
            worker = subprocess.run(command, env=env, text=True,
                                    capture_output=True, timeout=5)
            self.assertEqual(worker.returncode, 0, worker.stderr)
            argv = (root / 'arguments.bin').read_bytes().decode().split('\0')[:-1]
            self.assertEqual(argv, ['-B', '-u', 'tools/trajectory_evolution/experiments/appworld/reproduce.py',
                                    '--all', *flags])
            self.assertIn('appworld_controller_exit_code=0',
                          (root / 'trace_output/appworld/training/run.log').read_text())

    def test_real_shell_worker_keeps_failed_exit_and_log_without_exposing_arguments(self):
        import os
        import shutil
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / 'tools/trajectory_evolution/experiments/appworld/reproduce.sh'
            script.parent.mkdir(parents=True)
            shutil.copy2(EXPERIMENT / 'reproduce.sh', script)
            binaries = root / 'fixture-bin'
            binaries.mkdir()
            tmux = binaries / 'tmux'
            tmux.write_text('#!/usr/bin/env bash\n'
                            'printf "%s\\0" "$@" > "$APPWORLD_FIXTURE_TMUX"\n')
            interpreter = binaries / 'python-fixture'
            interpreter.write_text('#!/usr/bin/env bash\nprintf "fixture_started\\n"\nexit 7\n')
            for binary in (tmux, interpreter):
                binary.chmod(0o755)
            env = dict(os.environ, PATH=str(binaries) + os.pathsep + os.environ['PATH'],
                       AGENTFLOW_PYTHON=str(interpreter), TMUX_PANE='%fixture',
                       APPWORLD_FIXTURE_TMUX=str(root / 'tmux-arguments.bin'))
            argument = 'literal $() `never-run` with spaces'
            worker = subprocess.run(['bash', str(script), '--worker', '--all',
                                     '--seed-label', argument], env=env, text=True,
                                    capture_output=True, timeout=5)
            self.assertEqual(worker.returncode, 7, worker.stderr)
            log = (root / 'trace_output/appworld/training/run.log').read_text()
            self.assertIn('fixture_started', log)
            self.assertIn('appworld_controller_exit_code=7', log)
            self.assertNotIn(argument, log)
            self.assertEqual((root / 'tmux-arguments.bin').read_bytes().decode().split('\0')[:-1],
                             ['set-window-option', '-t', '%fixture', 'remain-on-exit', 'on'])

    def test_real_tmux_worker_survives_hangup_and_preserves_natural_failed_exit(self):
        import os
        import shlex
        import shutil
        import signal
        import time
        real_tmux = shutil.which('tmux')
        if real_tmux is None:
            self.skipTest('tmux unavailable')
        with tempfile.TemporaryDirectory(prefix='appworld-wrapper-hup-', dir='/tmp') as directory:
            root = Path(directory) / 'fixture root'
            root.mkdir()
            script = root / 'tools/trajectory_evolution/experiments/appworld/reproduce.sh'
            script.parent.mkdir(parents=True)
            shutil.copy2(EXPERIMENT / 'reproduce.sh', script)
            binaries = root / 'fixture-bin'
            binaries.mkdir()
            socket = str(root / 'tmux.socket')
            tmux = binaries / 'tmux'
            tmux.write_text('#!/usr/bin/env bash\nexec ' + shlex.quote(real_tmux) +
                            ' -f /dev/null -S "$APPWORLD_FIXTURE_SOCKET" "$@"\n')
            interpreter = binaries / 'python-fixture'
            interpreter.write_text('#!' + sys.executable + '\n'
                'import json, os, signal, time\nfrom pathlib import Path\n'
                'root = Path(os.environ["APPWORLD_FIXTURE_ROOT"])\n'
                '(root / "ready.json").write_text(json.dumps({"pid": os.getpid(), '
                '"hup_ignored": signal.getsignal(signal.SIGHUP) == signal.SIG_IGN}))\n'
                'while not (root / "finish").exists(): time.sleep(.02)\n'
                'raise SystemExit(7)\n')
            for binary in (tmux, interpreter):
                binary.chmod(0o755)
            env = dict(os.environ, PATH=str(binaries) + os.pathsep + os.environ['PATH'],
                       APPWORLD_FIXTURE_SOCKET=socket, APPWORLD_FIXTURE_ROOT=str(root),
                       AGENTFLOW_PYTHON=str(interpreter), APPWORLD_SESSION='fixture-hup')

            def isolated_tmux(*args):
                return subprocess.run([real_tmux, '-f', '/dev/null', '-S', socket, *args],
                                      text=True, capture_output=True, timeout=5)

            try:
                launch = subprocess.run(['bash', str(script), '--resume', '--seed-label', 'fixture'],
                                        env=env, text=True, capture_output=True, timeout=5)
                self.assertEqual(launch.returncode, 0, launch.stderr)
                deadline = time.monotonic() + 5
                while not (root / 'ready.json').is_file() and time.monotonic() < deadline:
                    time.sleep(.02)
                self.assertTrue((root / 'ready.json').is_file())
                ready = json.loads((root / 'ready.json').read_text())
                self.assertTrue(ready['hup_ignored'])
                worker = int(isolated_tmux('display-message', '-p', '-t', 'fixture-hup',
                                           '#{pane_pid}').stdout.strip())
                for pid in (worker, ready['pid']):
                    os.kill(pid, signal.SIGHUP)
                time.sleep(.1)
                pane = isolated_tmux('list-panes', '-t', 'fixture-hup', '-F',
                                     '#{pane_dead}:#{pane_dead_status}').stdout.strip()
                self.assertTrue(pane.startswith('0:'), pane)
                (root / 'finish').touch()
                deadline = time.monotonic() + 5
                while not pane.startswith('1:') and time.monotonic() < deadline:
                    time.sleep(.02)
                    pane = isolated_tmux('list-panes', '-t', 'fixture-hup', '-F',
                                         '#{pane_dead}:#{pane_dead_status}').stdout.strip()
                self.assertEqual(pane, '1:7')
                self.assertIn('appworld_controller_exit_code=7',
                              (root / 'trace_output/appworld/training/run.log').read_text())
                self.assertFalse((root / 'nohup.out').exists())
            finally:
                isolated_tmux('kill-server')

    def test_stage_failure_retains_fixed_appworld_code_without_exception_prose(self):
        import reproduce
        self.assertEqual(reproduce._stage_failure_reason(RuntimeError('appworld_training_not_complete')),
                         'appworld_training_not_complete')
        self.assertEqual(reproduce._stage_failure_reason(RuntimeError('appworld_error https://private/key')),
                         'appworld_stage_failed')
        self.assertEqual(reproduce._stage_failure_reason(ValueError('appworld_private_key')),
                         'appworld_stage_failed')

    def test_training_and_comparison_share_literal_projection_fields(self):
        import layout
        import training
        expected = {'evidence_field_bytes': 32768, 'keep_causal_anchors': True,
                    'keep_dispatch_bindings': True, 'keep_outer_request_inputs': True,
                    'require_dispatch_dependency': True, 'require_state_dependency': True,
                    'answer_field_bytes': 32768,
                    'keep_dispatch_bindings_before_preview': True,
                    'include_model_requests': False, 'share_repeated_inputs': True,
                    'share_repeated_text': True,
                    'evidence_projection_command': [str(layout.PYTHON), str(layout.HOST),
                                                    '--project-official-code']}
        baseline = {'level': 'L0', 'policy': None,
                    'agent': {'name': layout.AGENT, 'revision': 'fixture-revision'}}
        with patch.object(layout, 'version', return_value='fixture-version'), \
             patch.object(layout, 'ids', return_value=['fixture-train']):
            configs = [training.evaluation_config(['fixture-train'], baseline, 16),
                       layout.evaluation_config(['fixture-train'], baseline, 16),
                       layout.evaluation_config(['fixture-val'], baseline, 16)]
        self.assertEqual(layout.judge_projection(), expected)
        for config in configs:
            self.assertEqual(layout.judge_projection(config), expected)
            self.assertEqual(config['judge']['max_input_bytes'], 2097152)
        self.assertNotEqual(layout.judge_projection({'judge': {}}), expected)

    def test_projection_interpreter_is_stable_across_parent_python_aliases(self):
        import layout
        with patch.object(sys, 'executable', '/fixture/bin/python'):
            first = layout.judge_projection()
        with patch.object(sys, 'executable', '/fixture/bin/python3'):
            second = layout.judge_projection()
        self.assertEqual(first, second)
        self.assertEqual(first['evidence_projection_command'][0], str(layout.PYTHON))

    def test_retained_regrade_applies_current_projection_without_mutating_source(self):
        from copy import deepcopy
        import layout
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            report = root / 'capture/report.json'
            report.parent.mkdir()
            report.write_text(json.dumps({'trials': [{'task_id': 'fixture'}]}))
            original_bytes = report.read_bytes()
            config = {'judge': {'enabled': False, 'evidence_field_bytes': 256,
                                'answer_field_bytes': 1024, 'include_model_requests': True}}
            original_config = deepcopy(config)
            captured = []

            def native(command, **kwargs):
                captured.append(command)
                target = Path(command[command.index('--output-root') + 1])
                target.mkdir()
                (target / 'report.json').write_text(json.dumps({'complete': True, 'trials': []}))
                return subprocess.CompletedProcess(command, 0, '', '')

            with patch.object(layout.subprocess, 'run', side_effect=native):
                result = layout.regrade_report(report, root / 'dataset', config, root / 'review', env={})
            derived = json.loads((root / 'review.regrade.config.json').read_text())
            self.assertEqual(layout.judge_projection(derived), layout.judge_projection())
            self.assertEqual(config, original_config)
            self.assertEqual(report.read_bytes(), original_bytes)
            self.assertTrue(result['complete'])
            self.assertIn('--regrade-report', captured[0])
            self.assertNotIn('--resume-rerun-trial', captured[0])

    def test_existing_baseline_with_old_or_missing_projection_enters_regrade_recovery(self):
        import dataset
        import layout
        from unittest.mock import Mock
        for profile in ('current', 'old', 'missing', 'new_config_old_result', 'new_config_partial_result'):
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                l0 = layout.baseline_result(32, output_root=root)
                l0.parent.mkdir(parents=True)
                source = root / 'source.json'
                source.write_text(json.dumps({'complete': True, 'configuration': {
                    'variants': [{'id': 'candidate', 'version': 'fixture-revision'}]}}))
                published = root / 'published'
                published.mkdir()
                (published / 'L0.json').write_text(json.dumps({
                    'agent': {'revision': 'fixture-revision'}}))
                result = {'status': 'complete', 'source_report': str(source),
                          'host_version': 'fixture-host'}
                if profile in ('current', 'new_config_partial_result'):
                    result['judge_projection'] = layout.judge_projection()
                if profile == 'new_config_partial_result':
                    result['status'] = 'partial'
                l0.write_text(json.dumps(result))
                if profile != 'missing':
                    judge = layout.judge_projection() if profile in (
                        'current', 'new_config_old_result', 'new_config_partial_result') else {
                        'evidence_field_bytes': 256, 'answer_field_bytes': 1024}
                    (l0.parent / 'capture.config.json').write_text(json.dumps({'judge': judge}))
                seed = root / 'published/strategy.json'
                attempt = {'pilot_comparison': str(root / 'gepa/seed-stage32/comparison.json')}
                execute = Mock(return_value=0)
                with patch.object(reproduce, 'OUTPUT', root), \
                     patch.object(reproduce._APPWORLD_LAYOUT, 'PUBLISHED', published), \
                     patch.object(reproduce, '_host_version', return_value='fixture-host'), \
                     patch.object(reproduce, 'ensure_search_allowed'), \
                     patch.object(reproduce, 'begin_seed_attempt', return_value=attempt), \
                     patch.object(reproduce, 'verify', return_value=0), \
                     patch.object(reproduce, 'run', execute), \
                     patch.object(dataset, 'materialize', return_value={}):
                    reproduce.stage(seed, 32)
                baselines = [call for call in execute.call_args_list if '--baseline' in call.args]
                self.assertEqual(len(baselines), 0 if profile == 'current' else 1)
                self.assertEqual(len(execute.call_args_list), 1 if profile == 'current' else 2)


class OfflineGepaIsolationTests(unittest.TestCase):
    def test_all_output_aliases_are_temporary_and_escaped_save_is_rejected(self):
        production_plan = offline_gepa.PLAN
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with _isolated_gepa_outputs(root) as paths:
                self.assertEqual(set(paths), {'OUTPUT', 'GEPA', 'TRAINING', 'PUBLISHED',
                    'CONFIG', 'WORKSPACE', 'PLAN', 'STATUS', 'DATASET'})
                for name, path in paths.items():
                    self.assertEqual(getattr(offline_gepa, name), path)
                    self.assertTrue(path.is_relative_to(root))
                offline_gepa.save(offline_gepa.PLAN, {'fixture': True})
                self.assertEqual(json.loads((root / 'gepa/plan.json').read_text()), {'fixture': True})
                with self.assertRaisesRegex(AssertionError, 'outside its temporary fixture'):
                    offline_gepa.save(production_plan, {'must_not_write': True})

    def test_symlink_to_outside_fixture_cannot_receive_test_output(self):
        with tempfile.TemporaryDirectory() as directory, tempfile.TemporaryDirectory() as outside:
            root = Path(directory)
            (root / 'escaped').symlink_to(outside, target_is_directory=True)
            with _isolated_gepa_outputs(root):
                with self.assertRaisesRegex(AssertionError, 'outside its temporary fixture'):
                    offline_gepa.save(root / 'escaped/plan.json', {'must_not_write': True})
            self.assertFalse((Path(outside) / 'plan.json').exists())


class OfflineGepaRecoveryTests(unittest.TestCase):
    def test_cancelled_native_checkpoint_invalidates_only_search_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            _, seed, _, native_path, tree = self.fixture(directory, status='cancelled')
            native_before, tree_before = native_path.read_bytes(), tree.read_bytes()
            stages = {'train': {'status': 'complete'}, 'gepa_prepare': {'status': 'complete'},
                      'gepa_run': {'status': 'complete', 'data': {'callback': 'appworld.offline_gepa.run'}}}
            state = {'stage_results': stages}
            self.assertTrue(reproduce._resume_cancelled_native_gepa_receipt(state, seed, 'gepa_run'))
            self.assertEqual(stages['gepa_run']['status'], 'pending')
            self.assertEqual(stages['train']['status'], 'complete')
            self.assertEqual(stages['gepa_prepare']['status'], 'complete')
            self.assertEqual(native_before, native_path.read_bytes())
            self.assertEqual(tree_before, tree.read_bytes())

    def test_cancelled_checkpoint_receipt_keeps_other_states_and_unrelated_seed(self):
        for status in ('passed', 'rejected', 'pending', 'running'):
            with self.subTest(status=status), tempfile.TemporaryDirectory() as directory:
                _, seed, _, _, _ = self.fixture(directory, status=status)
                state = {'stage_results': {'gepa_run': {'status': 'complete'}}}
                self.assertFalse(reproduce._resume_cancelled_native_gepa_receipt(state, seed, 'gepa_run'))
                self.assertEqual(state['stage_results']['gepa_run']['status'], 'complete')
        with tempfile.TemporaryDirectory() as directory:
            _, seed, _, _, _ = self.fixture(directory, status='cancelled')
            state = {'stage_results': {'gepa_run': {'status': 'complete'}}}
            self.assertFalse(reproduce._resume_cancelled_native_gepa_receipt(state, seed, 'validation100'))
            changed = json.loads(seed.read_text()); changed['agent']['revision'] = 'changed'
            seed.write_text(json.dumps(changed))
            self.assertFalse(reproduce._resume_cancelled_native_gepa_receipt(state, seed, 'gepa_run'))

    def test_terminal_native_failures_reopen_only_matching_original_receipt(self):
        for status in ('error', 'interrupted', 'inconclusive', 'cancelled', 'superseded'):
            with self.subTest(status=status), tempfile.TemporaryDirectory() as directory:
                _, seed, _, native, tree = self.fixture(directory, status=status)
                protected = {path: path.read_bytes() for path in
                             (native, tree, offline_gepa.GEPA / 'submission.json')}
                for mismatch in ('candidate', 'job'):
                    state = {'stage_results': {'gepa_run': {'status': 'complete',
                             'data': {'job_id': 'foreign' if mismatch == 'job' else 'original'}}}}
                    if mismatch == 'candidate':
                        state['candidate'] = {'seed_label': 'foreign'}
                    self.assertFalse(reproduce._resume_cancelled_native_gepa_receipt(state, seed, 'gepa_run'))
                    self.assertEqual(state['stage_results']['gepa_run']['status'], 'complete')
                state = {'stage_results': {'gepa_run': {'status': 'complete',
                         'data': {'job_id': 'original'}}}}
                self.assertTrue(reproduce._resume_cancelled_native_gepa_receipt(state, seed, 'gepa_run'))
                self.assertEqual(state['stage_results']['gepa_run']['status'], 'pending')
                self.assertTrue(all(path.read_bytes() == body for path, body in protected.items()))

    def test_gepa_cli_pending_failure_resumes_without_proof_or_new_submission(self):
        for native_status in ('error', 'interrupted', 'inconclusive', 'cancelled',
                              'superseded', 'pending', 'running', None):
            with self.subTest(status=native_status), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                seed = root / 'published/fixture/strategy.json'
                seed.parent.mkdir(parents=True)
                seed.write_text('{}')
                identity = {'seed_label': 'fixture', 'seed_artifact': str(seed.resolve())}
                receipts = {name: {'status': 'complete'} for name in
                            ('prepare', 'train', 'seed', 'pilot32', 'validation100', 'gepa_prepare')}
                for name in ('pilot32', 'validation100'):
                    receipts[name]['gate_passed'] = True
                lifecycle = root / 'gepa/lifecycle.json'
                lifecycle.parent.mkdir()
                lifecycle.write_text(json.dumps({'candidate': identity, 'stage_results': receipts}))
                runtime = root / 'gepa/runtime'
                runtime.mkdir()
                (runtime / 'evaluation.json').write_text(json.dumps({'judge': {
                    'runtime_config': str(reproduce._APPWORLD_LAYOUT.RUNTIME / 'runtime.json')}}))
                (runtime / 'gepa.json').write_text(json.dumps({'prompt_file': str((ROOT /
                    'tools/trajectory_evolution/config/prompts/gepa-reflection.txt').resolve())}))
                native = root / 'gepa/status.json'
                active = [False]
                status = [native_status]
                calls = []
                def native_callback(script, *arguments, **kwargs):
                    self.assertEqual((script, arguments), ('offline_gepa.py',
                        ('--run', '--seed-artifact', seed)))
                    calls.append(arguments)
                    native.write_text(json.dumps({'job_id': 'original', 'job_status': status[0]}))
                    return 0
                with ExitStack() as stack:
                    for target, name, value in ((reproduce, 'OUTPUT', root),
                            (reproduce, '_CANDIDATE_STATE_LABEL', None),
                            (reproduce._APPWORLD_LAYOUT, 'GEPA', root / 'gepa'),
                            (reproduce._AppWorldDatasetAdapter, 'output_root', root)):
                        stack.enter_context(patch.object(target, name, value))
                    stack.enter_context(patch.object(reproduce, '_candidate_identity', return_value=identity))
                    stack.enter_context(patch.object(reproduce._AppWorldDatasetAdapter, 'split_counts',
                        return_value={'train': 134, 'val': 12, 'holdout': 500}))
                    stack.enter_context(patch.object(reproduce, 'seed_search', return_value={'attempts': []}))
                    stack.enter_context(patch.object(reproduce, '_experiment_status'))
                    stack.enter_context(patch.object(reproduce, 'verify', return_value=0))
                    stack.enter_context(patch.object(reproduce, '_native_gepa_service_active', side_effect=lambda: active[0]))
                    stack.enter_context(patch.object(reproduce, 'run', side_effect=native_callback))
                    stack.enter_context(patch.object(sys, 'argv', ['reproduce.py', '--gepa-run', '--seed-label', 'fixture']))
                    stack.enter_context(redirect_stdout(io.StringIO()))
                    stack.enter_context(patch.object(offline_gepa.subprocess, 'Popen', side_effect=AssertionError('no API/native launch')))
                    self.assertEqual(reproduce.main(), 1)
                    pending = json.loads(lifecycle.read_text())
                    self.assertEqual(pending['stage_results']['gepa_run']['status'], 'pending')
                    self.assertNotIn('comparison500', pending['stage_results'])
                    self.assertEqual(pending['stage_results']['gepa_run']['data']['job_id'], 'original')
                    active[0] = True
                    self.assertEqual(reproduce.main(), 1)
                    self.assertEqual(len(calls), 1)
                    active[0] = False
                    status[0] = 'rejected' if native_status == 'inconclusive' else 'passed'
                    self.assertEqual(reproduce.main(), 0)
                    self.assertEqual(len(calls), 2)
                    self.assertEqual(json.loads(lifecycle.read_text())['stage_results']['gepa_run']['status'], 'complete')
                    self.assertEqual(reproduce.main(), 0)
                    self.assertEqual(len(calls), 2)

    def test_live_gepa_service_guard_matches_only_its_real_config(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            proc = root / 'proc/123'
            proc.mkdir(parents=True)
            (proc / 'cwd').symlink_to(root)
            (proc / 'stat').write_text('123 (trajectory_evolution) S 1')
            config = root / 'gepa/runtime/service.json'
            with patch.object(reproduce._APPWORLD_LAYOUT, 'GEPA', root / 'gepa'), \
                 patch.object(reproduce, 'Path', side_effect=lambda value: root / 'proc' if value == '/proc' else Path(value)), \
                 patch.object(reproduce.os, 'getpid', return_value=456):
                for filename, expected in ((str(config), True), ('gepa/runtime/service.json', True),
                                           ('foreign/service.json', False)):
                    (proc / 'cmdline').write_bytes(b'\0'.join(value.encode() for value in
                        ('trajectory_evolution', 'serve', '--config', filename)))
                    self.assertEqual(reproduce._native_gepa_service_active(), expected)
                (proc / 'stat').write_text('123 (trajectory_evolution) Z 1')
                self.assertFalse(reproduce._native_gepa_service_active())

    def fixture(self, directory: str, *, status: str = "running"):
        from contextlib import ExitStack
        root = Path(directory)
        gepa, workspace, config = root / "gepa", root / "gepa/workspace", root / "gepa/runtime"
        published = root / "published"
        config.mkdir(parents=True)
        published.mkdir()
        artifact = {"level": "L3", "agent": {"agent_id": "fixture"}, "policy": {"options": {}}}
        seed = published / "strategy.json"
        seed.write_text(json.dumps(artifact))
        (published / "L0.json").write_text("{}")
        (config / "service.json").write_text("{}")
        state_path = workspace / "agent-evaluation/state.json"
        state_path.parent.mkdir(parents=True)
        jobs = {
            "original": {"user": offline_gepa.USER, "level": "L3", "gepa": True,
                         "status": status, "queued_ms": 1},
            "newer": {"user": offline_gepa.USER, "level": "L3", "gepa": True,
                      "status": "passed", "queued_ms": 2},
        }
        state_path.write_text(json.dumps({"jobs": jobs}))
        job_root = state_path.parent / "original"
        (job_root / "search").mkdir(parents=True)
        (job_root / "seed.json").write_text(json.dumps(artifact))
        tree = job_root / "search/tree.json"
        tree.write_text(json.dumps({"nodes": [{"status": "accepted"}, {"status": "evaluating"}],
                                    "rounds": 1, "status": "searching"}))
        (gepa / "submission.json").write_text(json.dumps({"job_id": "original"}))
        (gepa / "plan.json").write_text(json.dumps({"seed": str(seed)}))
        (gepa / "lifecycle.json").write_text(json.dumps({
            "dataset": {"split_counts": {"train": 90}},
            "stage_results": {"train": {"status": "complete", "data": {"callback": "appworld.training"}}}}))
        (gepa / "status.json").write_text(json.dumps({"status": "running", "nodes": 2,
                                                      "seed_search": {"current_seed": str(seed)}}))
        stack = ExitStack()
        stack.enter_context(_isolated_gepa_outputs(root, gepa=gepa, published=published))
        self.addCleanup(stack.close)
        return artifact, seed, jobs, state_path, tree

    def test_status_and_publication_follow_recorded_job_instead_of_newest(self):
        with tempfile.TemporaryDirectory() as directory:
            _, _, _, _, tree = self.fixture(directory)
            before = tree.read_bytes()
            result = offline_gepa.progress()
            self.assertEqual(result["job_id"], "original")
            self.assertEqual(result["nodes"], 2)
            self.assertEqual(result["rounds"], 1)
            self.assertEqual(offline_gepa.progress("newer")["job_id"], "newer")
            self.assertEqual(tree.read_bytes(), before)

    def test_same_seed_reuses_saved_object_without_manager_version_checks(self):
        from copy import deepcopy
        with tempfile.TemporaryDirectory() as directory:
            artifact, _, _, _, _ = self.fixture(directory)
            with patch.object(offline_gepa.controller, "version") as version:
                self.assertEqual(offline_gepa._recorded_job(deepcopy(artifact)), "original")
                changed = deepcopy(artifact)
                changed["policy"]["options"]["approved_experiences"] = [{"content": "changed"}]
                self.assertIsNone(offline_gepa._recorded_job(changed))
                version.assert_not_called()

    def recorded_training_fixture(self, directory):
        _, _, _, _, tree_path = self.fixture(directory)
        offline_gepa.DATASET.mkdir(parents=True)
        version = 'dataset-v1:original-frozen-train132'
        offline_gepa.save(offline_gepa.DATASET / 'manifest.json', {
            'metadata': {'dataset_version': version}, 'summary': {'splits': {'train': 132}}})
        tree = offline_gepa.read(tree_path)
        tree['dataset_version'] = version
        offline_gepa.save(tree_path, tree)
        offline_gepa.save(offline_gepa.PLAN, {'dataset': str(offline_gepa.DATASET),
            'dataset_version': version, 'split_counts': {'train': 132}})
        offline_gepa.save(offline_gepa.GEPA / 'lifecycle.json', {
            'dataset': {'split_counts': {'train': 132}},
            'stage_results': {'train': {'status': 'complete', 'data': {'callback': 'appworld.training'}}}})
        status = offline_gepa.TRAINING / 'full-training-status.json'
        status.parent.mkdir(parents=True, exist_ok=True)
        offline_gepa.save(status, {'status': 'learning_incomplete', 'training_tasks': 134})
        memory = offline_gepa.TRAINING / 'evolution/by-user' / offline_gepa.USER / 'memory/evolution-memory.json'
        memory.parent.mkdir(parents=True)
        offline_gepa.save(memory, {'entries': [{'candidate': {'status': 'EVOLUTION_CANDIDATE_STATUS_PROPOSED'}}]})
        return status, memory, tree_path

    def test_recorded_search_uses_real_frozen_train_receipt_without_marking_new_scope_complete(self):
        with tempfile.TemporaryDirectory() as directory:
            status, memory, _ = self.recorded_training_fixture(directory)
            original = status.read_bytes()
            self.assertEqual(offline_gepa._training_ready('original'), memory)
            with self.assertRaisesRegex(RuntimeError, 'appworld_training_not_complete'):
                offline_gepa._training_ready()
            self.assertEqual(status.read_bytes(), original)

    def test_recorded_search_rejects_unknown_changed_or_unfinished_training_scope(self):
        for change in ('version', 'count', 'unfinished', 'missing'):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as directory:
                status, _, tree_path = self.recorded_training_fixture(directory)
                original = status.read_bytes()
                if change == 'version':
                    value = offline_gepa.read(tree_path)
                    value['dataset_version'] = 'different'
                    offline_gepa.save(tree_path, value)
                elif change == 'count':
                    value = offline_gepa.read(offline_gepa.DATASET / 'manifest.json')
                    value['summary']['splits']['train'] = 134
                    offline_gepa.save(offline_gepa.DATASET / 'manifest.json', value)
                else:
                    value = offline_gepa.read(offline_gepa.GEPA / 'lifecycle.json')
                    if change == 'unfinished':
                        value['stage_results']['train']['status'] = 'running'
                    else:
                        value['stage_results'].pop('train')
                    offline_gepa.save(offline_gepa.GEPA / 'lifecycle.json', value)
                with self.assertRaisesRegex(RuntimeError, 'appworld_frozen_training_receipt_unavailable'):
                    offline_gepa._training_ready('original')
                self.assertEqual(status.read_bytes(), original)

    def completed_superset_fixture(self, directory):
        import hashlib
        from copy import deepcopy
        status, memory, tree = self.recorded_training_fixture(directory)
        current = offline_gepa.TRAINING / 'datasets/train'
        current.mkdir(parents=True)
        frozen_rows = [{'task_id': f'train-{index}', 'split': 'train',
                        'review_status': 'reviewed', 'task_version': f'frozen-{index}',
                        'task_input': {'instruction': f'public training {index}'},
                        'budget': {'max_steps': 300},
                        'provenance': {'kind': 'synthetic', 'source_fingerprints': ['frozen-input']}}
                       for index in range(132)]
        current_rows = deepcopy(frozen_rows)
        for row in current_rows:
            row['task_version'] = 'new-materialized-version'
            row['provenance']['source_fingerprints'] = ['expanded-input']
        current_rows.extend([dict(deepcopy(current_rows[0]), task_id=f'train-{index}')
                             for index in (132, 133)])
        def write_pack(folder, values):
            payload = ''.join(json.dumps(row) + '\n' for row in values).encode()
            (folder / 'train.jsonl').write_bytes(payload)
            manifest = offline_gepa.read(folder / 'manifest.json') if (folder / 'manifest.json').exists() else {}
            manifest.update(files={'train.jsonl': hashlib.sha256(payload).hexdigest()},
                            summary={'splits': {'train': len(values)}})
            offline_gepa.save(folder / 'manifest.json', manifest)
        write_pack(offline_gepa.DATASET, frozen_rows)
        write_pack(current, current_rows)
        offline_gepa.save(status, {'status': 'complete', 'dataset': str(current),
            'planned_tasks': 134, 'completed_tasks': 134, 'batch_size': 16,
            'planned_batches': 9, 'completed_batches': 9, 'imported_batches': 9,
            'deferred_reviews': [], 'last_learning': {'usable': True}})
        lifecycle = offline_gepa.read(offline_gepa.GEPA / 'lifecycle.json')
        lifecycle['dataset']['split_counts']['train'] = 134
        offline_gepa.save(offline_gepa.GEPA / 'lifecycle.json', lifecycle)
        return status, memory, tree, current, current_rows, write_pack

    def test_legacy_recorded_train132_uses_actual_completed_134_subset(self):
        with tempfile.TemporaryDirectory() as directory:
            status, memory, tree, current, _, _ = self.completed_superset_fixture(directory)
            protected = [status, tree, offline_gepa.PLAN, offline_gepa.DATASET / 'manifest.json',
                         current / 'manifest.json', current / 'train.jsonl']
            before = {path: path.read_bytes() for path in protected}
            with patch.object(offline_gepa.subprocess, 'Popen', side_effect=AssertionError('no service/API')):
                self.assertEqual(offline_gepa._training_ready('original'), memory)
            self.assertTrue(all(path.read_bytes() == value for path, value in before.items()))
            # Normal prepare records the current complete TRAIN receipt. Its
            # 134-task count remains honest on a later frozen-132 resume.
            plan = offline_gepa.read(offline_gepa.PLAN)
            plan['training_readiness'] = {'train_tasks': 134,
                'stage_result': {'status': 'complete', 'data': {'callback': 'appworld.training'}}}
            offline_gepa.save(offline_gepa.PLAN, plan)
            self.assertEqual(offline_gepa._training_ready('original'), memory)

    def test_completed_training_superset_rejects_missing_incomplete_and_changed_sources(self):
        for change in ('missing', 'incomplete', 'unimported', 'learning', 'dataset',
                       'duplicate', 'missing_task', 'budget', 'provenance', 'split',
                       'hash', 'frozen_version'):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as directory:
                status, _, _, current, values, write_pack = self.completed_superset_fixture(directory)
                if change == 'missing':
                    (current / 'manifest.json').unlink()
                elif change in ('incomplete', 'unimported', 'learning', 'dataset'):
                    value = offline_gepa.read(status)
                    if change == 'incomplete': value['status'] = 'running'
                    elif change == 'unimported': value['imported_batches'] = 8
                    elif change == 'learning': value['last_learning']['usable'] = False
                    else: value['dataset'] = str(current.parent / 'foreign')
                    offline_gepa.save(status, value)
                elif change == 'hash':
                    with (current / 'train.jsonl').open('a') as stream: stream.write(' ')
                elif change == 'frozen_version':
                    value = offline_gepa.read(offline_gepa.DATASET / 'manifest.json')
                    value['metadata']['dataset_version'] = 'different'
                    offline_gepa.save(offline_gepa.DATASET / 'manifest.json', value)
                else:
                    if change == 'duplicate': values[0]['task_id'] = values[1]['task_id']
                    elif change == 'missing_task': values[0]['task_id'] = 'foreign-task'
                    elif change == 'budget': values[0]['budget']['max_steps'] = 301
                    elif change == 'provenance': values[0]['provenance']['kind'] = 'foreign'
                    else: values[0]['split'] = 'holdout'
                    write_pack(current, values)
                with self.assertRaisesRegex(RuntimeError, 'appworld_frozen_training_receipt_unavailable'):
                    offline_gepa._training_ready('original')

    def test_prepare_preserves_existing_search_and_status(self):
        with tempfile.TemporaryDirectory() as directory:
            artifact, seed, _, _, tree = self.fixture(directory)
            original_tree = tree.read_bytes()
            original_submission = (offline_gepa.GEPA / "submission.json").read_bytes()
            with patch.object(offline_gepa, "require_seed_gates", return_value=artifact), \
                 patch.object(offline_gepa, "_training_ready", return_value=Path(directory) / "memory.json"), \
                 patch.object(offline_gepa, "build_dataset", return_value={"summary": {"splits": {"train": 90}}}), \
                 patch.object(offline_gepa, "_copy_training_configs"), \
                 patch.object(offline_gepa.controller, "ids", return_value=[]), \
                 patch.object(offline_gepa.controller, "evaluation_config", return_value={"variants": [], "judge": {}}):
                offline_gepa.prepare(seed)
            result = offline_gepa.read(offline_gepa.STATUS)
            self.assertEqual(result["status"], "running")
            self.assertEqual(result["job_id"], "original")
            self.assertEqual(result["nodes"], 2)
            self.assertEqual(result["seed_search"]["current_seed"], str(seed))
            self.assertEqual(tree.read_bytes(), original_tree)
            self.assertEqual((offline_gepa.GEPA / "submission.json").read_bytes(), original_submission)

    def test_resume_uses_real_local_management_http_and_never_resubmits(self):
        import http.server
        import threading
        from unittest.mock import Mock
        for status in ("pending", "running", "error", "interrupted", "inconclusive", "cancelled", "passed", "rejected"):
            with self.subTest(status=status), tempfile.TemporaryDirectory() as directory:
                artifact, seed, jobs, state_path, tree = self.fixture(directory, status=status)
                original_tree = tree.read_bytes()
                original_submission = (offline_gepa.GEPA / "submission.json").read_bytes()
                operations = []

                class Management(http.server.BaseHTTPRequestHandler):
                    def log_message(self, *args):
                        pass

                    def do_POST(self):
                        packet = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                        operations.append(packet["operation"])
                        if packet["operation"] == "agent_evaluation.status":
                            result = {"jobs": jobs}
                            body = json.dumps({"ok": True, "result": result}).encode()
                            if status in {"pending", "running"}:
                                jobs["original"]["status"] = "passed"
                                state_path.write_text(json.dumps({"jobs": jobs}))
                        elif packet["operation"] == "agent_evaluation.retry":
                            if packet.get("job_id") != "original":
                                raise AssertionError("retry changed search identity")
                            jobs["original"]["status"] = "passed"
                            state_path.write_text(json.dumps({"jobs": jobs}))
                            body = json.dumps({"ok": True, "result": {}}).encode()
                        else:
                            body = json.dumps({"ok": False, "error": "unexpected_submission"}).encode()
                        self.send_response(200)
                        self.send_header("Content-Type", "application/json")
                        self.send_header("Content-Length", str(len(body)))
                        self.end_headers()
                        self.wfile.write(body)

                server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Management)
                thread = threading.Thread(target=lambda: server.serve_forever(poll_interval=0.01), daemon=True)
                thread.start()
                process = Mock()
                process.stdout.readline.return_value = f"trajectory-integration.v1 listening=127.0.0.1:{server.server_port}\n"
                process.poll.return_value = None
                try:
                    with patch.object(offline_gepa, "require_seed_gates", return_value=artifact), \
                         patch.object(offline_gepa.controller, "environment", return_value={}), \
                         patch.object(offline_gepa.subprocess, "Popen", return_value=process) as spawn, \
                         patch.object(offline_gepa.select, "select", return_value=([process.stdout], [], [])):
                        result = offline_gepa.run(seed)
                    self.assertEqual(result["job_id"], "original")
                    self.assertEqual(result["nodes"], 2)
                    expected = ["agent_evaluation.status"]
                    if status in {"error", "interrupted", "inconclusive", "cancelled"}:
                        expected.append("agent_evaluation.retry")
                    self.assertEqual(operations, expected)
                    self.assertEqual(spawn.call_args.args[0], [str(offline_gepa.BINARY),
                        'serve', '--config', str(offline_gepa.CONFIG / 'service.json'),
                        '--no-auto-evaluate'])
                    self.assertEqual(tree.read_bytes(), original_tree)
                    self.assertEqual((offline_gepa.GEPA / "submission.json").read_bytes(), original_submission)
                finally:
                    server.shutdown()
                    server.server_close()
                    thread.join(timeout=1)


    def test_budget_prepare_keeps_recorded_train132_when_global_is134(self):
        import dataset
        with tempfile.TemporaryDirectory(dir='/tmp') as directory:
            status, _, tree = self.recorded_training_fixture(directory)
            seed = offline_gepa.PUBLISHED / 'candidate/strategy.json'
            # The fixture's original seed lives at its already-recorded path.
            seed = Path(offline_gepa.read(offline_gepa.GEPA / 'status.json')
                        ['seed_search']['current_seed'])
            artifact = offline_gepa.read(seed)
            original_tree = tree.read_bytes()
            original_status = status.read_bytes()
            original_submission = (offline_gepa.GEPA / 'submission.json').read_bytes()
            tasks = [{'task_id': f'public-train-{i}'} for i in range(134)]
            with patch.object(offline_gepa, 'require_seed_gates', return_value=artifact), \
                 patch.object(offline_gepa, '_copy_training_configs'), \
                 patch.object(offline_gepa.controller, 'ids', return_value=[]), \
                 patch.object(offline_gepa.controller, 'evaluation_config',
                              return_value={'variants': [], 'judge': {}}), \
                 patch.object(dataset, 'rows', return_value=tasks), \
                 patch.object(dataset, 'materialize', return_value={'path': str(offline_gepa.DATASET)}) as materialize, \
                 patch.object(offline_gepa.subprocess, 'Popen', side_effect=AssertionError('no service/API')):
                prepared = offline_gepa.prepare(seed, max_model_calls=1024)
            self.assertEqual(prepared['search']['max_model_calls'], 1024)
            self.assertEqual(prepared['dataset_version'], 'dataset-v1:original-frozen-train132')
            self.assertEqual(prepared['split_counts']['train'], 132)
            materialize.assert_called_once_with(offline_gepa.BINARY, view='gepa',
                frozen_training_tasks=[row['task_id'] for row in tasks[:132]],
                expected_dataset_version='dataset-v1:original-frozen-train132')
            self.assertEqual(tree.read_bytes(), original_tree)
            self.assertEqual(status.read_bytes(), original_status)
            self.assertEqual((offline_gepa.GEPA / 'submission.json').read_bytes(), original_submission)
            self.assertEqual(offline_gepa.read(offline_gepa.CONFIG / 'gepa.json')['max_model_calls'], 1024)


class GepaModelBudgetTests(unittest.TestCase):
    def test_normal_cli_forwards_explicit_optimizer_budget(self):
        for value in (1, 512, 1024):
            with self.subTest(value=value), \
                 patch.object(sys, 'argv', ['reproduce.py', '--gepa-run', '--seed-label', 'fixture',
                                           '--gepa-max-model-calls', str(value)]), \
                 patch.object(reproduce, '_experiment_status'), \
                 patch.object(reproduce, '_EXPERIMENT_TRACKING', False), \
                 patch.object(reproduce, '_EXPERIMENT_LABEL', None), \
                 patch.object(reproduce, '_run_common_all', return_value={}) as common, \
                 redirect_stdout(io.StringIO()):
                self.assertEqual(reproduce.main(), 0)
                self.assertEqual(common.call_args.kwargs,
                                 {'stop_at': 'gepa_run', 'gepa_max_model_calls': value})

    def test_normal_cli_without_budget_preserves_existing_call(self):
        with patch.object(sys, 'argv', ['reproduce.py', '--gepa-run', '--seed-label', 'fixture']), \
             patch.object(reproduce, '_experiment_status'), \
             patch.object(reproduce, '_EXPERIMENT_TRACKING', False), \
             patch.object(reproduce, '_EXPERIMENT_LABEL', None), \
             patch.object(reproduce, '_run_common_all', return_value={}) as common, \
             redirect_stdout(io.StringIO()):
            self.assertEqual(reproduce.main(), 0)
            self.assertEqual(common.call_args.kwargs, {'stop_at': 'gepa_run'})

    def test_invalid_budget_or_non_optimizer_stage_is_rejected_before_writer(self):
        from contextlib import redirect_stderr
        cases = [('--gepa-run', '0'), ('--gepa-run', '1025'),
                 ('--gepa-run', 'not-an-int'), ('--validation100', '1024')]
        for action, value in cases:
            with self.subTest(action=action, value=value), \
                 patch.object(sys, 'argv', ['reproduce.py', action, '--seed-label', 'fixture',
                                           '--gepa-max-model-calls', value]), \
                 patch.object(reproduce, '_run_common_all') as common, \
                 patch.object(reproduce, '_experiment_status') as status, \
                 redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                reproduce.main()
            self.assertEqual(error.exception.code, 2)
            common.assert_not_called()
            status.assert_not_called()

    def test_common_prepare_only_reopens_changed_explicit_budget(self):
        for current, requested, expected_calls in ((512, 1024, 1), (1024, 1024, 0), (512, None, 0)):
            with self.subTest(current=current, requested=requested), \
                 tempfile.TemporaryDirectory(dir='/tmp') as directory:
                root = Path(directory)
                seed = root / 'published/fixture/strategy.json'
                seed.parent.mkdir(parents=True)
                seed.write_text('{}')
                identity = {'seed_label': 'fixture', 'seed_artifact': str(seed.resolve())}
                receipts = {name: {'status': 'complete'} for name in
                            ('prepare', 'train', 'seed', 'pilot32', 'validation100', 'gepa_prepare')}
                for name in ('pilot32', 'validation100'):
                    receipts[name]['gate_passed'] = True
                lifecycle = root / 'gepa/lifecycle.json'
                lifecycle.parent.mkdir()
                lifecycle.write_text(json.dumps({'candidate': identity, 'stage_results': receipts}))
                runtime = root / 'gepa/runtime'
                runtime.mkdir()
                evaluation = {'judge': {'runtime_config': str(reproduce._APPWORLD_LAYOUT.RUNTIME / 'runtime.json')}}
                (runtime / 'evaluation.json').write_text(json.dumps(evaluation))
                config = {'max_model_calls': current, 'prompt_file': str((ROOT /
                    'tools/trajectory_evolution/config/prompts/gepa-reflection.txt').resolve())}
                (runtime / 'gepa.json').write_text(json.dumps(config))
                protected = (runtime / 'evaluation.json').read_bytes()
                calls = []
                def prepare_callback(script, *arguments, **kwargs):
                    self.assertEqual(script, 'offline_gepa.py')
                    self.assertEqual(arguments, ('--prepare', '--seed-artifact', seed,
                                                 '--max-model-calls', '1024'))
                    calls.append(arguments)
                    (runtime / 'gepa.json').write_text(json.dumps(dict(config, max_model_calls=1024)))
                    return 0
                with patch.object(reproduce, 'OUTPUT', root), \
                     patch.object(reproduce, '_CANDIDATE_STATE_LABEL', None), \
                     patch.object(reproduce, '_candidate_identity', return_value=identity), \
                     patch.object(reproduce._AppWorldDatasetAdapter, 'output_root', root), \
                     patch.object(reproduce._AppWorldDatasetAdapter, 'split_counts',
                                  return_value={'train': 132, 'val': 12, 'holdout': 500}), \
                     patch.object(reproduce, 'seed_search', return_value={'attempts': []}), \
                     patch.object(reproduce, '_experiment_status'), \
                     patch.object(reproduce, 'verify', return_value=0), \
                     patch.object(reproduce, 'run', side_effect=prepare_callback):
                    result = reproduce._run_common_all(seed, 'fixture', stop_at='gepa_prepare',
                                                       gepa_max_model_calls=requested)
                self.assertEqual(len(calls), expected_calls)
                self.assertEqual(result['stage_results']['gepa_prepare']['status'], 'complete')
                for name in ('prepare', 'train', 'seed', 'pilot32', 'validation100'):
                    self.assertEqual(result['stage_results'][name], receipts[name])
                self.assertEqual((runtime / 'evaluation.json').read_bytes(), protected)
                self.assertEqual(json.loads((runtime / 'gepa.json').read_text())['max_model_calls'],
                                 1024 if expected_calls else current)


class EntrypointRecoveryTests(unittest.TestCase):
    def test_seed_command_recovers_real_local_http_503_in_a_child_process(self) -> None:
        import http.server
        import os
        import shutil
        import threading
        requests = []

        class Provider(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_GET(self):
                requests.append(self.path)
                body = json.dumps({'level': 'L3', 'policy': {'fixture': True}}).encode()
                self.send_response(503 if len(requests) == 1 else 200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Provider)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory(prefix='appworld-entry-recovery-', dir='/tmp') as td:
                root = Path(td)
                here = root / 'tools/trajectory_evolution/experiments/appworld'
                here.mkdir(parents=True)
                shutil.copyfile(Path(reproduce.__file__), here / 'reproduce.py')
                (here / 'layout.py').write_text('''import json
from pathlib import Path
ROOT = Path(__file__).resolve().parents[4]
PUBLISHED = ROOT / 'trace_output/appworld/published'
def save(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value))
''')
                # Replace only external lifecycle dependencies. The command's
                # real main/recovery path and urllib exception cross a process
                # boundary; the localhost provider never sees actual evidence.
                (here / 'learning.py').write_text('''import json, os, urllib.request
def validate_seed_label(label):
    assert label == 'fixture'
def freeze_seed(controller, *, label):
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(os.environ['FIXTURE_HTTP_URL'], timeout=2) as response:
        artifact = json.loads(response.read())
    controller.save(controller.PUBLISHED / label / 'strategy.json', artifact)
    return artifact
''')
                env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1',
                           FIXTURE_HTTP_URL=f'http://127.0.0.1:{server.server_port}/fixture')
                for key in ('HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY',
                            'http_proxy', 'https_proxy', 'all_proxy'):
                    env.pop(key, None)
                result = subprocess.run([sys.executable, '-B', str(here / 'reproduce.py'),
                    '--seed', '--seed-label', 'fixture'], cwd=root, env=env,
                    capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(requests, ['/fixture', '/fixture'])
                self.assertIn('automatic_seed_recovery', result.stdout)
                self.assertTrue((root / 'trace_output/appworld/published/fixture/strategy.json').is_file())
                self.assertFalse((root / 'trace_output/appworld/training').exists())
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

    def test_temporary_seed_fault_recovers_without_training_or_capture(self) -> None:
        import learning
        import urllib.error
        artifact = {'level': 'L3', 'policy': {'proof': 'fixture'}}
        busy = urllib.error.HTTPError('https://example.invalid/v1', 503,
                                      'temporary fixture', {}, None)
        with patch.object(learning, 'freeze_seed', side_effect=[busy, artifact]) as freeze, \
             patch.object(reproduce.time, 'sleep') as sleep, \
             patch.object(reproduce, 'run', side_effect=AssertionError('no Agent/training rerun')):
            self.assertEqual(reproduce.seed_with_recovery(label='fixture'), artifact)
        self.assertEqual(freeze.call_count, 2)
        self.assertTrue(all(call.kwargs == {'label': 'fixture'} for call in freeze.call_args_list))
        sleep.assert_called_once_with(10)

    def test_seed_transport_recovery_has_a_fixed_bound(self) -> None:
        import learning
        with patch.object(learning, 'freeze_seed', side_effect=TimeoutError('fixture')) as freeze, \
             patch.object(reproduce.time, 'sleep') as sleep:
            with self.assertRaisesRegex(RuntimeError, 'seed_recovery_exhausted:model_transport_timeout'):
                reproduce.seed_with_recovery(label='fixture')
        self.assertEqual(freeze.call_count, 3)
        self.assertEqual(sleep.call_count, 2)

    def test_auth_contract_and_empty_controls_are_not_retried(self) -> None:
        import learning
        import urllib.error
        errors = [urllib.error.HTTPError('https://example.invalid/v1', 401, 'fixture', {}, None),
                  urllib.error.HTTPError('https://example.invalid/v1', 403, 'fixture', {}, None),
                  RuntimeError('appworld_seed_no_supported_control_draft'),
                  RuntimeError('native_runtime_control-compile_failed')]
        for error in errors:
            with self.subTest(error=type(error).__name__, code=getattr(error, 'code', None)), \
                 patch.object(learning, 'freeze_seed', side_effect=error) as freeze, \
                 patch.object(reproduce.time, 'sleep') as sleep:
                with self.assertRaises(type(error)) as caught:
                    reproduce.seed_with_recovery(label='fixture')
                self.assertIs(caught.exception, error)
                freeze.assert_called_once()
                sleep.assert_not_called()

    def test_training_pending_recovery_is_bounded_and_permanent_exit_is_immediate(self) -> None:
        pending = {'status': 'learning_incomplete', 'completed_tasks': 90, 'imported_batches': 5}
        with patch.object(reproduce, 'run', side_effect=[3, 0]) as run, \
             patch.object(reproduce, '_read', return_value=pending), \
             patch.object(reproduce.time, 'sleep') as sleep:
            reproduce.train_with_recovery()
        self.assertEqual(run.call_count, 2)
        sleep.assert_called_once_with(10)
        with patch.object(reproduce, 'run', return_value=3) as run, \
             patch.object(reproduce, '_read', return_value=pending), \
             patch.object(reproduce.time, 'sleep') as sleep:
            with self.assertRaisesRegex(RuntimeError, 'training_recovery_exhausted'):
                reproduce.train_with_recovery()
        self.assertEqual(run.call_count, 3)
        self.assertEqual(sleep.call_count, 2)
        with patch.object(reproduce, 'run', return_value=1) as run, \
             patch.object(reproduce, '_read', return_value={'status': 'failed'}), \
             patch.object(reproduce.time, 'sleep') as sleep:
            with self.assertRaisesRegex(RuntimeError, 'training.py_failed:1'):
                reproduce.train_with_recovery()
        run.assert_called_once()
        sleep.assert_not_called()


class OfflineGepaExternalPreparationTests(unittest.TestCase):
    def test_native_regrade_config_uses_closest_actual_source(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as td:
            stage = Path(td) / "l0-current-stage500"
            source = stage / "regrade/retry-01/report.json"
            source.parent.mkdir(parents=True)
            offline_gepa.save(stage / "result.json", {"status": "complete", "source_report": str(source)})
            offline_gepa.save(stage / "capture.config.json", {"judge": {"keep_outer_request_inputs": False}})
            ancestor = stage / "regrade.regrade.config.json"
            nearest = stage / "regrade/retry-01.regrade.config.json"
            offline_gepa.save(ancestor, {"judge": {"keep_outer_request_inputs": True}})
            self.assertEqual(offline_gepa._baseline_evaluation_file(stage), ancestor)
            offline_gepa.save(nearest, {"judge": {"keep_outer_request_inputs": True, "max_model_calls": 4000}})
            self.assertEqual(offline_gepa._baseline_evaluation_file(stage), nearest)

    def test_external_source_cannot_redirect_configuration_outside_its_stage(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as td:
            stage = Path(td) / "l0-current-stage500"
            offline_gepa.save(stage / "result.json", {"source_report": str(Path(td) / "other/report.json")})
            with self.assertRaisesRegex(RuntimeError, "appworld_external_baseline_source_outside_stage"):
                offline_gepa._baseline_evaluation_file(stage)

    def prepare_fixture(self, root, *, existing_source):
        from contextlib import ExitStack
        from copy import deepcopy
        app = offline_gepa.controller
        gepa, training, published = [root / name for name in ("gepa", "training", "published")]
        config = gepa / "runtime"
        runtime = training / "runtime"
        for path in (config, runtime, published):
            path.mkdir(parents=True)
        native_source = ROOT / "trace_output/appworld/gepa/l0-current-stage500/capture.config.json"
        real_l0 = ROOT / "trace_output/appworld/published/L0.json"
        l0 = json.loads(real_l0.read_text()) if real_l0.exists() else {
            "schema_version": 1, "agent": {"revision": app.HOST_VERSION, "name": app.AGENT},
            "user_id": app.OWNER, "level": "L0", "policy": None}
        (published / "L0.json").write_text(json.dumps(l0))
        real_runtime = ROOT / "trace_output/appworld/training/runtime/runtime.json"
        if real_runtime.exists():
            (runtime / "runtime.json").write_bytes(real_runtime.read_bytes())
        else:
            offline_gepa.save(runtime / "runtime.json", {"schema_version": 1, "model": {"fixture": True}})
        offline_gepa.save(runtime / "service.json", {"schema_version": 1, "instrumentation": {}})
        with patch.object(app, "version", return_value="fixture-l0-version"):
            source = (json.loads(native_source.read_text()) if native_source.exists()
                      else app.evaluation_config(["fixture-public-task"], l0, 16))
        source.update(splits=["holdout"], max_trials=500)
        # The copied production config may have been written by a different
        # Python executable. This isolated source fixture and prepare() must
        # describe the same current judge command under the test interpreter.
        source["judge"].update(app.judge_projection())
        source["judge"]["max_model_calls"] = 4000
        capture_config = gepa / "l0-current-stage500/capture.config.json"
        if existing_source:
            capture_config.parent.mkdir(parents=True)
            capture_config.write_text(json.dumps(source))
            source_dataset = training / "datasets/comparison500"
            source_dataset.mkdir(parents=True)
            (source_dataset / "manifest.json").write_text('{"fixture":true}')
        seed = deepcopy(l0)
        seed.update(level="L3", policy={})
        seed["agent"]["appworld_options"] = {"l1_window_mode": "observed_phase_v3"}
        with ExitStack() as stack:
            stack.enter_context(_isolated_gepa_outputs(root, gepa=gepa,
                                                       training=training, published=published))
            stack.enter_context(patch.object(app, "RUNTIME", runtime))
            stack.enter_context(patch.object(app, "version", return_value="fixture-l0-version"))
            stack.enter_context(patch.object(app, "ids", return_value=["fixture-public-task"]))
            stack.enter_context(patch.object(offline_gepa, "require_seed_gates", return_value=seed))
            stack.enter_context(patch.object(offline_gepa, "_recorded_job", return_value=None))
            stack.enter_context(patch.object(offline_gepa, "_training_ready", return_value=training / "memory.json"))
            stack.enter_context(patch.object(offline_gepa, "build_dataset", return_value={"metadata": {}, "summary": {}}))
            process = stack.enter_context(patch.object(offline_gepa.subprocess, "Popen"))
            offline_gepa.prepare(root / "seed.json", max_rounds=1, max_model_calls=1)
            process.assert_not_called()
        return source, json.loads((config / "evaluation.json").read_text()), json.loads((config / "gepa.json").read_text())

    def test_pending_native_source_opts_into_reference_and_preserves_actual_execution(self):
        from copy import deepcopy
        with tempfile.TemporaryDirectory(dir="/tmp") as td:
            root = Path(td)
            source, prepared, config = self.prepare_fixture(root, existing_source=True)
            for key in ("holdout_baseline_result_file", "holdout_baseline_dataset", "holdout_baseline_evaluation_file"):
                self.assertIn(key, config)
            self.assertFalse(Path(config["holdout_baseline_result_file"]).exists())
            self.assertEqual(source["resources"], prepared["resources"])
            for left, right in [(deepcopy(source["variants"][0]), deepcopy(prepared["variants"][0]))]:
                for key in ("id", "version", "artifact", "instrumentation_config"):
                    left.pop(key, None); right.pop(key, None)
                self.assertEqual(left, right)
            left, right = deepcopy(source), deepcopy(prepared)
            for key in ("task_ids", "splits", "variants", "max_trials", "max_report_bytes",
                        "max_jsonl_bytes", "max_markdown_bytes", "artifact_retention", "judge"):
                left.pop(key, None); right.pop(key, None)
            self.assertEqual(left, right)
            left, right = deepcopy(source["judge"]), deepcopy(prepared["judge"])
            for key in ("runtime_config", "max_model_calls", "keep_dispatch_bindings_before_preview"):
                left.pop(key, None); right.pop(key, None)
            self.assertEqual(left, right)
            self.assertEqual(prepared['judge']['runtime_config'],
                             str(root / 'training/runtime/runtime.json'))
            self.assertEqual(config['prompt_file'], str((ROOT /
                'tools/trajectory_evolution/config/prompts/gepa-reflection.txt').resolve()))
            self.assertFalse(Path(config['prompt_file']).is_symlink())
            self.assertEqual(json.loads((root / "training/runtime/runtime.json").read_text()),
                             json.loads((root / "gepa/runtime/runtime.json").read_text()))

    def test_fresh_prepare_retains_native_baseline_flow_without_external_source(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as td:
            _, _, config = self.prepare_fixture(Path(td), existing_source=False)
            self.assertFalse(any(key.startswith("holdout_baseline_") for key in config))

    def test_prepare_uses_three_metric_search_contract_and_reports_resources(self):
        with tempfile.TemporaryDirectory(dir='/tmp') as td:
            root = Path(td)
            _, _, config = self.prepare_fixture(root, existing_source=False)
            agent_gate = json.loads((root/'gepa/runtime/agent-evaluation.json').read_text())['gate']
            self.assertTrue(agent_gate['resources_report_only'])
            self.assertEqual(agent_gate['min_invalid_loop_reduction'], .15)
            self.assertFalse(any(key in agent_gate for key in
                ('max_latency_ratio', 'max_token_ratio', 'max_llm_call_ratio')))
            self.assertEqual(config['objective']['token_weight'], 0)
            self.assertEqual(config['objective']['latency_weight'], 0)
            self.assertEqual(config['objective']['invalid_loop_weight'], .2)



class PilotCaptureExpansionTests(unittest.TestCase):
    def fixture(self, root):
        source, target = root / 'pilot', root / 'validation'
        directory = source / 'shards/shard-00'
        directory.mkdir(parents=True)
        seed = root / 'strategy.json'
        artifact = {'agent': {'revision': 'frozen'}}
        seed.write_text(json.dumps(artifact))
        ids = [f'task-{i:03d}' for i in range(100)]
        rows = [{'task_id': task} for task in ids[:32]]
        (source / 'comparison.json').write_text(json.dumps({
            'status': 'complete', 'host_version': 'host-current', 'items': rows}))
        (source / 'progress.json').write_text(json.dumps({
            'planned_tasks': 32, 'artifact': artifact, 'items': {row['task_id']: row for row in rows}}))
        checkpoint = source / 'shards/shard-00.json'
        checkpoint.write_text(json.dumps({'artifact': artifact, 'complete': True,
                                         'judge_projection': reproduce._APPWORLD_LAYOUT.judge_projection(),
                                         'task_ids': ids[:32], 'items': rows}))
        (directory / 'source-evidence.json').write_text('{"original":true}')
        return seed, {'pilot_comparison': str(source / 'comparison.json'),
                      'validation_comparison': str(target / 'comparison.json')}, ids

    def test_expansion_retains_original_evidence_without_copying(self):
        with tempfile.TemporaryDirectory() as td, patch.object(reproduce, '_host_version', return_value='host-current'):
            seed, attempt, ids = self.fixture(Path(td))
            self.assertTrue(reproduce._inherit_pilot_capture(seed, attempt, ids))
            target = Path(attempt['validation_comparison']).parent
            source = Path(attempt['pilot_comparison']).parent
            self.assertTrue((target / 'shards/shard-00').is_symlink())
            self.assertEqual((target / 'shards/shard-00').resolve(), (source / 'shards/shard-00').resolve())
            self.assertEqual(len(json.loads((source / 'comparison.json').read_text())['items']), 32)
            self.assertFalse(reproduce._inherit_pilot_capture(seed, attempt, ids))

    def test_changed_strategy_cannot_inherit_execution(self):
        with tempfile.TemporaryDirectory() as td, patch.object(reproduce, '_host_version', return_value='host-current'):
            seed, attempt, ids = self.fixture(Path(td))
            seed.write_text('{"agent":{"revision":"different"}}')
            self.assertFalse(reproduce._inherit_pilot_capture(seed, attempt, ids))
            self.assertFalse(Path(attempt['validation_comparison']).parent.exists())

    def test_different_task_scope_cannot_inherit_execution(self):
        with tempfile.TemporaryDirectory() as td, patch.object(reproduce, '_host_version', return_value='host-current'):
            seed, attempt, ids = self.fixture(Path(td))
            ids[0] = 'another-task'
            self.assertFalse(reproduce._inherit_pilot_capture(seed, attempt, ids))
            self.assertFalse(Path(attempt['validation_comparison']).parent.exists())

    def test_old_judge_profile_requires_pilot_regrade_first(self):
        with tempfile.TemporaryDirectory() as td, patch.object(reproduce, '_host_version', return_value='host-current'):
            seed, attempt, ids = self.fixture(Path(td))
            checkpoint = Path(attempt['pilot_comparison']).parent / 'shards/shard-00.json'
            saved = json.loads(checkpoint.read_text())
            saved['judge_projection'] = {'old': True}
            checkpoint.write_text(json.dumps(saved))
            self.assertFalse(reproduce._inherit_pilot_capture(seed, attempt, ids))
            self.assertFalse(Path(attempt['validation_comparison']).parent.exists())


class FinalBaselineSourceTests(unittest.TestCase):
    def fixture(self, root):
        report = root / 'capture/report.json'
        result = root / 'result.json'
        original = {'task_id': 'task', 'trial_id': 'trial', 'verdict': 'passed',
                    'tokens': None, 'tokens_complete': False, 'known_agent_tokens': 123,
                    'execution_costs_complete': False, 'execution_attempts': 2,
                    'latency_ms': 321.5, 'l1_applied': 0, 'l2_applied': 0, 'l3_applied': 0}
        result.write_text(json.dumps({'status': 'complete', 'source_report': str(report),
                                     'judge_projection': offline_gepa.controller.judge_projection(),
                                     'items': [original]}))
        row = {'task_id': 'task', 'trial_id': 'trial', 'verdict': 'passed',
               'evidence': {'source_reference': {'report': str(report), 'trial_id': 'trial',
                            'variant_id': 'candidate', 'full_execution_evidence': 'original_source'}}}
        return result, original, row

    def test_retained_receipt_preserves_actual_levels_and_unknown_attempt_cost(self):
        from unittest.mock import Mock
        with tempfile.TemporaryDirectory(dir='/tmp') as td:
            result, original, row = self.fixture(Path(td))
            before = result.read_bytes()
            comparator = Mock()
            items = offline_gepa._final_baseline_items([row], {'summary': {
                'baseline_source': {'result_file': str(result)}}}, comparator)
            self.assertEqual({k: v for k, v in items[0].items() if k != 'source_shard'}, original)
            self.assertFalse(items[0]['tokens_complete'])
            self.assertEqual(result.read_bytes(), before)
            comparator.compact_row.assert_not_called()

    def test_changed_trial_reference_cannot_be_used_as_baseline(self):
        from unittest.mock import Mock
        with tempfile.TemporaryDirectory(dir='/tmp') as td:
            result, _, row = self.fixture(Path(td))
            row['evidence']['source_reference']['trial_id'] = 'different'
            with self.assertRaisesRegex(RuntimeError, 'source_mismatch'):
                offline_gepa._final_baseline_items([row], {'summary': {
                    'baseline_source': {'result_file': str(result)}}}, Mock())

    def test_regular_native_baseline_uses_existing_compact_reader(self):
        from unittest.mock import Mock
        comparator = Mock()
        comparator.compact_row.return_value = {'task_id': 'task', 'l1_applied': 0}
        row = {'task_id': 'task'}
        self.assertEqual(offline_gepa._final_baseline_items([row], {}, comparator),
                         [{'task_id': 'task', 'l1_applied': 0}])
        comparator.compact_row.assert_called_once_with(row, 'native-final-L0')

    def test_old_judge_rules_cannot_be_published_with_new_candidate(self):
        from unittest.mock import Mock
        with tempfile.TemporaryDirectory(dir='/tmp') as td:
            result, _, row = self.fixture(Path(td))
            value = json.loads(result.read_text())
            value['judge_projection']['require_dispatch_dependency'] = False
            result.write_text(json.dumps(value))
            with self.assertRaisesRegex(RuntimeError, 'source_judge_changed'):
                offline_gepa._final_baseline_items([row], {'summary': {
                    'baseline_source': {'result_file': str(result)}}}, Mock())

    def test_preview_losing_dispatch_bindings_cannot_be_used_for_new_comparison(self):
        from unittest.mock import Mock
        with tempfile.TemporaryDirectory(dir='/tmp') as td:
            result, _, row = self.fixture(Path(td))
            value = json.loads(result.read_text())
            value['judge_projection'].pop('keep_dispatch_bindings_before_preview')
            result.write_text(json.dumps(value))
            with self.assertRaisesRegex(RuntimeError, 'source_judge_changed'):
                offline_gepa._final_baseline_items([row], {'summary': {
                    'baseline_source': {'result_file': str(result)}}}, Mock())


class StageTransportRecoveryTests(unittest.TestCase):
    def test_attached_healthy_native_cache_uses_one_normal_finalize_without_backoff(self):
        with patch.object(reproduce, 'run', return_value=0) as run, \
             patch.object(reproduce, '_read', side_effect=[{'status': 'partial'}, {'status': 'complete'}]), \
             patch.object(reproduce, '_stage_review_recovery_state', return_value='cached'), \
             patch.object(reproduce.time, 'sleep') as sleep:
            reproduce._stage_compare_with_recovery(('--workers', 16), Path('/unused'), 500,
                                                   previous_invocations=1)
        run.assert_called_once_with('compare.py', '--workers', 16, check=False)
        sleep.assert_not_called()

    def test_native_cache_guard_rejects_wrong_scope_cost_profile_and_manifest(self):
        import copy
        import math
        import compare
        import training
        for changed in ('none', 'scope', 'identity', 'cost', 'profile', 'manifest', 'execution'):
            with self.subTest(changed=changed), tempfile.TemporaryDirectory() as td:
                root = Path(td); shard = root / 'shards/shard-00'
                source = shard / 'capture/report.json'; candidate = shard / 'regrade/report.json'
                source.parent.mkdir(parents=True); candidate.parent.mkdir()
                row = {'task_id': 'fixture', 'trial_id': 'fixture-trial', 'execution_status': 'completed',
                       'duration_ms': 12, 'metrics': {'calls': 2}, 'variant_id': 'candidate',
                       'evidence': {'output': {'quality_score': 0.9166666666666665},
                           'trace': {'events': [{'kind': 'run.start'}, {'kind': 'run.end'}]},
                           'loop_review': {'status': 'missing'}}}
                original = {'complete': True, 'run_id': 'source-run', 'dataset_version': 'fixture-dataset', 'trials': [row]}
                reviewed = copy.deepcopy(original); reviewed['run_id'] = 'review-run'
                reviewed['trials'][0]['evidence']['loop_review']['status'] = 'complete'
                reviewed['trials'][0]['evidence']['output']['quality_score'] = math.nextafter(0.9166666666666665, 0.0)
                if changed == 'scope': reviewed['trials'][0]['task_id'] = 'wrong-task'
                if changed == 'identity': reviewed['trials'][0]['trial_id'] = 'wrong-trial'
                if changed == 'cost': reviewed['trials'][0]['metrics']['calls'] = 3
                if changed == 'execution': row['execution_status'] = 'error'
                source.write_text(json.dumps(original)); candidate.write_text(json.dumps(reviewed))
                for p, report in ((source, original), (candidate, reviewed)):
                    p.with_name('manifest.json').write_text(json.dumps({'complete': True,
                        'run_id': report['run_id'], 'dataset_version': report['dataset_version'],
                        'files': {'report.json': 'existing-native-binding'}}))
                if changed == 'manifest': candidate.with_name('manifest.json').unlink()
                current = reproduce._APPWORLD_LAYOUT.judge_projection()
                (shard / 'config.json').write_text(json.dumps({'judge': current}))
                projection = {} if changed == 'profile' else current
                (shard / 'regrade.regrade.config.json').write_text(json.dumps({'judge': projection}))
                (root / 'progress.json').write_text(json.dumps({'planned_tasks': 1,
                    'shards': {'0': {'execution_complete': True, 'source_report': str(source)}}}))
                with patch.object(compare.c, 'framework_capture_complete', return_value=True), \
                     patch.object(compare.c, '_execution_error_code', side_effect=lambda r: 'execution_error' if r.get('execution_status') == 'error' else None), \
                     patch.object(compare, 'replay_reasons', side_effect=lambda r, **kw: [] if r['evidence']['loop_review']['status'] == 'complete' else ['judge_missing']), \
                     patch.object(training, 'review_complete', side_effect=lambda r: r['evidence']['loop_review']['status'] == 'complete'), \
                     patch.object(training, 'capture_recovery_required', return_value=False):
                    result = reproduce._stage_review_recovery_state(root, 1, baseline=False)
                self.assertEqual(result, 'cached' if changed == 'none' else None)

    def test_transport_failure_retries_same_entry_with_bounded_backoff(self):
        with patch.object(reproduce, 'run', side_effect=[1, 0]) as run, \
             patch.object(reproduce, '_read', return_value={}), \
             patch.object(reproduce, '_stage_review_recovery_state', return_value='transport'), \
             patch.object(reproduce.time, 'sleep') as sleep:
            reproduce._stage_compare_with_recovery(('--baseline',), Path('/unused'), 100, baseline=True)
        self.assertEqual(run.call_count, 2)
        self.assertEqual(run.call_args_list[0], run.call_args_list[1])
        sleep.assert_called_once_with(30)

    def test_persistent_transport_failure_stops_at_three_invocations(self):
        with patch.object(reproduce, 'run', return_value=1) as run, \
             patch.object(reproduce, '_read', return_value={}), \
             patch.object(reproduce, '_stage_review_recovery_state', return_value='transport'), \
             patch.object(reproduce.time, 'sleep') as sleep:
            with self.assertRaisesRegex(RuntimeError, 'compare.py_failed:1'):
                reproduce._stage_compare_with_recovery((), Path('/unused'), 32)
        self.assertEqual(run.call_count, 3)
        self.assertEqual(sleep.call_count, 2)

    def test_execution_or_profile_failure_is_not_retried(self):
        with patch.object(reproduce, 'run', return_value=1) as run, \
             patch.object(reproduce, '_read', return_value={}), \
             patch.object(reproduce, '_stage_review_recovery_state', return_value=None), \
             patch.object(reproduce.time, 'sleep') as sleep:
            with self.assertRaisesRegex(RuntimeError, 'compare.py_failed:1'):
                reproduce._stage_compare_with_recovery((), Path('/unused'), 32)
        run.assert_called_once()
        sleep.assert_not_called()

    def test_complete_core_failure_does_not_trigger_judge_retries(self):
        with patch.object(reproduce, 'run', return_value=0) as run, \
             patch.object(reproduce, '_read', return_value={'status': 'complete', 'goal_passed': False}), \
             patch.object(reproduce, '_stage_review_recovery_state') as guard, \
             patch.object(reproduce.time, 'sleep') as sleep:
            reproduce._stage_compare_with_recovery((), Path('/unused'), 32)
        run.assert_called_once()
        guard.assert_not_called()
        sleep.assert_not_called()

    def test_awaiting_l0_waits_then_uses_normal_finalize(self):
        values = [{'baseline_status': 'awaiting_l0'}, {'status': 'complete'}]
        with patch.object(reproduce, 'run', return_value=0) as run, \
             patch.object(reproduce, '_read', side_effect=values), \
             patch.object(reproduce, '_stage_review_recovery_state', return_value='ready'), \
             patch.object(reproduce, '_stage_baseline_ready', side_effect=[False, True, True]), \
             patch.object(reproduce.time, 'sleep') as sleep:
            reproduce._stage_compare_with_recovery((), Path('/unused'), 32, l0=Path('/l0'))
        self.assertEqual(run.call_count, 2)
        sleep.assert_called_once_with(30)

    def test_attached_controller_counts_prior_invocation_and_rejects_unsafe_recovery(self):
        with patch.object(reproduce, 'run', return_value=1) as run, \
             patch.object(reproduce, '_read', return_value={}), \
             patch.object(reproduce, '_stage_review_recovery_state', return_value='transport'), \
             patch.object(reproduce.time, 'sleep') as sleep:
            with self.assertRaisesRegex(RuntimeError, 'compare.py_failed:1'):
                reproduce._stage_compare_with_recovery((), Path('/unused'), 500,
                                                       previous_invocations=1)
        self.assertEqual(run.call_count, 2)
        self.assertEqual(sleep.call_count, 2)
        for value in ({}, {'status': 'complete', 'goal_passed': False}):
            with self.subTest(value=value), \
                 patch.object(reproduce, 'run') as run, \
                 patch.object(reproduce, '_read', return_value=value), \
                 patch.object(reproduce, '_stage_review_recovery_state', return_value=None), \
                 patch.object(reproduce.time, 'sleep') as sleep:
                if value:
                    reproduce._stage_compare_with_recovery((), Path('/unused'), 500,
                                                           previous_invocations=1)
                else:
                    with self.assertRaisesRegex(RuntimeError, 'recovery_not_transport'):
                        reproduce._stage_compare_with_recovery((), Path('/unused'), 500,
                                                               previous_invocations=1)
                run.assert_not_called()
                sleep.assert_not_called()

    def test_attached_awaiting_l0_only_waits_and_normally_finalizes(self):
        with patch.object(reproduce, 'run', return_value=0) as run, \
             patch.object(reproduce, '_read', side_effect=[{'baseline_status': 'awaiting_l0'},
                                                         {'status': 'complete'}]), \
             patch.object(reproduce, '_stage_review_recovery_state', return_value='ready'), \
             patch.object(reproduce, '_stage_baseline_ready', side_effect=[False, True, True]), \
             patch.object(reproduce.time, 'sleep') as sleep:
            reproduce._stage_compare_with_recovery(('--workers', 16), Path('/unused'), 500,
                                                   l0=Path('/l0'), previous_invocations=1)
        run.assert_called_once_with('compare.py', '--workers', 16, check=False)
        sleep.assert_called_once_with(30)

    def test_native_guard_requires_current_profile_complete_capture_and_transport(self):
        import compare
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            folder = root / 'regrade/retry-01'
            folder.mkdir(parents=True)
            source = folder / 'report.json'
            row = {'task_id': 'fixture', 'checks': [{'check_id': 'model',
                   'reason': 'evaluation_judge_http_429'}]}
            source.write_text(json.dumps({'complete': True, 'trials': [row]}))
            source.with_name('manifest.json').write_text(json.dumps({'complete': True}))
            config = folder.with_name(folder.name + '.regrade.config.json')
            config.write_text(json.dumps({'judge': reproduce._APPWORLD_LAYOUT.judge_projection()}))
            with patch.object(compare.c, 'framework_capture_complete', return_value=True) as capture, \
                 patch.object(compare.c, '_execution_error_code', return_value=None), \
                 patch.object(compare, 'replay_reasons', return_value=['judge_error']):
                self.assertEqual(reproduce._stage_review_recovery_state(root, 1, baseline=True), 'transport')
                capture.return_value = False
                self.assertIsNone(reproduce._stage_review_recovery_state(root, 1, baseline=True))
                capture.return_value = True
                row['checks'][0]['reason'] = 'invalid_json'
                source.write_text(json.dumps({'complete': True, 'trials': [row]}))
                self.assertIsNone(reproduce._stage_review_recovery_state(root, 1, baseline=True))
                config.write_text(json.dumps({'judge': {'require_state_dependency': False}}))
                self.assertIsNone(reproduce._stage_review_recovery_state(root, 1, baseline=True))


if __name__ == "__main__":
    unittest.main()
