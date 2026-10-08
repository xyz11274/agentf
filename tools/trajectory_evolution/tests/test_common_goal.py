import unittest

from tools.trajectory_evolution.experiments import common_goal


class CommonGoalTest(unittest.TestCase):
    def setUp(self):
        self.baseline = {
            'quality': .50, 'strict_passed': 20, 'avg_invalid_loop': 1.0,
            'latency_ms': 10, 'tokens': 100, 'model_calls': 5,
        }
        self.candidate = {
            'quality': .525, 'strict_passed': 21, 'avg_invalid_loop': .80,
            'latency_ms': 10, 'tokens': 100, 'model_calls': 5,
        }

    def test_common_contract_passes_on_paired_scope(self):
        result = common_goal.evaluate(
            self.baseline, self.candidate,
            baseline_loop_by_task={'a': 1}, candidate_loop_by_task={'a': .8},
            baseline_loop_basis='model_review', candidate_loop_basis='model_review')
        self.assertTrue(result['passed'])

    def test_zero_baseline_loop_is_blocked_not_treated_as_zero(self):
        baseline = {**self.baseline, 'avg_invalid_loop': 0}
        result = common_goal.evaluate(
            baseline, self.candidate,
            baseline_loop_by_task={'a': 0}, candidate_loop_by_task={'a': 0},
            baseline_loop_basis='model_review', candidate_loop_basis='model_review')
        self.assertFalse(result['passed'])
        self.assertEqual(result['loop_state'], 'blocked_unverifiable')

    def test_intent_is_optional_when_unmeasured(self):
        result = common_goal.evaluate(
            self.baseline, self.candidate,
            baseline_loop_by_task={'a': 1}, candidate_loop_by_task={'a': .8},
            baseline_loop_basis='model_review', candidate_loop_basis='model_review')
        self.assertNotIn('intent_not_degraded', result['checks'])

    def test_resource_increase_fails(self):
        result = common_goal.evaluate(
            self.baseline, {**self.candidate, 'tokens': 101},
            baseline_loop_by_task={'a': 1}, candidate_loop_by_task={'a': .8},
            baseline_loop_basis='model_review', candidate_loop_basis='model_review')
        self.assertFalse(result['passed'])
        self.assertFalse(result['checks']['resources_not_increased'])

    def test_explicit_three_metric_contract_preserves_resource_failure(self):
        result = common_goal.evaluate(
            self.baseline, {**self.candidate, 'tokens': 101},
            baseline_loop_by_task={'a': 1}, candidate_loop_by_task={'a': .8},
            baseline_loop_basis='model_review', candidate_loop_basis='model_review',
            require_resources=False)
        self.assertTrue(result['passed'])
        self.assertFalse(result['checks']['resources_not_increased'])
        self.assertEqual(result['resource_state'], 'fail')
        self.assertNotIn('resources_not_increased', result['required_checks'])

    def test_three_metric_contract_still_blocks_unpaired_loop(self):
        result = common_goal.evaluate(
            self.baseline, self.candidate,
            baseline_loop_by_task={'a': 1}, candidate_loop_by_task={'b': .8},
            baseline_loop_basis='model_review', candidate_loop_basis='model_review',
            require_resources=False)
        self.assertFalse(result['passed'])
        self.assertEqual(result['loop_state'], 'blocked_unverifiable')


if __name__ == '__main__':
    unittest.main()
