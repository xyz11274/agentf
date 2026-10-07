from __future__ import annotations

import json
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


HERE = Path(__file__).resolve()
BUSINESS = HERE.parents[1] / "business-coverage"
EXPERIMENTS = HERE.parents[1]
for path in (BUSINESS, EXPERIMENTS):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))


def load_entry():
    spec = importlib.util.spec_from_file_location("business_offline_entry", BUSINESS / "offline_experiment.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class BusinessSharedEntryTest(unittest.TestCase):
    def test_gate_uses_paired_task_loop_rows(self):
        module = load_entry()
        old_output = module.layout.OUTPUT
        try:
            with tempfile.TemporaryDirectory() as directory:
                output = Path(directory)
                module.layout.OUTPUT = output
                rows = []
                for variant, loop in (("L0", 1), ("best", 0.8)):
                    for task_id in ("task-a", "task-b"):
                        rows.append({
                            "task_id": task_id, "variant_id": variant, "split": "val",
                            "verdict": "passed", "invalid_loops": loop,
                            "evidence": {"loop_review": {"status": "complete", "count": loop}},
                        })
                report = output / "report.json"
                report.write_text(json.dumps({"complete": True, "trials": rows}))
                result = {
                    "status": "complete", "report_path": str(report),
                    "metrics": {
                        "L0": {"val": {"quality": 0.5, "strict_passed": 10,
                                        "avg_invalid_loop": 1.0, "latency_ms": 10,
                                        "tokens": 100, "model_calls": 2}},
                        "best": {"val": {"quality": 0.55, "strict_passed": 11,
                                          "avg_invalid_loop": 0.8, "latency_ms": 10,
                                          "tokens": 100, "model_calls": 2}},
                    },
                }
                passed, evidence = module._business_goal_gate(result)
                self.assertTrue(passed, evidence)
                self.assertEqual(evidence["goal"]["paired_loop_coverage"], 2)
        finally:
            module.layout.OUTPUT = old_output


if __name__ == "__main__":
    unittest.main()
