import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


MODULE = Path(__file__).with_name("progress_report.py")
SPEC = importlib.util.spec_from_file_location("progress_report_under_test", MODULE)
progress_report = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(progress_report)


def _trial(task, split, variant, quality, verdict="failed"):
    return {
        "task_id": task,
        "split": split,
        "variant_id": variant,
        "verdict": verdict,
        "duration_ms": 10,
        "metrics": {"tokens": 100, "llm_calls": 2},
        "evidence": {
            "assessment": {"quality": quality},
            "loop_review": {"wasted_iterations": [{"id": "w"}]},
            "intent_observations": [{
                "status": "measured", "matched": ["x"], "extra": [], "missing": []
            }],
        },
    }


class ProgressReportTest(unittest.TestCase):
    def test_level_counts_use_applied_boundary_receipts_only(self):
        trial = _trial("r", "val", "candidate", .5)
        trial["evidence"]["traces"] = [{"events": [
            {"kind": "advice.applied", "event_id": "l1",
             "attributes": {"receipt": {"boundary": "llm"}},
             "application": {"status": "applied"}},
            {"kind": "advice.applied", "event_id": "a1",
             "attributes": {"receipt": {"boundary": "actions"}},
             "application": {"status": "applied"}},
            {"kind": "advice.applied", "event_id": "s1",
             "attributes": {"receipt": {"boundary": "schedule"}},
             "application": {"status": "applied"}},
            {"kind": "advice.applied", "event_id": "l3",
             "attributes": {"receipt": {"boundary": "loop"}},
             "application": {"status": "applied"}},
            {"kind": "advice.applied", "event_id": "reject",
             "attributes": {"receipt": {"boundary": "llm"}},
             "application": {"status": "rejected"}},
        ]}]
        result = progress_report._aggregate([trial])
        self.assertEqual(result["level_counts"], {"L1": 1, "L2": 2, "L3": 1})

    def test_judge_and_contract_errors_are_unscored_but_keep_costs(self):
        """错误 judge/contract 的诊断分值不能变成 quality=0 或 loop=0。"""
        valid = _trial("valid", "holdout", "candidate", .8, "passed")
        invalid = _trial("judge", "holdout", "candidate", 0.0, "error")
        invalid["error_code"] = "evaluation_judge_failed"
        invalid["duration_ms"] = 30
        invalid["metrics"] = {"tokens": 300, "llm_calls": 4}
        invalid["evidence"]["loop_review"] = {
            "status": "uncertain", "wasted_iterations": []
        }
        contract = _trial("contract", "holdout", "candidate", 0.0, "error")
        contract["error_code"] = "contract_invalid"
        contract["duration_ms"] = 50
        contract["metrics"] = {"tokens": 500, "llm_calls": 6}
        contract["evidence"]["loop_review"] = {
            "status": "not_reviewed", "wasted_iterations": []
        }

        result = progress_report._aggregate([valid, invalid, contract], planned=3)

        self.assertAlmostEqual(result["quality"], .8)
        self.assertEqual(result["scored"], 1)
        self.assertEqual(result["unscored"], 2)
        self.assertIsNone(progress_report._invalid_loops(invalid))
        self.assertEqual(result["invalid_loop"], 1.0)
        self.assertEqual(result["errors"], 2)
        self.assertEqual(result["error_counts"], {
            "contract_invalid": 1, "evaluation_judge_failed": 1
        })
        # Duration/token/call means deliberately include failed executions so
        # the report still exposes the cost paid for an unavailable judge.
        self.assertAlmostEqual(result["latency_ms"], 30.0)
        self.assertAlmostEqual(result["tokens"], 300.0)
        self.assertAlmostEqual(result["llm_calls"], 4.0)
        self.assertEqual(result["partial"], 2)
        self.assertEqual(result["missing"], 0)
        self.assertFalse(result["quality_complete"])
        only_errors = progress_report._aggregate([invalid, contract], planned=2)
        self.assertIsNone(only_errors["quality"])
        self.assertIsNone(only_errors["invalid_loop"])
        self.assertEqual(only_errors["errors"], 2)

    def test_partial_assessment_is_reported_without_being_hidden(self):
        trial = _trial("partial", "val", "candidate", .5, "failed")
        trial["evidence"]["assessment"].update(quality_upper=1.0, coverage=.5)
        result = progress_report._aggregate([trial], planned=2)
        self.assertAlmostEqual(result["quality"], .5)
        self.assertEqual(result["partial"], 1)
        self.assertEqual(result["missing"], 1)
        self.assertEqual(result["quality_status"], "partial")
        rendered = progress_report.markdown({
            "training": {"status": "x", "phase": "y", "batch": "z",
                         "completed": 0, "planned": 0, "reports": 0},
            "selection": {"parent_node": "p", "current_node": "c"},
            "comparison": {"L0": result, "parent": result, "current": result},
            "comparison_scope": "test",
            "holdout": {"reports": 1},
        })
        self.assertIn("partial / 缺失", rendered)
        self.assertIn("错误数", rendered)

    def test_training_uses_canonical_batches_and_keeps_denominators(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            training = output / "training"
            (training / "001").mkdir(parents=True)
            (training / "009-attempt-01").mkdir(parents=True)
            report = {"run_id": "one", "trials": [
                _trial("a", "train", "business-v1", .8, "passed"),
                _trial("b", "train", "business-v1", .4),
            ]}
            (training / "001" / "report.json").write_text(json.dumps(report))
            (training / "009-attempt-01" / "report.json").write_text(json.dumps({
                "run_id": "attempt", "trials": [_trial("bad", "train", "business-v1", 1.0, "passed")]
            }))
            (training / "status.json").write_text(json.dumps({
                "status": "running", "phase": "capture", "planned_tasks": 4,
                "captured_tasks": 2, "batch": "002"
            }))
            data = progress_report.snapshot(output)
            metrics = data["training"]["metrics"]
            self.assertEqual(data["training"]["reports"], 1)
            self.assertEqual(data["training"]["completed"], 2)
            self.assertEqual(data["training"]["planned"], 4)
            self.assertEqual(metrics["strict_passed"], 1)
            self.assertAlmostEqual(metrics["quality"], .6)
            self.assertEqual(metrics["invalid_loop"], 1.0)

    def test_paired_holdout_has_l0_and_current_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            path = output / "gepa" / "node-7"
            path.mkdir(parents=True)
            report = {"run_id": "holdout", "trials": [
                _trial("a", "holdout", "baseline", .5),
                _trial("a", "holdout", "candidate", .7, "passed"),
            ]}
            (path / "final_holdout.json").write_text(json.dumps(report))
            data = progress_report.snapshot(output)
            self.assertEqual(data["holdout"]["reports"], 1)
            self.assertIsNotNone(data["comparison"]["L0"])
            self.assertIsNotNone(data["comparison"]["current"])
            self.assertEqual(data["comparison"]["L0"]["completed"], 1)
            self.assertEqual(data["comparison"]["current"]["strict_passed"], 1)
            self.assertIn("holdout", progress_report.markdown(data))

    def test_gepa_attempt_holdout_is_loaded_and_mapped_once(self):
        """真实 GEPA evaluations/attempt-N 报告必须进入三行播报且不重复。"""
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            root = output / "gepa" / "workspace" / "agent-evaluation" / "job" / "search"
            pair = root / "evaluations" / "pair" / "attempt-1"
            parent = root / "evaluations" / "parent" / "attempt-1"
            pair.mkdir(parents=True)
            parent.mkdir(parents=True)
            pair_report = {
                "run_id": "pair-report",
                "configuration": {"variants": [
                    {"id": "baseline", "version": "l0-v"},
                    {"id": "candidate", "version": "current-v"},
                ]},
                "trials": [
                    _trial("a", "holdout", "baseline", .4),
                    _trial("a", "holdout", "candidate", .8, "passed"),
                ],
            }
            parent_report = {
                "run_id": "parent-report",
                "configuration": {"variants": [
                    {"id": "candidate", "version": "parent-v"},
                ]},
                "trials": [_trial("a", "holdout", "candidate", .6, "passed")],
            }
            (pair / "report.json").write_text(json.dumps(pair_report))
            (parent / "report.json").write_text(json.dumps(parent_report))
            tree = {
                "selected_version": "current-v",
                "nodes": [
                    {"version": "l0-v", "parent": None},
                    {"version": "parent-v", "parent": 0},
                    {"version": "current-v", "parent": 1},
                ],
                "final_holdout": {"candidates": [{"node": 2, "version": "current-v"}]},
                "evaluations": {
                    "pair": {"split": "holdout", "status": "complete",
                             "version": "current-v", "baseline_version": "l0-v",
                             "task_ids": [f"task-{i}" for i in range(200)],
                             "report": "evaluations/pair/attempt-1/report.json"},
                    "parent": {"split": "holdout", "status": "complete",
                                "version": "parent-v",
                                "task_ids": [f"task-{i}" for i in range(200)],
                                "report": "evaluations/parent/attempt-1/report.json"},
                },
            }
            (root / "tree.json").write_text(json.dumps(tree))
            state_dir = output / "gepa" / "workspace" / "agent-evaluation"
            (state_dir / "state.json").write_text(json.dumps({
                "jobs": {"job": {"search": {
                    "final_holdout_selection": {"node": 2, "version": "current-v"}
                }}},
            }))
            data = progress_report.snapshot(output)
            self.assertEqual(data["holdout"]["reports"], 2)
            self.assertEqual(data["comparison"]["L0"]["completed"], 1)
            self.assertEqual(data["comparison"]["L0"]["planned"], 200)
            self.assertEqual(data["comparison"]["parent"]["strict_passed"], 1)
            self.assertEqual(data["comparison"]["parent"]["planned"], 200)
            self.assertEqual(data["comparison"]["current"]["strict_passed"], 1)
            self.assertEqual(data["comparison"]["current"]["planned"], 200)
            self.assertEqual(data["comparison"]["current"]["completed"], 1)


if __name__ == "__main__":
    unittest.main()
