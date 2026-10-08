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
        stack.enter_context(patch.object(reproduce, 'OUTPUT', output))
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


def _candidate_checkpoint(comparison: Path, artifact: dict, items: list) -> dict:
    # Binding a candidate must preserve the already completed native grading
    # source produced by _candidate, alongside its exact artifact/task rows.
    progress = json.loads(comparison.with_name(comparison.stem + '-progress.json').read_text())
    return dict(progress, artifact=artifact, total_parallelism=16, workers_per_shard=16,
                items={row['task_id']: row for row in items})


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
            seed = output / 'published/current/strategy.json'
            seed.parent.mkdir(parents=True)
            seed.write_text(json.dumps({'level': 'L3', 'policy': {'candidate': 5}}))
            search['current_seed'] = str(seed.resolve())
            for attempt in search['attempts']:
                attempt['seed'] = str(seed.resolve())
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

    def test_five_distinct_failed_seeds_retain_count_and_allow_sixth(self) -> None:
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
                    progress.write_text(json.dumps(_candidate_checkpoint(comparison, artifact, value['items'])))
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
                self.assertEqual(state['status'], 'searching')
                seed.write_text(json.dumps({'level': 'L3', 'policy': {'candidate': 5}}))
                self.assertEqual(reproduce.begin_seed_attempt(seed)['attempt'], 6)
                self.assertEqual(len(reproduce.seed_search()['attempts']), 6)
                self.assertEqual(reproduce.seed_search()['consecutive_failures'], 5)
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
                comparison.with_name('progress.json').write_text(json.dumps(
                    _candidate_checkpoint(comparison, artifact, value['items'])))
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
                    progress.write_text(json.dumps(_candidate_checkpoint(comparison, artifact, value['items'])))
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
            checkpoint = _candidate_checkpoint(comparison, artifact, value['items'])
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
    def native_publish_fixture(self, root, count=3, *, job='fixture-job', correct=210, loops=1):
        import compare
        import hashlib
        gepa, published = root / 'gepa', root / 'published'
        dataset = root / 'training/datasets/comparison500'
        search = gepa / 'workspace/agent-evaluation' / job / 'search'
        versions = search / 'versions'
        versions.mkdir(parents=True)
        dataset.mkdir(parents=True, exist_ok=True)
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
        def native_rows(variant, version=None):
            baseline = variant == 'baseline'
            passed = 200 if baseline else correct
            tokens, calls, duration, waste = (10, 2, 100, 2) if baseline else (9, 1, 90, loops)
            return [{'task_id': f'task-{i:03d}', 'trial_id': f'{variant}-{i}',
                     'task_version': f'fixture-task-version-{i}',
                     'split': 'holdout', 'variant_id': variant, 'execution_status': 'completed',
                     'duration_ms': duration, 'verdict': 'passed' if i < passed else 'failed',
                     'metrics': {'tokens': tokens, 'llm_calls': calls},
                     'evidence': {'loaded_version': version or 'baseline-version', 'output': {'final': {
                         'appworld_reward': int(i < passed), 'strict_passed': i < passed,
                         'quality_score': int(i < passed), 'tokens_complete': True,
                         'tokens': tokens, 'model_calls': calls, 'latency_ms': 9999,
                         'framework_capture': {'status': 'complete'}}},
                         'assessment': {'version': 'goal-assessment-v1', 'quality': int(i < passed),
                                        'quality_upper': int(i < passed), 'coverage': 1, 'critical_failures': 0},
                         'loop_review': {'status': 'complete', 'count': int(waste) +
                             int(i < round((waste - int(waste)) * 500)), 'observed_iterations': 3},
                         'trace': {'identity': {'run_id': f'{variant}-{i}'},
                                   'attributes': {'capture_complete': True},
                                   'events': [{'kind': 'run.start'}, {'kind': 'run.end'}]}}} for i in range(500)]
        baseline = native_rows('baseline')
        baseline_source = gepa / 'l0-current-stage500/capture' / job / 'report.json'
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
             patch.object(offline_gepa, 'progress', return_value={'job_id': job, 'job_status': 'passed'}), \
             patch.object(reproduce._APPWORLD_LAYOUT, 'version', side_effect=lambda a: a['agent']['revision']), \
             patch.object(offline_gepa.controller, 'version', side_effect=lambda a: a['agent']['revision']):
            context = compare._native_gepa_evaluation_context(config_path, dataset_version)
            tree = {'status': 'completed', 'dataset_version': dataset_version,
                    'baseline_version': 'baseline-version',
                    'evaluation_context': context, 'candidate_evaluation_context': 'frozen-execution',
                    'nodes': [], 'final_holdout': {'status': 'completed', 'requested_top_k': 3,
                                                 'candidates': []}, 'evaluations': {}}
            reports = []
            for node in range(1, count + 1):
                version = f'version-{node}' if job == 'fixture-job' else f'{job}-version-{node}'
                report = search / f'evaluations/eval-{node}/attempt-1/report.json'
                report.parent.mkdir(parents=True)
                run_id = f'evaluation-{node}'
                report.write_text(json.dumps({'complete': True, 'run_id': run_id,
                    'dataset_version': dataset_version,
                    'configuration': {'allow_unreviewed': False, 'model_enabled': True,
                                      'judge_fingerprint': 'frozen-judge',
                                      'variants': [{'id': 'baseline', 'version': 'baseline-version'},
                                                   {'id': 'candidate', 'version': version}]},
                    'trials': baseline + native_rows('candidate', version)}))
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

    def test_deleted_native_history_allows_only_verified_better_new_result(self):
        import shutil
        for case in ('better', 'worse', 'scope-missing', 'new-source-invalid'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                with self.native_publish_fixture(root, count=1, job='old',
                        correct=230 if case == 'worse' else 210) as old:
                    published = offline_gepa.publish()
                    target = old['published'] / 'strategy'
                    if case == 'scope-missing':
                        manifest = json.loads((target / 'manifest.json').read_text())
                        manifest.pop('dataset_version')
                        (target / 'manifest.json').write_text(json.dumps(manifest))
                    original = {path.name: path.read_text() for path in target.iterdir()}
                shutil.rmtree(root / 'gepa')  # Only this closed synthetic fixture.
                with self.native_publish_fixture(root, count=1, job='new',
                        correct=220 if case == 'worse' else 230, loops=1.2) as new:
                    if case == 'new-source-invalid':
                        new['reports'][0].write_text('{}')
                        with self.assertRaisesRegex(RuntimeError, 'native_report_missing'):
                            offline_gepa.publish()
                        result = None
                    else:
                        result = offline_gepa.publish()
                    if case == 'better':
                        self.assertEqual(result['version'], 'new-version-1', result.get('publication_reason'))
                        self.assertTrue(result['goal_passed'])
                        self.assertEqual(result['goal_checks']['candidate']['strict_passed'], 230)
                        self.assertEqual(result['goal_checks']['candidate']['avg_invalid_loop'], 1.2)
                        history = json.loads((root / 'gepa/previous-results.json').read_text())['records']
                        self.assertEqual(history[0]['original_json'], original)
                        self.assertFalse(history[0]['native_proof_available'])
                        self.assertEqual(history[0]['goal_passed'], published['goal_passed'])
                        saved = json.loads((target / 'baseline.json').read_text())
                        self.assertEqual(saved['metrics']['strict_passed'], 200)
                    else:
                        for name, text in original.items():
                            self.assertEqual((target / name).read_text(), text)
                        if result is not None:
                            self.assertEqual(result['publication_status'], 'not_published')
                            self.assertEqual(result['selected_version'], published['version'])
                        self.assertFalse((root / 'gepa/previous-results.json').exists())

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
                    target = fixture['published'] / 'strategy'
                    self.assertEqual(json.loads((target / 'strategy.json').read_text()),
                                     json.loads(Path(result['proof_artifact']).read_text()))
                    delivered = json.loads((target / 'manifest.json').read_text())
                    self.assertTrue(delivered['goal_passed'])
                    self.assertEqual(delivered['selected_node'], result['selected_node'])
                    self.assertEqual(len(set(delivered['task_ids'])), 500)

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
OUTPUT = Path(__file__).resolve().parents[4] / 'trace_output/appworld'
GEPA = OUTPUT / 'gepa'
def save(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value))
''')
            (here / 'learning.py').write_text('''from pathlib import Path
def validate_seed_label(label):
    return label
def _completed_training_tasks(controller):
    marker = Path(__file__).resolve().parents[4] / 'trace_output/appworld/training/complete'
    if not marker.is_file():
        raise RuntimeError('appworld_training_not_complete')
''')
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
def native_callback(script, *args, **kwargs):
    if script == 'offline_gepa.py' and '--run' in args:
        path = reproduce.OUTPUT / 'gepa/status.json'
        state = reproduce._read(path)
        state.update(job_id='fixture-job', job_status='passed')
        reproduce.layout.save(path, state)
    return 0
reproduce.run = native_callback
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


class OfflineGepaCommandRetryTests(unittest.TestCase):
    """Run the real controller/offline loop around a model-free fake manager."""
    fixture = OfflineGepaRecoveryTests.fixture

    def run_command_fixture(self, *, failures=1, error='gepa_model_rate_limited',
                            cancelled=False, model_budget_exhausted=False,
                            deadline_seconds=1000, cancel_after_seconds=None):
        from tools.trajectory_evolution.experiments.common_lifecycle import (
            LifecycleController, STAGE_NAMES, StageResult,
        )
        with tempfile.TemporaryDirectory() as directory:
            artifact, seed, _, _, tree = self.fixture(directory)
            root = Path(directory)
            original_tree = offline_gepa.read(tree)
            original_tree.update(model_calls=8, trials_used=32, started_ms=1,
                                 reflection_stages=[{'file': '1.pool-map-0', 'reused': False}])
            offline_gepa.save(tree, original_tree)
            report = tree.parent / 'evaluations/healthy/report.json'
            report.parent.mkdir(parents=True)
            report.write_text(json.dumps({'complete': True, 'trials': [{
                'task_id': 'train-healthy', 'trial_id': 'retained-trial',
                'verdict': 'passed', 'metrics': {'quality': 1},
                'evidence': {'loop_review': {'status': 'complete', 'count': 0}},
            }]}))
            completed_map = tree.parent / 'reflections/1.pool-map-0.draft.json'
            completed_map.parent.mkdir()
            completed_map.write_text('{"reason":"retained diagnosis","experiences":[]}')
            protected = {path: path.read_bytes() for path in (seed, tree, report, completed_map)}
            remaining = failures
            operations, delays, stages = [], [], []
            clock = [0.0]

            class Process:
                def __init__(self):
                    self.stdout = io.StringIO('trajectory-integration.v1 listening=127.0.0.1:1\n')
                    self.closed = False
                def poll(self):
                    return 0 if self.closed else None
                def terminate(self):
                    self.closed = True
                def wait(self, timeout=None):
                    self.closed = True
                    return 0
                def kill(self):
                    self.closed = True
            process = Process()

            def progress(job_id):
                self.assertEqual(job_id, 'original')
                return {'job_id': job_id, 'job_status': 'error' if remaining else 'passed',
                        'job_error': error if remaining else None,
                        'cancel_requested': cancelled or (cancel_after_seconds is not None
                            and clock[0] >= cancel_after_seconds),
                        'model_calls': 8, 'max_model_calls': 8 if model_budget_exhausted else 64,
                        'deadline_ms': (2000000000 + deadline_seconds) * 1000,
                        'rounds': 1, 'nodes': 2}

            class Opener:
                @contextmanager
                def open(inner, request, timeout=None):
                    nonlocal remaining
                    message = json.loads(request.data)
                    operation = message['operation']
                    operations.append(operation)
                    if operation == 'agent_evaluation.status':
                        result = {'jobs': {'original': {'status': 'running'}}}
                    elif operation == 'agent_evaluation.retry':
                        self.assertEqual(set(message), {'operation', 'request_id', 'job_id'})
                        self.assertEqual(message['job_id'], 'original')
                        remaining -= 1
                        result = {'accepted': True}
                    else:
                        raise AssertionError('fresh submission or unexpected management operation')
                    yield io.StringIO(json.dumps({'ok': True, 'result': result}))

            def sleep(seconds):
                self.assertGreaterEqual(seconds, 0)
                self.assertLessEqual(seconds, 5)
                delays.append(seconds)
                clock[0] += seconds
                self.assertLess(clock[0], 1000, 'automatic retry did not stop at its bound')

            class Adapter:
                name = 'fake-appworld'
                output_root = root
                def split_counts(self):
                    return {'train': 134, 'val': 12, 'holdout': 500}

            state_path = root / 'command-lifecycle.json'
            prior = {stage: {'status': 'complete', 'artifact': 'retained:' + stage}
                     for stage in STAGE_NAMES[:STAGE_NAMES.index('gepa_run')]}
            for stage in ('pilot32', 'validation100'):
                prior[stage]['gate_passed'] = True
            state_path.write_text(json.dumps({'stage_results': prior}))

            def runner(stage, context):
                stages.append(stage)
                self.assertEqual(stage, 'gepa_run')
                result = offline_gepa.run(seed)
                return StageResult('complete' if result['job_status'] in ('passed', 'rejected')
                                   else 'pending', data={'job_id': result['job_id']})

            with patch.object(offline_gepa, 'require_seed_gates', return_value=artifact), \
                 patch.object(offline_gepa, '_recorded_job', return_value='original'), \
                 patch.object(offline_gepa.controller, 'environment', return_value={}), \
                 patch.object(offline_gepa.subprocess, 'Popen', return_value=process) as launch, \
                 patch.object(offline_gepa.select, 'select', return_value=([process.stdout], [], [])), \
                 patch.object(offline_gepa.urllib.request, 'build_opener', return_value=Opener()), \
                 patch.object(offline_gepa, 'progress', side_effect=progress), \
                 patch.object(offline_gepa.time, 'sleep', side_effect=sleep), \
                 patch.object(offline_gepa.time, 'time', side_effect=lambda: 2000000000 + clock[0]), \
                 patch.object(offline_gepa.time, 'monotonic', side_effect=lambda: clock[0]):
                state = LifecycleController(Adapter(), runner, state_path).run(stop_at='gepa_run')
            launch.assert_called_once()
            self.assertEqual(stages, ['gepa_run'])
            self.assertTrue(process.closed)
            self.assertEqual(state['stage_results']['gepa_run']['data']['job_id'], 'original')
            for stage, receipt in prior.items():
                self.assertEqual(state['stage_results'][stage], receipt)
            for path, original in protected.items():
                self.assertEqual(path.read_bytes(), original, 'retry rewrote cached task/map/budget')
            return state, operations.count('agent_evaluation.retry'), sum(delays)

    def test_one_command_recovers_429_same_job_without_resetting_cache_or_budget(self):
        state, retries, delay = self.run_command_fixture()
        self.assertEqual((state['status'], retries), ('complete', 1))
        self.assertGreaterEqual(delay, 15)

    def test_persistent_429_stops_after_three_same_job_retries(self):
        state, retries, delay = self.run_command_fixture(failures=99)
        self.assertEqual((state['status'], retries), ('pending', 3))
        self.assertGreaterEqual(delay, 105)

    def test_cancel_permanent_config_and_exhausted_budget_do_not_retry(self):
        for options in ({'cancelled': True}, {'error': 'gepa_model_key_missing'},
                        {'model_budget_exhausted': True}, {'deadline_seconds': 0}):
            with self.subTest(options=options):
                state, retries, _ = self.run_command_fixture(**options)
                self.assertEqual((state['status'], retries), ('pending', 0))

    def test_cancel_or_deadline_during_backoff_stops_without_retry(self):
        for options in ({'cancel_after_seconds': 5}, {'deadline_seconds': 5}):
            with self.subTest(options=options):
                state, retries, delay = self.run_command_fixture(**options)
                self.assertEqual((state['status'], retries, delay), ('pending', 0, 5))


class NoNewCandidateTerminalTests(unittest.TestCase):
    def test_same_seed_rejection_finishes_without_proof_or_publication(self):
        for no_candidate, final_state, publication_blocked in ((True, None, False), (True, {}, False),
                (True, {'candidates': []}, False), (False, {'candidates': [{'node': 1}]}, False),
                (False, {'candidates': [{'node': 1}]}, True)):
            with self.subTest(no_candidate=no_candidate, final_state=final_state,
                              publication_blocked=publication_blocked), \
                 tempfile.TemporaryDirectory() as directory:
                output = Path(directory)
                seed = output / 'published/strategy/strategy.json'
                seed.parent.mkdir(parents=True)
                seed.write_text('{"level":"L3"}')
                seed.with_name('manifest.json').write_text('{"original_publication":true}')
                original = {path: path.read_bytes() for path in seed.parent.iterdir()}
                gepa = output / 'gepa'
                runtime = gepa / 'runtime'; runtime.mkdir(parents=True)
                (runtime / 'evaluation.json').write_text(json.dumps({'judge': {
                    'runtime_config': str(reproduce._APPWORLD_LAYOUT.RUNTIME / 'runtime.json')}}))
                (runtime / 'gepa.json').write_text(json.dumps({'prompt_file': str((ROOT /
                    'tools/trajectory_evolution/config/prompts/gepa-reflection.txt').resolve())}))
                (gepa / 'plan.json').write_text(json.dumps({'seed': str(seed)}))
                identity = {'seed_label': 'strategy', 'seed_artifact': str(seed.resolve())}
                receipts = {name: {'status': 'complete'} for name in
                            ('prepare', 'train', 'seed', 'pilot32', 'validation100', 'gepa_prepare')}
                for name in ('pilot32', 'validation100'):
                    receipts[name]['gate_passed'] = True
                lifecycle = gepa / 'lifecycle.json'
                lifecycle.write_text(json.dumps({'candidate': identity, 'stage_results': receipts}))
                tree = gepa / 'workspace/agent-evaluation/original/search/tree.json'
                tree.parent.mkdir(parents=True)
                tree.write_text(json.dumps({'status': 'validation_rejected' if no_candidate else 'completed',
                    'holdout_started': not no_candidate,
                    'final_holdout': final_state}))
                tree_before = tree.read_bytes()
                operations = []

                def native_callback(script, *arguments, **kwargs):
                    self.assertEqual(script, 'offline_gepa.py')
                    operations.append(arguments[0])
                    if arguments[0] == '--run':
                        (gepa / 'status.json').write_text(json.dumps({'job_id': 'original',
                            'job_status': 'rejected' if no_candidate else 'passed'}))
                    elif arguments[0] == '--proof':
                        proof = gepa / 'final-comparison/manifest.json'
                        proof.parent.mkdir()
                        proof.write_text(json.dumps({'goal_passed': True, 'selected_node': 1,
                            'candidate_goal_checks': [{'node': 1, 'gate': {'candidate': {'quality': 1}}}]}))
                    elif arguments[0] == '--publish' and publication_blocked:
                        (gepa / 'status.json').write_text(json.dumps({'status': 'not_published',
                            'publication': {'publication_status': 'not_published',
                                            'publication_reason': 'appworld_delivery_historical_scope_unverified',
                                            'requested_goal_passed': True}}))
                    elif arguments[0] != '--publish':
                        raise AssertionError('unexpected native operation')
                    return 0

                with _isolated_gepa_outputs(output), \
                     patch.object(reproduce._APPWORLD_LAYOUT, 'GEPA', gepa), \
                     patch.object(reproduce._AppWorldDatasetAdapter, 'output_root', output), \
                     patch.object(reproduce, '_candidate_identity', return_value=identity), \
                     patch.object(reproduce, 'verify', return_value=0), \
                     patch.object(reproduce, '_native_gepa_service_active', return_value=False), \
                     patch.object(offline_gepa, '_recorded_job', return_value='original'), \
                     patch.object(reproduce, 'run', side_effect=native_callback), \
                     patch.object(sys, 'argv', ['reproduce.py', '--all']), \
                     redirect_stdout(io.StringIO()), \
                     patch.object(offline_gepa.subprocess, 'Popen', side_effect=AssertionError('no live API')):
                    self.assertEqual(reproduce.main(), 0)
                    if no_candidate:
                        self.assertEqual(reproduce.main(), 0)
                result = json.loads(lifecycle.read_text())
                if no_candidate:
                    self.assertEqual(result['status'], 'not_qualified')
                    self.assertEqual(result['goal_status'], 'goal_not_met')
                    self.assertEqual(result['holdout_status'], 'not_run')
                    self.assertNotIn('last_error', result)
                    self.assertEqual(operations, ['--run'])
                    self.assertNotIn('publish', result['stage_results'])
                    self.assertFalse((gepa / 'final-comparison').exists())
                else:
                    self.assertEqual(result['status'], 'not_published' if publication_blocked else 'published')
                    self.assertEqual(operations, ['--run', '--proof', '--publish'])
                    if publication_blocked:
                        self.assertNotIn('last_error', result)
                        self.assertTrue(result['stage_results']['publish']['data']['requested_goal_passed'])
                self.assertEqual(tree.read_bytes(), tree_before)
                for path, before in original.items():
                    self.assertEqual(path.read_bytes(), before)

    def test_missing_foreign_or_started_holdout_is_not_no_candidate_terminal(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            seed = output / 'published/strategy/strategy.json'
            gepa = output / 'gepa'; gepa.mkdir()
            (gepa / 'status.json').write_text('{"job_id":"original","job_status":"rejected"}')
            plan = gepa / 'plan.json'
            tree = gepa / 'workspace/agent-evaluation/original/search/tree.json'
            tree.parent.mkdir(parents=True)
            with patch.object(reproduce._APPWORLD_LAYOUT, 'GEPA', gepa):
                self.assertFalse(reproduce._native_gepa_no_new_candidate(seed))
                for planned, state in ((output / 'foreign.json', {'status': 'validation_rejected'}),
                                      (seed, {'status': 'error'}),
                                      (seed, {'status': 'validation_rejected', 'holdout_started': True}),
                                      (seed, {'status': 'validation_rejected', 'final_holdout': {
                                          'candidates': [{'node': 1}]}})):
                    plan.write_text(json.dumps({'seed': str(planned)}))
                    tree.write_text(json.dumps(state))
                    self.assertFalse(reproduce._native_gepa_no_new_candidate(seed))


class PublishedRestartTests(unittest.TestCase):
    def test_cleared_restart_uses_generic_seed_and_reaches_missing_gate_prefix(self):
        for failed_count in (32, 100):
            with self.subTest(failed_count=failed_count), tempfile.TemporaryDirectory() as directory:
                output = Path(directory)
                seed = output / 'published/strategy/strategy.json'
                old_alias = output / 'published/old-seed/strategy.json'
                for path in (seed, old_alias):
                    path.parent.mkdir(parents=True)
                    path.write_text(json.dumps({'level': 'L3', 'agent': {'revision': 'retained-seed'}}))
                seed.with_name('manifest.json').write_text(json.dumps({
                    'version': 'retained-seed', 'source_strategy': str(old_alias)}))
                baseline = seed.with_name('baseline.json')
                baseline.write_text(json.dumps({'status': 'complete', 'items': [{'task_id': 'old-task'}],
                                                'source_report': str(output / 'deleted-native/report.json')}))
                before = {path: path.read_bytes() for path in (seed, old_alias, baseline)}
                training = output / 'training/full-training-status.json'
                training.parent.mkdir()
                training.write_text(json.dumps({'status': 'complete'}))

                def stage(seed_path, count):
                    self.assertEqual(seed_path, seed.resolve())
                    if count == failed_count:
                        raise RuntimeError(f'appworld_seed_stage{count}_goal_gate_not_passed')

                def verify(seed_path, count, *args, **kwargs):
                    self.assertEqual(seed_path, seed.resolve())
                    return int(count == failed_count)

                with _isolated_gepa_outputs(output), \
                     patch.object(reproduce._APPWORLD_LAYOUT, 'GEPA', output / 'gepa'), \
                     patch.object(reproduce, 'assert_idle_for_fresh'), \
                     patch.object(reproduce, 'stage', side_effect=stage) as stages, \
                     patch.object(reproduce, 'verify', side_effect=verify) as checks, \
                     patch.object(reproduce, 'run') as native, \
                     patch.object(sys, 'argv', ['reproduce.py', '--restart-gepa', '--all']), \
                     redirect_stdout(io.StringIO()), patch('sys.stderr', new=io.StringIO()):
                    self.assertEqual(reproduce.main(), 1)
                self.assertEqual([call.args[1] for call in stages.call_args_list],
                                 [32] if failed_count == 32 else [32, 100])
                self.assertEqual([call.args[1] for call in checks.call_args_list],
                                 [32] if failed_count == 32 else [32, 100])
                native.assert_not_called()
                self.assertFalse((output / 'gepa/l0-current-stage500').exists())
                for path, original in before.items():
                    self.assertEqual(path.read_bytes(), original)
                lifecycle = json.loads((output / 'gepa/lifecycle.json').read_text())
                self.assertEqual(lifecycle['candidate']['seed_artifact'], str(seed.resolve()))
                failed = 'pilot32' if failed_count == 32 else 'validation100'
                self.assertFalse(lifecycle['stage_results'][failed]['gate_passed'])

    def test_explicit_full_restart_uses_publication_instead_of_retired_plan(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            for label in ('strategy', 'retired-seed'):
                path = output / 'published' / label / 'strategy.json'
                path.parent.mkdir(parents=True)
                path.write_text('{}')
            seed = output / 'published/strategy/strategy.json'
            plan = output / 'gepa/plan.json'; plan.parent.mkdir()
            plan.write_text(json.dumps({'seed': str(output / 'published/retired-seed/strategy.json')}))
            with patch.object(reproduce, 'OUTPUT', output), \
                 patch.object(reproduce, '_experiment_status'), \
                 patch.object(reproduce, 'restart_gepa', return_value={'status': 'old_gepa_removed'}) as reset, \
                 patch.object(reproduce, '_run_common_all', return_value={}) as common, \
                 patch.object(sys, 'argv', ['reproduce.py', '--restart-gepa', '--all']), \
                 redirect_stdout(io.StringIO()):
                self.assertEqual(reproduce.main(), 0)
            reset.assert_called_once_with(seed.resolve())
            common.assert_called_once_with(seed.resolve(), 'strategy', stop_at=None)

    def test_restart_alone_defaults_to_all_and_other_actions_remain_exclusive(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            seed = output / 'published/strategy/strategy.json'
            seed.parent.mkdir(parents=True)
            seed.write_text('{}')
            with patch.object(reproduce, 'OUTPUT', output), \
                 patch.object(reproduce, '_experiment_status'), \
                 patch.object(reproduce, 'restart_gepa', return_value={}), \
                 patch.object(reproduce, '_run_common_all', return_value={}) as common, \
                 patch.object(sys, 'argv', ['reproduce.py', '--restart-gepa']), \
                 redirect_stdout(io.StringIO()):
                self.assertEqual(reproduce.main(), 0)
            common.assert_called_once_with(seed.resolve(), 'strategy', stop_at=None)
        for argv in (['reproduce.py'], ['reproduce.py', '--restart-gepa', '--status'],
                     ['reproduce.py', '--all', '--status']):
            with self.subTest(argv=argv), patch.object(sys, 'argv', argv), \
                 patch('sys.stderr', new=io.StringIO()):
                with self.assertRaises(SystemExit) as stopped:
                    reproduce.main()
                self.assertEqual(stopped.exception.code, 2)

    def test_nonempty_search_still_requires_real_seed_gates_before_reset(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            retained = output / 'gepa/workspace/retained.json'
            retained.parent.mkdir(parents=True)
            retained.write_bytes(b'{"actual_checkpoint":true}')
            with _isolated_gepa_outputs(output), \
                 patch.object(reproduce._APPWORLD_LAYOUT, 'GEPA', output / 'gepa'), \
                 patch.object(reproduce, 'assert_idle_for_fresh'), \
                 patch.object(reproduce, 'verify', side_effect=RuntimeError('seed_gate_required')):
                with self.assertRaisesRegex(RuntimeError, 'seed_gate_required'):
                    reproduce.restart_gepa(output / 'published/strategy/strategy.json')
            self.assertEqual(retained.read_bytes(), b'{"actual_checkpoint":true}')

    def test_empty_symlink_search_is_still_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            target = output / 'other'; target.mkdir()
            gepa = output / 'gepa'; gepa.symlink_to(target, target_is_directory=True)
            with patch.object(reproduce._APPWORLD_LAYOUT, 'GEPA', gepa), \
                 patch.object(reproduce, 'assert_idle_for_fresh'), \
                 patch.object(reproduce, 'verify') as checks:
                with self.assertRaisesRegex(RuntimeError, 'appworld_gepa_reset_symlink_refused'):
                    reproduce.restart_gepa(output / 'published/strategy/strategy.json')
            checks.assert_not_called()


class GepaModelBudgetTests(unittest.TestCase):
    def test_gepa_default_uses_retained_generic_publication_without_dead_source(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            seed = output / 'published/strategy/strategy.json'
            seed.parent.mkdir(parents=True)
            seed.write_text('{}')
            (seed.parent / 'manifest.json').write_text(json.dumps({
                'source_strategy': str(output / 'deleted-gepa/versions/004.json')}))
            with patch.object(reproduce, 'OUTPUT', output), \
                 patch.object(reproduce, 'seed_search', return_value={}), \
                 patch.object(sys, 'argv', ['reproduce.py', '--gepa-run']), \
                 patch.object(reproduce, '_experiment_status'), \
                 patch.object(reproduce, '_EXPERIMENT_TRACKING', False), \
                 patch.object(reproduce, '_EXPERIMENT_LABEL', None), \
                 patch.object(reproduce, '_run_common_all', return_value={}) as common, \
                 redirect_stdout(io.StringIO()):
                self.assertEqual(reproduce.main(), 0)
            common.assert_called_once_with(seed.resolve(), 'strategy', stop_at='gepa_run')

    def test_gepa_default_preserves_existing_plan_over_generic_publication(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            for label in ('strategy', 'frozen-seed'):
                seed = output / 'published' / label / 'strategy.json'
                seed.parent.mkdir(parents=True)
                seed.write_text('{}')
            planned = output / 'published/frozen-seed/strategy.json'
            plan = output / 'gepa/plan.json'; plan.parent.mkdir()
            plan.write_text(json.dumps({'seed': str(planned)}))
            with patch.object(reproduce, 'OUTPUT', output), \
                 patch.object(reproduce, 'seed_search', return_value={}), \
                 patch.object(sys, 'argv', ['reproduce.py', '--gepa-run']), \
                 patch.object(reproduce, '_experiment_status'), \
                 patch.object(reproduce, '_EXPERIMENT_TRACKING', False), \
                 patch.object(reproduce, '_EXPERIMENT_LABEL', None), \
                 patch.object(reproduce, '_run_common_all', return_value={}) as common, \
                 redirect_stdout(io.StringIO()):
                self.assertEqual(reproduce.main(), 0)
            common.assert_called_once_with(planned.resolve(), 'frozen-seed', stop_at='gepa_run')

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



class AutomaticSeedSearchTests(unittest.TestCase):
    """The real shared lifecycle/native gate around model-free capture fixtures."""
    @contextmanager
    def fixture(self, directory, *, passed_after=None, fail_at=32, duplicate=False,
                duplicate_version=False, missing_loop=False, stale_judge=False,
                pending_review=False, generation_error=None, duplicate_once=False):
        import learning
        root = Path(directory)
        seed = root / 'published/strategy/strategy.json'
        seed.parent.mkdir(parents=True)
        artifact = {'level': 'L3', 'policy': {'candidate': 0,
                    'snapshot': {'experiences': []}}}
        seed.write_text(json.dumps(artifact))
        seed.with_name('manifest.json').write_text(json.dumps({'version': 'fixture-native-0'}))
        protected = {path: path.read_bytes() for path in seed.parent.iterdir()}
        lifecycle = root / 'gepa/lifecycle.json'
        lifecycle.parent.mkdir()
        receipts = {name: {'status': 'complete', 'data': {'retained': name}}
                    for name in ('prepare', 'train', 'seed')}
        lifecycle.write_text(json.dumps({'candidate': {
            'seed_label': 'strategy', 'seed_artifact': str(seed.resolve()),
            'seed_version': 'fixture-native-0'}, 'stage_results': receipts}))
        baselines, ids = {}, {}
        for count in (32, 100):
            baselines[count] = root / f'gepa/l0-stage{count}/result.json'
            baselines[count].parent.mkdir()
            ids[count] = _l0(baselines[count], count=count)
        events, generations = [], []
        remaining_error = [generation_error]

        def freeze(*, label):
            generations.append(label)
            self.assertEqual(__import__('os').environ.get('APPWORLD_REGENERATE_CONTROLS'), '1')
            for name in ('APPWORLD_AUTH_PHASE_FROM', 'APPWORLD_INCREMENTAL_TRAINING_FROM',
                         'APPWORLD_ENGLISH_TRIGGERS', 'APPWORLD_ENGLISH_TRIGGERS_FROM'):
                self.assertNotIn(name, __import__('os').environ)
            if remaining_error[0]:
                error, remaining_error[0] = remaining_error[0], None
                raise error
            index = 0 if duplicate or (duplicate_once and len(generations) == 1) else len(reproduce.seed_search()['attempts'])
            value = {'level': 'L3', 'policy': {'candidate': index,
                     'snapshot': {'experiences': []}}}
            target = root / 'published' / label / 'strategy.json'
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(json.dumps(value))
            target.with_name('manifest.json').write_text(json.dumps({
                'version': 'fixture-native-0' if duplicate_version else f'fixture-native-{index}'}))
            return value

        def stage(candidate, count):
            value = reproduce._read(candidate)
            index = value['policy']['candidate']
            events.append((index, count))
            attempt = reproduce.begin_seed_attempt(candidate)
            comparison = Path(attempt['pilot_comparison' if count == 32 else 'validation_comparison'])
            comparison.parent.mkdir(parents=True, exist_ok=True)
            fails = count == fail_at and (passed_after is None or index < passed_after)
            _candidate(comparison, ids[count], quality=.4 if fails else .42,
                       missing='avg_invalid_loop' if missing_loop else None)
            captured = reproduce._read(comparison)
            progress = comparison.with_name('progress.json')
            captured.update(seed=str(candidate.resolve()), progress=str(progress))
            for row in captured['items']:
                row['l1_applied'] = 0
            if pending_review:
                captured.update(status='partial', review_distribution={'uncertain': 1},
                    metrics={'candidate': {'planned': count, 'completed': count, 'missing': 0,
                                           'execution_errors': 0, 'judge_errors': 1}})
                for row in captured['items']:
                    row['execution_status'] = 'completed'
                captured['items'][0].update(verdict='uncertain', quality=None, quality_score=None)
            comparison.write_text(json.dumps(captured))
            progress.write_text(json.dumps(_candidate_checkpoint(comparison, value, captured['items'])))
            # A complete-looking compact cannot replace a mismatched frozen
            # native judge source; the production gate remains authoritative.
            if stale_judge:
                source = Path(reproduce._read(progress)['shards']['0']['source_report'])
                config = source.parent.with_name(source.parent.name + '.regrade.config.json')
                settings = reproduce._read(config)
                settings['judge']['require_state_dependency'] = False
                config.write_text(json.dumps(settings))
            reproduce.verify(candidate, count, comparison)

        def native(script, *arguments, **kwargs):
            self.assertEqual(script, 'offline_gepa.py')
            events.append(arguments[0])
            self.assertIn(arguments[0], ('--prepare', '--run'))
            if arguments[0] == '--run':
                status = reproduce._read(root / 'gepa/status.json')
                status.update(job_id='fixture-only', job_status='rejected')
                reproduce._APPWORLD_LAYOUT.save(root / 'gepa/status.json', status)
            return 0

        with _isolated_gepa_outputs(root), ExitStack() as stack:
            stack.enter_context(patch.object(reproduce._APPWORLD_LAYOUT, 'GEPA', root / 'gepa'))
            stack.enter_context(patch.object(reproduce, '_CANDIDATE_STATE_LABEL', None))
            stack.enter_context(patch.object(reproduce, '_host_version', return_value=None))
            stack.enter_context(patch.object(reproduce, '_experiment_status'))
            stack.enter_context(patch.object(reproduce._AppWorldDatasetAdapter, 'output_root', root))
            stack.enter_context(patch.object(reproduce._AppWorldDatasetAdapter, 'split_counts',
                return_value={'train': 134, 'val': 12, 'holdout': 500}))
            stack.enter_context(patch.object(learning, '_completed_training_tasks',
                return_value={f'train-{i}' for i in range(134)}))
            stack.enter_context(patch.object(reproduce, 'prepare', side_effect=AssertionError('cached prepare')))
            stack.enter_context(patch.object(reproduce, 'train_with_recovery', side_effect=AssertionError('cached TRAIN')))
            stack.enter_context(patch.object(reproduce, 'seed_with_recovery', side_effect=freeze))
            stack.enter_context(patch.object(reproduce, 'stage', side_effect=stage))
            stack.enter_context(patch.object(reproduce, '_stage_l0_source', side_effect=lambda count, path: baselines[count]))
            stack.enter_context(patch.object(reproduce, '_native_gepa_service_active', return_value=False))
            stack.enter_context(patch.object(reproduce, '_native_gepa_no_new_candidate', return_value=True))
            stack.enter_context(patch.object(reproduce, 'run', side_effect=native))
            stack.enter_context(patch.object(offline_gepa.subprocess, 'Popen', side_effect=AssertionError('no API/Agent')))
            stack.enter_context(redirect_stdout(io.StringIO()))
            yield seed, root, events, generations, receipts
        for path, original in protected.items():
            self.assertEqual(path.read_bytes(), original, 'retained publication changed')

    def test_all_generates_new_native_seed_and_crosses_both_original_gates_before_gepa(self):
        for fail_at in (32, 100):
            with self.subTest(fail_at=fail_at), tempfile.TemporaryDirectory() as directory, \
                 self.fixture(directory, passed_after=1, fail_at=fail_at) as (seed, root, events, generated, retained):
                result = reproduce._run_all_with_seed_search(seed, 'strategy', gepa_max_model_calls=512)
                expected = [(0, 32)] + ([(0, 100)] if fail_at == 100 else [])
                self.assertEqual(events, expected + [(1, 32), (1, 100), '--prepare', '--run'])
                self.assertEqual(len(generated), 1)
                self.assertEqual(result['status'], 'not_qualified')
                self.assertEqual(reproduce.seed_search()['status'], 'found')
                for name in ('prepare', 'train'):
                    self.assertEqual(result['stage_results'][name], retained[name])
                for name in ('pilot32', 'validation100'):
                    self.assertIs(result['stage_results'][name]['gate_passed'], True)
                self.assertEqual(result['stage_results']['gepa_run']['status'], 'complete')

    def test_five_distinct_complete_failures_continue_to_sixth_candidate(self):
        with tempfile.TemporaryDirectory() as directory, \
             self.fixture(directory, passed_after=5) as (seed, root, events, generated, retained):
            result = reproduce._run_all_with_seed_search(seed, 'strategy')
            self.assertEqual(result['status'], 'not_qualified')
            self.assertEqual(events, [(i, 32) for i in range(6)] + [(5, 100), '--prepare', '--run'])
            self.assertEqual(len(generated), 5)
            search = reproduce.seed_search()
            self.assertEqual(search['status'], 'found')
            self.assertEqual(len([r for r in search['attempts'] if r['status'] == 'failed']), 5)
            self.assertEqual(len(search['attempts']), 6)
            self.assertEqual(len({reproduce._read(Path(row['seed']))['policy']['candidate']
                                 for row in search['attempts']}), 6)

    def _retained_legacy_review(self, seed, root, events):
        """Build five complete native-backed failures under the retired policy."""
        current = seed
        for index in range(5):
            if index:
                label = f'seed-auto-r1-a{index + 1}'
                with patch.dict(__import__('os').environ, {'APPWORLD_REGENERATE_CONTROLS': '1'}):
                    reproduce.seed_with_recovery(label=label)
                current = root / 'published' / label / 'strategy.json'
            with self.assertRaises(RuntimeError):
                reproduce.stage(current, 32)
        search = reproduce.seed_search()
        search.update(status='review_required', consecutive_failures=5, current_seed=str(current.resolve()),
            review_requirement='Review actual failure traces, then verify new training import or implementation fix and targeted tests before explicitly starting a new round.',
            automatic_seed_generation={'round': 1, 'status': 'frozen', 'seed': str(current.resolve()),
                'seed_label': current.parent.name, 'source_seed': str(seed.resolve()), 'source_attempt': 4})
        reproduce._save_search(search)
        paid = {'seed_label': current.parent.name, 'published': True,
                'control_learning': {'model_calls': 3, 'validated_model_draft': {'retained': 'healthy'}},
                'english_trigger_learning': {'model_calls': 2, 'attempts': [{'draft': ['retained']} ]}}
        (root / 'training').mkdir(exist_ok=True)
        (root / 'training/seed-compile-status.json').write_text(json.dumps(paid))
        protected = {path: path.read_bytes() for path in root.rglob('*.json')
                     if '/published/' in str(path) or '/l0-stage' in str(path)
                     or '/seed-search-' in str(path)}
        return current, paid, protected

    def test_retired_five_failure_checkpoint_resumes_with_new_label_and_preserved_paid(self):
        with tempfile.TemporaryDirectory() as directory, \
             self.fixture(directory, passed_after=5) as (seed, root, events, generated, retained):
            old, paid, protected = self._retained_legacy_review(seed, root, events)
            with patch.object(sys, 'argv', ['reproduce.py', '--all']):
                self.assertEqual(reproduce.main(), 0)
            search = reproduce.seed_search()
            self.assertEqual(search['round'], 2)
            self.assertEqual(search['reviews'][0]['reason'], 'failed_seed_count_policy_withdrawn')
            self.assertEqual(search['attempts'][4]['seed_generation_diagnostic'], paid)
            self.assertEqual([r['status'] for r in search['attempts'][:5]], ['failed'] * 5)
            self.assertEqual(generated[-1], 'seed-auto-r2-a6')
            self.assertEqual(events, [(i, 32) for i in range(6)] + [(5, 100), '--prepare', '--run'])
            for path, data in protected.items():
                self.assertEqual(path.read_bytes(), data)

    def test_explicit_review_restart_accepts_new_label_and_rejects_frozen_failed_label(self):
        for explicit in (False, True):
            with self.subTest(explicit=explicit), tempfile.TemporaryDirectory() as directory, \
                 self.fixture(directory, passed_after=5) as (seed, root, events, generated, retained):
                old, paid, protected = self._retained_legacy_review(seed, root, events)
                summary = 'Reviewed actual formal failures and verified the implementation repairs.'
                before = reproduce.seed_search()
                with self.assertRaisesRegex(RuntimeError, 'unchanged_after_review'):
                    reproduce.restart_search_after_review(summary, candidate_seed=old)
                self.assertEqual(reproduce.seed_search(), before)
                args = ['reproduce.py', '--all', '--restart-search-after-review', summary]
                if explicit:
                    args += ['--seed-label', 'reviewed-new-candidate']
                with patch.object(sys, 'argv', args):
                    self.assertEqual(reproduce.main(), 0)
                self.assertEqual(generated[-1], 'reviewed-new-candidate' if explicit else 'seed-auto-r2-a6')
                self.assertEqual(reproduce.seed_search()['attempts'][4]['seed_generation_diagnostic'], paid)
                for path, data in protected.items():
                    self.assertEqual(path.read_bytes(), data)

    def test_unrelated_review_cannot_be_cleared_by_failed_count_migration(self):
        with tempfile.TemporaryDirectory() as directory, \
             self.fixture(directory, passed_after=5) as (seed, root, events, generated, retained):
            old, paid, protected = self._retained_legacy_review(seed, root, events)
            search = reproduce.seed_search()
            search['review_requirement'] = 'Approved source authority has been withdrawn; review is required.'
            reproduce._save_search(search)
            original = reproduce.seed_search()
            result = reproduce._run_all_with_seed_search(old, old.parent.name)
            self.assertEqual(result['status'], 'review_required')
            self.assertEqual(reproduce.seed_search(), original)
            self.assertEqual(events, [(i, 32) for i in range(5)])

    def test_duplicate_body_or_native_version_does_not_retest_or_count(self):
        for options in ({'duplicate': True}, {'duplicate_version': True}):
            with self.subTest(options=options), tempfile.TemporaryDirectory() as directory, \
                 self.fixture(directory, **options) as (seed, root, events, generated, retained):
                result = reproduce._run_all_with_seed_search(seed, 'strategy')
                self.assertEqual((result['status'], result['stop_reason']),
                                 ('not_qualified', 'appworld_seed_generation_duplicate'))
                self.assertEqual(events, [(0, 32)])
                self.assertEqual(len(generated), 1)
                self.assertEqual(reproduce.seed_search()['consecutive_failures'], 1)
                self.assertEqual(len(reproduce.seed_search()['attempts']), 1)
                self.assertEqual(reproduce._run_all_with_seed_search(seed, 'strategy')['status'], 'not_qualified')
                self.assertEqual(len(generated), 1)
                self.assertEqual(events, [(0, 32)])

    def test_missing_loop_or_invalid_native_judge_never_advances_seed(self):
        for options in ({'missing_loop': True}, {'stale_judge': True}):
            with self.subTest(options=options), tempfile.TemporaryDirectory() as directory, \
                 self.fixture(directory, **options) as (seed, root, events, generated, retained):
                with self.assertRaisesRegex(RuntimeError, 'stage_gate_not_passed:pilot32'):
                    reproduce._run_all_with_seed_search(seed, 'strategy')
                self.assertEqual(events, [(0, 32)])
                self.assertEqual(generated, [])
                self.assertNotIn('automatic_seed_generation', reproduce.seed_search())

    def test_pending_formal_review_keeps_current_seed_and_failure_allowance(self):
        with tempfile.TemporaryDirectory() as directory, \
             self.fixture(directory, pending_review=True) as (seed, root, events, generated, retained):
            with self.assertRaisesRegex(RuntimeError, 'appworld_stage_review_pending'):
                reproduce._run_all_with_seed_search(seed, 'strategy')
            search = reproduce.seed_search()
            self.assertEqual(search['current_seed'], str(seed.resolve()))
            self.assertEqual(search['consecutive_failures'], 0)
            self.assertEqual(search['attempts'][0]['status'], 'pending')
            self.assertEqual(generated, [])
            self.assertNotIn('automatic_seed_generation', search)
            self.assertEqual(reproduce._read(root / 'gepa/lifecycle.json')
                             ['stage_results']['pilot32']['status'], 'pending')

    def test_interrupted_or_rate_limited_freeze_resumes_same_reserved_label(self):
        for error in (KeyboardInterrupt(), RuntimeError('appworld_seed_recovery_exhausted:model_http_429')):
            with self.subTest(error=type(error).__name__), tempfile.TemporaryDirectory() as directory, \
                 self.fixture(directory, passed_after=1, generation_error=error) as (seed, root, events, generated, retained):
                with self.assertRaises(type(error)):
                    reproduce._run_all_with_seed_search(seed, 'strategy')
                reserved = reproduce.seed_search()['automatic_seed_generation']
                self.assertEqual(reserved['status'], 'pending')
                self.assertFalse(Path(reserved['seed']).is_file())
                self.assertEqual(reproduce.seed_search()['consecutive_failures'], 1)
                with patch.object(sys, 'argv', ['reproduce.py', '--all']):
                    self.assertEqual(reproduce.main(), 0)
                self.assertEqual(generated, [reserved['seed_label'], reserved['seed_label']])
                self.assertEqual(events, [(0, 32), (1, 32), (1, 100), '--prepare', '--run'])
                self.assertEqual(reproduce.seed_search()['status'], 'found')



    def test_one_command_skips_duplicate_without_retesting_and_preserves_paid_receipt(self):
        with tempfile.TemporaryDirectory() as directory, \
             self.fixture(directory, passed_after=1, duplicate_once=True) as (seed, root, events, generated, retained):
            original = reproduce.seed_with_recovery
            paid = {}
            def freeze(*, label):
                artifact = original(label=label)
                diagnostic = {'seed_label': label, 'control_learning': {'model_calls': 3},
                              'english_trigger_learning': {'model_calls': 2}}
                (root / 'training').mkdir(exist_ok=True)
                (root / 'training/seed-compile-status.json').write_text(json.dumps(diagnostic))
                paid[label] = diagnostic
                return artifact
            with patch.object(reproduce, 'seed_with_recovery', side_effect=freeze), \
                 patch.object(sys, 'argv', ['reproduce.py', '--all']):
                self.assertEqual(reproduce.main(), 0)
            self.assertEqual(generated, ['seed-auto-r1-a2', 'seed-auto-r1-a2-2'])
            self.assertEqual(events, [(0, 32), (1, 32), (1, 100), '--prepare', '--run'])
            search = reproduce.seed_search()
            self.assertEqual(len(search['attempts']), 2)
            self.assertEqual([r['status'] for r in search['attempts']], ['failed', 'passed'])
            self.assertEqual(len(search['generation_history']), 1)
            old = search['generation_history'][0]
            self.assertEqual(old['status'], 'duplicate')
            self.assertEqual(old['seed_generation_diagnostic'], paid[generated[0]])
            self.assertEqual(search['attempts'][1]['seed_generation_diagnostic'], paid[generated[1]])
            self.assertTrue(Path(old['seed']).is_file())

    def test_one_command_recovers_transient_seed_429_on_same_reserved_label(self):
        with tempfile.TemporaryDirectory() as directory, \
             self.fixture(directory, passed_after=1,
                generation_error=RuntimeError('appworld_seed_recovery_exhausted:model_http_429')) \
                as (seed, root, events, generated, retained):
            with patch.object(reproduce.time, 'sleep') as sleep, \
                 patch.object(sys, 'argv', ['reproduce.py', '--all']):
                self.assertEqual(reproduce.main(), 0)
            self.assertEqual(generated, ['seed-auto-r1-a2', 'seed-auto-r1-a2'])
            self.assertEqual(events, [(0, 32), (1, 32), (1, 100), '--prepare', '--run'])
            self.assertEqual(sleep.call_args_list, [unittest.mock.call(15)])
            state = reproduce._read(root / 'gepa/lifecycle.json')
            for name in ('prepare', 'train'):
                self.assertEqual(state['stage_results'][name], retained[name])
            self.assertEqual(reproduce.seed_search()['status'], 'found')

    def test_temporary_transport_keeps_running_with_bounded_backoff_and_interrupt(self):
        import urllib.error
        with tempfile.TemporaryDirectory() as directory, \
             self.fixture(directory, passed_after=1) as (seed, root, events, generated, retained):
            old = (root / 'gepa/lifecycle.json').read_bytes()
            error = urllib.error.HTTPError('https://fixture.invalid', 503, 'capacity', {}, None)
            with patch.object(reproduce, '_run_all_with_seed_search',
                    side_effect=[error, error, error, error, KeyboardInterrupt()]) as runner, \
                 patch.object(reproduce.time, 'sleep') as sleep:
                with self.assertRaises(KeyboardInterrupt):
                    reproduce._run_all_autonomously(seed, 'strategy')
            self.assertEqual(runner.call_count, 5)
            self.assertEqual(sleep.call_args_list, [unittest.mock.call(v) for v in (15, 30, 60, 60)])
            state = reproduce._read(root / 'gepa/lifecycle.json')
            self.assertEqual(state['autonomous_recovery']['reason'], 'model_http_503')
            self.assertEqual(state['stage_results'], json.loads(old)['stage_results'])
            self.assertEqual(events, [])
            self.assertEqual(generated, [])

    def test_native_retry_respects_original_budget_deadline_cancel_and_error(self):
        for progress, expected in (
            ({'model_calls': 1024, 'max_model_calls': 1024}, 'appworld_gepa_model_budget_exhausted'),
            ({'deadline_ms': 1}, 'appworld_gepa_deadline_exhausted'),
            ({'cancel_requested': True}, 'appworld_native_gepa_cancelled'),
            ({'job_status': 'error', 'job_error': 'source_authority_invalid'},
                'appworld_native_gepa_unclassified_error')):
            with self.subTest(expected=expected), tempfile.TemporaryDirectory() as directory, \
                 self.fixture(directory, passed_after=1) as (seed, root, events, generated, retained):
                state = reproduce._read(root / 'gepa/lifecycle.json')
                state.update(phase='gepa_run', status='pending')
                reproduce._APPWORLD_LAYOUT.save(root / 'gepa/lifecycle.json', state)
                with patch.object(reproduce, '_run_all_with_seed_search',
                        side_effect=RuntimeError('appworld_native_gepa_checkpoint_requires_resume')) as runner, \
                     patch.object(offline_gepa, 'progress', return_value=progress), \
                     patch.object(reproduce.time, 'sleep') as sleep:
                    result = reproduce._run_all_autonomously(seed, 'strategy')
                self.assertEqual(result['stop_reason'], expected)
                self.assertEqual(result['status'], 'cancelled' if progress.get('cancel_requested') else 'blocked')
                self.assertEqual(result['goal_status'], 'goal_not_met')
                self.assertEqual(runner.call_count, 1)
                sleep.assert_not_called()
                self.assertEqual(result['stage_results'], state['stage_results'])

    def test_invalid_source_is_normal_blocked_result_without_retry_or_false_complete(self):
        with tempfile.TemporaryDirectory() as directory, \
             self.fixture(directory, passed_after=1) as (seed, root, events, generated, retained):
            with patch.object(reproduce, '_run_all_with_seed_search',
                    side_effect=RuntimeError('appworld_frozen_seed_validated_source_missing')) as runner, \
                 patch.object(reproduce.time, 'sleep') as sleep, \
                 patch.object(sys, 'argv', ['reproduce.py', '--all']):
                self.assertEqual(reproduce.main(), 0)
            state = reproduce._read(root / 'gepa/lifecycle.json')
            self.assertEqual(state['status'], 'blocked')
            self.assertEqual(state['goal_status'], 'goal_not_met')
            self.assertEqual(state['stop_reason'], 'appworld_frozen_seed_validated_source_missing')
            self.assertEqual(runner.call_count, 1)
            sleep.assert_not_called()
            self.assertEqual(events, [])
            self.assertEqual(generated, [])


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
            self.assertIs(config['final_holdout_include_seed'], False)

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

class NativeCumulativeBudgetRecoveryTests(unittest.TestCase):
    fixture = OfflineGepaRecoveryTests.fixture

    def budget_fixture(self, directory, *, stop='max_rounds'):
        artifact, seed, jobs, state_path, tree = self.fixture(directory, status='rejected')
        canonical_seed = seed.parent / 'strategy' / 'strategy.json'
        canonical_seed.parent.mkdir(); seed.replace(canonical_seed); seed = canonical_seed
        binding = patch.object(reproduce._APPWORLD_LAYOUT, 'GEPA', offline_gepa.GEPA)
        binding.start(); self.addCleanup(binding.stop)
        old = {'max_rounds': 30, 'max_model_calls': 512, 'max_trials': 8000,
               'timeout_ms': 50 * 3600000}
        offline_gepa.save(offline_gepa.CONFIG / 'gepa.json', old)
        offline_gepa.save(offline_gepa.PLAN, {'seed': str(seed.resolve()),
            'search': {'max_rounds': 30, 'max_model_calls': 512}, 'dataset_version': 'frozen-public'})
        offline_gepa.save(offline_gepa.STATUS, {'job_id': 'original', 'job_status': 'rejected'})
        value = offline_gepa.read(tree)
        value.update(status='validation_rejected', stop_reason=stop, rounds=30, model_calls=480,
            trials_used=1024, active=None, batch=None, holdout_started=False,
            budget_extensions=[dict(old, reason='initial_budget')])
        offline_gepa.save(tree, value)
        stages = {name: {'status': 'complete', 'data': {'retained': name}}
            for name in ('prepare', 'train', 'seed', 'pilot32', 'validation100', 'gepa_prepare', 'gepa_run')}
        stages['comparison500'] = {'status': 'blocked', 'reason': 'appworld_gepa_no_new_qualified_candidate'}
        offline_gepa.save(offline_gepa.GEPA / 'lifecycle.json', {'stage_results': stages,
            'candidate': reproduce._candidate_identity(seed, seed.parent.name)})
        report = tree.parent / 'evaluations/healthy/report.pb'
        report.parent.mkdir(parents=True); report.write_bytes(b'original-native-healthy-report')
        draft = tree.parent / 'reflections/map.draft.json'
        draft.parent.mkdir(); draft.write_text('{"original_model_draft":true}')
        baseline = offline_gepa.GEPA / 'l0-current-stage32/result.json'
        baseline.parent.mkdir(); baseline.write_text('{"original_frozen_L0":true}')
        return artifact, seed, state_path, tree, {p: p.read_bytes() for p in (seed, report, draft, baseline)}, stages

    def test_real_prepare_and_run_extend_same_job_keep_old_grades_and_paid(self):
        with tempfile.TemporaryDirectory() as directory:
            artifact, seed, state_path, tree, protected, stages = self.budget_fixture(directory)
            before = tree.read_bytes()
            requested = reproduce._request_native_gepa_budget_extension(seed)
            self.assertEqual((requested['max_rounds'], requested['max_model_calls']), (60, 1024))
            self.assertEqual(tree.read_bytes(), before)
            lifecycle = offline_gepa.read(offline_gepa.GEPA / 'lifecycle.json')
            for name in ('prepare','train','seed','pilot32','validation100'):
                self.assertEqual(lifecycle['stage_results'][name], stages[name])
            self.assertEqual(lifecycle['stage_results']['gepa_run']['status'], 'pending')
            with patch.object(offline_gepa, 'require_seed_gates', return_value=artifact), \
                 patch.object(offline_gepa, '_training_ready', return_value=Path(directory)/'retained-memory.json'), \
                 patch.object(offline_gepa, 'build_dataset', return_value={'metadata': {'dataset_version': 'frozen-public'}, 'summary': {'splits': {'train':134,'val':12,'holdout':500}}}), \
                 patch.object(offline_gepa, '_copy_training_configs'), \
                 patch.object(offline_gepa.controller, 'ids', return_value=['retained-task']), \
                 patch.object(offline_gepa.controller, 'evaluation_config', return_value={'judge': {}, 'variants': []}):
                plan = offline_gepa.prepare(seed, max_model_calls=1024)
            self.assertEqual(plan['budget_extension']['previous_limits']['max_model_calls'],512)
            self.assertEqual(offline_gepa.read(offline_gepa.CONFIG/'gepa.json')['max_rounds'],60)
            operations=[]
            class Process:
                def __init__(self):
                    self.stdout=io.StringIO('trajectory-integration.v1 listening=127.0.0.1:1\n'); self.closed=False
                def poll(self):return 0 if self.closed else None
                def terminate(self):self.closed=True
                def wait(self,timeout=None):self.closed=True;return 0
                def kill(self):self.closed=True
            process=Process()
            class Opener:
                @contextmanager
                def open(inner,request,timeout=None):
                    message=json.loads(request.data);operations.append(message['operation'])
                    if message['operation']=='agent_evaluation.status':result={'jobs':{'original':{'status':'rejected'}}}
                    elif message['operation']=='agent_evaluation.retry':
                        self.assertEqual(message['job_id'],'original')
                        self.assertEqual(set(message),{'operation','request_id','job_id'})
                        value=offline_gepa.read(tree);value.update(rounds=31,model_calls=481,status='searching')
                        offline_gepa.save(tree,value);result={'accepted':True}
                    else:raise AssertionError('new submission would lose original job identity')
                    yield io.StringIO(json.dumps({'ok':True,'result':result}))
            with patch.object(offline_gepa,'require_seed_gates',return_value=artifact), \
                 patch.object(offline_gepa.controller,'environment',return_value={}), \
                 patch.object(offline_gepa.subprocess,'Popen',return_value=process), \
                 patch.object(offline_gepa.select,'select',return_value=([process.stdout],[],[])), \
                 patch.object(offline_gepa.urllib.request,'build_opener',return_value=Opener()), \
                 patch.object(offline_gepa,'progress',return_value={'job_id':'original','job_status':'passed','rounds':31,'model_calls':481}):
                result=offline_gepa.run(seed)
            self.assertEqual(result['job_id'],'original')
            self.assertEqual(operations,['agent_evaluation.status','agent_evaluation.retry'])
            self.assertEqual(offline_gepa.read(tree)['nodes'],json.loads(before)['nodes'])
            self.assertEqual(offline_gepa.read(tree)['trials_used'],1024)
            for path,data in protected.items():self.assertEqual(path.read_bytes(),data)

    def test_rejected_budget_extension_refuses_no_increase_holdout_or_unverified_basis(self):
        for mutation in ('no-increase','holdout','published','active','legacy-exhausted','plan-mismatch'):
            with self.subTest(mutation=mutation),tempfile.TemporaryDirectory() as directory:
                artifact,seed,state_path,tree,protected,stages=self.budget_fixture(directory)
                if mutation=='no-increase':
                    config=offline_gepa.read(offline_gepa.CONFIG/'gepa.json');config['max_rounds']=256
                    offline_gepa.save(offline_gepa.CONFIG/'gepa.json',config)
                    plan=offline_gepa.read(offline_gepa.PLAN);plan['search']['max_rounds']=256;offline_gepa.save(offline_gepa.PLAN,plan)
                elif mutation=='published':
                    state=offline_gepa.read(state_path);state['jobs']['original']['published']=True;offline_gepa.save(state_path,state)
                elif mutation=='plan-mismatch':
                    plan=offline_gepa.read(offline_gepa.PLAN);plan['search']['max_model_calls']=480;offline_gepa.save(offline_gepa.PLAN,plan)
                else:
                    value=offline_gepa.read(tree)
                    if mutation=='holdout':value['holdout_started']=True
                    elif mutation=='active':value['active']={'actual-worker':'healthy'}
                    else:value.update(reflection_exhausted=True,budget_extensions=[])
                    offline_gepa.save(tree,value)
                before=tree.read_bytes()
                if mutation=='plan-mismatch':
                    with self.assertRaisesRegex(RuntimeError,'previous_budget_unverified'):
                        reproduce._request_native_gepa_budget_extension(seed)
                else:self.assertIsNone(reproduce._request_native_gepa_budget_extension(seed))
                self.assertEqual(tree.read_bytes(),before)
                for path,data in protected.items():self.assertEqual(path.read_bytes(),data)

    def test_native_retry_does_not_swallow_wrong_seed_or_existing_holdout(self):
        for mutation in ('wrong-seed','holdout','no-increase','wrong-config'):
            with self.subTest(mutation=mutation),tempfile.TemporaryDirectory() as directory:
                artifact,seed,state_path,tree,_,_=self.budget_fixture(directory)
                requested=reproduce._request_native_gepa_budget_extension(seed)
                plan=offline_gepa.read(offline_gepa.PLAN)
                offline_gepa.save(offline_gepa.CONFIG/'gepa.json',requested)
                if mutation=='wrong-seed':offline_gepa.save(tree.parent.parent/'seed.json',{'wrong':True})
                elif mutation=='holdout':
                    value=offline_gepa.read(tree);value['final_holdout']={'candidates':[{'node':1}]};offline_gepa.save(tree,value)
                elif mutation=='no-increase':plan['budget_extension']['requested_limits']=plan['budget_extension']['previous_limits']
                else:offline_gepa.save(offline_gepa.CONFIG/'gepa.json',{'max_model_calls':512})
                with self.assertRaisesRegex(RuntimeError,'offline_gepa_budget_extension_'):
                    offline_gepa._rejected_budget_extension_ready(plan,artifact,{'status':'rejected'},'original')

    def test_single_outer_command_extends_then_reenters_same_seed_without_submit(self):
        with tempfile.TemporaryDirectory() as directory:
            artifact,seed,state_path,tree,protected,_=self.budget_fixture(directory)
            calls=[]
            def continuation(actual_seed,label,**budgets):
                calls.append((actual_seed,label,budgets))
                self.assertEqual(actual_seed,seed)
                self.assertEqual(budgets['gepa_max_model_calls'],1024)
                self.assertEqual(offline_gepa.read(offline_gepa.PLAN)['budget_extension']['job_id'],'original')
                self.assertEqual(offline_gepa.read(offline_gepa.GEPA/'lifecycle.json')['stage_results']['gepa_run']['status'],'pending')
                return {'status':'published','phase':'publish'}
            with patch.object(reproduce,'_run_all_with_seed_search',side_effect=continuation), \
                 patch.object(reproduce,'_reserve_after_unqualified_native_search') as reserve:
                result=reproduce._run_all_autonomously(seed,seed.parent.name,gepa_max_model_calls=512)
            self.assertEqual(result['status'],'published');self.assertEqual(len(calls),1);reserve.assert_not_called()
            self.assertEqual(offline_gepa.read(tree)['model_calls'],480)
            for path,data in protected.items():self.assertEqual(path.read_bytes(),data)

    def test_hard_cap_reserves_distinct_candidate_preserves_old_job_and_paid(self):
        with tempfile.TemporaryDirectory() as directory:
            artifact,seed,state_path,tree,protected,_=self.budget_fixture(directory,stop='model_budget')
            cfg=offline_gepa.read(offline_gepa.CONFIG/'gepa.json');cfg['max_model_calls']=1024;cfg['max_rounds']=256
            offline_gepa.save(offline_gepa.CONFIG/'gepa.json',cfg)
            plan=offline_gepa.read(offline_gepa.PLAN);plan['search'].update(max_model_calls=1024,max_rounds=256);offline_gepa.save(offline_gepa.PLAN,plan)
            search={'round':2,'status':'searching','attempts':[{'attempt':6,'round':2,'seed':str(seed.resolve()),'seed_label':seed.parent.name,'status':'passed','grade':'original-frozen-grade','seed_generation_diagnostic':{'controls':{'model_calls':3},'english':{'model_calls':3}}}]}
            reproduce._save_search(search)
            before_tree=tree.read_bytes();before_state=state_path.read_bytes()
            self.assertIsNone(reproduce._request_native_gepa_budget_extension(seed))
            candidate,label=reproduce._reserve_after_unqualified_native_search(seed)
            result=reproduce.seed_search()
            self.assertNotEqual(candidate,seed);self.assertFalse(candidate.exists())
            self.assertEqual(result['attempts'],search['attempts'])
            self.assertEqual(result['native_job_history'][0]['job_id'],'original')
            self.assertEqual(result['native_job_history'][0]['model_calls'],480)
            self.assertEqual(result['native_job_history'][0]['submission']['job_id'],'original')
            self.assertEqual(result['automatic_seed_generation']['source_seed'],str(seed.resolve()))
            self.assertEqual(result['automatic_seed_generation']['seed_label'],label)
            self.assertEqual(tree.read_bytes(),before_tree);self.assertEqual(state_path.read_bytes(),before_state)
            for path,data in protected.items():self.assertEqual(path.read_bytes(),data)

    def test_normal_main_retires_old_plan_then_resumes_new_label_after_interrupt(self):
        with tempfile.TemporaryDirectory() as directory:
            artifact,seed,state_path,tree,protected,_=self.budget_fixture(directory,stop='model_budget')
            cfg=offline_gepa.read(offline_gepa.CONFIG/'gepa.json');cfg.update(max_model_calls=1024,max_rounds=256)
            offline_gepa.save(offline_gepa.CONFIG/'gepa.json',cfg)
            plan=offline_gepa.read(offline_gepa.PLAN);plan['search'].update(max_model_calls=1024,max_rounds=256);offline_gepa.save(offline_gepa.PLAN,plan)
            reproduce._save_search({'round':2,'status':'found','current_seed':str(seed.resolve()),
                'attempts':[{'attempt':6,'round':2,'seed':str(seed.resolve()),'status':'passed'}]})
            before=tree.read_bytes();seen=[]
            def interrupted(actual_seed,label,**budgets):
                seen.append((actual_seed,label))
                self.assertNotEqual(actual_seed,seed)
                self.assertFalse(offline_gepa.PLAN.exists())
                self.assertFalse((offline_gepa.GEPA/'submission.json').exists())
                raise KeyboardInterrupt()
            with patch.object(reproduce,'_run_all_with_seed_search',side_effect=interrupted), \
                 patch.object(reproduce,'_native_gepa_service_active',return_value=False), \
                 patch.object(reproduce,'_experiment_status'),redirect_stdout(io.StringIO()):
                with patch.object(sys,'argv',['reproduce.py','--all','--gepa-max-model-calls','1024']):
                    self.assertEqual(reproduce.main(),130)
                    candidate=reproduce.seed_search()['current_seed']
                    self.assertEqual(reproduce.main(),130)
            self.assertEqual(len(seen),2)
            self.assertEqual(seen[0],seen[1]);self.assertEqual(str(seen[0][0]),candidate)
            search=reproduce.seed_search()
            self.assertEqual(len(search['native_job_history']),1)
            self.assertEqual(search['native_job_history'][0]['plan'],plan)
            self.assertEqual(search['native_job_history'][0]['submission'],{'job_id':'original'})
            self.assertEqual(tree.read_bytes(),before)
            for path,data in protected.items():self.assertEqual(path.read_bytes(),data)

class FlattenedJudgeTransportRecoveryTests(unittest.TestCase):
    @contextmanager
    def fixture(self, directory, *, count=32, mutation=None):
        root=Path(directory);seed=root/'published/strategy/strategy.json';seed.parent.mkdir(parents=True)
        seed.write_text('{"level":"L3","policy":{"fixture":true}}')
        gepa=root/'gepa';output=gepa/f'strategy-stage{count}';capture=output/'shards/shard-00/capture';capture.mkdir(parents=True)
        source=capture/'report.json';current=reproduce._APPWORLD_LAYOUT.judge_projection()
        projection=json.loads(json.dumps(current))
        if mutation=='judge':projection['require_state_dependency']=not projection.get('require_state_dependency',True)
        (capture.parent/'config.json').write_text(json.dumps({'judge':projection}))
        rows=[{'task_id':f'task-{i:03d}','trial_id':f'trial-{i:03d}','execution_status':'completed',
            'checks':[{'check_id':'model','status':'error','reason':'evaluation_judge_http_429'}],
            'evidence':{'output':{'framework_capture':{'status':'complete'}},
                        'loop_review':{'status':'error'}}} for i in range(count)]
        if mutation=='execution':rows[0]['evidence']['output']['framework_capture']['status']='incomplete'
        elif mutation=='schema':rows[0]['checks'][0]['reason']='evaluation_judge_schema_invalid'
        elif mutation=='coverage':rows.pop()
        source.write_text(json.dumps({'complete':True,'run_id':'captured-native','dataset_version':'frozen-dataset','trials':rows}))
        source.with_name('manifest.json').write_text(json.dumps({'complete':True,'run_id':'captured-native','dataset_version':'frozen-dataset'}))
        progress={'planned_tasks':count,'artifact':json.loads(seed.read_text()),
            'shards':{'0':{'execution_complete':True,'source_report':str(source)}}}
        if mutation=='artifact':progress['artifact']={'level':'L3','policy':{'wrong':True}}
        (output/'progress.json').write_text(json.dumps(progress))
        state={'phase':'pilot32' if count==32 else 'validation100',
            'candidate':{'seed_artifact':str(seed.resolve())}}
        if mutation=='phase':state['phase']='gepa_run'
        before={path:path.read_bytes() for path in (seed,source,source.with_name('manifest.json'),output/'progress.json')}
        with _isolated_gepa_outputs(root),patch.object(reproduce._APPWORLD_LAYOUT,'GEPA',gepa), \
             patch.object(reproduce,'_CANDIDATE_STATE_LABEL',None):
            yield state,seed,source,before
        for path,data in before.items():self.assertEqual(path.read_bytes(),data)

    def test_flattened_compare_transport_recovers_both_complete_native_scopes(self):
        for count in (32,100):
            with self.subTest(count=count),tempfile.TemporaryDirectory() as directory,self.fixture(directory,count=count) as (state,seed,source,before):
                self.assertEqual(reproduce._stage_review_recovery_state(source.parent.parent.parent.parent,count,baseline=False),'transport')
                for reason in ('compare.py_failed:1','RuntimeError:compare.py_failed:1'):
                    self.assertEqual(reproduce._autonomous_retry_reason(RuntimeError(reason),state),'appworld_stage_review_pending')

    def test_partial_capture_wrong_judge_schema_artifact_or_phase_cannot_retry(self):
        for mutation in ('execution','judge','schema','coverage','artifact','phase'):
            with self.subTest(mutation=mutation),tempfile.TemporaryDirectory() as directory,self.fixture(directory,mutation=mutation) as (state,seed,source,before):
                self.assertIsNone(reproduce._autonomous_retry_reason(RuntimeError('RuntimeError:compare.py_failed:1'),state))

    def test_one_outer_command_backoff_reuses_completed_capture_and_seed(self):
        with tempfile.TemporaryDirectory() as directory,self.fixture(directory) as (state,seed,source,before):
            state_path=Path(directory)/'gepa/lifecycle.json'
            reproduce._APPWORLD_LAYOUT.save(state_path,state)
            calls=[]
            def run(actual_seed,label,**budgets):
                calls.append(actual_seed)
                if len(calls)==1:raise RuntimeError('RuntimeError:compare.py_failed:1')
                return {'status':'complete','phase':'publish'}
            with patch.object(reproduce,'_run_all_with_seed_search',side_effect=run), \
                 patch.object(reproduce,'_experiment_status'),patch.object(reproduce.time,'sleep') as sleep:
                result=reproduce._run_all_autonomously(seed,'strategy')
            self.assertEqual(result['status'],'complete');self.assertEqual(calls,[seed,seed])
            sleep.assert_called_once_with(15)

class WorkerCheckpointRecoveryTests(unittest.TestCase):
    def test_internal_readonly_check_uses_frozen_capture_and_waits_for_live_writer(self):
        helper=FlattenedJudgeTransportRecoveryTests()
        with tempfile.TemporaryDirectory() as directory,helper.fixture(directory) as (state,seed,source,before):
            state.update(status='blocked',stop_reason='compare.py_failed:1')
            lifecycle=Path(directory)/'gepa/lifecycle.json';reproduce._APPWORLD_LAYOUT.save(lifecycle,state)
            with patch.object(reproduce,'_active_output_processes',return_value=[]) as processes:
                self.assertEqual(reproduce._worker_resume_check(),75)
                with patch.object(sys,'argv',['reproduce.py','--worker-resume-check']):self.assertEqual(reproduce.main(),75)
            with patch.object(reproduce,'_active_output_processes',return_value=[123]):
                self.assertEqual(reproduce._worker_resume_check(),76)
            self.assertEqual(reproduce._worker_checkpoint_seed(state),seed)
            self.assertEqual(reproduce._read(lifecycle),state)

    def test_unknown_authority_fault_terminal_or_user_cancel_does_not_restart(self):
        helper=FlattenedJudgeTransportRecoveryTests()
        for change in ('unknown','complete','cancel','wrong-judge','incomplete-capture'):
            with self.subTest(change=change),tempfile.TemporaryDirectory() as directory, \
                 helper.fixture(directory,mutation={'wrong-judge':'judge','incomplete-capture':'execution'}.get(change)) as (state,seed,source,before):
                state.update(status='blocked',stop_reason='compare.py_failed:1')
                if change=='unknown':state['stop_reason']='appworld_frozen_seed_validated_source_missing'
                elif change=='complete':state['status']='complete'
                elif change=='cancel':reproduce._APPWORLD_LAYOUT.save(Path(directory)/'gepa/status.json',{'experiment':{'reason':'user_interrupt'}})
                reproduce._APPWORLD_LAYOUT.save(Path(directory)/'gepa/lifecycle.json',state)
                with patch.object(reproduce,'_active_output_processes') as processes:
                    self.assertEqual(reproduce._worker_resume_check(),0)
                    processes.assert_not_called()

    def test_retry_wait_retains_reserved_label_and_spent_seed_diagnostic(self):
        with tempfile.TemporaryDirectory() as directory,_isolated_gepa_outputs(Path(directory)):
            root=Path(directory);source=root/'published/original/strategy.json';source.parent.mkdir(parents=True);source.write_text('{"level":"L3"}')
            candidate=root/'published/reserved/strategy.json'
            state={'phase':'seed','status':'retry_wait','candidate':{'seed_artifact':str(source)},
                   'autonomous_recovery':{'reason':'model_http_429','retry':2}}
            search={'round':2,'current_seed':str(candidate),'automatic_seed_generation':{'round':2,'status':'pending','seed':str(candidate),'seed_label':'reserved','source_seed':str(source)},'generation_history':[{'seed_generation_diagnostic':{'model_calls':3}}]}
            reproduce._save_search(search);reproduce._APPWORLD_LAYOUT.save(root/'gepa/lifecycle.json',state)
            before=(root/'gepa/status.json').read_bytes()
            with patch.object(reproduce,'_active_output_processes',return_value=[]):
                self.assertEqual(reproduce._worker_resume_check(),75)
                self.assertEqual(reproduce._worker_checkpoint_seed(state),candidate)
            self.assertEqual((root/'gepa/status.json').read_bytes(),before)

    def run_worker_shell(self, *, exits, checks, action='--all'):
        import os
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);here=root/'tools/trajectory_evolution/experiments/appworld';here.mkdir(parents=True)
            here.joinpath('reproduce.sh').write_bytes((EXPERIMENT / 'reproduce.sh').read_bytes())
            bindir=root/'bin';bindir.mkdir();state=root/'fake-state.json';calls=root/'fake-calls.jsonl';delays=root/'fake-delays'
            state.write_text(json.dumps({'exits':list(exits),'checks':list(checks)}))
            fake=bindir/'fake-python'
            fake.write_text('#!'+sys.executable+'\n'+'''import json,os,sys\nfrom pathlib import Path\ns=Path(os.environ['FAKE_STATE']);v=json.loads(s.read_text());a=sys.argv[1:]\nwith Path(os.environ['FAKE_CALLS']).open('a') as f:f.write(json.dumps(a)+'\\n')\nk='checks' if '--worker-resume-check' in a else 'exits'\ncode=v[k].pop(0) if v[k] else 0\ns.write_text(json.dumps(v));raise SystemExit(code)\n''');fake.chmod(0o755)
            sleeper=bindir/'sleep';sleeper.write_text('#!/bin/sh\nprintf "%s\\n" "$1" >> "$FAKE_DELAYS"\n');sleeper.chmod(0o755)
            env=dict(os.environ,AGENTFLOW_PYTHON=str(fake),PATH=str(bindir)+os.pathsep+os.environ['PATH'],FAKE_STATE=str(state),FAKE_CALLS=str(calls),FAKE_DELAYS=str(delays));env.pop('TMUX_PANE',None)
            result=subprocess.run(['bash',str(here/'reproduce.sh'),'--worker',action,'--gepa-max-model-calls','1024'],env=env,cwd=root,timeout=10,capture_output=True,text=True)
            return result.returncode,[json.loads(x) for x in calls.read_text().splitlines()],delays.read_text().splitlines() if delays.exists() else [],(root/'trace_output/appworld/training/run.log').read_text()

    def test_real_worker_shell_restarts_only_qualified_failure_and_keeps_entry_args(self):
        code,calls,delays,log=self.run_worker_shell(exits=[1,0],checks=[75,0])
        actions=[row for row in calls if '--worker-resume-check' not in row]
        self.assertEqual(code,0);self.assertEqual(len(actions),2)
        self.assertEqual(delays,['15'])
        self.assertIn('--all',actions[0]);self.assertIn('--all',actions[1])
        self.assertNotIn('--worker-checkpoint-resume',actions[0]);self.assertIn('--worker-checkpoint-resume',actions[1])
        self.assertIn('--gepa-max-model-calls',actions[1]);self.assertIn('1024',actions[1])
        self.assertIn('appworld_controller_exit_code=0',log)

    def test_worker_waits_for_writer_and_never_repeats_reset_or_user_cancel(self):
        code,calls,delays,_=self.run_worker_shell(exits=[1,0],checks=[76,75,0],action='--restart-gepa')
        actions=[row for row in calls if '--worker-resume-check' not in row]
        self.assertEqual((code,delays),(0,['5','15']))
        self.assertIn('--restart-gepa',actions[0]);self.assertNotIn('--restart-gepa',actions[1]);self.assertIn('--all',actions[1])
        for exit_code in (130,143):
            with self.subTest(exit_code=exit_code):
                code,calls,delays,_=self.run_worker_shell(exits=[exit_code],checks=[75])
                self.assertEqual((code,len(calls),delays),(exit_code,1,[]))
        code,calls,delays,_=self.run_worker_shell(exits=[1],checks=[0])
        self.assertEqual((code,len(calls),delays),(1,2,[]))


class RepairedProjectionCheckpointTests(unittest.TestCase):
    @contextmanager
    def fixture(self, directory, *, mutation=None):
        helper=FlattenedJudgeTransportRecoveryTests()
        with helper.fixture(directory) as (state,seed,source,before):
            root=Path(directory);row=json.loads(source.read_text())
            trial,run,task='a'*32,'b'*40,'appworld-fixture_1'
            official=root/'official-evidence'/('agentflow-'+'c'*32)
            log=official/'tasks/fixture_1/logs/environment_io.md';log.parent.mkdir(parents=True)
            code='print("executed fixture")';log.write_text('```python\n'+code+'\n```\n\n```\nExecution successful.\n```\n')
            failed='print("failed proposal")'
            events=[{'event_id':'event-1','sequence':'1','kind':'run.start','input':{'task_id':task}},
                    {'event_id':'decision-1','sequence':'2','kind':'loop.decision','loop_id':'main','caused_by':['event-1']},
                    {'event_id':'failed-start','sequence':'3','kind':'action.start','actor_id':'skill_executor','action_id':'failed','caused_by':['decision-1'],'input':{'code':failed}},
                    {'event_id':'failed-end','sequence':'4','kind':'action.end','actor_id':'skill_executor','action_id':'failed','caused_by':['failed-start'],'input':{'code':failed},'attributes':{'executed':True,'extensions':{'source_event_name':'skill_executor.tool_result'}},'output':{'status':'error','tool_result':json.dumps({'error':'fixture failure'})}},
                    {'event_id':'world-start','sequence':'5','kind':'action.start','loop_id':'main','action_id':'world','caused_by':['decision-1'],'input':{'code':code}},
                    {'event_id':'world-end','sequence':'6','kind':'action.end','loop_id':'main','action_id':'world','caused_by':['world-start'],'input':{'code':code},'attributes':{'executed':True},'output':{'observation':'Execution successful.'}}]
            evidence={'execution_run_id':run,'execution_status':'completed',
                      'output':{'framework_capture':{'status':'complete'},'official_evidence_dir':str(official),'official_experiment_name':official.name},
                      'trace':{'identity':{'run_id':run,'conversation_id':trial},'events':events},
                      'evidence_projection':{'status':'error','error_code':'evaluation_evidence_projection_unavailable'}}
            native={'task_id':task,'trial_id':trial,'execution_status':'completed',
                    'checks':[{'check_id':'model','status':'error','reason':'evaluation_evidence_projection_unavailable'}],'evidence':evidence}
            if mutation=='schema':native['checks'][0]['reason']='evaluation_judge_schema_invalid'
            elif mutation=='world_binding':
                events[2]['input']['code']=code;events[3]['input']['code']=code
            elif mutation=='missing_io':log.unlink()
            elif mutation=='incomplete_capture':evidence['output']['framework_capture']['status']='incomplete'
            row['trials'][0]=native;source.write_text(json.dumps(row))
            atomics=source.parent/'trials';atomics.mkdir();atomic=atomics/(trial+'.json');atomic.write_text(json.dumps(native))
            if mutation=='atomic_mismatch':
                changed=json.loads(atomic.read_text());changed['evidence']['trace']['events'][0]['input']['unverified']=True
                atomic.write_text(json.dumps(changed))
            comparison={'status':'partial','seed':str(seed),'items':[{'task_id':r['task_id'],'execution_status':'completed'} for r in row['trials']],
                        'metrics':{'candidate':{'planned':32,'completed':32,'missing':0,'execution_errors':0,'judge_errors':1}},'review_distribution':{'complete':31,'missing':1}}
            comparison_path=source.parents[3]/'comparison.json';comparison_path.write_text(json.dumps(comparison))
            before.clear();before.update({path:path.read_bytes() for path in (seed,source,source.with_name('manifest.json'),atomic,comparison_path,source.parents[3]/'progress.json')})
            state.update(status='blocked',stop_reason='stage_gate_not_passed:pilot32',stage_results={'pilot32':{'status':'complete','gate_passed':False,'data':{'callback':'appworld.stage32','comparison':str(comparison_path)}}})
            yield state,seed,source,before,comparison
            for path,data in before.items():self.assertEqual(path.read_bytes(),data)

    def test_actual_flattened_gate_and_worker_qualify_only_repaired_native_projection(self):
        with tempfile.TemporaryDirectory() as directory,self.fixture(directory) as (state,seed,source,before,comparison):
            self.assertEqual(reproduce._stage_review_recovery_state(source.parents[3],32,baseline=False),'projection')
            for reason in ('stage_gate_not_passed:pilot32','RuntimeError:stage_gate_not_passed:pilot32','appworld_stage_review_pending'):
                self.assertEqual(reproduce._autonomous_retry_reason(RuntimeError(reason),state),'appworld_stage_review_pending')
            self.assertTrue(reproduce._comparison_review_pending(comparison,seed,32))
            reproduce._APPWORLD_LAYOUT.save(Path(directory)/'gepa/lifecycle.json',state)
            with patch.object(reproduce,'_active_output_processes',return_value=[]):self.assertEqual(reproduce._worker_resume_check(),75)

    def test_missing_review_reopens_legacy_complete_receipt_without_releasing_gate(self):
        with tempfile.TemporaryDirectory() as directory,self.fixture(directory) as (state,seed,source,before,comparison):
            candidate=dict(state['candidate'])
            self.assertTrue(reproduce._resume_pending_comparison_receipts(state,seed,candidate,None))
            self.assertEqual(state['stage_results']['pilot32']['status'],'pending')
            self.assertIsNone(state['stage_results']['pilot32']['gate_passed'])
            self.assertEqual(state['candidate'],candidate)
            self.assertFalse(reproduce._comparison_review_pending(dict(comparison,status='complete'),seed,32))

    def test_unknown_schema_failed_binding_missing_io_and_incomplete_capture_do_not_restart(self):
        for mutation in ('schema','world_binding','missing_io','incomplete_capture','atomic_mismatch'):
            with self.subTest(mutation=mutation),tempfile.TemporaryDirectory() as directory,self.fixture(directory,mutation=mutation) as (state,seed,source,before,comparison):
                for reason in ('stage_gate_not_passed:pilot32','appworld_stage_review_pending'):
                    self.assertIsNone(reproduce._autonomous_retry_reason(RuntimeError(reason),state))
                reproduce._APPWORLD_LAYOUT.save(Path(directory)/'gepa/lifecycle.json',state)
                with patch.object(reproduce,'_active_output_processes') as processes:
                    self.assertEqual(reproduce._worker_resume_check(),0);processes.assert_not_called()

    def test_one_command_backoff_keeps_the_same_saved_projection_source(self):
        with tempfile.TemporaryDirectory() as directory,self.fixture(directory) as (state,seed,source,before,comparison):
            reproduce._APPWORLD_LAYOUT.save(Path(directory)/'gepa/lifecycle.json',state);calls=[]
            def run(actual_seed,label,**budgets):
                calls.append(actual_seed)
                if len(calls)==1:raise RuntimeError('stage_gate_not_passed:pilot32')
                return {'status':'complete','phase':'publish'}
            with patch.object(reproduce,'_run_all_with_seed_search',side_effect=run),patch.object(reproduce,'_experiment_status'),patch.object(reproduce.time,'sleep') as sleep:
                result=reproduce._run_all_autonomously(seed,'strategy')
            self.assertEqual(result['status'],'complete');self.assertEqual(calls,[seed,seed]);sleep.assert_called_once_with(15)


class RateLimitBudgetSubprocessRecoveryTests(unittest.TestCase):
    fixture = OfflineGepaRecoveryTests.fixture
    budget_fixture = NativeCumulativeBudgetRecoveryTests.budget_fixture

    def rate_fixture(self, directory, *, stop='model_budget'):
        artifact, seed, state_path, tree, protected, stages = self.budget_fixture(directory, stop=stop)
        jobs = offline_gepa.read(state_path)
        jobs['jobs']['original'].update(status='error', error='gepa_model_rate_limited')
        offline_gepa.save(state_path, jobs)
        value = offline_gepa.read(tree)
        value.update(status='error', error='gepa_model_rate_limited', model_calls=512,
                     started_ms=1, active=1, batch={'retained_pending_maps': True})
        if stop != 'model_budget':
            value['model_calls'] = 480
        offline_gepa.save(tree, value)
        offline_gepa.save(offline_gepa.STATUS, {'job_id': 'original', 'job_status': 'error'})
        lifecycle = offline_gepa.read(offline_gepa.GEPA / 'lifecycle.json')
        lifecycle.update(status='pending', phase='gepa_run')
        lifecycle['stage_results']['gepa_run']['data']['job_id'] = 'original'
        offline_gepa.save(offline_gepa.GEPA / 'lifecycle.json', lifecycle)
        scripts = Path(directory) / 'child-entry'; scripts.mkdir()
        # This is a real isolated child process, standing in for the native
        # budget closure tested independently by --rate-limit-budget. Its
        # first exit is flattened by production reproduce.run(), not a mock.
        (scripts / 'offline_gepa.py').write_text("""import json,os,sys
from pathlib import Path
root=Path(sys.argv[1]); stop=sys.argv[2]
assert os.environ.get('AGENTFLOW_SHARED_LIFECYCLE')=='1'
submission=json.loads((root/'gepa/submission.json').read_text())
plan=json.loads((root/'gepa/plan.json').read_text())
assert sys.argv[3]==plan['seed']
ledger=root/'child-calls.json';calls=json.loads(ledger.read_text()) if ledger.exists() else []
calls.append({'job_id':submission['job_id'],'seed':sys.argv[3]});ledger.write_text(json.dumps(calls))
if len(calls)==1 or stop=='reject':sys.exit(1)
treepath=root/'gepa/workspace/agent-evaluation'/submission['job_id']/'search/tree.json'
tree=json.loads(treepath.read_text());tree.update(status='validation_rejected',stop_reason=stop,active=None,batch=None);tree.pop('error',None);treepath.write_text(json.dumps(tree))
statepath=root/'gepa/workspace/agent-evaluation/state.json';state=json.loads(statepath.read_text());state['jobs'][submission['job_id']].update(status='rejected');statepath.write_text(json.dumps(state))
(root/'gepa/status.json').write_text(json.dumps({'job_id':submission['job_id'],'job_status':'rejected'}))
""")
        return seed, state_path, tree, protected, scripts

    def test_actual_child_exit_rate_limit_retries_same_job_then_extends_cumulative_budget(self):
        for stop in ('model_budget', 'gepa_search_time_reserved'):
            with self.subTest(stop=stop), tempfile.TemporaryDirectory() as directory:
                seed, state_path, tree, protected, scripts = self.rate_fixture(directory, stop=stop)
                old = offline_gepa.read(tree)
                calls = []
                def shared_prefix(actual_seed, label, **budgets):
                    calls.append((actual_seed, label, budgets))
                    if len(calls) <= 2:
                        reproduce.run('offline_gepa.py', directory, stop, str(actual_seed))
                        return {'status':'not_qualified', 'stop_reason':'appworld_gepa_no_new_qualified_candidate'}
                    receipt = offline_gepa.read(offline_gepa.PLAN)['budget_extension']
                    self.assertEqual(receipt['job_id'], 'original')
                    self.assertEqual(receipt['model_calls_used'], old['model_calls'])
                    self.assertEqual(receipt['previous_limits']['max_model_calls'], 512)
                    self.assertEqual(budgets['gepa_max_model_calls'], 1024)
                    self.assertEqual(offline_gepa.read(offline_gepa.GEPA/'lifecycle.json')['stage_results']['gepa_run']['status'],'pending')
                    return {'status':'complete', 'phase':'gepa_prepare'}
                with patch.object(reproduce,'HERE',scripts), patch.object(reproduce,'ROOT',Path(directory)), \
                     patch.object(reproduce,'_run_all_with_seed_search',side_effect=shared_prefix), \
                     patch.object(reproduce,'_experiment_status'), patch.object(reproduce.time,'sleep') as sleep, \
                     patch.object(reproduce,'_reserve_after_unqualified_native_search') as reserve, redirect_stdout(io.StringIO()):
                    result=reproduce._run_all_autonomously(seed,'strategy',gepa_max_model_calls=512)
                self.assertEqual(result['status'],'complete')
                self.assertEqual([x[0] for x in calls],[seed,seed,seed])
                sleep.assert_called_once_with(15);reserve.assert_not_called()
                ledger=json.loads((Path(directory)/'child-calls.json').read_text())
                self.assertEqual(ledger,[{'job_id':'original','seed':str(seed)}]*2)
                current=offline_gepa.read(tree)
                for key in ('model_calls','trials_used','nodes','started_ms','budget_extensions'):
                    self.assertEqual(current[key],old[key])
                for path,data in protected.items():self.assertEqual(path.read_bytes(),data)

    def test_actual_child_exit_cannot_reclassify_changed_source_holdout_cancel_or_unknown_error(self):
        for mutation in ('frozen-seed','receipt-job','lifecycle-job','unknown-error','holdout','replay','cancel'):
            with self.subTest(mutation=mutation),tempfile.TemporaryDirectory() as directory:
                seed,state_path,tree,protected,scripts=self.rate_fixture(directory)
                if mutation=='frozen-seed':
                    offline_gepa.save(tree.parent.parent/'seed.json',{'different_source':True})
                elif mutation=='receipt-job':
                    offline_gepa.save(offline_gepa.STATUS,{'job_id':'newer','job_status':'error'})
                elif mutation=='lifecycle-job':
                    path=offline_gepa.GEPA/'lifecycle.json';value=offline_gepa.read(path)
                    value['stage_results']['gepa_run']['data']['job_id']='newer';offline_gepa.save(path,value)
                elif mutation=='holdout':
                    value=offline_gepa.read(tree);value['holdout_started']=True;offline_gepa.save(tree,value)
                else:
                    value=offline_gepa.read(state_path);job=value['jobs']['original']
                    if mutation=='unknown-error':job['error']='gepa_source_not_approved'
                    elif mutation=='replay':job['final_holdout_replay']=True
                    else:job['cancel_requested']=True
                    offline_gepa.save(state_path,value)
                before={path:path.read_bytes() for path in (tree,state_path,seed,offline_gepa.PLAN)}
                def shared_prefix(actual_seed,label,**budgets):
                    reproduce.run('offline_gepa.py',directory,'reject',str(actual_seed))
                with patch.object(reproduce,'HERE',scripts),patch.object(reproduce,'ROOT',Path(directory)), \
                     patch.object(reproduce,'_run_all_with_seed_search',side_effect=shared_prefix), \
                     patch.object(reproduce,'_experiment_status'),patch.object(reproduce.time,'sleep') as sleep,redirect_stdout(io.StringIO()):
                    result=reproduce._run_all_autonomously(seed,'strategy')
                self.assertEqual(result['status'],'blocked');sleep.assert_not_called()
                self.assertEqual(len(json.loads((Path(directory)/'child-calls.json').read_text())),1)
                for path,data in before.items():self.assertEqual(path.read_bytes(),data)
                for path,data in protected.items():self.assertEqual(path.read_bytes(),data)


class GenerationReceiptRegressionTests(unittest.TestCase):
    def test_generation_receipt_attempt_retains_fees_before_gate_without_draft_copy(self):
        with tempfile.TemporaryDirectory(dir='/tmp') as directory:
            root = Path(directory)
            seed = root/'published/candidate-receipt/strategy.json'
            seed.parent.mkdir(parents=True)
            artifact = {'level': 'L3', 'policy': {'fixture': True}}
            seed.write_text(json.dumps(artifact))
            receipt = {'schema_version': 1, 'seed_label': seed.parent.name,
                'control_learning': {'status': 'generated', 'model_calls': 2,
                    'validated_model_draft': {'action_rules': [], 'loop_rules': []},
                    'attempts': [{'attempt': 1, 'status': 'rejected', 'usage': None},
                        {'attempt': 2, 'status': 'generated', 'usage': {'total_tokens': 11}}]},
                'english_trigger_learning': {'status': 'compiled', 'model_calls': 3, 'attempts': [],
                    'incremental_generation': {'model_calls': 3, 'model_attempts': 3,
                        'batches': [{'attempt': 3, 'status': 'compiled', 'usage': {'total_tokens': 17},
                            'draft': [{'memory_id': 'approved-fixture', 'trigger_pattern': 'login access_token'}]}]}}}
            manifest = seed.with_name('manifest.json')
            manifest.write_text(json.dumps({'version': 'fixture-native-version', 'generation_receipt': receipt}))
            with patch.object(reproduce, 'OUTPUT', root), \
                 patch.object(reproduce._APPWORLD_LAYOUT, 'version', return_value='fixture-native-version'):
                attempt = reproduce.begin_seed_attempt(seed)
                summary = attempt['seed_generation_receipt']
                self.assertEqual(summary['manifest_ref'], str(manifest.resolve()))
                self.assertEqual(summary['control_learning']['model_calls'], 2)
                self.assertEqual(summary['english_trigger_learning']['model_calls'], 3)
                self.assertIsNone(summary['control_learning']['attempts'][0]['usage'])
                self.assertTrue(all(key not in json.dumps(summary) for key in
                    ('validated_model_draft', 'trigger_pattern', 'source_experiences', 'snapshot')))
                self.assertNotIn('seed_generation_diagnostic', attempt)
                self.assertEqual(reproduce.seed_search()['attempts'][0], attempt)
                self.assertEqual(reproduce.begin_seed_attempt(seed)['seed_generation_receipt'], summary)
                # A different native version cannot claim these immutable paid receipts.
                manifest.write_text(json.dumps({'version': 'wrong-version', 'generation_receipt': receipt}))
                with self.assertRaisesRegex(RuntimeError, 'generation_receipt_mismatch'):
                    reproduce._retain_seed_generation_receipt({}, seed)

class UnavailableSeedGenerationRecoveryTests(unittest.TestCase):
    @contextmanager
    def fixture(self,directory):
        root=Path(directory);source=root/'published/strategy/strategy.json';source.parent.mkdir(parents=True)
        source.write_text('{"level":"L3","policy":{"source":"approved-fixture"}}')
        seed=root/'published/seed-auto-r1-a2/strategy.json'
        gepa=root/'gepa';gepa.mkdir()
        state={'phase':'seed','candidate':{'seed_artifact':str(seed)},'stage_results':{
            'prepare':{'status':'complete','data':{'original':True}},
            'train':{'status':'complete','data':{'original':True}},
            'seed':{'status':'failed','reason':'RuntimeError:appworld_english_trigger_no_evidence_supported_fallback'}}}
        search={'round':1,'status':'searching','attempts':[], 'current_seed':str(seed),
            'automatic_seed_generation':{'round':1,'status':'pending','seed':str(seed),
                'seed_label':seed.parent.name,'source_seed':str(source),'source_attempt':1,
                'method':'approved_training_native_seed'}}
        diagnostic={'seed_label':seed.parent.name,'validated_evidence_required':True,
            'control_learning':{'model_calls':2,'status':'generated'},
            'english_trigger_learning':{'model_calls':3,'status':'specific_conditions_failed',
                'attempts':[{'attempt':i,'status':'rejected','usage':{'total_tokens':i*10}} for i in (1,2,3)],
                'evidence_fallback':{'status':'no_native_valid_trigger','model_calls':0}}}
        with _isolated_gepa_outputs(root),patch.object(reproduce._APPWORLD_LAYOUT,'GEPA',gepa), \
             patch.object(reproduce,'_CANDIDATE_STATE_LABEL',None):
            reproduce._APPWORLD_LAYOUT.save(gepa/'lifecycle.json',state)
            reproduce._save_search(search)
            reproduce._APPWORLD_LAYOUT.save(root/'training/seed-compile-status.json',diagnostic)
            protected={source:source.read_bytes()}
            for name in ('training/healthy-report.json','gepa/l0-current-stage32/result.json'):
                path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text('{"retained_original":true}')
                protected[path]=path.read_bytes()
            yield seed,source,state,diagnostic,protected
            for path,data in protected.items():self.assertEqual(path.read_bytes(),data)

    def test_one_command_archives_unavailable_generation_and_continues_new_label(self):
        with tempfile.TemporaryDirectory() as directory,self.fixture(directory) as (seed,source,state,diagnostic,protected):
            calls=[]
            def shared_prefix(actual_seed,label,**budgets):
                calls.append((actual_seed,label))
                if len(calls)==1:raise RuntimeError('RuntimeError:appworld_english_trigger_no_evidence_supported_fallback')
                search=reproduce.seed_search()
                old=search['generation_history'][0]
                self.assertEqual(old['seed'],str(seed));self.assertEqual(old['status'],'unsupported')
                self.assertEqual(old['seed_generation_diagnostic'],diagnostic)
                self.assertEqual(search['attempts'],[])
                self.assertEqual(search['automatic_seed_generation']['source_seed'],str(source))
                self.assertNotEqual(actual_seed,seed);self.assertFalse(actual_seed.exists())
                self.assertEqual(label,actual_seed.parent.name)
                self.assertEqual(reproduce._read(Path(directory)/'gepa/lifecycle.json')['stage_results']['train'],state['stage_results']['train'])
                return {'status':'complete','phase':'seed'}
            with patch.object(reproduce,'_run_all_with_seed_search',side_effect=shared_prefix), \
                 patch.object(reproduce,'_experiment_status'),patch.object(reproduce.time,'sleep') as sleep,redirect_stdout(io.StringIO()):
                result=reproduce._run_all_autonomously(seed,seed.parent.name)
            self.assertEqual(result['status'],'complete');self.assertEqual(len(calls),2);sleep.assert_not_called()
            self.assertEqual(reproduce._read(Path(directory)/'training/seed-compile-status.json'),diagnostic)

    def test_nested_seed_search_failure_resumes_actual_pending_after_two_scored_candidates(self):
        with tempfile.TemporaryDirectory() as directory, AutomaticSeedSearchTests.fixture(self,directory,
                passed_after=2) as (initial,root,events,generated,retained):
            original_freeze=reproduce.seed_with_recovery.side_effect
            real_search=reproduce._run_all_with_seed_search
            generation_calls=[];outer_calls=[];failed=[]
            def freeze(*,label):
                generation_calls.append(label)
                if len(generation_calls)==2:
                    seed=root/'published'/label/'strategy.json';failed.append(seed)
                    diagnostic={'seed_label':label,'validated_evidence_required':True,
                        'control_learning':{'model_calls':2,'status':'generated'},
                        'english_trigger_learning':{'model_calls':3,'status':'specific_conditions_failed',
                            'attempts':[{'attempt':i,'status':'rejected','usage':{'total_tokens':i*10}}
                                        for i in (1,2,3)],
                            'evidence_fallback':{'status':'no_native_valid_trigger','model_calls':0}}}
                    reproduce._APPWORLD_LAYOUT.save(root/'training/seed-compile-status.json',diagnostic)
                    raise RuntimeError('appworld_english_trigger_no_evidence_supported_fallback')
                return original_freeze(label=label)
            def prefix(actual_seed,label,**budgets):
                outer_calls.append(actual_seed)
                value=real_search(actual_seed,label,**budgets)
                # Stop this model-free fixture after the real unchanged
                # 32/100 gates and native stage boundaries have succeeded.
                self.assertEqual(value['status'],'not_qualified')
                return {'status':'complete','phase':'gepa_run'}
            with patch.object(reproduce,'seed_with_recovery',side_effect=freeze), \
                 patch.object(reproduce,'_run_all_with_seed_search',side_effect=prefix):
                result=reproduce._run_all_autonomously(initial,'strategy')
            self.assertEqual(result['status'],'complete')
            self.assertEqual(outer_calls[0],initial)
            self.assertNotEqual(outer_calls[1],initial)
            self.assertNotEqual(outer_calls[1],failed[0])
            self.assertEqual(len(generation_calls),3)
            search=reproduce.seed_search();old=search['generation_history'][0]
            self.assertEqual(old['seed'],str(failed[0]));self.assertEqual(old['status'],'unsupported')
            self.assertNotEqual(old['source_seed'],str(initial))
            self.assertEqual(old['seed_generation_diagnostic']['english_trigger_learning']['model_calls'],3)
            self.assertEqual(old['seed_generation_diagnostic']['control_learning']['model_calls'],2)
            self.assertEqual([row['status'] for row in search['attempts']],['failed','failed','passed'])
            self.assertEqual(events,[(0,32),(1,32),(2,32),(2,100),'--prepare','--run'])

    def test_old_worker_readonly_check_allows_main_to_advance_saved_local_failure(self):
        with tempfile.TemporaryDirectory() as directory,self.fixture(directory) as (seed,source,state,diagnostic,protected):
            root=Path(directory);state.update(status='blocked',stop_reason='appworld_stage_failed')
            reproduce._APPWORLD_LAYOUT.save(root/'gepa/lifecycle.json',state)
            value=reproduce._read(root/'gepa/status.json')
            value['experiment']={'status':'blocked','reason':'appworld_stage_failed'}
            reproduce._APPWORLD_LAYOUT.save(root/'gepa/status.json',value)
            before={path:path.read_bytes() for path in root.rglob('*.json')}
            with patch.object(reproduce,'_active_output_processes',return_value=[]):
                self.assertEqual(reproduce._worker_resume_check(),75)
            for path,data in before.items():self.assertEqual(path.read_bytes(),data)
            calls=[]
            def prefix(actual_seed,label,**budgets):
                calls.append(actual_seed)
                if len(calls)==1:raise RuntimeError('RuntimeError:appworld_english_trigger_no_evidence_supported_fallback')
                return {'status':'complete','phase':'seed'}
            with patch.object(sys,'argv',['reproduce.py','--all','--worker-checkpoint-resume']), \
                 patch.object(reproduce,'_active_output_processes',return_value=[]), \
                 patch.object(reproduce,'_run_all_with_seed_search',side_effect=prefix), \
                 patch.object(reproduce,'_experiment_status'),redirect_stdout(io.StringIO()):
                self.assertEqual(reproduce.main(),0)
            self.assertEqual(calls[0],seed);self.assertNotEqual(calls[1],seed)
            old=reproduce.seed_search()['generation_history'][0]
            self.assertEqual(old['seed_generation_diagnostic'],diagnostic)

    def test_unavailable_generation_requires_exact_paid_native_and_source_receipts(self):
        for mutation in ('paid-two','paid-bool','wrong-label','wrong-candidate','fallback-pending','source-missing','frozen-artifact','source-error'):
            with self.subTest(mutation=mutation),tempfile.TemporaryDirectory() as directory,self.fixture(directory) as (seed,source,state,diagnostic,protected):
                error=RuntimeError('appworld_english_trigger_no_evidence_supported_fallback')
                if mutation=='paid-two':diagnostic['english_trigger_learning']['model_calls']=2
                elif mutation=='paid-bool':diagnostic['english_trigger_learning']['model_calls']=True
                elif mutation=='wrong-label':diagnostic['seed_label']='other-candidate'
                elif mutation=='wrong-candidate':state['candidate']['seed_artifact']=str(source)
                elif mutation=='fallback-pending':diagnostic['english_trigger_learning']['evidence_fallback']['status']='pending'
                elif mutation=='source-missing':
                    search=reproduce.seed_search();search['automatic_seed_generation']['source_seed']=str(source.parent.parent/'missing/strategy.json');reproduce._save_search(search)
                elif mutation=='frozen-artifact':seed.parent.mkdir(parents=True);seed.write_text('{"already_frozen":true}')
                else:error=RuntimeError('native_runtime_recompile_failed')
                reproduce._APPWORLD_LAYOUT.save(Path(directory)/'training/seed-compile-status.json',diagnostic)
                before=(Path(directory)/'gepa/status.json').read_bytes()
                self.assertIsNone(reproduce._continue_unavailable_seed_generation(seed,error,state))
                self.assertEqual((Path(directory)/'gepa/status.json').read_bytes(),before)


class InterruptedWorkerCheckpointTests(unittest.TestCase):
    @contextmanager
    def seed_fixture(self, directory):
        root=Path(directory);source=root/'published/original/strategy.json';source.parent.mkdir(parents=True)
        source.write_text(json.dumps({'level':'L3','policy':{'snapshot':{'experiences':[
            {'memory_id':'experience-a','content_revision':'revision-a','target_scope':reproduce._APPWORLD_LAYOUT.SCOPE}],
            'identity':{'user_id':reproduce._APPWORLD_LAYOUT.OWNER,'agent_id':reproduce._APPWORLD_LAYOUT.AGENT},
            'options':{'require_validated_evidence':True,'target_scopes':[reproduce._APPWORLD_LAYOUT.SCOPE]}},'options':{
            'approved_experiences':[{'memory_id':'experience-a','content_revision':'revision-a'}]}}}))
        source.with_name('manifest.json').write_text(json.dumps({'validated_evidence_required':True,'version':'native-source-version'}))
        seed=root/'published/reserved/strategy.json'
        state={'status':'running','phase':'seed','stop_reason':'stage_gate_not_passed:pilot32',
            'candidate':{'seed_artifact':str(seed)},'stage_results':{'train':{'status':'complete'}}}
        search={'round':1,'current_seed':str(seed),'automatic_seed_generation':{
            'round':1,'status':'pending','method':'approved_training_native_seed','seed':str(seed),
            'seed_label':'reserved','source_seed':str(source)}}
        diagnostic={'seed_label':'reserved','validated_evidence_required':True,
            'english_trigger_learning':{'status':'pending','model_calls':1,'incremental_generation':{
                'source_experiences':[{'memory_id':'experience-a','content_revision':'revision-a'}],
                'batches':[{'attempt':1,'status':'draft_ready','memory_ids':['experience-a'],
                    'usage':{'total_tokens':11},'draft':[{'memory_id':'experience-a','trigger_pattern':'login'}]}]}}}
        with _isolated_gepa_outputs(root),patch.object(reproduce._APPWORLD_LAYOUT,'GEPA',root/'gepa'),patch.object(reproduce,'_CANDIDATE_STATE_LABEL',None),patch.object(reproduce._APPWORLD_LAYOUT,'version',return_value='native-source-version'):
            reproduce._save_search(search);reproduce._APPWORLD_LAYOUT.save(root/'gepa/lifecycle.json',state)
            reproduce._APPWORLD_LAYOUT.save(root/'training/seed-compile-status.json',diagnostic)
            yield root,state,seed,source,diagnostic

    def test_reserved_paid_seed_without_fresh_fault_resumes_readonly_and_waits_for_writer(self):
        with tempfile.TemporaryDirectory() as directory,self.seed_fixture(directory) as (root,state,seed,source,diagnostic):
            before={p:p.read_bytes() for p in root.rglob('*.json')}
            self.assertTrue(reproduce._interrupted_worker_checkpoint(state))
            with patch.object(reproduce,'_active_output_processes',return_value=[]):
                self.assertEqual(reproduce._worker_resume_check(),75)
                with patch.object(sys,'argv',['reproduce.py','--worker-resume-check']):self.assertEqual(reproduce.main(),75)
            with patch.object(reproduce,'_active_output_processes',return_value=[123]):
                self.assertEqual(reproduce._worker_resume_check(),76)
            for path,data in before.items():self.assertEqual(path.read_bytes(),data)
            self.assertFalse(seed.exists())

    def test_running_pilot_uses_own_capture_not_old_gate_fault(self):
        helper=FlattenedJudgeTransportRecoveryTests()
        with tempfile.TemporaryDirectory() as directory,helper.fixture(directory) as (state,seed,source,before):
            config=source.parent.parent/'config.json';settings=reproduce._read(config)
            settings['variants']=[{'id':'candidate','artifact':reproduce._read(seed)}]
            reproduce._APPWORLD_LAYOUT.save(config,settings)
            state.update(status='running',stop_reason='stage_gate_not_passed:validation100',
                candidate=reproduce._candidate_identity(seed,seed.parent.name))
            reproduce._APPWORLD_LAYOUT.save(Path(directory)/'gepa/lifecycle.json',state)
            self.assertTrue(reproduce._interrupted_worker_checkpoint(state))
            with patch.object(reproduce,'_active_output_processes',return_value=[]):self.assertEqual(reproduce._worker_resume_check(),75)
            with patch.object(reproduce,'_active_output_processes',return_value=[123]):self.assertEqual(reproduce._worker_resume_check(),76)
            self.assertEqual(reproduce._read(Path(directory)/'gepa/lifecycle.json'),state)

    def test_interrupt_requires_bound_receipts_not_cancel_authority_or_stale_error(self):
        for mutation in ('cancel','failed-stage','source-error','unknown-stop','candidate','label','source','paid-bool','binding','no-batch'):
            with self.subTest(mutation=mutation),tempfile.TemporaryDirectory() as directory,self.seed_fixture(directory) as (root,state,seed,source,diagnostic):
                if mutation=='cancel':state['cancel_requested']=True
                elif mutation=='failed-stage':state['stage_results']['seed']={'status':'failed','reason':'invalid_credentials'}
                elif mutation=='source-error':state['last_error']='appworld_frozen_seed_validated_source_missing'
                elif mutation=='unknown-stop':state['stop_reason']='permission_denied'
                elif mutation=='candidate':state['candidate']['seed_artifact']=str(source)
                elif mutation=='label':diagnostic['seed_label']='other'
                elif mutation=='source':source.unlink()
                elif mutation=='paid-bool':diagnostic['english_trigger_learning']['model_calls']=True
                elif mutation=='binding':diagnostic['english_trigger_learning']['incremental_generation']['source_experiences'][0]['content_revision']='changed'
                elif mutation=='no-batch':diagnostic['english_trigger_learning']['incremental_generation']['batches']=[]
                reproduce._APPWORLD_LAYOUT.save(root/'training/seed-compile-status.json',diagnostic)
                reproduce._APPWORLD_LAYOUT.save(root/'gepa/lifecycle.json',state)
                before={p:p.read_bytes() for p in root.rglob('*.json')}
                self.assertFalse(reproduce._interrupted_worker_checkpoint(state))
                with patch.object(reproduce,'_active_output_processes') as processes:
                    self.assertEqual(reproduce._worker_resume_check(),0);processes.assert_not_called()
                for path,data in before.items():self.assertEqual(path.read_bytes(),data)
        for mutation in ('judge','artifact'):
            with self.subTest(mutation=mutation),tempfile.TemporaryDirectory() as directory,FlattenedJudgeTransportRecoveryTests().fixture(directory,mutation=mutation) as (state,seed,source,before):
                state.update(status='running',candidate=reproduce._candidate_identity(seed,seed.parent.name))
                self.assertFalse(reproduce._interrupted_worker_checkpoint(state))
        for mutation in ('capture-artifact','capture-outside'):
            with self.subTest(mutation=mutation),tempfile.TemporaryDirectory() as directory,FlattenedJudgeTransportRecoveryTests().fixture(directory) as (state,seed,source,before):
                config=source.parent.parent/'config.json';settings=reproduce._read(config)
                settings['variants']=[{'id':'candidate','artifact':reproduce._read(seed)}]
                reproduce._APPWORLD_LAYOUT.save(config,settings)
                state.update(status='running',candidate=reproduce._candidate_identity(seed,seed.parent.name))
                if mutation=='capture-artifact':
                    settings['variants'][0]['artifact']={'wrong':True};reproduce._APPWORLD_LAYOUT.save(config,settings)
                else:
                    progress_path=source.parent.parent.parent.parent/'progress.json';progress=reproduce._read(progress_path)
                    progress['shards']['0']['source_report']=str(Path(directory)/'outside/report.json')
                    reproduce._APPWORLD_LAYOUT.save(progress_path,progress)
                self.assertFalse(reproduce._interrupted_worker_checkpoint(state))
                for path,data in before.items():path.write_bytes(data)

    def test_compiled_inheritance_directory_binds_all_126_native_snapshot_sources_not_26_active(self):
        with tempfile.TemporaryDirectory() as directory,self.seed_fixture(directory) as (root,state,seed,source,diagnostic):
            artifact=reproduce._read(source)
            cards=[{'memory_id':f'experience-{i}','content_revision':f'revision-{i}',
                'target_scope':reproduce._APPWORLD_LAYOUT.SCOPE} for i in range(126)]
            artifact['policy']['snapshot']['experiences']=cards
            artifact['policy']['options']['approved_experiences']=[{key:row[key] for key in ('memory_id','content_revision')} for row in cards[:26]]
            reproduce._APPWORLD_LAYOUT.save(source,artifact)
            english=diagnostic['english_trigger_learning'];english['status']='compiled'
            english['incremental_generation']['source_experiences']=[{key:row[key] for key in ('memory_id','content_revision')} for row in cards]
            english['incremental_generation']['batches'][0].update(status='pending',memory_ids=['experience-125'])
            reproduce._APPWORLD_LAYOUT.save(root/'training/seed-compile-status.json',diagnostic)
            self.assertEqual(reproduce._interrupted_worker_checkpoint(state),'ready')
            with patch.object(reproduce,'_active_output_processes',return_value=[]):self.assertEqual(reproduce._worker_resume_check(),75)
            english['incremental_generation']['source_experiences'][-1]['content_revision']='other'
            reproduce._APPWORLD_LAYOUT.save(root/'training/seed-compile-status.json',diagnostic)
            self.assertIsNone(reproduce._interrupted_worker_checkpoint(state))

    def test_frozen_complete_seed_checkpoint_reuses_exact_native_artifact_without_paid_call(self):
        with tempfile.TemporaryDirectory() as directory,self.seed_fixture(directory) as (root,state,seed,source,diagnostic):
            seed.parent.mkdir(parents=True);seed.write_bytes(source.read_bytes())
            seed.with_name('manifest.json').write_bytes(source.with_name('manifest.json').read_bytes())
            state['stage_results']['seed']={'status':'complete','artifact':str(seed)}
            reproduce._APPWORLD_LAYOUT.save(root/'gepa/lifecycle.json',state)
            before={path:path.read_bytes() for path in root.rglob('*.json')}
            self.assertEqual(reproduce._interrupted_worker_checkpoint(state),'ready')
            with patch.object(reproduce,'_active_output_processes',return_value=[]):self.assertEqual(reproduce._worker_resume_check(),75)
            for path,data in before.items():self.assertEqual(path.read_bytes(),data)

    def test_partial_capture_only_waits_while_bound_writer_is_alive(self):
        helper=FlattenedJudgeTransportRecoveryTests()
        with tempfile.TemporaryDirectory() as directory,helper.fixture(directory,mutation='execution') as (state,seed,source,before):
            config=source.parent.parent/'config.json';settings=reproduce._read(config)
            settings['variants']=[{'id':'candidate','artifact':reproduce._read(seed)}]
            reproduce._APPWORLD_LAYOUT.save(config,settings)
            state.update(status='running',candidate=reproduce._candidate_identity(seed,seed.parent.name))
            reproduce._APPWORLD_LAYOUT.save(Path(directory)/'gepa/lifecycle.json',state)
            self.assertEqual(reproduce._interrupted_worker_checkpoint(state),'waiting')
            with patch.object(reproduce,'_active_output_processes',return_value=[123]):self.assertEqual(reproduce._worker_resume_check(),76)
            with patch.object(reproduce,'_active_output_processes',return_value=[]):self.assertEqual(reproduce._worker_resume_check(),0)

    def test_real_worker_shell_recovers_abnormal_exit_without_repeating_reset(self):
        helper=WorkerCheckpointRecoveryTests()
        for code in (137,1):
            with self.subTest(code=code):
                result,calls,delays,log=helper.run_worker_shell(exits=[code,0],checks=[75,0])
                actions=[row for row in calls if '--worker-resume-check' not in row]
                self.assertEqual((result,len(actions),delays),(0,2,['15']))
                self.assertIn('--worker-checkpoint-resume',actions[1])


class GeneratedControlWorkerCheckpointTests(InterruptedWorkerCheckpointTests):
    def test_native_generated_three_paid_controls_before_english_revalidate_without_reset(self):
        from copy import deepcopy
        with tempfile.TemporaryDirectory() as directory,self.seed_fixture(directory) as (root,state,seed,source,diagnostic):
            diagnostic.pop('english_trigger_learning')
            diagnostic['control_learning']={'status':'generated','model_calls':3,
                'generated_rules':{'action_rules':0,'loop_rules':1,'schedule_rules':0},
                'validated_model_draft':{'action_rules':[],'loop_rules':[{
                    'memory_id':'experience-a','replan_on_failure':True,'max_replans':1}], 'schedule_rules':[]},
                'attempts':[{'status':'generated','usage':{'total_tokens':17}}]}
            reproduce._APPWORLD_LAYOUT.save(root/'training/seed-compile-status.json',diagnostic)
            runtime=root/'training/runtime';reproduce._APPWORLD_LAYOUT.save(runtime/'service.json',{'control_learning':{'enabled':True}})
            native_calls=[]
            def native(binary,workdir,operation,payload):
                native_calls.append(list(operation));policy=deepcopy(payload['policy'])
                if operation==['runtime','recompile']:policy['options']=deepcopy(payload['options'])
                elif operation==['runtime','control-compile']:
                    self.assertEqual(payload['draft'],diagnostic['control_learning']['validated_model_draft'])
                    policy['options']['loop_rules']=[{'target_scope':reproduce._APPWORLD_LAYOUT.SCOPE,
                        'source_experience':{'memory_id':'experience-a','content_revision':'revision-a'}}]
                else:self.fail('worker must not call model/query/Agent')
                return json.dumps(policy)
            before={path:path.read_bytes() for path in root.rglob('*.json')}
            with patch.object(reproduce._APPWORLD_LAYOUT,'RUNTIME',runtime),patch.object(reproduce._APPWORLD_LAYOUT,'native',side_effect=native):
                self.assertEqual(reproduce._interrupted_worker_checkpoint(state),'ready')
                self.assertEqual(native_calls[-1],['runtime','control-compile'])
                self.assertEqual(native_calls.count(['runtime','control-compile']),1)
                self.assertTrue(all(op in (['runtime','recompile'],['runtime','control-compile']) for op in native_calls))
                with patch.object(reproduce,'_active_output_processes',return_value=[]):self.assertEqual(reproduce._worker_resume_check(),75)
                for paid in (1,2,3):
                    with self.subTest(paid=paid):
                        diagnostic['control_learning']['model_calls']=paid
                        reproduce._APPWORLD_LAYOUT.save(root/'training/seed-compile-status.json',diagnostic)
                        self.assertEqual(reproduce._interrupted_worker_checkpoint(state),'ready')
                for paid in (True,-1,4):
                    with self.subTest(rejected_paid=paid):
                        bad=deepcopy(diagnostic);bad['control_learning']['model_calls']=paid
                        reproduce._APPWORLD_LAYOUT.save(root/'training/seed-compile-status.json',bad)
                        native_calls.clear()
                        self.assertIsNone(reproduce._interrupted_worker_checkpoint(state))
                        self.assertEqual(native_calls,[])
                reproduce._APPWORLD_LAYOUT.save(root/'training/seed-compile-status.json',diagnostic)
                initial=deepcopy(diagnostic);initial['english_trigger_learning']={'status':'pending','model_calls':0}
                reproduce._APPWORLD_LAYOUT.save(root/'training/seed-compile-status.json',initial)
                self.assertEqual(reproduce._interrupted_worker_checkpoint(state),'ready')
                reproduce._APPWORLD_LAYOUT.save(root/'training/seed-compile-status.json',diagnostic)
                bad=deepcopy(diagnostic);bad['control_learning']['validated_model_draft']['loop_rules'][0]['memory_id']='foreign'
                reproduce._APPWORLD_LAYOUT.save(root/'training/seed-compile-status.json',bad)
                native_calls.clear();self.assertIsNone(reproduce._interrupted_worker_checkpoint(state));self.assertEqual(native_calls,[])
                reproduce._APPWORLD_LAYOUT.save(root/'training/seed-compile-status.json',diagnostic)
                with patch.object(reproduce._APPWORLD_LAYOUT,'native',side_effect=RuntimeError('control_source_not_approved')):
                    self.assertIsNone(reproduce._interrupted_worker_checkpoint(state))
            for path,data in before.items():self.assertEqual(path.read_bytes(),data)


class AutonomousActiveSeedBindingTests(unittest.TestCase):
    """Real nested prefix/terminal handlers, isolated native bookkeeping only."""
    @contextmanager
    def advanced_fixture(self,directory,*,rate_limited=False):
        real_terminal=reproduce._native_gepa_no_new_candidate
        with AutomaticSeedSearchTests.fixture(self,directory,passed_after=2) as values:
            initial,root,events,generated,retained=values
            actual=[];source_files={};native_runs=[]
            def native(script,*arguments,**kwargs):
                self.assertEqual(script,'offline_gepa.py');events.append(arguments[0])
                active=Path(arguments[arguments.index('--seed-artifact')+1]).resolve()
                artifact=reproduce._read(active);actual[:]=[active]
                job='same-native-job'
                tree=offline_gepa.WORKSPACE/'agent-evaluation'/job/'search/tree.json'
                seed_copy=tree.parent.parent/'seed.json'
                if arguments[0]=='--prepare':
                    old={'max_rounds':30,'max_model_calls':512,'max_trials':8000,'timeout_ms':50*3600000}
                    offline_gepa.save(offline_gepa.CONFIG/'gepa.json',old)
                    offline_gepa.save(offline_gepa.PLAN,{'seed':str(active),'search':{'max_rounds':30,'max_model_calls':512}})
                    offline_gepa.save(offline_gepa.GEPA/'submission.json',{'job_id':job})
                    offline_gepa.save(seed_copy,artifact)
                    report=tree.parent/'evaluations/retained/report.pb';offline_gepa.save(report.with_suffix('.json'),{'healthy_task':'original'})
                    report.write_bytes(b'original healthy capture')
                    draft=tree.parent/'reflections/map.draft.json';offline_gepa.save(draft,{'paid_map':'original'})
                    source_files.update({p:p.read_bytes() for p in (active,seed_copy,report,draft)})
                    return 0
                self.assertEqual(arguments[0],'--run');native_runs.append(active)
                error=rate_limited and len(native_runs)==1
                offline_gepa.save(offline_gepa.WORKSPACE/'agent-evaluation/state.json',{'jobs':{job:{
                    'status':'error' if error else 'rejected','error':'gepa_model_rate_limited' if error else None,
                    'user':offline_gepa.USER,'level':'L3','gepa':True}}})
                offline_gepa.save(tree,{'status':'error' if error else 'validation_rejected',
                    'error':'gepa_model_rate_limited' if error else None,'stop_reason':'model_budget',
                    'active':1 if error else None,'batch':{'paid_maps':True} if error else None,
                    'model_calls':512,'trials_used':1024,'rounds':30,'started_ms':1,
                    'budget_extensions':[{'reason':'initial_budget','max_model_calls':512}],
                    'holdout_started':False,'nodes':[{'id':0,'formal':'original'}]})
                status=reproduce._read(offline_gepa.STATUS);status.update(job_id=job,job_status='error' if error else 'rejected')
                offline_gepa.save(offline_gepa.STATUS,status)
                if error:raise RuntimeError('offline_gepa.py_failed:1')
                return 0
            with patch.object(reproduce,'_native_gepa_no_new_candidate',side_effect=real_terminal), \
                 patch.object(reproduce,'run',side_effect=native), \
                 patch.object(reproduce._APPWORLD_LAYOUT,'version',side_effect=lambda artifact:f"fixture-native-{artifact['policy']['candidate']}"), \
                 patch.object(reproduce.time,'sleep') as sleep:
                yield initial,root,events,generated,retained,actual,source_files,native_runs,sleep

    def test_real_inner_two_failures_rebind_before_same_job_budget_extension(self):
        with tempfile.TemporaryDirectory() as td,self.advanced_fixture(td) as values:
            initial,root,events,generated,retained,active,protected,native_runs,sleep=values
            real_prefix=reproduce._run_all_with_seed_search;calls=[]
            def prefix(seed,label,**budgets):
                calls.append(seed)
                if len(calls)==1:return real_prefix(seed,label,**budgets)
                self.assertEqual(seed,active[0]);self.assertNotEqual(seed,initial)
                self.assertEqual(budgets['gepa_max_model_calls'],1024)
                receipt=offline_gepa.read(offline_gepa.PLAN)['budget_extension']
                self.assertEqual(receipt['seed'],str(seed));self.assertEqual(receipt['job_id'],'same-native-job')
                return {'status':'published','phase':'publish'}
            with patch.object(reproduce,'_run_all_with_seed_search',side_effect=prefix), \
                 patch.object(reproduce,'_reserve_after_unqualified_native_search') as reserve:
                result=reproduce._run_all_autonomously(initial,'strategy')
            self.assertEqual(result['status'],'published');self.assertEqual(len(generated),2)
            self.assertEqual(calls,[initial,active[0]]);reserve.assert_not_called();sleep.assert_not_called()
            self.assertEqual(events,[(0,32),(1,32),(2,32),(2,100),'--prepare','--run'])
            self.assertEqual([r['status'] for r in reproduce.seed_search()['attempts']],['failed','failed','passed'])
            for path,data in protected.items():self.assertEqual(path.read_bytes(),data)
            self.assertEqual(offline_gepa.read(offline_gepa.WORKSPACE/'agent-evaluation/same-native-job/search/tree.json')['model_calls'],512)

    def test_real_inner_rate_limit_at_cap_rebinds_before_normal_retry_and_extension(self):
        with tempfile.TemporaryDirectory() as td,self.advanced_fixture(td,rate_limited=True) as values:
            initial,root,events,generated,retained,active,protected,native_runs,sleep=values
            real_prefix=reproduce._run_all_with_seed_search;calls=[]
            def prefix(seed,label,**budgets):
                calls.append(seed)
                if len(calls)<=2:return real_prefix(seed,label,**budgets)
                self.assertEqual(seed,active[0]);self.assertEqual(budgets['gepa_max_model_calls'],1024)
                return {'status':'published','phase':'publish'}
            with patch.object(reproduce,'_run_all_with_seed_search',side_effect=prefix), \
                 patch.object(reproduce,'_reserve_after_unqualified_native_search') as reserve:
                result=reproduce._run_all_autonomously(initial,'strategy')
            self.assertEqual(result['status'],'published');self.assertEqual(calls,[initial,active[0],active[0]])
            self.assertEqual(native_runs,[active[0],active[0]]);self.assertEqual(len(generated),2)
            sleep.assert_called_once_with(15);reserve.assert_not_called()
            self.assertEqual(offline_gepa.read(offline_gepa.PLAN)['budget_extension']['model_calls_used'],512)
            for path,data in protected.items():self.assertEqual(path.read_bytes(),data)

    def test_changed_seed_requires_plan_native_body_version_and_search_binding(self):
        for defect in ('plan','native-body','native-version','current-seed'):
            with self.subTest(defect=defect),tempfile.TemporaryDirectory() as td,self.advanced_fixture(td) as values:
                initial,root,events,generated,retained,active,protected,native_runs,sleep=values
                result=reproduce._run_all_with_seed_search(initial,'strategy');self.assertEqual(result['status'],'not_qualified')
                current=active[0];state=offline_gepa.read(offline_gepa.GEPA/'lifecycle.json')
                if defect=='plan':
                    plan=offline_gepa.read(offline_gepa.PLAN);plan['seed']=str(initial);offline_gepa.save(offline_gepa.PLAN,plan)
                elif defect=='native-body':
                    offline_gepa.save(offline_gepa.WORKSPACE/'agent-evaluation/same-native-job/seed.json',{'wrong_source':True})
                elif defect=='native-version':
                    value=offline_gepa.read(current);value['policy']['candidate']=99;offline_gepa.save(current,value)
                else:
                    search=reproduce.seed_search();search['current_seed']=str(initial);reproduce._save_search(search)
                before=offline_gepa.read(offline_gepa.PLAN)
                with self.assertRaisesRegex(RuntimeError,'native_seed_binding_invalid'):
                    reproduce._autonomous_native_seed(initial,state)
                self.assertEqual(offline_gepa.read(offline_gepa.PLAN),before);sleep.assert_not_called()


class LegacyTerminalWorkerRecoveryTests(unittest.TestCase):
    """A new worker may recover a stale outer only from the real bound job."""
    @contextmanager
    def legacy_fixture(self, directory):
        with AutonomousActiveSeedBindingTests.advanced_fixture(self, directory) as values:
            initial, root, events, generated, retained, active, protected, native_runs, sleep = values
            # Simulate only the old outer's omitted synchronization. The real
            # nested prefix, terminal lookup and reserve guard remain intact.
            with patch.object(reproduce, '_autonomous_native_seed', return_value=(initial, 'strategy')):
                result = reproduce._run_all_autonomously(initial, 'strategy')
            self.assertEqual(result['status'], 'blocked')
            self.assertEqual(result['stop_reason'], 'appworld_gepa_terminal_receipt_invalid')
            self.assertEqual(result['phase'], 'gepa_run')
            self.assertNotEqual(active[0], initial)
            self.assertEqual(events, [(0, 32), (1, 32), (2, 32), (2, 100), '--prepare', '--run'])
            yield values
            for path, original in protected.items():
                self.assertEqual(path.read_bytes(), original)
            sleep.assert_not_called()

    def test_old_progressed_outer_terminal_worker_resumes_actual_seed_without_writes(self):
        with tempfile.TemporaryDirectory(dir='/tmp') as td, self.legacy_fixture(td) as values:
            initial, root, events, generated, retained, active, protected, native_runs, sleep = values
            preserved = {p: p.read_bytes() for p in (root / 'gepa').rglob('*') if p.is_file()}
            with patch.object(reproduce, '_active_output_processes', return_value=[]):
                self.assertEqual(reproduce._worker_resume_check(), 75)
            with patch.object(reproduce, '_active_output_processes', return_value=[123]):
                self.assertEqual(reproduce._worker_resume_check(), 76)
            for path, original in preserved.items():
                self.assertEqual(path.read_bytes(), original, 'worker qualification wrote a receipt')
            calls = []
            def resumed(seed, label, **budget):
                calls.append((seed, label, budget['gepa_max_model_calls']))
                return {'status': 'published', 'phase': 'publish'}
            with patch.object(reproduce, '_run_all_with_seed_search', side_effect=resumed):
                result = reproduce._run_all_autonomously(active[0], active[0].parent.name)
            self.assertEqual(result['status'], 'published')
            self.assertEqual(calls, [(active[0], active[0].parent.name, 1024)])
            extension = offline_gepa.read(offline_gepa.PLAN)['budget_extension']
            self.assertEqual(extension['seed'], str(active[0]))
            self.assertEqual(extension['job_id'], 'same-native-job')
            self.assertEqual(extension['model_calls_used'], 512)

    def test_legacy_terminal_worker_rejects_wrong_binding_cancel_and_real_errors(self):
        defects = ('candidate', 'artifact', 'plan', 'current-seed', 'immutable-seed',
                   'submission', 'job-owner', 'native-job', 'job-error', 'tree-error',
                   'cancelled', 'job-cancel', 'tree-cancel', 'active', 'batch',
                   'holdout', 'final-holdout', 'pilot-gate', 'stage-job', 'last-error')
        for defect in defects:
            with self.subTest(defect=defect), tempfile.TemporaryDirectory(dir='/tmp') as td, self.legacy_fixture(td) as values:
                initial, root, events, generated, retained, active, protected, native_runs, sleep = values
                state_path = root / 'gepa/lifecycle.json';state = reproduce._read(state_path)
                tree_path = root / 'gepa/workspace/agent-evaluation/same-native-job/search/tree.json'
                job_path = root / 'gepa/workspace/agent-evaluation/state.json'
                if defect == 'candidate':state['candidate']['seed_version'] = 'unbound-version'
                elif defect == 'artifact':
                    artifact = reproduce._read(active[0]);artifact['policy']['candidate'] = 99
                    offline_gepa.save(active[0], artifact)
                    protected.pop(active[0])  # The deliberate negative mutation is not a test write leak.
                elif defect == 'plan':
                    plan = offline_gepa.read(offline_gepa.PLAN);plan['seed'] = str(initial)
                    offline_gepa.save(offline_gepa.PLAN, plan)
                elif defect == 'current-seed':
                    search = reproduce.seed_search();search['current_seed'] = str(initial);reproduce._save_search(search)
                elif defect == 'immutable-seed':
                    path = tree_path.parent.parent / 'seed.json'
                    offline_gepa.save(path, {'level': 'L3', 'wrong': True});protected.pop(path)
                elif defect == 'submission':offline_gepa.save(root / 'gepa/submission.json', {'job_id': 'unbound-job'})
                elif defect in ('job-owner', 'native-job', 'job-error', 'job-cancel'):
                    jobs = offline_gepa.read(job_path);job = jobs['jobs']['same-native-job']
                    key, value = {'job-owner': ('user', 'different-owner'), 'native-job': ('status', 'error'),
                                  'job-error': ('error', 'model_transport_failed'), 'job-cancel': ('cancel_requested', True)}[defect]
                    job[key] = value;offline_gepa.save(job_path, jobs)
                elif defect in ('tree-error', 'tree-cancel', 'active', 'batch', 'holdout', 'final-holdout'):
                    tree = offline_gepa.read(tree_path)
                    key, value = {'tree-error': ('error', 'candidate_failed'), 'tree-cancel': ('cancel_requested', True),
                        'active': ('active', 1), 'batch': ('batch', {}), 'holdout': ('holdout_started', True),
                        'final-holdout': ('final_holdout', {'candidates': [1]})}[defect]
                    tree[key] = value;offline_gepa.save(tree_path, tree)
                elif defect == 'cancelled':state['status'] = 'cancelled'
                elif defect == 'pilot-gate':state['stage_results']['pilot32']['gate_passed'] = False
                elif defect == 'stage-job':state['stage_results']['gepa_run']['data']['job_id'] = 'wrong-job'
                else:state['last_error'] = 'unclassified_real_error'
                offline_gepa.save(state_path, state)
                with patch.object(reproduce, '_active_output_processes') as processes:
                    self.assertEqual(reproduce._worker_resume_check(), 0)
                    processes.assert_not_called()

    def test_legacy_terminal_worker_does_not_classify_other_errors_as_retry(self):
        for fault in ('appworld_autonomous_native_seed_binding_invalid', 'reflection_candidate_error',
                      'appworld_gepa_model_budget_exhausted', 'appworld_gepa_terminal_receipt_invalid_extra'):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory(dir='/tmp') as td, self.legacy_fixture(td) as values:
                root = values[1];state_path = root / 'gepa/lifecycle.json';state = reproduce._read(state_path)
                state['stop_reason'] = fault;offline_gepa.save(state_path, state)
                self.assertIsNone(reproduce._autonomous_retry_reason(RuntimeError(fault), state))
                with patch.object(reproduce, '_active_output_processes') as processes:
                    self.assertEqual(reproduce._worker_resume_check(), 0)
                    processes.assert_not_called()


class Baseline100InterruptedWorkerRecoveryTests(unittest.TestCase):
    @contextmanager
    def fixture(self, directory):
        import compare
        root = Path(directory); gepa=root/'gepa'; seed=root/'published/current/strategy.json'
        reproduce._APPWORLD_LAYOUT.save(seed, {'level':'L3','policy':{'frozen':True}})
        reproduce._APPWORLD_LAYOUT.save(seed.with_name('manifest.json'), {'version':'candidate-native'})
        l0=root/'published/L0.json'; reproduce._APPWORLD_LAYOUT.save(l0, {
            'level':'L0','policy':None,'agent':{'revision':'baseline-revision'}})
        tasks=[f'task-{i:03d}' for i in range(100)]
        dataset=root/'training/datasets/comparison500';dataset.mkdir(parents=True)
        (dataset/'holdout.jsonl').write_text('\n'.join(json.dumps({'task_id':t,'task_version':'version-'+t}) for t in tasks)+'\n')
        pilot=gepa/'current-stage32/comparison.json'; pilot.parent.mkdir(parents=True)
        baseline32=gepa/'l0-current-stage32/result.json';baseline32.parent.mkdir(parents=True)
        ids=_l0(baseline32);_candidate(pilot,ids,strict=7,quality=.6)
        value=reproduce._read(pilot);progress=_candidate_checkpoint(pilot,reproduce._read(seed),value['items'])
        for shard in progress['shards'].values():shard['complete']=True
        progress.update(planned_tasks=32,total_parallelism=0,workers_per_shard=16)
        reproduce._APPWORLD_LAYOUT.save(pilot.with_name('progress.json'),progress)
        value.update(seed=str(seed),source_l0=str(baseline32),progress=str(pilot.with_name('progress.json')),host_version='fixture-host')
        reproduce._APPWORLD_LAYOUT.save(pilot,value)
        baseline=gepa/'l0-current-stage100';baseline.mkdir(parents=True)
        config={'task_ids':tasks,'repetitions':1,'splits':['holdout'],'max_trials':100,
                'variants':[{'id':'candidate','version':'baseline-revision','instrumentation_config':str(root/'training/runtime/l0-evaluation-service.json')}],
                'judge':reproduce._APPWORLD_LAYOUT.judge_projection()}
        reproduce._APPWORLD_LAYOUT.save(baseline/'capture.config.json',config)
        (baseline/'capture.host-version').write_text('fixture-host')
        folder=baseline/'capture';reproduce._APPWORLD_LAYOUT.save(folder/'started.json',{
            'planned_trials':100,'run_id':'native-run','dataset_version':'native-dataset'})
        row={'task_id':tasks[0],'task_version':'version-'+tasks[0],'trial_id':'trial-a','variant_id':'candidate',
             'execution_status':'completed','verdict':'passed','checks':[],
             'evidence':{'loaded_version':'baseline-native','output':{'framework_capture':{'status':'complete'}},
                         'trace':{'events':[{'kind':'run.start'},{'kind':'run.end'}]}}}
        trial=folder/'trials/trial-a.json';reproduce._APPWORLD_LAYOUT.save(trial,row)
        with _isolated_gepa_outputs(root),patch.object(reproduce._APPWORLD_LAYOUT,'GEPA',gepa), \
             patch.object(reproduce._APPWORLD_LAYOUT,'RUNTIME',root/'training/runtime'),patch.object(compare,'DATASET',dataset), \
             patch.object(reproduce,'_host_version',return_value='fixture-host'),patch.object(reproduce,'_CANDIDATE_STATE_LABEL',None), \
             patch.object(reproduce._APPWORLD_LAYOUT,'version',side_effect=lambda a:'baseline-native' if a.get('level')=='L0' else 'candidate-native'):
            search={'round':2,'current_seed':str(seed),'attempts':[{'round':2,'seed':str(seed),'host_version':'fixture-host',
                'pilot_comparison':str(pilot),'validation_comparison':str(gepa/'current-stage100/comparison.json')}],
                'automatic_seed_generation':{'round':2,'status':'frozen','seed':str(seed),'source_seed':str(root/'published/previous/strategy.json')}}
            reproduce._save_search(search)
            state={'status':'running','phase':'validation100','stop_reason':'appworld_seed_generation_duplicate',
                'candidate':reproduce._candidate_identity(seed,'current'),'stage_results':{
                    'pilot32':{'status':'complete','gate_passed':True},'validation100':{'status':'pending'}}}
            reproduce._APPWORLD_LAYOUT.save(gepa/'lifecycle.json',state)
            yield root,state,seed,baseline,trial,pilot

    def test_real_formal_pilot_before_candidate_progress_preserves_partial_baseline_readonly(self):
        with tempfile.TemporaryDirectory() as directory,self.fixture(directory) as (root,state,seed,baseline,trial,pilot):
            passed,proof=gate.verify_stage('32',pilot,Path(reproduce._read(pilot)['source_l0']),seed)
            self.assertTrue(passed,proof)
            before={p:p.read_bytes() for p in root.rglob('*') if p.is_file()}
            self.assertFalse((root/'gepa/current-stage100/progress.json').exists())
            self.assertEqual(reproduce._interrupted_worker_checkpoint(state),'ready')
            with patch.object(reproduce,'_active_output_processes',return_value=[]):self.assertEqual(reproduce._worker_resume_check(),75)
            with patch.object(reproduce,'_active_output_processes',return_value=[123]):self.assertEqual(reproduce._worker_resume_check(),76)
            for p,data in before.items():self.assertEqual(p.read_bytes(),data)

    def test_baseline_temporary_judge_failure_recovers_but_permanent_or_unbound_evidence_does_not(self):
        for mutation in ('temporary','conflict','contract','schema','task','revision','judge','host','pilot','cancel','unknown','outside_output','authority_fault','bad_trace'):
            with self.subTest(mutation=mutation),tempfile.TemporaryDirectory() as directory,self.fixture(directory) as (root,state,seed,baseline,trial,pilot):
                row=reproduce._read(trial)
                if mutation in ('temporary','schema','conflict'):
                    row.update(verdict='error',error_code='evaluation_judge_failed')
                    row['checks']=[{'check_id':'model','status':'error','reason':'evaluation_judge_http_429' if mutation=='temporary' else 'evaluation_judge_reason_conflict' if mutation=='conflict' else 'evaluation_judge_schema_invalid'}]
                elif mutation=='bad_trace':row['evidence']['trace']['events']=[{'kind':'run.start'}]
                elif mutation=='contract':row.update(execution_status='error',error_code='evaluation_entry_capture_contract_invalid')
                elif mutation=='task':row['task_id']='task-outside-100'
                elif mutation=='revision':row['task_version']='replaced'
                elif mutation=='judge':
                    config=reproduce._read(baseline/'capture.config.json');config['judge']['require_state_dependency']=False;reproduce._APPWORLD_LAYOUT.save(baseline/'capture.config.json',config)
                elif mutation=='host':(baseline/'capture.host-version').write_text('wrong-host')
                elif mutation=='pilot':
                    value=reproduce._read(pilot);value['items'][0]['quality']=0;reproduce._APPWORLD_LAYOUT.save(pilot,value)
                elif mutation=='cancel':state['cancel_requested']=True
                elif mutation=='unknown':state['stop_reason']='source_authority_revoked'
                elif mutation=='outside_output':
                    search=reproduce.seed_search();search['attempts'][0]['validation_comparison']=str(root/'outside/comparison.json');reproduce._save_search(search)
                elif mutation=='authority_fault':
                    state['last_error']='source_authority_revoked'
                    row.update(verdict='error',error_code='evaluation_judge_failed')
                    row['checks']=[{'check_id':'model','status':'error','reason':'evaluation_judge_http_429'}]
                    search=reproduce.seed_search();search['experiment']={'status':'blocked','reason':'compare.py_failed:1'};reproduce._save_search(search)
                reproduce._APPWORLD_LAYOUT.save(trial,row);reproduce._APPWORLD_LAYOUT.save(root/'gepa/lifecycle.json',state)
                with patch.object(reproduce,'_active_output_processes',return_value=[]):
                    self.assertEqual(reproduce._worker_resume_check(),75 if mutation in ('temporary','conflict') else 0)
                if mutation in ('temporary','conflict'):
                    blocked=dict(state,status='blocked',stop_reason='compare.py_failed:1')
                    self.assertEqual(reproduce._autonomous_retry_reason(RuntimeError('compare.py_failed:1'),blocked),'appworld_stage_review_pending')

    def test_actual_running_baseline_ignores_only_bound_previous_compare_exit(self):
        with tempfile.TemporaryDirectory() as directory,self.fixture(directory) as (root,state,seed,baseline,trial,pilot):
            state['stop_reason']='appworld_stage_failed'
            state['stage_results']['validation100']={'status':'failed','reason':'RuntimeError:compare.py_failed:1','data':{}}
            reproduce._APPWORLD_LAYOUT.save(root/'gepa/lifecycle.json',state)
            before={p:p.read_bytes() for p in root.rglob('*') if p.is_file()}
            with patch.object(reproduce,'_active_output_processes',return_value=[123]):self.assertEqual(reproduce._worker_resume_check(),76)
            with patch.object(reproduce,'_active_output_processes',return_value=[]):self.assertEqual(reproduce._worker_resume_check(),75)
            for p,data in before.items():self.assertEqual(p.read_bytes(),data)
            for reason in ('source_authority_revoked','evaluation_entry_capture_contract_invalid','model_credentials_invalid'):
                state['stage_results']['validation100']['reason']=reason
                reproduce._APPWORLD_LAYOUT.save(root/'gepa/lifecycle.json',state)
                with patch.object(reproduce,'_active_output_processes',return_value=[]):self.assertEqual(reproduce._worker_resume_check(),0)

    def test_latest_baseline_review_cannot_hide_permanent_judge_failure_or_wrong_source(self):
        from copy import deepcopy
        for fault in ('transport','conflict','schema','credentials','uncertain','foreign_tasks','wrong_judge'):
            with self.subTest(fault=fault),tempfile.TemporaryDirectory() as directory,self.fixture(directory) as (root,state,seed,baseline,trial,pilot):
                rows=[]
                for i in range(100):
                    row=deepcopy(reproduce._read(trial));row.update(task_id=f'task-{i:03d}',task_version=f'version-task-{i:03d}',trial_id=f'trial-{i:03d}',verdict='error',error_code='evaluation_judge_failed')
                    row['checks']=[{'check_id':'model','status':'error','reason':'evaluation_judge_http_429'}];rows.append(row)
                reason={'conflict':'evaluation_judge_reason_conflict','schema':'evaluation_judge_schema_invalid','credentials':'evaluation_judge_http_401','uncertain':'evaluation_judge_uncertain'}.get(fault)
                if reason:rows[0]['checks'][0]['reason']=reason
                if fault=='foreign_tasks':rows[0]['task_id']='outside-100'
                source=baseline/'regrade/report.json';reproduce._APPWORLD_LAYOUT.save(source,{'complete':True,'trials':rows})
                reproduce._APPWORLD_LAYOUT.save(source.with_name('manifest.json'),{'complete':True})
                current=reproduce._APPWORLD_LAYOUT.judge_projection()
                if fault=='wrong_judge':current['require_state_dependency']=False
                reproduce._APPWORLD_LAYOUT.save(baseline/'regrade.regrade.config.json',{'judge':current})
                with patch.object(reproduce,'_active_output_processes',return_value=[]):
                    self.assertEqual(reproduce._worker_resume_check(),75 if fault in ('transport','conflict') else 0)

    def test_empty_baseline_waits_only_for_live_writer_and_failed_latest_attempt_not_hidden(self):
        with tempfile.TemporaryDirectory() as directory,self.fixture(directory) as (root,state,seed,baseline,trial,pilot):
            trial.unlink()
            with patch.object(reproduce,'_active_output_processes',return_value=[]):self.assertEqual(reproduce._worker_resume_check(),0)
            with patch.object(reproduce,'_active_output_processes',return_value=[123]):self.assertEqual(reproduce._worker_resume_check(),76)
            newer=baseline/'capture-retry-01';reproduce._APPWORLD_LAYOUT.save(newer/'started.json',reproduce._read(baseline/'capture/started.json'))
            reproduce._APPWORLD_LAYOUT.save(newer/'trials/failure.json',{'task_id':'task-000','task_version':'version-task-000','variant_id':'candidate','execution_status':'error','error_code':'evaluation_entry_capture_contract_invalid','evidence':{'loaded_version':'baseline-native'}})
            with patch.object(reproduce,'_active_output_processes',return_value=[123]):self.assertEqual(reproduce._worker_resume_check(),0)
class InitialNativeReviewRecoveryTests(unittest.TestCase):
    fixture = OfflineGepaRecoveryTests.fixture

    def review_fixture(self, directory, *, baseline=False, reason='evaluation_judge_reason_conflict'):
        import hashlib
        artifact, old_seed, jobs, native, tree_path = self.fixture(directory, status='error')
        seed = old_seed.parent / 'strategy' / 'strategy.json'
        seed.parent.mkdir(); old_seed.replace(seed)
        offline_gepa.save(offline_gepa.PLAN, {'seed': str(seed)})
        offline_gepa.save(offline_gepa.STATUS, {'job_id': 'original', 'job_status': 'error'})
        source = tree_path.parent.parent
        offline_gepa.save(source/'baseline.json', {'level': 'L0'})
        code = 'gepa_baseline_evaluation_inconclusive' if baseline else 'gepa_seed_evaluation_inconclusive'
        version = 'native-L0' if baseline else 'native-seed'
        offline_gepa.DATASET.mkdir(parents=True)
        offline_gepa.save(offline_gepa.DATASET/'manifest.json', {'metadata': {'dataset_version':'native-dataset'}})
        (offline_gepa.DATASET/'val.jsonl').write_text(''.join(json.dumps({'task_id':f'val-{i}', 'task_version':f'v-{i}'})+'\n' for i in range(2)))
        offline_gepa.save(offline_gepa.CONFIG/'evaluation.json', {'fixture':True})
        offline_gepa.save(offline_gepa.CONFIG/'gepa.json', {'max_model_calls':64, 'timeout_ms':100000000000000, 'objective':{'token_weight':0.05}})
        offline_gepa.save(offline_gepa.CONFIG/'agent-evaluation.json', {'gate':{'resources_report_only':True}})
        report_path = tree_path.parent/'evaluations/entry/attempt-2/report.json'
        report_path.parent.mkdir(parents=True)
        rows=[]
        for i in range(2):
            rows.append({'trial_id':f'trial-{i}','task_id':f'val-{i}','task_version':f'v-{i}',
                'variant_id':'candidate','split':'val','repetition':1.0,'execution_status':'completed',
                'verdict':'passed' if i else 'error', 'error_code':'' if i else 'evaluation_judge_failed',
                'checks':[] if i else [{'check_id':'model','status':'error','reason':reason}],
                'metrics':{'tokens':100}, 'evidence':{'loaded_version':version,
                    'trace':{'events':[{'kind':'run.start'},{'kind':'run.end'}]},
                    'assessment':{'quality':1,'coverage':1},'loop_review':{'status':'complete','count':0}}})
        report={'schema_version':1,'report_kind':'task_evaluation.v1','complete':True,
            'dataset_version':'native-dataset','run_id':'native-report',
            'configuration':{'splits':['val'],'repetitions':1.0,'model_enabled':True,
                'allow_unreviewed':False,'judge_fingerprint':'native-judge',
                'variants':[{'id':'candidate','version':version}]},'trials':rows}
        def write_report(value):
            payload=json.dumps(value).encode();report_path.write_bytes(payload)
            digest=hashlib.sha256(payload).hexdigest()
            offline_gepa.save(report_path.with_name('manifest.json'),{'complete':True,'run_id':'native-report',
                'dataset_version':'native-dataset','files':{'report.json':digest}})
            value_tree=offline_gepa.read(tree_path)
            if value_tree.get('evaluations'):
                value_tree['evaluations']['entry']['report_hash']=digest
                offline_gepa.save(tree_path,value_tree)
            return digest
        digest=write_report(report)
        contexts={'evaluation_context':'native-grading','candidate_evaluation_context':'native-execution'}
        jobs['original'].update(error=code,seed_version='native-seed',**contexts)
        offline_gepa.save(native,{'jobs':jobs})
        offline_gepa.save(tree_path,dict(contexts,status='error',error=code,rounds=0.0,
            seed_version='native-seed',baseline_version='native-L0',dataset_version='native-dataset',model_calls=0.0,started_ms=1,
            active=None,batch=None,nodes=[{'id':0.0,'status':'seed'}],trials_used=4.0,
            evaluations={'entry':dict(contexts,version=version,split='val',status='inconclusive',attempt=2.0,
                task_ids=['val-0','val-1'],report='evaluations/entry/attempt-2/report.json',report_hash=digest)}))
        lifecycle={'status':'pending','phase':'gepa_run','candidate':reproduce._candidate_identity(seed,'strategy'),
            'stage_results':{'gepa_run':{'status':'pending','reason':'appworld_native_gepa_checkpoint_requires_resume',
                'data':{'job_id':'original'}}}}
        offline_gepa.save(offline_gepa.GEPA/'lifecycle.json',lifecycle)
        binding=patch.object(reproduce._APPWORLD_LAYOUT,'GEPA',offline_gepa.GEPA)
        binding.start();self.addCleanup(binding.stop)
        return artifact,seed,native,tree_path,report,write_report,lifecycle

    @contextmanager
    def identity(self):
        import compare
        with patch.object(offline_gepa.controller,'version',side_effect=lambda a:'native-L0' if a.get('level')=='L0' else 'native-seed'), \
             patch.object(compare,'_native_gepa_evaluation_context',return_value='native-grading'):
            yield

    def test_actual_native_double_attempt_and_generic_judge_error_are_resumable(self):
        for baseline in (False,True):
            with self.subTest(baseline=baseline),tempfile.TemporaryDirectory() as directory:
                artifact,seed,native,tree,report,write,lifecycle=self.review_fixture(directory,baseline=baseline)
                protected={p:p.read_bytes() for p in (native,tree,seed,tree.parent/'evaluations/entry/attempt-2/report.json')}
                with self.identity():
                    self.assertTrue(offline_gepa.initial_review_recovery(artifact,'original'))
                    self.assertIsNotNone(reproduce._initial_native_review_progress(lifecycle,seed))
                    self.assertEqual(reproduce._autonomous_retry_reason(RuntimeError('offline_gepa.py_failed:1'),lifecycle),
                        'appworld_native_gepa_checkpoint_requires_resume')
                    with patch.object(reproduce,'_worker_checkpoint_seed',return_value=seed), \
                         patch.object(reproduce,'_active_output_processes',return_value=[]):
                        self.assertEqual(reproduce._worker_resume_check(),75)
                    with patch.object(reproduce,'_worker_checkpoint_seed',return_value=seed), \
                         patch.object(reproduce,'_active_output_processes',return_value=['healthy writer']):
                        self.assertEqual(reproduce._worker_resume_check(),76)
                for path,payload in protected.items():self.assertEqual(path.read_bytes(),payload)

    def test_unknown_schema_auth_uncertainty_bad_execution_and_scope_are_not_resumed(self):
        for change in ('http_401','http_403','schema_invalid','uncertain','runner','loaded','task','trace','digest',
                       'holdout','cancel','contexts','attempt-bool','attempt-fraction','attempt-nan','wrong-job'):
            with self.subTest(change=change),tempfile.TemporaryDirectory() as directory:
                artifact,seed,native,tree,report,write,lifecycle=self.review_fixture(directory)
                value=offline_gepa.read(tree)
                if change in ('http_401','http_403','schema_invalid'):
                    report['trials'][0]['checks'][0]['reason']='evaluation_judge_'+change;write(report)
                elif change=='uncertain':
                    report['trials'][0].update(error_code='',verdict='uncertain',checks=[]);write(report)
                elif change=='runner':
                    report['trials'][0].update(error_code='runner_nonzero_exit',execution_status='failed');write(report)
                elif change=='loaded':report['trials'][0]['evidence']['loaded_version']='foreign';write(report)
                elif change=='task':report['trials'][0]['task_version']='foreign';write(report)
                elif change=='trace':report['trials'][0]['evidence']['trace']['events'].pop();write(report)
                elif change=='digest':(tree.parent/'evaluations/entry/attempt-2/report.json').write_text('{}')
                elif change=='holdout':value['holdout_started']=True;offline_gepa.save(tree,value)
                elif change=='contexts':value['candidate_evaluation_context']='changed';offline_gepa.save(tree,value)
                elif change.startswith('attempt-'):
                    value['evaluations']['entry']['attempt']={'attempt-bool':True,'attempt-fraction':2.5,'attempt-nan':float('nan')}[change];offline_gepa.save(tree,value)
                elif change=='wrong-job':offline_gepa.save(offline_gepa.STATUS,{'job_id':'newer'})
                else:
                    jobs=offline_gepa.read(native);jobs['jobs']['original']['cancel_requested']=True;offline_gepa.save(native,jobs)
                with self.identity():
                    if change!='wrong-job':self.assertFalse(offline_gepa.initial_review_recovery(artifact,'original'))
                    self.assertIsNone(reproduce._initial_native_review_progress(lifecycle,seed))

    def test_pending_normal_return_and_flattened_child_failure_retry_exact_current_seed(self):
        for flattened in (False,True):
            with self.subTest(flattened=flattened),tempfile.TemporaryDirectory() as directory:
                artifact,seed,native,tree,report,write,lifecycle=self.review_fixture(directory)
                before={p:p.read_bytes() for p in (native,tree,seed,tree.parent/'evaluations/entry/attempt-2/report.json')}
                calls=[]
                def inner(actual_seed,label,**budgets):
                    calls.append((actual_seed,label))
                    if len(calls)==1:
                        if flattened:raise RuntimeError('offline_gepa.py_failed:1')
                        return {'status':'pending'}
                    return {'status':'complete'}
                with self.identity(),patch.object(reproduce,'_run_all_with_seed_search',side_effect=inner), \
                     patch.object(reproduce,'_experiment_status'),patch.object(reproduce.time,'sleep') as sleep,redirect_stdout(io.StringIO()):
                    result=reproduce._run_all_autonomously(seed,'strategy')
                self.assertEqual(result['status'],'complete')
                self.assertEqual(calls,[(seed,'strategy')]*2);sleep.assert_called_once_with(15)
                for path,payload in before.items():self.assertEqual(path.read_bytes(),payload)

    def test_offline_native_poll_retries_only_the_original_job_and_keeps_paid_cache(self):
        # Reuse the existing real offline.run/LifecycleController fake-native
        # contract. Source proof is tested above, transport only is stubbed here.
        with patch.object(offline_gepa,'initial_review_recovery',return_value=True):
            test=OfflineGepaCommandRetryTests()
            test._testMethodName='test_one_command_recovers_429_same_job_without_resetting_cache_or_budget'
            try:
                state,retries,delay=test.run_command_fixture(error='gepa_baseline_evaluation_inconclusive')
            finally:
                test.doCleanups()
            self.assertEqual((state['status'],retries),('complete',1))
            self.assertGreaterEqual(delay,15)


class InitialNativeReviewRetryRejectionTests(InitialNativeReviewRecoveryTests):
    def rejection_fixture(self, http_status, body, *, operation='agent_evaluation.retry'):
        import urllib.error
        with tempfile.TemporaryDirectory() as directory:
            artifact,seed,native,tree,report,write,lifecycle=self.review_fixture(directory)
            protected={p:p.read_bytes() for p in (native,tree,seed,tree.parent/'evaluations/entry/attempt-2/report.json')}
            calls=[]
            class Process:
                def __init__(inner):
                    inner.stdout=io.StringIO('trajectory-integration.v1 listening=127.0.0.1:1\n');inner.closed=False
                def poll(inner):return 0 if inner.closed else None
                def terminate(inner):inner.closed=True
                def wait(inner,timeout=None):inner.closed=True;return 0
            process=Process()
            class Opener:
                @contextmanager
                def open(inner,request,timeout=None):
                    message=json.loads(request.data);calls.append(message['operation'])
                    if message['operation']==operation:
                        if http_status!=200:
                            raise urllib.error.HTTPError(request.full_url,http_status,'redacted',{},io.BytesIO(body.encode()))
                        yield io.StringIO(body)
                    else:
                        self.assertEqual(message['operation'],'agent_evaluation.status')
                        yield io.StringIO(json.dumps({'ok':True,'result':{'jobs':{'original':{'status':'error','error':'gepa_seed_evaluation_inconclusive'}}}}))
            with self.identity(),patch.object(offline_gepa,'require_seed_gates',return_value=artifact),                  patch.object(offline_gepa.controller,'environment',return_value={}),                  patch.object(offline_gepa.subprocess,'Popen',return_value=process),                  patch.object(offline_gepa.select,'select',return_value=([process.stdout],[],[])),                  patch.object(offline_gepa.urllib.request,'build_opener',return_value=Opener()):
                self.assertTrue(offline_gepa.initial_review_recovery(artifact,'original'))
                with self.assertRaises(RuntimeError):offline_gepa.run(seed)
                receipt=offline_gepa.read(offline_gepa.STATUS).get('retry_error')
                transient=http_status in (408,429,500,502,503,504)
                self.assertEqual(receipt is None,transient)
                self.assertEqual(offline_gepa.initial_review_recovery(artifact,'original'),transient)
                self.assertEqual(reproduce._initial_native_review_progress(lifecycle,seed) is not None,transient)
                reason=reproduce._autonomous_retry_reason(RuntimeError('offline_gepa.py_failed:1'),lifecycle)
                self.assertEqual(reason is not None,transient)
                if not transient:
                    self.assertEqual(set(receipt),{'job_id','code','http_status'})
                    self.assertEqual(receipt['job_id'],'original');self.assertEqual(receipt['http_status'],http_status)
                    self.assertNotIn('private-payload',json.dumps(receipt))
                    # A later launch checks the saved denial before retrying,
                    # so flattening the child exception cannot authorize a loop.
                    with patch.object(reproduce,'_worker_checkpoint_seed',return_value=seed),                          patch.object(reproduce,'_active_output_processes',return_value=[]):
                        self.assertEqual(reproduce._worker_resume_check(),0)
                for path,payload in protected.items():self.assertEqual(path.read_bytes(),payload)
            self.assertTrue(process.closed)
            self.assertEqual(calls.count('agent_evaluation.retry'),0 if operation.endswith('status') else 1)

    def test_permanent_native_retry_denial_stops_flattened_outer_retries(self):
        for status,code in ((409,'evaluation_initial_review_context_changed'),(409,'evaluation_job_obsolete'),
                            (409,'evaluation_job_not_retryable'),(401,'unauthorized'),
                            (400,'runtime_rule_source_not_approved')):
            with self.subTest(code=code):
                self.rejection_fixture(status,json.dumps({'error':{'code':code,'private':'private-payload'}}))
        self.rejection_fixture(401,json.dumps({'error':{'code':'unauthorized'}}),operation='agent_evaluation.status')

    def test_management_transients_do_not_create_permanent_denial_receipt(self):
        for status in (408,429,500,502,503,504):
            with self.subTest(status=status):self.rejection_fixture(status,'{}')

    def test_unknown_response_is_not_initial_review_authority(self):
        for status,body in ((409,'not JSON private-payload'),(200,'not JSON private-payload'),
                            (200,'[]'),(200,'{"ok":true,"result":[]}'),(200,'{"ok":true,"result":{"accepted":false}}'),
                            (200,'{"ok":false,"private":"private-payload"}')):
            with self.subTest(status=status,body_type=body[:1]):self.rejection_fixture(status,body)
        for result in ({}, {'jobs':[]}, {'jobs':{'foreign':{}}},
                       {'jobs':{'original':{}}}, {'jobs':{'original':{'status':'unknown'}}}):
            with self.subTest(status_response=result):
                self.rejection_fixture(200,json.dumps({'ok':True,'result':result}),operation='agent_evaluation.status')
        with tempfile.TemporaryDirectory() as directory:
            artifact,seed,native,tree,report,write,lifecycle=self.review_fixture(directory)
            with self.identity():
                offline_gepa.save(offline_gepa.STATUS,{'job_id':'original','retry_error':{'job_id':'foreign','code':'unauthorized','http_status':401}})
                self.assertTrue(offline_gepa.initial_review_recovery(artifact,'original'))
                offline_gepa.save(offline_gepa.STATUS,{'job_id':'original','retry_error':[]})
                self.assertFalse(offline_gepa.initial_review_recovery(artifact,'original'))


class StageKnownTransientConsistencyTests(unittest.TestCase):
    baseline_fixture = Baseline100InterruptedWorkerRecoveryTests.fixture

    def test_closed_stage32_and100_http500_and_partial_transport_are_retryable(self):
        import copy,compare
        for count in (32,100):
            for reason in ('evaluation_judge_http_500','evaluation_judge_transport_18'):
                with self.subTest(count=count,reason=reason),tempfile.TemporaryDirectory() as directory:
                    root=Path(directory);source=root/'shards/shard-00/capture/report.json'
                    current=reproduce._APPWORLD_LAYOUT.judge_projection()
                    rows=[{'task_id':f'task-{i:03d}','trial_id':f'trial-{i:03d}',
                           'execution_status':'completed','verdict':'error','error_code':'evaluation_judge_failed',
                           'checks':[{'check_id':'model','status':'error','reason':reason}],
                           'evidence':{'output':{'framework_capture':{'status':'complete'}},
                                       'trace':{'events':[{'kind':'run.start'},{'kind':'run.end'}]}}}
                          for i in range(count)]
                    reproduce._APPWORLD_LAYOUT.save(source,{'complete':True,'trials':rows})
                    reproduce._APPWORLD_LAYOUT.save(source.with_name('manifest.json'),{'complete':True})
                    reproduce._APPWORLD_LAYOUT.save(source.parent.parent/'config.json',{'judge':current})
                    reproduce._APPWORLD_LAYOUT.save(root/'progress.json',{'planned_tasks':count,
                        'shards':{'0':{'execution_complete':True,'source_report':str(source)}}})
                    before={p:p.read_bytes() for p in root.rglob('*') if p.is_file()}
                    self.assertEqual(reproduce._stage_review_recovery_state(root,count,baseline=False),'transport')
                    for p,old in before.items():self.assertEqual(p.read_bytes(),old)
                    for invalid in ('evaluation_judge_http_401','evaluation_judge_http_403',
                                    'evaluation_judge_schema_invalid','evaluation_judge_uncertain'):
                        rows[0]['checks'][0]['reason']=invalid
                        reproduce._APPWORLD_LAYOUT.save(source,{'complete':True,'trials':rows})
                        self.assertIsNone(reproduce._stage_review_recovery_state(root,count,baseline=False))
                    rows[0]['checks'][0]['reason']=reason;rows[0]['execution_status']='error'
                    reproduce._APPWORLD_LAYOUT.save(source,{'complete':True,'trials':rows})
                    self.assertIsNone(reproduce._stage_review_recovery_state(root,count,baseline=False))

    def test_baseline100_atomic_http500_and_transport18_resume_without_replaying_agents(self):
        for reason in ('evaluation_judge_http_500','evaluation_judge_transport_18'):
            with self.subTest(reason=reason),tempfile.TemporaryDirectory() as directory,self.baseline_fixture(directory) as (root,state,seed,baseline,trial,pilot):
                row=reproduce._read(trial);row.update(verdict='error',error_code='evaluation_judge_failed')
                row['checks']=[{'check_id':'model','status':'error','reason':reason}]
                reproduce._APPWORLD_LAYOUT.save(trial,row)
                before={p:p.read_bytes() for p in root.rglob('*') if p.is_file()}
                self.assertEqual(reproduce._baseline100_worker_checkpoint(seed,state),'transport')
                self.assertEqual(reproduce._autonomous_retry_reason(RuntimeError('compare.py_failed:1'),state),'appworld_stage_review_pending')
                with patch.object(reproduce,'_active_output_processes',return_value=[]):self.assertEqual(reproduce._worker_resume_check(),75)
                state['cancel_requested']=True
                self.assertIsNone(reproduce._baseline100_worker_checkpoint(seed,state))
                for p,old in before.items():self.assertEqual(p.read_bytes(),old)

class FinalNativeReviewBudgetRecoveryTests(InitialNativeReviewRecoveryTests):
    def final_fixture(self, directory):
        artifact,seed,native,tree_path,report,write,lifecycle=self.review_fixture(directory,reason='evaluation_judge_http_503')
        dataset=offline_gepa.read(offline_gepa.DATASET/'manifest.json');dataset['summary']={'splits':{'holdout':2}};offline_gepa.save(offline_gepa.DATASET/'manifest.json',dataset)
        (offline_gepa.DATASET/'holdout.jsonl').write_bytes((offline_gepa.DATASET/'val.jsonl').read_bytes())
        plan=offline_gepa.read(offline_gepa.PLAN);plan['split_counts']={'holdout':2};offline_gepa.save(offline_gepa.PLAN,plan)
        report['configuration']['splits']=['holdout']
        for row in report['trials']:row['split']='holdout'
        write(report)
        tree=offline_gepa.read(tree_path);tree.pop('error');tree.update(status='inconclusive',rounds=4.0,model_calls=64.0,holdout_started=True)
        tree['evaluations']['entry'].update(split='holdout')
        final={'protocol':'top-k-v1','campaign_id':'frozen-campaign','status':'inconclusive','candidates':[{'node':0.0,'rank':1.0,'version':'native-seed','status':'inconclusive','report_kind':'task_evaluation.v1','trials':2.0}]}
        tree['final_holdout']=final;offline_gepa.save(tree_path,tree);offline_gepa.save(tree_path.parent/'versions/0.json',artifact)
        state=offline_gepa.read(native);job=state['jobs']['original'];job.pop('error');job.update(status='inconclusive',gepa=True,search={'final_holdout':final});offline_gepa.save(native,state)
        offline_gepa.save(offline_gepa.STATUS,{'job_id':'original','job_status':'inconclusive'})
        return artifact,seed,native,tree_path,report,write,lifecycle

    def test_final_pending_at_reflection_cap_resumes_same_job_without_new_budget(self):
        with tempfile.TemporaryDirectory() as directory:
            artifact,seed,native,tree,report,write,state=self.final_fixture(directory)
            protected={p:p.read_bytes() for p in (native,tree,seed,tree.parent/'evaluations/entry/attempt-2/report.json')}
            calls=[]
            def prefix(actual,label,**budgets):
                calls.append((actual,budgets));return {'status':'pending' if len(calls)==1 else 'complete','phase':'gepa_run'}
            with self.identity(),patch.object(reproduce,'_run_all_with_seed_search',side_effect=prefix), \
                    patch.object(reproduce,'_autonomous_native_seed',return_value=(seed,'strategy')), \
                    patch.object(reproduce,'_rate_limited_native_resume_progress',return_value=None), \
                    patch.object(reproduce,'_initial_native_review_progress',return_value=None), \
                    patch.object(reproduce,'_experiment_status'),patch.object(reproduce.time,'sleep'),redirect_stdout(io.StringIO()):
                self.assertTrue(offline_gepa.final_holdout_review_recovery(artifact,'original'))
                self.assertIsNotNone(reproduce._final_native_review_progress(state,seed))
                self.assertEqual(reproduce._run_all_autonomously(seed,'strategy')['status'],'complete')
                self.assertEqual(calls,[(seed,{}),(seed,{})])
            for path,before in protected.items():self.assertEqual(path.read_bytes(),before)

    def test_final_review_source_unknown_cancel_expired_and_prior_denial_do_not_allow_cap_exception(self):
        for failure in ('401','schema','unknown','cancel','source','context','version','open','hash','deadline','denial'):
            with self.subTest(failure=failure),tempfile.TemporaryDirectory() as directory:
                artifact,seed,native,tree_path,report,write,state=self.final_fixture(directory)
                tree=offline_gepa.read(tree_path);jobs=offline_gepa.read(native)
                if failure in ('401','schema','unknown'):
                    report['trials'][0]['checks'][0]['reason']={'401':'evaluation_judge_http_401','schema':'evaluation_judge_schema_invalid','unknown':'evaluation_judge_uncertain'}[failure];write(report)
                elif failure=='cancel':jobs['jobs']['original']['cancel_requested']=True
                elif failure=='source':report['trials'][0]['task_version']='foreign';write(report)
                elif failure=='context':tree['candidate_evaluation_context']='foreign'
                elif failure=='version':tree['final_holdout']['candidates'][0]['version']='foreign'
                elif failure=='open':report['trials'][0]['evidence']['trace']['events'].pop();write(report)
                elif failure=='hash':tree['evaluations']['entry']['report_hash']='changed'
                elif failure=='deadline':
                    cfg=offline_gepa.read(offline_gepa.CONFIG/'gepa.json');cfg['timeout_ms']=1;offline_gepa.save(offline_gepa.CONFIG/'gepa.json',cfg)
                else:offline_gepa.save(offline_gepa.STATUS,{'job_id':'original','retry_error':{'job_id':'original','code':'evaluation_context_changed'}})
                # The writer owns report_hash updates; never revert its newest source pointer.
                if failure in ('context','version','hash'):offline_gepa.save(tree_path,tree)
                offline_gepa.save(native,jobs)
                with self.identity():
                    self.assertFalse(offline_gepa.final_holdout_review_recovery(artifact,'original'))
                    self.assertIsNone(reproduce._final_native_review_progress(state,seed))


    def test_native_cached_final_pair_selects_frozen_report_without_fresh_entry_ambiguity(self):
        import copy
        with tempfile.TemporaryDirectory() as directory:
            artifact,seed,native,tree_path,report,write,state=self.final_fixture(directory)
            tree=offline_gepa.read(tree_path)
            fresh=copy.deepcopy(tree['evaluations']['entry'])
            tree['evaluations']['fresh-candidate']=fresh
            tree['evaluations']['entry'].update(baseline_version='native-L0',baseline_source={'kind':'locked-native-source'})
            final=tree['final_holdout']['candidates'][0]
            final.update(report_kind='gepa_cached_pair.v1',trials=4.0,baseline_source={'kind':'locked-native-source'})
            offline_gepa.save(tree_path,tree)
            jobs=offline_gepa.read(native);jobs['jobs']['original']['search']['final_holdout']=tree['final_holdout'];offline_gepa.save(native,jobs)
            report['report_kind']='gepa_cached_pair.v1'
            report['configuration']['variants'].insert(0,{'id':'baseline','version':'native-L0'})
            baseline=copy.deepcopy(report['trials'])
            for row in baseline:
                row.update(variant_id='baseline',trial_id=row['trial_id']+'-baseline',verdict='passed',error_code='',checks=[])
                row['evidence']['loaded_version']='native-L0'
            report['trials']=baseline+report['trials'];write(report)
            with self.identity():
                self.assertTrue(offline_gepa.final_holdout_review_recovery(artifact,'original'))
                self.assertIsNotNone(reproduce._final_native_review_progress(state,seed))
                changed=offline_gepa.read(tree_path);changed['final_holdout']['candidates'][0]['baseline_source']={'kind':'different'}
                offline_gepa.save(tree_path,changed);jobs['jobs']['original']['search']['final_holdout']=changed['final_holdout'];offline_gepa.save(native,jobs)
                self.assertFalse(offline_gepa.final_holdout_review_recovery(artifact,'original'))


class CompletedFinalGoalContinuationTests(unittest.TestCase):
    native_publish_fixture = StageGateRegressionTest.native_publish_fixture

    @contextmanager
    def completed_final_fixture(self, directory):
        root=Path(directory)
        with self.native_publish_fixture(root,count=1,correct=200) as native:
            seed=root/'published/strategy/strategy.json'
            artifact={'level':'L3','policy':{},'agent':{'revision':'immutable-seed'}}
            reproduce._APPWORLD_LAYOUT.save(seed,artifact)
            reproduce._APPWORLD_LAYOUT.save(seed.with_name('manifest.json'),{'version':'immutable-seed'})
            gepa=native['gepa'];jobroot=native['tree_path'].parent.parent
            offline_gepa.save(jobroot/'seed.json',artifact)
            tree=native['tree'];tree.update(seed_version='immutable-seed',holdout_started=True,active=None,batch=None,rounds=30,model_calls=500,trials_used=1000)
            native['tree_path'].write_text(json.dumps(tree))
            jobs={'fixture-job':{'user':offline_gepa.USER,'level':'L3','gepa':True,'status':'rejected',
                'evaluation_context':tree['evaluation_context'],'candidate_evaluation_context':tree['candidate_evaluation_context'],
                'search':{'final_holdout':tree['final_holdout']}}}
            offline_gepa.save(offline_gepa.WORKSPACE/'agent-evaluation/state.json',{'jobs':jobs})
            offline_gepa.save(offline_gepa.PLAN,{'seed':str(seed.resolve())})
            offline_gepa.save(gepa/'submission.json',{'job_id':'fixture-job'})
            offline_gepa.save(offline_gepa.STATUS,{'job_id':'fixture-job','job_status':'rejected'})
            offline_gepa.publish(proof_only=True)
            reproduce._save_search({'round':1,'status':'found','current_seed':str(seed.resolve()),
                'attempts':[{'attempt':1,'seed':str(seed.resolve()),'status':'passed'}]})
            state={'status':'blocked','phase':'comparison500','last_error':'stage_gate_not_passed:comparison500',
                'candidate':reproduce._candidate_identity(seed,'strategy'),'stage_results':{
                'prepare':{'status':'complete'},'train':{'status':'complete'},
                'gepa_run':{'status':'complete','data':{'job_id':'fixture-job'}},
                'comparison500':{'status':'complete','gate_passed':False}}}
            offline_gepa.save(gepa/'lifecycle.json',state)
            with patch.object(reproduce._APPWORLD_LAYOUT,'GEPA',gepa),patch.object(reproduce,'_native_gepa_service_active',return_value=False):
                yield root,seed,state,native,jobs

    def test_completed_final_rejection_readonly_worker_reserves_distinct_train_candidate_once(self):
        with tempfile.TemporaryDirectory() as directory,self.completed_final_fixture(directory) as (root,seed,state,native,jobs):
            protected={path:path.read_bytes() for path in [seed,native['tree_path'],*native['reports']]}
            proof_before=(native['gepa']/'final-comparison/manifest.json').read_bytes()
            before={p:p.read_bytes() for p in root.rglob('*') if p.is_file()}
            checkpoint=reproduce._completed_failed_final_checkpoint(state)
            self.assertIsNotNone(checkpoint)
            with patch.object(reproduce,'_active_output_processes',return_value=[]):self.assertEqual(reproduce._worker_resume_check(),75)
            with patch.object(reproduce,'_active_output_processes',return_value=[123]):self.assertEqual(reproduce._worker_resume_check(),76)
            for p,bytes_before in before.items():self.assertEqual(p.read_bytes(),bytes_before)
            seen=[]
            def prefix(candidate,label,**budgets):
                seen.append(candidate);self.assertNotEqual(candidate,seed)
                self.assertFalse(offline_gepa.PLAN.exists());self.assertFalse((native['gepa']/'submission.json').exists())
                return {'status':'published','phase':'publish'}
            with patch.object(reproduce,'_run_all_with_seed_search',side_effect=prefix):
                self.assertEqual(reproduce._run_all_autonomously(seed,'strategy')['status'],'published')
            self.assertEqual(len(seen),1)
            search=reproduce.seed_search();self.assertEqual(len(search['native_job_history']),1)
            self.assertEqual(search['native_job_history'][0]['final_proof'],checkpoint['proof'])
            feedback=search['automatic_seed_generation']['native_search_feedback']
            self.assertEqual(feedback['stop_reason'],'appworld_gepa_final_goal_not_met')
            self.assertNotIn('final_proof',feedback);self.assertNotIn('candidate_goal_checks',feedback)
            self.assertEqual((native['gepa']/'final-comparison/manifest.json').read_bytes(),proof_before)
            for p,old in protected.items():self.assertEqual(p.read_bytes(),old)
            self.assertIsNone(reproduce._completed_failed_final_checkpoint(offline_gepa.read(native['gepa']/'lifecycle.json')))

    def test_completed_final_permanent_unknown_incomplete_or_changed_source_never_advances(self):
        for fault in ('partial','source','cancel','unknown','state-unknown','judge401','changed-proof'):
            with self.subTest(fault=fault),tempfile.TemporaryDirectory() as directory,self.completed_final_fixture(directory) as (root,seed,state,native,jobs):
                if fault=='partial':native['tree']['final_holdout']['candidates'][0]['status']='inconclusive'
                elif fault=='source':native['tree']['seed_version']='different'
                elif fault=='cancel':jobs['fixture-job']['cancel_requested']=True
                elif fault=='unknown':jobs['fixture-job']['error']='source_authority_revoked'
                elif fault=='state-unknown':state['last_error']='source_authority_revoked';offline_gepa.save(native['gepa']/'lifecycle.json',state)
                elif fault=='judge401':
                    report=offline_gepa.read(native['reports'][0]);report['trials'][-1].update(verdict='error',error_code='evaluation_judge_failed',checks=[{'check_id':'model','status':'error','reason':'evaluation_judge_http_401'}]);native['reports'][0].write_text(json.dumps(report))
                else:
                    proof=offline_gepa.read(native['gepa']/'final-comparison/manifest.json');proof['goal_passed']=True;offline_gepa.save(native['gepa']/'final-comparison/manifest.json',proof)
                native['tree_path'].write_text(json.dumps(native['tree']));offline_gepa.save(offline_gepa.WORKSPACE/'agent-evaluation/state.json',{'jobs':jobs})
                protected={p:p.read_bytes() for p in root.rglob('*') if p.is_file()}
                self.assertIsNone(reproduce._completed_failed_final_checkpoint(state))
                with patch.object(reproduce,'_active_output_processes',return_value=[]):self.assertEqual(reproduce._worker_resume_check(),0)
                for p,before in protected.items():self.assertEqual(p.read_bytes(),before)


    def test_completed_final_inprocess_gate_failure_continues_without_repeating_holdout(self):
        with tempfile.TemporaryDirectory() as directory,self.completed_final_fixture(directory) as (root,seed,state,native,jobs):
            initial=dict(state,status='running',last_error=None)
            initial['stage_results']=dict(state['stage_results'],comparison500={'status':'pending'})
            offline_gepa.save(native['gepa']/'lifecycle.json',initial)
            calls=[]
            def prefix(actual_seed,label,**budgets):
                calls.append(actual_seed)
                if len(calls)==1:
                    offline_gepa.save(native['gepa']/'lifecycle.json',state)
                    raise RuntimeError('stage_gate_not_passed:comparison500')
                self.assertNotEqual(actual_seed,seed)
                return {'status':'published','phase':'publish'}
            with patch.object(reproduce,'_run_all_with_seed_search',side_effect=prefix):
                self.assertEqual(reproduce._run_all_autonomously(seed,'strategy')['status'],'published')
            self.assertEqual(len(calls),2)
            self.assertEqual(len(reproduce.seed_search()['native_job_history']),1)
