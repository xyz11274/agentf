import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock


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
                "status": "measured", "complete": True, "conditional": False,
                "turn": 1, "expected_targets": ["x"], "actual_targets": ["x"],
                "matched": ["x"], "extra": [], "missing": []
            }],
        },
    }


class ProgressReportTest(unittest.TestCase):
    def test_live_checkpoints_prefer_requested_attempt_over_stale_report(self):
        with tempfile.TemporaryDirectory() as directory:
            search = Path(directory) / "search"
            eval_id = "evaluation"
            old = search / "evaluations" / eval_id / "attempt-1"
            current = search / "evaluations" / eval_id / "attempt-2" / "trials"
            old.mkdir(parents=True)
            current.mkdir(parents=True)
            (old / "report.json").write_text(json.dumps({"trials": []}))
            for task in ("a", "b"):
                (current / f"{task}.json").write_text(json.dumps(
                    _trial(task, "train", "parent", .7, "passed")))
            tree_path = search / "tree.json"
            entry = {"attempt": 2, "report": f"evaluations/{eval_id}/attempt-1/report.json",
                     "split": "train", "task_ids": ["a", "b", "c"]}

            rows, planned, attempt_dir = progress_report._live_checkpoint_rows(
                tree_path, eval_id, entry)

            self.assertEqual({row["task_id"] for row in rows}, {"a", "b"})
            self.assertEqual(planned, 3)
            self.assertEqual(attempt_dir.name, "attempt-2")

    def test_gepa_only_snapshot_skips_training_and_holdout_scans(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            active = output / "gepa" / "workspace" / "agent-evaluation" / "job" / "search"
            active.mkdir(parents=True)
            (output / "gepa" / "status.json").write_text(json.dumps(
                {"progress": {"job_id": "job"}}))
            (active / "tree.json").write_text(json.dumps({
                "active": 0,
                "nodes": [{"status": "accepted", "version": "v0"}],
                "evaluations": {},
            }))

            with mock.patch.object(progress_report, "_training_snapshot",
                                   side_effect=AssertionError("training scan invoked")), \
                    mock.patch.object(progress_report, "_holdout_snapshot",
                                       side_effect=AssertionError("holdout scan invoked")):
                data = progress_report.snapshot(output, gepa_only=True)

            self.assertEqual(data["training"]["status"], "skipped")
            self.assertTrue(data["holdout"]["skipped"])
            self.assertEqual(data["comparison_scope"], data["selection"]["scope"])
            self.assertIn("训练全量扫描：已跳过", progress_report.markdown(data))

    def test_selection_snapshot_prefers_status_job_over_stale_trees(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            gepa = output / "gepa"
            active = gepa / "workspace" / "agent-evaluation" / "a-active" / "search"
            stale = gepa / "workspace" / "agent-evaluation" / "z-stale" / "search"
            active.mkdir(parents=True)
            stale.mkdir(parents=True)
            def write_tree(path, current_count, current_bytes):
                path.write_text(json.dumps({
                    "active": 1,
                    "nodes": [
                        {"status": "accepted", "version": "parent"},
                        {"status": "running", "parent": 0, "version": "current",
                         "reflection_feedback_count": current_count,
                         "reflection_input_bytes": current_bytes},
                    ],
                    "evaluations": {},
                }))
            write_tree(active / "tree.json", 9, 2400)
            write_tree(stale / "tree.json", 99, 99000)
            (gepa / "status.json").write_text(json.dumps({"progress": {"job_id": "a-active"}}))

            snapshot = progress_report._selection_snapshot(output)

            self.assertEqual(snapshot["current_reflection_feedback_count"], 9)
            self.assertEqual(snapshot["current_reflection_input_bytes"], 2400)

    def test_selection_snapshot_without_status_uses_most_recent_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            trees = [
                output / "gepa" / "workspace" / "agent-evaluation" / name / "search" / "tree.json"
                for name in ("newest", "older")
            ]
            for tree, count in zip(trees, (7, 70)):
                tree.parent.mkdir(parents=True)
                tree.write_text(json.dumps({
                    "active": 1,
                    "nodes": [{"status": "accepted"},
                              {"status": "running", "parent": 0,
                               "reflection_feedback_count": count}],
                    "evaluations": {},
                }))
            os.utime(trees[0], ns=(2_000_000_000, 2_000_000_000))
            os.utime(trees[1], ns=(1_000_000_000, 1_000_000_000))

            snapshot = progress_report._selection_snapshot(output)

            self.assertEqual(snapshot["current_reflection_feedback_count"], 7)

    def test_reflection_evidence_metrics_use_only_node_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            root = output / "gepa" / "workspace" / "agent-evaluation" / "job" / "search"
            root.mkdir(parents=True)
            tree = {
                "active": 1,
                "nodes": [
                    {"status": "accepted", "reflection_feedback_count": 5,
                     "reflection_input_bytes": 1200},
                    {"status": "running", "parent": 0,
                     "reflection_feedback_count": 3, "reflection_input_bytes": 800},
                ],
                "evaluations": {},
            }
            (root / "tree.json").write_text(json.dumps(tree))
            rendered = progress_report.markdown(progress_report.snapshot(output))
            self.assertIn("| 反思详细证据题数 | 暂无 | 暂无 | 5 | 3 |", rendered)
            self.assertIn("| 反思输入字节数 | 暂无 | 暂无 | 1200 | 800 |", rendered)

    def test_missing_reflection_evidence_fields_render_unavailable(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            root = output / "gepa" / "workspace" / "agent-evaluation" / "job" / "search"
            root.mkdir(parents=True)
            tree = {
                "active": 1,
                "nodes": [{"status": "accepted"}, {"status": "running", "parent": 0}],
                "evaluations": {},
            }
            (root / "tree.json").write_text(json.dumps(tree))
            rendered = progress_report.markdown(progress_report.snapshot(output))
            self.assertIn("| 反思详细证据题数 | 暂无 | 暂无 | 暂无 | 暂无 |", rendered)
            self.assertIn("| 反思输入字节数 | 暂无 | 暂无 | 暂无 | 暂无 |", rendered)

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
        self.assertEqual(result["level_counts"], {
            "L1": 1, "L2": 2, "L2_action": 1, "L2_schedule": 1, "L3": 1
        })

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

    def test_quality_matches_native_hard_check_and_preserves_partial_business_score(self):
        budget_failure = _trial("TOKEN_TEST_01_084", "train", "candidate", 1.0, "failed")
        budget_failure["evidence"]["assessment"].update(
            quality_upper=1.0, coverage=1.0, critical_failures=0
        )
        budget_failure["checks"] = [{
            "check_id": "budget.steps", "status": "failed"
        }]
        self.assertEqual(progress_report._quality(budget_failure), 0.0)

        partial_business_failure = _trial("business", "train", "candidate", .5, "failed")
        partial_business_failure["checks"] = [{
            "check_id": "model", "status": "failed"
        }]
        self.assertEqual(progress_report._quality(partial_business_failure), .5)

        errored = _trial("error", "train", "candidate", 1.0, "error")
        errored["error_code"] = "evaluation_judge_failed"
        self.assertIsNone(progress_report._quality(errored))

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

    def test_live_checkpoint_trials_are_counted_by_unique_task(self):
        """Before report.json exists, show real task checkpoints (not 0/100)."""
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            root = output / "gepa" / "workspace" / "agent-evaluation" / "job" / "search"
            evaluation = root / "evaluations" / "eval-live" / "attempt-1" / "trials"
            evaluation.mkdir(parents=True)
            for index, task in enumerate(("task-a", "task-b", "task-c")):
                (evaluation / f"{index}.json").write_text(json.dumps(
                    _trial(task, "train", "seed", .5, "failed")))
            # A duplicate checkpoint for task-b must not inflate completed count.
            (evaluation / "retry.json").write_text(json.dumps(
                _trial("task-b", "train", "seed", .6, "passed")))
            tree = {
                "active": 1,
                "baseline_version": "l0-v",
                "nodes": [
                    {"version": "seed-v", "status": "accepted", "parent": None},
                    {"version": None, "status": "running", "parent": 0},
                ],
                "evaluations": {
                    "eval-live": {
                        "split": "train", "status": "running", "version": "seed-v",
                        "attempt": 1, "task_ids": [f"task-{x}" for x in "abcdef"],
                    },
                },
            }
            (root / "tree.json").write_text(json.dumps(tree))
            data = progress_report.snapshot(output)
            live = data["selection"]["live_evaluation"]
            self.assertIsNotNone(live)
            self.assertEqual(live["completed"], 3)
            self.assertEqual(live["planned"], 6)
            self.assertEqual(data["comparison"]["current"], None)
            rendered = progress_report.markdown(data)
            self.assertIn("当前评测 checkpoint完成题数（train）", rendered)
            self.assertIn("3/6题", rendered)

    def test_selection_uses_baseline_validation_cache_for_l0(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            root = output / "gepa" / "workspace" / "agent-evaluation" / "job" / "search"
            root.mkdir(parents=True)
            tree = {
                "active": 0,
                "baseline_version": "l0-v",
                "baseline_validation": {
                    "task-a": {"quality": .4, "strict_pass_rate": 0.,
                               "invalid_loops": 2., "duration_ms": 10.,
                               "tokens": 100., "llm_calls": 2.},
                    "task-b": {"quality": .8, "strict_pass_rate": 1.,
                               "invalid_loops": 0., "duration_ms": 20.,
                               "tokens": 200., "llm_calls": 4.},
                },
                "nodes": [{"version": "l0-v", "status": "accepted", "parent": None}],
                "evaluations": {},
            }
            (root / "tree.json").write_text(json.dumps(tree))
            data = progress_report.snapshot(output)
            l0 = data["selection"]["rows"]["L0"]
            self.assertEqual(l0["completed"], 2)
            self.assertEqual(l0["planned"], 2)
            self.assertAlmostEqual(l0["quality"], .6)
            self.assertEqual(l0["strict_passed"], 1)
            self.assertAlmostEqual(l0["invalid_loop"], 1.)
            self.assertIsNone(l0["intent_f1"])

    def test_intent_f1_shows_measured_task_denominator(self):
        measured = _trial("measured", "val", "candidate", .5)
        unmeasured = _trial("unmeasured", "val", "candidate", .5)
        unmeasured["evidence"]["intent_observations"] = [{
            "status": "unlabeled", "matched": None, "extra": None, "missing": None
        }]
        result = progress_report._aggregate([measured, unmeasured], planned=2)
        rendered = progress_report._intent_cell(result)
        self.assertIn("可测 1/2 题", rendered)
        self.assertIn("样本不足", rendered)

    def test_intent_f1_excludes_conditional_partial_and_unobserved_rows(self):
        trial = _trial("a", "val", "candidate", .5)
        rows = trial["evidence"]["intent_observations"]
        rows.extend([
            {"status": "conditional_review", "complete": False, "conditional": True,
             "turn": 2, "expected_targets": ["missing"], "actual_targets": [],
             "matched": [], "extra": [], "missing": ["missing"]},
            {"status": "partial", "complete": False, "conditional": False,
             "turn": 3, "expected_targets": ["missing"], "actual_targets": [],
             "matched": [], "extra": [], "missing": ["missing"]},
            {"status": "dispatch_unobserved", "turn": 4,
             "expected_targets": ["missing"], "actual_targets": [],
             "matched": [], "extra": [], "missing": ["missing"]},
        ])
        result = progress_report._aggregate([trial], planned=1)
        self.assertEqual(result["intent_f1"], 1.0)
        self.assertEqual(result["intent_turn_keys"], ["a#1"])
        self.assertEqual(result["intent_measured"], 1)

    def test_intent_f1_requires_definite_gold_and_matching_turn_coverage(self):
        parent = _trial("a", "val", "parent", .5)
        child = _trial("a", "val", "child", .5)
        child["evidence"]["intent_observations"].append({
            "status": "measured", "complete": True, "conditional": False,
            "turn": 2, "expected_targets": ["y"], "actual_targets": ["y"],
            "matched": ["y"], "extra": [], "missing": [],
        })
        old = progress_report._aggregate([parent], planned=1)
        new = progress_report._aggregate([child], planned=1)
        self.assertEqual(old["intent_turn_keys"], ["a#1"])
        self.assertEqual(new["intent_turn_keys"], ["a#1", "a#2"])
        self.assertIn("不可比较", progress_report._intent_cell(new, old))
        self.assertIn("不可比较", progress_report._intent_cell(old, new))
        no_gold = _trial("b", "val", "child", .5)
        no_gold["evidence"]["intent_observations"][0].pop("expected_targets")
        missing = progress_report._aggregate([no_gold], planned=1)
        self.assertIsNone(missing["intent_f1"])
        self.assertEqual(missing["intent_task_measured"], 0)


if __name__ == "__main__":
    unittest.main()
