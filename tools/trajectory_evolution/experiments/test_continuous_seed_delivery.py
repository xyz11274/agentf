"""No model calls: promotion and immutable per-version delivery boundaries."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import continuous_seed_delivery as c
import test_deadline_finish as support
from test_deadline_finish import view, node


class ContinuousTests(unittest.TestCase):
    def setup_best(self, root):
        current = view(alive=False)
        return support.DeadlineTests().fake_best(root, current), current

    def test_old_audit_cannot_overwrite_same_version_current_judge_regrade(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            best, current = self.setup_best(root)
            audits = []
            for judge, score in [('current', .8), ('old', .9)]:
                path = root / (judge + '.json')
                c.d.write(path, {'configuration': {'judge_fingerprint': judge}})
                audits.append({'node': 1, 'version': 'v1', 'artifact': {'version': 'v1'},
                    'validation': {'quality': score}, 'judge_fingerprint': judge,
                    'screening_report_path': str(path),
                    'screening_task_ids': list(current['tree']['nodes'][0]['validation'])})
            best._independent_val_candidates.return_value = audits
            chosen, pool = c.d.business_selection(best, current, required_judge='current')
            self.assertEqual(chosen['validation']['quality'], .8)
            self.assertEqual(len(pool), 1)

    def test_same_winner_expands_incomplete_frozen_pool_without_rewriting(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            best, current = self.setup_best(root)
            old = c.promote(best, current, root / 'state', 'one')
            frozen_bytes = Path(old['selection_file']).read_bytes()
            current['tree']['nodes'].append(node(2, .6))
            c.d.write(best.JOB_ROOT / current['job_id'] / 'search/versions/2.json', {'version': 'v2'})
            new = c.promote(best, current, root / 'state', 'two')
            self.assertEqual(new['version'], old['version'])
            self.assertNotEqual(new['selection_file'], old['selection_file'])
            self.assertEqual(Path(old['selection_file']).read_bytes(), frozen_bytes)
            self.assertEqual(len(c.d.read(Path(new['selection_file']))['candidate_ids']), 2)

    def test_old_judge_high_score_cannot_beat_current_judge_candidate(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            best, current = self.setup_best(root)
            current['tree']['nodes'].append(node(2, .6))
            c.d.write(best.JOB_ROOT / current['job_id'] / 'search/versions/2.json', {'version': 'v2'})
            for version, judge in [('v1', 'old'), ('v2', 'current')]:
                c.d.write(root / (version + '.json'), {'configuration': {'judge_fingerprint': judge}})
            best._cached_val_report = lambda job, tree, version, ids: ([], str(root / (version + '.json')))
            chosen, pool = c.d.business_selection(best, current, required_judge='current')
            self.assertEqual(chosen['version'], 'v2')
            self.assertEqual(len(pool), 1)

    def test_missing_current_judge_candidate_blocks_comparison_instead_of_fallback(self):
        with tempfile.TemporaryDirectory() as temp:
            best, current = self.setup_best(Path(temp))
            with self.assertRaisesRegex(RuntimeError, 'current_judge_candidate_missing'):
                c.d.business_selection(best, current, required_judge='current')
            best.run_comparison.assert_not_called()

    def test_new_validation_winner_replaces_primary_without_rewriting_old_freeze(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            best, current = self.setup_best(root)
            old = c.promote(best, current, root / 'state', 'first')
            old_freeze = Path(old['selection_file']).read_bytes()
            current['tree']['nodes'].append(node(2, .9))
            c.d.write(best.JOB_ROOT / current['job_id'] / 'search/versions/2.json', {'version': 'v2'})
            new = c.promote(best, current, root / 'state', 'next')
            self.assertEqual(new['version'], 'v2')
            self.assertEqual(new['supersedes'], 'v1')
            self.assertEqual(Path(old['selection_file']).read_bytes(), old_freeze)
            self.assertNotEqual(old['directory'], new['directory'])
            best.run_comparison.assert_not_called()

    def test_holdout_score_cannot_promote_a_worse_validation_candidate(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            best, current = self.setup_best(root)
            old = c.promote(best, current, root / 'state', 'first')
            weaker = node(2, .6)
            weaker['holdout'] = {'quality': 1.0}
            current['tree']['nodes'].append(weaker)
            c.d.write(best.JOB_ROOT / current['job_id'] / 'search/versions/2.json', {'version': 'v2'})
            new = c.promote(best, current, root / 'state', 'next')
            self.assertEqual(old['version'], new['version'])
            self.assertEqual(old['validation'], new['validation'])

    def test_superseded_worker_yields_after_validation_without_spending_holdout(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            best, current = self.setup_best(root)
            chosen = c.d.business_selection(best, current)
            result = c.d.business_delivery(best, current, selection=chosen,
                result_path=root / 'result.json', superseded=lambda: True)
            self.assertEqual(result['status'], 'superseded')
            best._final_validation.assert_called_once()
            best.run_comparison.assert_not_called()


if __name__ == '__main__':
    unittest.main()
