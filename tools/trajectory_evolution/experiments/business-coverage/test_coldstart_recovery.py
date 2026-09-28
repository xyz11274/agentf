"""Focused recovery-contract tests for the offline training scheduler.

These tests import only the scheduler module; they do not start an Agent,
invoke the native binary, or contact a model service.
"""

import unittest

import coldstart


def completed(**overrides):
    value = {
        'shared_status': 'completed',
        'agent_evaluation_error': '',
        'run_outcomes': {'completed': 3, 'failed': 0},
        'memory_management': {'business-training': {'status': 'idle', 'pending': 0}},
    }
    value.update(overrides)
    return value


class LearningRecoveryContractTests(unittest.TestCase):
    def test_zero_exit_with_terminal_status_is_usable(self):
        self.assertTrue(coldstart.learning_result_usable(completed(), 0))

    def test_task_failure_exit_three_is_usable_and_preserved(self):
        result = completed(run_outcomes={'completed': 2, 'failed': 1})
        self.assertTrue(coldstart.learning_result_usable(result, 3))
        self.assertEqual(result['run_outcomes']['failed'], 1)

    def test_unresolved_relations_are_terminal(self):
        self.assertTrue(coldstart.learning_result_usable(
            completed(shared_status='completed_with_unresolved'), 3))

    def test_pending_counters_are_not_completion(self):
        for field in ('pending_runs', 'unpublished_runs', 'training_pending_users',
                      'memory_pending_reviews', 'agent_evaluation_pending_jobs',
                      'shared_pending_users'):
            with self.subTest(field=field):
                self.assertFalse(coldstart.learning_result_usable(
                    completed(**{field: 1}), 3))
                # A clean process exit cannot promote a pending JSON status
                # to a learned batch either.
                self.assertFalse(coldstart.learning_result_usable(
                    completed(**{field: 1}), 0))

    def test_zero_exit_malformed_or_pending_status_is_rejected(self):
        """rc=0 must not bypass the JSON recovery boundary."""
        self.assertFalse(coldstart.learning_result_usable(
            completed(shared_status='running'), 0))
        self.assertFalse(coldstart.learning_result_usable(
            completed(run_outcomes={}), 0))
        self.assertFalse(coldstart.learning_result_usable(None, 0))

    def test_terminal_capture_is_reused_after_learning_retry(self):
        """学习失败重试时，完整 capture 不应再次调用模型。"""
        self.assertFalse(coldstart.capture_needs_resume(dict(
            complete=True,
            trials=[
                {'execution_status': 'completed'},
                {'execution_status': 'failed'},
            ])))

    def test_pending_and_waiting_statuses_are_not_completion(self):
        for status in ('pending', 'running', 'waiting_for_model', 'failed'):
            with self.subTest(status=status):
                self.assertFalse(coldstart.learning_result_usable(
                    completed(shared_status=status), 3))
        nested = completed(memory_management={
            'business-training': {'status': 'pending', 'pending': 0}})
        self.assertFalse(coldstart.learning_result_usable(nested, 3))

    def test_empty_malformed_and_fatal_results_are_rejected(self):
        for result, returncode in ((None, 0), ({}, 0), ({'run_outcomes': {}}, 0),
                                    (completed(), 2), (completed(), -15),
                                    (completed(), 130)):
            with self.subTest(result=result, returncode=returncode):
                self.assertFalse(coldstart.learning_result_usable(result, returncode))

    def test_pipeline_failures_are_not_task_level_failures(self):
        for field in ('training_failed_users', 'memory_management_failed_users',
                      'shared_quarantined_users'):
            with self.subTest(field=field):
                self.assertFalse(coldstart.learning_result_usable(
                    completed(**{field: 1}), 3))


if __name__ == '__main__':
    unittest.main()
