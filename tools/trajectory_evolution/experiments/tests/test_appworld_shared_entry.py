"""Model-free checks for AppWorld's single lifecycle entry point."""
from __future__ import annotations

from pathlib import Path
import json
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve()
APPWORLD = HERE.parents[1] / "appworld"
if str(APPWORLD) not in sys.path:
    sys.path.insert(0, str(APPWORLD))
import reproduce  # noqa: E402


class AppWorldSharedEntryTest(unittest.TestCase):
    def test_implicit_gepa_resume_follows_recorded_candidate_label(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            seed = root / 'published/accepted-candidate/strategy.json'
            with patch.object(reproduce, 'OUTPUT', root), \
                 patch.object(reproduce, 'seed_search', return_value={'current_seed': str(seed)}), \
                 patch.object(reproduce, '_run_common_all', return_value={'status': 'complete'}) as common, \
                 patch.object(reproduce, '_experiment_status'), \
                 patch.object(sys, 'argv', ['reproduce.py', '--gepa-run',
                     '--gepa-max-model-calls', '1024']):
                self.assertEqual(reproduce.main(), 0)
            common.assert_called_once_with(seed, 'accepted-candidate', stop_at='gepa_run',
                                           gepa_max_model_calls=1024)

    def test_implicit_gepa_publish_and_all_use_plan_seed_before_search_candidate(self):
        for flag, stop_at in (('--gepa-run', 'gepa_run'), ('--publish', 'publish'), ('--all', None)):
            with self.subTest(flag=flag), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                bound = root / 'published/candidate-typeerror-causal-current/strategy.json'
                search = root / 'published/candidate-saved-result-recovery-current/strategy.json'
                bound.parent.mkdir(parents=True)
                bound.write_text('{}')
                (root / 'gepa').mkdir()
                (root / 'gepa/plan.json').write_text(json.dumps({'seed': str(bound)}))
                with patch.object(reproduce, 'OUTPUT', root), \
                     patch.object(reproduce, 'seed_search', return_value={'current_seed': str(search)}) as history, \
                     patch.object(reproduce, '_run_common_all', return_value={'status': 'complete'}) as common, \
                     patch.object(reproduce, '_experiment_status'), \
                     patch.object(sys, 'argv', ['reproduce.py', flag]):
                    self.assertEqual(reproduce.main(), 0)
                common.assert_called_once_with(bound, 'candidate-typeerror-causal-current', stop_at=stop_at)
                history.assert_not_called()

    def test_explicit_seed_selection_is_not_overridden_by_plan(self):
        for flag, stop_at in (('--gepa-run', 'gepa_run'), ('--publish', 'publish'), ('--all', None)):
            for option in ('--seed-artifact', '--seed-label'):
                with self.subTest(flag=flag, option=option), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    explicit = root / 'published/explicit-candidate/strategy.json'
                    planned = root / 'published/candidate-typeerror-causal-current/strategy.json'
                    planned.parent.mkdir(parents=True)
                    planned.write_text('{}')
                    (root / 'gepa').mkdir()
                    (root / 'gepa/plan.json').write_text(json.dumps({'seed': str(planned)}))
                    value = str(explicit) if option == '--seed-artifact' else 'explicit-candidate'
                    with patch.object(reproduce, 'OUTPUT', root), \
                         patch.object(reproduce, 'seed_search') as history, \
                         patch.object(reproduce, '_run_common_all', return_value={'status': 'complete'}) as common, \
                         patch.object(reproduce, '_experiment_status'), \
                         patch.object(sys, 'argv', ['reproduce.py', flag, option, value]):
                        self.assertEqual(reproduce.main(), 0)
                    common.assert_called_once_with(explicit, 'explicit-candidate', stop_at=stop_at)
                    history.assert_not_called()

    def test_all_without_existing_plan_seed_starts_at_seed0(self):
        for missing_seed_plan in (False, True):
            with self.subTest(missing_seed_plan=missing_seed_plan), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                if missing_seed_plan:
                    (root / 'gepa').mkdir()
                    (root / 'gepa/plan.json').write_text(json.dumps({'seed': str(
                        root / 'published/missing-prior-seed/strategy.json')}))
                with patch.object(reproduce, 'OUTPUT', root), \
                     patch.object(reproduce, 'seed_search', return_value={
                         'current_seed': str(root / 'published/stale-candidate/strategy.json')}) as history, \
                     patch.object(reproduce, '_run_common_all', return_value={'status': 'complete'}) as common, \
                     patch.object(reproduce, '_experiment_status'), \
                     patch.object(sys, 'argv', ['reproduce.py', '--all']):
                    self.assertEqual(reproduce.main(), 0)
                common.assert_called_once_with(root / 'published/seed0/strategy.json', 'seed0', stop_at=None)
                history.assert_not_called()

    def test_pilot_and_validation_still_follow_current_search_seed(self):
        for flag, stop_at in (('--pilot32', 'pilot32'), ('--validation100', 'validation100')):
            with self.subTest(flag=flag), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                candidate = root / 'published/current-candidate/strategy.json'
                (root / 'gepa').mkdir()
                (root / 'gepa/plan.json').write_text(json.dumps({'seed': str(
                    root / 'published/gepa-bound-candidate/strategy.json')}))
                with patch.object(reproduce, 'OUTPUT', root), \
                     patch.object(reproduce, 'seed_search', return_value={'current_seed': str(candidate)}), \
                     patch.object(reproduce, '_run_common_all', return_value={'status': 'complete'}) as common, \
                     patch.object(reproduce, '_experiment_status'), \
                     patch.object(sys, 'argv', ['reproduce.py', flag]):
                    self.assertEqual(reproduce.main(), 0)
                common.assert_called_once_with(candidate, 'current-candidate', stop_at=stop_at)

    def test_plan_seed_keeps_existing_published_path_guard(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'gepa').mkdir()
            (root / 'gepa/plan.json').write_text(json.dumps({'seed': str(root / 'outside/strategy.json')}))
            with patch.object(reproduce, 'OUTPUT', root), \
                 patch.object(reproduce, '_run_common_all') as common, \
                 patch.object(reproduce, '_experiment_status'), \
                 patch.object(sys, 'argv', ['reproduce.py', '--gepa-run']):
                with self.assertRaisesRegex(RuntimeError, 'appworld_seed_artifact_must_match_publish_label'):
                    reproduce.main()
            common.assert_not_called()

    def test_mutating_stage_flag_selects_shared_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            calls = []

            def fake_common(seed, label, *, stop_at=None):
                calls.append((Path(seed), label, stop_at))
                return {"status": "complete", "phase": stop_at}

            with patch.object(reproduce, "OUTPUT", Path(directory)), \
                 patch.object(reproduce, "_run_common_all", side_effect=fake_common), \
                 patch.object(sys, "argv", ["reproduce.py", "--validation100"]):
                self.assertEqual(reproduce.main(), 0)

            self.assertEqual(len(calls), 1)
            self.assertEqual(calls[0][2], "validation100")

    def test_restart_uses_same_shared_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            seed = root / 'published/parent/strategy.json'
            with patch.object(reproduce, 'OUTPUT', root), \
                 patch.object(reproduce, 'restart_gepa', return_value={'status': 'old_gepa_removed'}) as reset, \
                 patch.object(reproduce, '_run_common_all', return_value={'status': 'complete'}) as common, \
                 patch.object(sys, 'argv', ['reproduce.py', '--restart-gepa',
                     '--seed-artifact', str(seed), '--gepa-max-model-calls', '1024']):
                self.assertEqual(reproduce.main(), 0)
            reset.assert_called_once_with(seed)
            common.assert_called_once_with(seed, 'parent', stop_at='gepa_run',
                                           gepa_max_model_calls=1024)


class AppWorldSearchResetTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.output = Path(self.temporary.name)
        self.gepa = self.output / 'gepa'
        self.gepa.mkdir()
        self.seed = self.output / 'published/parent/strategy.json'
        self.seed.parent.mkdir(parents=True)
        self.seed.write_text('{"approved":true}\n')
        self.native = self.gepa / 'workspace/agent-evaluation/prior/search/tree.json'
        self.write(self.native, {'rounds': 30, 'model_calls': 395,
                               'nodes': [{'status': 'accepted'}, {'status': 'error'}]})
        self.write(self.gepa / 'submission.json', {'job_id': 'prior'})
        self.write(self.gepa / 'status.json', {'seed_search': {'status': 'found'}})
        self.write(self.gepa / 'lifecycle.json', {'stage_results': {
            'train': {'status': 'complete'}, 'pilot32': {'status': 'complete', 'gate_passed': True},
            'validation100': {'status': 'complete', 'gate_passed': True},
            'gepa_run': {'status': 'pending'}}})
        for count in (32, 100, 500):
            self.write(self.gepa / f'l0-current-stage{count}/result.json', {'score': count})
            if count != 500:
                self.write(self.gepa / f'parent-stage{count}/comparison.json', {'score': count})
        self.write(self.gepa / 'published-source/report.json', {'score': .676})
        self.write(self.output / 'published/strategy/manifest.json', {
            'native_source_reports': [str(self.gepa / 'published-source/report.json')]})
        self.write(self.gepa / 'discarded-node/report.json', {'discard': True})
        self.layout = SimpleNamespace(GEPA=self.gepa, PUBLISHED=self.output / 'published',
            baseline_result=lambda count: self.gepa / f'l0-current-stage{count}/result.json',
            save=self.write)
        for target, value in [('_APPWORLD_LAYOUT', self.layout),
            ('stage_path', lambda seed, count: self.gepa / f'parent-stage{count}/comparison.json'),
            ('_stage_l0_source', lambda count, comparison: self.layout.baseline_result(count))]:
            p = patch.object(reproduce, target, value); p.start(); self.addCleanup(p.stop)
        self.idle = patch.object(reproduce, 'assert_idle_for_fresh').start()
        self.verify = patch.object(reproduce, 'verify').start()
        self.addCleanup(patch.stopall)

    @staticmethod
    def write(path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value))

    def test_reset_removes_tree_but_preserves_grades_and_seed(self):
        before = self.seed.read_bytes()
        source = self.gepa / 'published-source/report.json'
        scores = source.read_bytes()
        result = reproduce.restart_gepa(self.seed)
        self.assertFalse(self.native.exists())
        self.assertFalse((self.gepa / 'discarded-node').exists())
        self.assertEqual(source.read_bytes(), scores)
        self.assertEqual(self.seed.read_bytes(), before)
        self.assertEqual(result['prior_search']['model_calls'], 395)
        stages = json.loads((self.gepa / 'lifecycle.json').read_text())['stage_results']
        self.assertNotIn('gepa_run', stages)
        self.assertTrue(stages['validation100']['gate_passed'])
        self.assertEqual(self.verify.call_count, 2)

    def test_invalid_seed_gate_prevents_any_deletion(self):
        self.verify.side_effect = RuntimeError('appworld_seed_stage32_goal_gate_not_passed')
        with self.assertRaises(RuntimeError): reproduce.restart_gepa(self.seed)
        self.assertTrue(self.native.is_file())

    def test_active_writer_prevents_any_deletion(self):
        self.idle.side_effect = RuntimeError('appworld_native_gepa_writer_active')
        with self.assertRaises(RuntimeError): reproduce.restart_gepa(self.seed)
        self.assertTrue(self.native.is_file())

    def test_published_native_source_cannot_be_erased(self):
        self.write(self.output / 'published/strategy/manifest.json',
                   {'native_source_reports': [str(self.native)]})
        with self.assertRaisesRegex(RuntimeError, 'published_native_gepa_source'):
            reproduce.restart_gepa(self.seed)
        self.assertTrue(self.native.is_file())


if __name__ == "__main__":
    unittest.main()
