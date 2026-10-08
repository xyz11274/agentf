"""Model-free contract tests for the shared dataset lifecycle."""
from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tools.trajectory_evolution.experiments.common_lifecycle import (
    LifecycleController, StageResult,
)


class _Adapter:
    name = "fixture"
    output_root = Path("/tmp/fixture-output")

    def split_counts(self):
        return {"train": 1, "val": 1, "holdout": 1}

    def materialize(self, view):
        return {"view": view}

    def baseline(self, scope):
        return {"scope": scope}


class SharedLifecycleContractTest(unittest.TestCase):
    def test_gate_stage_cannot_be_skipped(self):
        with tempfile.TemporaryDirectory() as directory:
            calls = []

            def runner(stage, context):
                calls.append(stage)
                if stage == "pilot32":
                    return StageResult("skipped", data={"gate_delegated": True})
                return StageResult("complete", gate_passed=True)

            result = LifecycleController(_Adapter(), runner, Path(directory) / "state.json").run()
            self.assertEqual(result["status"], "blocked")
            self.assertEqual(result["phase"], "pilot32")
            self.assertEqual(result["last_error"], "stage_skipped_not_allowed:pilot32")
            self.assertEqual(calls, ["prepare", "train", "seed", "pilot32"])

    def test_gate_requires_explicit_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            def runner(stage, context):
                if stage == "pilot32":
                    return StageResult("complete", gate_passed=False)
                return StageResult("complete", gate_passed=True)

            result = LifecycleController(_Adapter(), runner, Path(directory) / "state.json").run()
            self.assertEqual(result["status"], "blocked")
            self.assertEqual(result["phase"], "pilot32")
            self.assertEqual(result["last_error"], "stage_gate_not_passed:pilot32")


if __name__ == "__main__":
    unittest.main()
