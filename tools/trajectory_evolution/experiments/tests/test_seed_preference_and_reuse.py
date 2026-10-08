"""User seed preference and same-execution holdout reuse; no model calls."""
from contextlib import ExitStack
from copy import deepcopy
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

EXPERIMENTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(EXPERIMENTS))
sys.path.insert(0, str(EXPERIMENTS / 'business-coverage'))
import best_strategy as b
import continuous_seed_delivery as c
import test_deadline_finish as support

JUDGE = '605-fake-current-judge'


class SeedPreferenceTests(unittest.TestCase):
    def fixture(self, root):
        current = support.view(alive=False)
        best = support.DeadlineTests().fake_best(root, current)
        source = root / 'seed-val50.json'
        c.d.write(source, {'configuration': {'judge_fingerprint': JUDGE}})
        historical = root / 'node1-val50.json'
        c.d.write(historical, {'configuration': {'judge_fingerprint': JUDGE}})
        best._cached_val_report = lambda *args: ([], str(historical))
        external = {'job_id': support.JOB, 'node': None,
            'external_candidate_id': 'seed011', 'version': 'seed-v011',
            'artifact': {'version': 'seed-v011'},
            'validation': {'quality': .6, 'strict_passed': 25},
            'screening_task_ids': sorted(current['tree']['nodes'][0]['validation']),
            'screening_report_path': str(source), 'judge_fingerprint': JUDGE,
            'selection_source': 'external_seed_registry'}
        best.discover_external_seed_candidates = Mock(return_value=[external])
        best._independent_val_candidates.return_value = [external]
        return best, current, external

    def test_user_prefers_measured_seed_despite_higher_node1_quality(self):
        with tempfile.TemporaryDirectory() as temp:
            best, current, external = self.fixture(Path(temp))
            chosen, pool = c.d.business_selection(best, current,
                required_judge=JUDGE, prefer_seeds=True)
            self.assertEqual(chosen['version'], external['version'])
            self.assertLess(chosen['validation']['quality'], .7)
            self.assertTrue(any(item['node'] == 1 for item in pool),
                            'historical Node1 remains a peer audit candidate')
            self.assertEqual(chosen['validation'], external['validation'])
            best.run_comparison.assert_not_called()

    def test_seed_freeze_restores_same_selection_after_scores_change(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            best, current, external = self.fixture(root)
            freeze = root / 'frozen/selection.json'
            original, _ = c.d.business_selection(best, current, freeze_path=freeze,
                required_judge=JUDGE, prefer_seeds=True)
            original = deepcopy(original)
            before = freeze.read_bytes()
            external['validation'] = {'quality': .95, 'strict_passed': 48}
            current['tree']['nodes'][1] = support.node(1, .99)
            restored, _ = c.d.business_selection(best, current, freeze_path=freeze,
                required_judge=JUDGE, prefer_seeds=True)
            self.assertEqual(restored['version'], original['version'])
            self.assertEqual(restored['validation'], original['validation'])
            self.assertEqual(freeze.read_bytes(), before)
            best.run_comparison.assert_not_called()

    def test_seed_preference_replaces_higher_scoring_old_node_primary(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            best, current, external = self.fixture(root)
            state = root / 'state'
            old = c.promote(best, current, state, 'old',
                            required_judge=JUDGE, prefer_seeds=False)
            self.assertEqual(old['node'], 1)
            old_bytes = Path(old['selection_file']).read_bytes()
            preferred = c.promote(best, current, state, 'seed-preference',
                                  required_judge=JUDGE, prefer_seeds=True)
            self.assertEqual(preferred['version'], external['version'])
            self.assertEqual(preferred['candidate_id'], 'seed011')
            self.assertEqual(preferred['supersedes'], old['version'])
            self.assertEqual(Path(old['selection_file']).read_bytes(), old_bytes)
            unchanged = c.promote(best, current, state, 'resume',
                                 required_judge=JUDGE, prefer_seeds=True)
            self.assertEqual(unchanged['version'], preferred['version'])
            self.assertEqual(unchanged['selection_file'], preferred['selection_file'])


class HoldoutReuseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.output = self.root / 'output'
        self.source = self.root / 'dataset'
        self.source.mkdir()
        self.ids = ['holdout-a', 'holdout-b']
        (self.source / 'val.jsonl').write_text('{"task_id":"val-a"}\n')
        (self.source / 'holdout.jsonl').write_text(''.join(
            json.dumps({'task_id': task})+'\n' for task in self.ids))
        self.selection = {'job_id': support.JOB, 'node': None,
                          'version': 'seed-v011', 'external_candidate_id': 'seed011'}
        stack = ExitStack()
        self.addCleanup(stack.close)
        stack.enter_context(patch.object(b, 'ROOT', self.root))
        stack.enter_context(patch.object(b, 'OUTPUT', self.output))
        stack.enter_context(patch.object(b, 'SOURCE', self.source))
        self.spawn = stack.enter_context(patch.object(b.subprocess, 'run',
            side_effect=AssertionError('reuse must not start Agent or model')))
        c.d.write(self.output / 'audit/l0-400-20260929/judge-profile.json',
                  {'status': 'complete', 'judge_fingerprint': JUDGE})

    def cache(self, previous_purpose='deadline_frozen_audit'):
        version = self.selection['version']
        tag = b._sha256_bytes(version.encode())[:10]
        task_hash = b._sha256_bytes('\n'.join(sorted(self.ids)).encode())
        folder = self.output / 'published/best-strategy-runs' / support.JOB / (
            f'node-None-{tag}-{previous_purpose}-holdout-2-{task_hash[:10]}-candidate')
        report_path = folder / 'native.json'
        report = {'complete': True, 'configuration': {'judge_fingerprint': JUDGE},
            'trials': [{'task_id': task, 'trial_id': 'trial-'+task,
                       'variant_id': 'best', 'split': 'holdout', 'verdict': 'passed',
                       'evidence': {'loaded_version': version,
                                    'assessment': {'quality': 1.0}}} for task in self.ids]}
        metrics = {'best': {'holdout': {'quality_complete': True,
                   'completed': 2, 'scored': 2, 'errors': 0}}}
        result = {'status': 'complete', 'version': version, 'report_path': str(report_path),
            'metrics': metrics, 'request_identity': {'task_ids_sha256': task_hash,
                                                   'include_l0': False}}
        request = {'source_dataset_sha256': ':'.join(
            b._sha256(self.source / f'{split}.jsonl') for split in ('val', 'holdout'))}
        c.d.write(report_path, report)
        c.d.write(folder / 'result.json', result)
        c.d.write(folder / 'request.json', request)
        return folder, report, result

    def test_completed_holdout_reused_both_directions_between_best_and_peer(self):
        for previous, purpose in [('deadline_frozen_audit', 'deadline_holdout_best'),
                                  ('deadline_holdout_best', 'deadline_frozen_audit')]:
            with self.subTest(previous=previous, purpose=purpose):
                folder, report, result = self.cache(previous)
                original = (folder / 'result.json').read_bytes()
                reused = b._reuse_candidate_holdout(self.selection, self.ids, purpose)
                self.assertIsNotNone(reused)
                self.assertEqual(reused['new_agent_tasks'], 0)
                self.assertEqual(reused['version'], self.selection['version'])
                self.assertEqual(reused['reused_for_purpose'], purpose)
                self.assertEqual(reused['metrics'], result['metrics'])
                self.assertEqual((folder / 'result.json').read_bytes(), original)
                self.spawn.assert_not_called()
                # Remove only this synthetic result so next direction cannot
                # accidentally reuse the earlier subtest's same-policy cache.
                (folder / 'result.json').unlink()

    def test_reuse_rejects_wrong_judge_task_or_loaded_artifact(self):
        folder, valid_report, _ = self.cache()
        cases = {}
        cases['old_judge'] = deepcopy(valid_report)
        cases['old_judge']['configuration']['judge_fingerprint'] = 'old-judge'
        cases['wrong_task'] = deepcopy(valid_report)
        cases['wrong_task']['trials'][1]['task_id'] = 'different-task'
        cases['wrong_loaded_version'] = deepcopy(valid_report)
        cases['wrong_loaded_version']['trials'][0]['evidence']['loaded_version'] = 'different-policy'
        cases['wrong_split'] = deepcopy(valid_report)
        cases['wrong_split']['trials'][0]['split'] = 'val'
        cases['incomplete'] = deepcopy(valid_report)
        cases['incomplete']['complete'] = False
        for name, report in cases.items():
            with self.subTest(case=name):
                c.d.write(folder / 'native.json', report)
                self.assertIsNone(b._reuse_candidate_holdout(
                    self.selection, self.ids, 'deadline_holdout_best'))
        self.spawn.assert_not_called()

    def test_reuse_rejects_changed_dataset_or_different_requested_task_set(self):
        self.cache()
        self.assertIsNone(b._reuse_candidate_holdout(
            self.selection, ['holdout-a', 'other-task'], 'deadline_holdout_best'))
        (self.source / 'holdout.jsonl').write_text('{"task_id":"changed-data"}\n')
        self.assertIsNone(b._reuse_candidate_holdout(
            self.selection, self.ids, 'deadline_holdout_best'))
        self.spawn.assert_not_called()


if __name__ == '__main__':
    unittest.main()
