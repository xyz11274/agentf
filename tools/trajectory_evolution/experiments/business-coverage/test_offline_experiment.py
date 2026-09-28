"""Small recovery-boundary tests for the non-programmer experiment entrypoint."""

from contextlib import ExitStack
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import offline_experiment


class ResumeBoundaryTests(unittest.TestCase):
    def _paths(self, root: Path):
        config = root / "config"
        plan = config / "plan.json"
        status = root / "gepa/status.json"
        workspace = root / "gepa/workspace"
        config.mkdir(parents=True)
        status.parent.mkdir(parents=True)
        workspace.mkdir(parents=True)
        return plan, status, workspace

    def test_resume_keeps_existing_plan_and_tree(self):
        with tempfile.TemporaryDirectory() as folder, ExitStack() as stack:
            plan, status, workspace = self._paths(Path(folder))
            plan.write_text(json.dumps({"schema_version": 1}))
            status.write_text(json.dumps({"status": "running"}))
            for name, value in (("PLAN", plan), ("STATUS", status),
                                ("WORKSPACE", workspace)):
                stack.enter_context(patch.object(offline_experiment.gepa, name, value))
            resume = stack.enter_context(patch.object(
                offline_experiment.gepa, "run_to_publication",
                return_value={"source_job": "same-job"}))
            fresh = stack.enter_context(patch.object(
                offline_experiment.gepa, "fresh", side_effect=AssertionError("must not reset")))
            result = offline_experiment._resume_gepa_or_start()
            self.assertEqual(result["publication"]["source_job"], "same-job")
            resume.assert_called_once_with()
            fresh.assert_not_called()
            self.assertTrue(plan.is_file())
            self.assertTrue(status.is_file())

    def test_resume_without_checkpoint_starts_fresh(self):
        with tempfile.TemporaryDirectory() as folder, ExitStack() as stack:
            plan, status, workspace = self._paths(Path(folder))
            for name, value in (("PLAN", plan), ("STATUS", status),
                                ("WORKSPACE", workspace)):
                stack.enter_context(patch.object(offline_experiment.gepa, name, value))
            fresh = stack.enter_context(patch.object(
                offline_experiment.gepa, "fresh",
                return_value={"status": "published"}))
            run = stack.enter_context(patch.object(
                offline_experiment.gepa, "run_to_publication",
                side_effect=AssertionError("no checkpoint")))
            self.assertEqual(offline_experiment._resume_gepa_or_start()["status"], "published")
            fresh.assert_called_once_with()
            run.assert_not_called()

    def test_resume_refuses_orphan_state_without_plan(self):
        with tempfile.TemporaryDirectory() as folder, ExitStack() as stack:
            plan, status, workspace = self._paths(Path(folder))
            status.write_text(json.dumps({"status": "failed"}))
            for name, value in (("PLAN", plan), ("STATUS", status),
                                ("WORKSPACE", workspace)):
                stack.enter_context(patch.object(offline_experiment.gepa, name, value))
            with self.assertRaisesRegex(RuntimeError, "^gepa_resume_plan_missing$"):
                offline_experiment._resume_gepa_or_start()
            self.assertTrue(status.is_file())


if __name__ == "__main__":
    unittest.main()
