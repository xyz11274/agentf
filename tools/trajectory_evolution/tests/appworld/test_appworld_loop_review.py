"""Focused checks for AppWorld model-reviewed loop accounting."""
from __future__ import annotations

import json
import sys
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[4]
EXPERIMENT = ROOT / "tools/trajectory_evolution/experiments/appworld"
sys.path.insert(0, str(EXPERIMENT))
import layout as app
import compare as compact


def trial(review=None, *, reward=1):
    evidence = {
        "output": {"final": {"appworld_reward": reward, "quality_score": 1.0}},
        "execution_status": "completed",
        "assessment": {"version": "goal-assessment-v1", "quality": float(reward),
                       "quality_upper": float(reward), "coverage": 1.0,
                       "critical_failures": 0},
    }
    if review is not None:
        evidence["loop_review"] = review
    return {"task_id": "appworld-x", "execution_status": "completed",
            "verdict": "passed" if reward else "failed", "evidence": evidence}


class AppWorldLoopReviewTest(unittest.TestCase):
    def test_actual_receipt_boundaries_override_configured_level_and_deduplicate(self):
        row = trial({"status": "complete", "count": 0})
        row["evidence"]["output"]["final"].update(
            level="L3", l1_applied=0, l2_applied=0, l3_applied=99)
        events = []
        for index, boundary in enumerate(("llm", "actions", "schedule", "loop"), 1):
            events.append({"event_id": f"event-{index}", "kind": "advice.applied",
                           "application": {"status": "applied"},
                           "attributes": {"receipt": {"boundary": boundary}}})
        # The copied L1 event must not be counted twice.
        events.append(dict(events[0]))
        events.append({"event_id": "event-6", "kind": "run.end"})
        row["evidence"]["trace"] = {"identity": {"run_id": "fixture-run"},
                                      "attributes": {"capture_complete": True},
                                      "events": events}
        metrics = app.metrics([row])
        self.assertEqual(metrics["levels"], {"L1": 1, "L2": 2, "L2_action": 1,
                                            "L2_schedule": 1, "L3": 1})
        compact_row = compact.compact_row(row, "fixture")
        self.assertEqual(compact_row["l1_applied"], 1)
        self.assertEqual(compact_row["l2_applied"], 2)
        self.assertEqual(compact_row["l3_applied"], 1)

    def test_closed_l0_trace_measures_zero_but_incomplete_trace_stays_unavailable(self):
        row = trial({"status": "complete", "count": 0})
        row["evidence"]["trace"] = {"attributes": {"capture_complete": True},
                                      "events": [{"kind": "run.start"}, {"kind": "run.end"}]}
        metrics = app.metrics([row])
        self.assertEqual(metrics["levels"]["L1"], 0)
        self.assertEqual(metrics["levels"]["L2"], 0)
        self.assertEqual(metrics["levels"]["L3"], 0)
        row["evidence"]["trace"]["attributes"]["capture_complete"] = False
        self.assertIsNone(app.metrics([row])["levels"]["L3"])

    def test_complete_without_measurable_count_is_uncertain(self):
        self.assertEqual(compact.review_row_status({"status": "complete"}), "uncertain")
        self.assertEqual(compact.review_row_status({"status": "complete", "count": 0}), "complete")
        self.assertEqual(compact.review_row_status({"status": "complete", "wasted_iterations": []}), "complete")

    def test_model_review_count_is_used(self):
        row = trial({"status": "complete", "count": 3,
                     "wasted_iterations": [{"iteration_id": f"loop-{i}"} for i in range(3)]})
        metrics = app.metrics([row])
        self.assertEqual(metrics["avg_invalid_loop"], 3.0)
        self.assertEqual(metrics["invalid_loop_coverage"], 1)
        self.assertEqual(metrics["loop_basis"], "model_review")

    def test_uncertain_review_stays_unknown_and_native_lower_bound_is_ignored(self):
        row = trial({"status": "uncertain", "count": 0})
        row["evidence"]["trajectory"] = {"summary": {"invalid_loop_lower_bound": 9}}
        row["evidence"]["output"]["final"]["repeated_code_count"] = 7
        metrics = app.metrics([row])
        self.assertIsNone(metrics["avg_invalid_loop"])
        self.assertEqual(metrics["invalid_loop_coverage"], 0)
        self.assertEqual(metrics["loop_basis"], "model_review_unavailable")

    def test_judge_transport_error_does_not_hide_deterministic_reward(self):
        row = trial({"status": "complete", "count": 0}, reward=0)
        row["error_code"] = "evaluation_judge_failed"
        row["verdict"] = "error"
        row["checks"] = [{"check_id": "model", "status": "error",
                           "reason": "evaluation_judge_transport_28"}]
        metrics = app.metrics([row])
        self.assertEqual(metrics["scored"], 0)
        self.assertEqual(metrics["errored"], 1)
        self.assertEqual(metrics["official_scored"], 1)
        self.assertEqual(metrics["judge_errors"], 1)
        self.assertEqual(metrics["execution_errors"], 0)
        self.assertIsNone(metrics["strict_passed"])
        self.assertIsNone(metrics["quality"])
        self.assertIsNone(metrics["avg_invalid_loop"])

    def test_objective_failure_is_not_judge_infrastructure_error(self):
        row = trial({"status": "complete", "count": 1}, reward=0)
        row["checks"] = [{"check_id": "strict", "status": "failed",
                           "reason": "task did not satisfy the official test"},
                          {"check_id": "model", "status": "failed",
                           "reason": "目标未完成。"}]
        metrics = app.metrics([row])
        self.assertEqual(metrics["scored"], 1)
        self.assertEqual(metrics["judge_errors"], 0)
        self.assertEqual(metrics["execution_errors"], 0)

    def test_native_model_check_failure_requires_replay_evidence(self):
        """A native row can finish execution while its judge fields need retry."""
        row = trial({"status": "uncertain", "count": 0}, reward=0)
        row["checks"] = [{"check_id": "model", "status": "error",
                           "reason": "evaluation_judge_transport_28"}]
        reasons = compact.replay_reasons(row, review_enabled=True)
        self.assertIn("loop_review:uncertain", reasons)
        self.assertIn("assessment:judge_error", reasons)

    def test_preserve_review_failure_keeps_report_and_config_for_regrade(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            report = root / "capture" / "report.json"
            config = root / "config.json"
            report.parent.mkdir()
            report.write_text(json.dumps({"complete": True, "trials": [
                {"task_id": "appworld-x", "trial_id": "trial-x"}
            ]}), encoding="utf-8")
            config.write_text(json.dumps({"task_ids": ["appworld-x"],
                                          "judge": {"enabled": True}}), encoding="utf-8")
            # A real native capture already includes its manifest; recovery
            # copies that binding and must not invent another fingerprint.
            native_manifest = {"complete": True, "run_id": "fixture-run",
                               "files": {"report.json": "native-fixture-binding"}}
            (report.parent / "manifest.json").write_text(
                json.dumps(native_manifest), encoding="utf-8")
            destination = root / "output"
            source = compact.preserve_review_failure(
                destination, "shard-00", report, config,
                RuntimeError("native assessment incomplete"),
                status="regrade_required",
                reasons=["loop_review:uncertain", "assessment:judge_error"])
            replay_report = Path(source)
            replay_config = replay_report.with_name("capture.config.json")
            replay_manifest = replay_report.with_name("manifest.json")
            self.assertTrue(replay_report.is_file())
            self.assertTrue(replay_config.is_file())
            self.assertTrue(replay_manifest.is_file())
            self.assertEqual(json.loads(replay_manifest.read_text()), native_manifest)
            self.assertEqual(json.loads(replay_report.read_text())["trials"][0]["task_id"],
                             "appworld-x")
            detail = json.loads((replay_report.parent / "error.json").read_text())
            self.assertEqual(detail["status"], "regrade_required")
            self.assertEqual(sorted(detail["replay_reasons"]), [
                "assessment:judge_error", "loop_review:uncertain"])

    def test_regrade_reuses_native_verdict_and_preserves_original_execution(self):
        from copy import deepcopy
        original = trial({"status": "uncertain", "count": 0})
        original.update(trial_id="trial-x", verdict="error",
                        error_code="evaluation_judge_failed", duration_ms=456,
                        metrics={"tokens": 123, "model_calls": 2})
        original["checks"] = []  # A missing model check requires native full regrade.
        original_copy = deepcopy(original)
        repaired = deepcopy(original)
        repaired.update(verdict="passed")
        repaired.pop("error_code")
        repaired["checks"] = [{"check_id": "model", "status": "passed"}]
        repaired["evidence"]["assessment"] = {"quality": 1.0, "critical_failures": 0}
        repaired["evidence"]["loop_review"] = {"status": "complete", "count": 0}
        native = {"complete": True, "trials": [repaired]}
        with patch.object(app, "regrade_report", return_value=native):
            rows = compact._review_rows([original], report_path=Path("capture/report.json"),
                                        dataset=Path("dataset"), config_path=Path("config.json"),
                                        output_root=Path("regrade"), env={})
        self.assertIs(rows, native["trials"])
        self.assertEqual(original, original_copy)
        self.assertEqual(rows[0]["verdict"], "passed")
        self.assertIsNone(app._trial_error_code(rows[0]))
        self.assertEqual(app._invalid_loops(rows[0]), 0.0)
        self.assertEqual(rows[0]["metrics"], original["metrics"])
        # Candidate and L0 consume the same complete native regrade contract.
        self.assertIs(compact._validated_regrade({"trials": [original]}, native), native)

    def test_regrade_rejects_changed_scope_execution_and_costs(self):
        from copy import deepcopy
        original = trial({"status": "uncertain", "count": 0})
        original.update(trial_id="trial-x", duration_ms=456, metrics={"tokens": 123})
        for mutation in ("task", "reward", "duration", "metrics"):
            changed = deepcopy(original)
            if mutation == "task":
                changed["task_id"] = "appworld-other"
            elif mutation == "reward":
                changed["evidence"]["output"]["final"]["appworld_reward"] = 0
            elif mutation == "duration":
                changed["duration_ms"] = 1
            else:
                changed["metrics"]["tokens"] = 1
            with self.subTest(mutation=mutation), self.assertRaisesRegex(
                    RuntimeError, "comparison_regrade_changed_execution_or_scope"):
                compact._validated_regrade({"trials": [original]},
                                          {"complete": True, "trials": [changed]})

    def test_gepa_copied_runtime_retains_private_learning_prompt_paths(self):
        import offline_gepa
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, destination = root / "training/runtime", root / "gepa/runtime"
            private = source / "appworld-prompts"
            private.mkdir(parents=True)
            prompt = private / "dialogue-system.txt"
            prompt.write_text("fixture private learning constraints", encoding="utf-8")
            config = {"prompts": {"dialogue_system": "appworld-prompts/dialogue-system.txt"}}
            (source / "runtime.json").write_text(json.dumps(config), encoding="utf-8")
            (source / "service.json").write_text("{}", encoding="utf-8")
            with patch.object(offline_gepa, "CONFIG", destination), patch.object(app, "RUNTIME", source):
                offline_gepa._copy_training_configs()
            copied = json.loads((destination / "runtime.json").read_text())
            resolved = destination / copied["prompts"]["dialogue_system"]
            self.assertEqual(resolved.read_text(), prompt.read_text())
            self.assertTrue((destination / "appworld-prompts").is_symlink())

    def test_regrade_command_uses_saved_report_and_never_agent_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.json"
            source.write_text("{}", encoding="utf-8")
            config = root / "config.json"
            config.write_text(json.dumps({"judge": {"enabled": False}}), encoding="utf-8")
            destination = root / "regraded"

            def fake_run(command, **kwargs):
                self.assertIn("--regrade-report", command)
                self.assertNotIn("--resume-report", command)
                self.assertEqual(command[0], str(app.BINARY))
                self.assertTrue(json.loads((destination.with_name(destination.name + ".regrade.config.json")).read_text())["judge"]["enabled"])
                self.assertEqual(json.loads((destination.with_name(destination.name + ".regrade.config.json")).read_text())["parallelism"], 16)
                self.assertEqual(kwargs["env"]["TRAJECTORY_EVALUATION_PARALLELISM"], "16")
                destination.mkdir()
                (destination / "report.json").write_text(json.dumps({"complete": True, "trials": []}), encoding="utf-8")
                return __import__("subprocess").CompletedProcess(command, 0, "", "")

            with patch.object(app.subprocess, "run", side_effect=fake_run):
                report = app.regrade_report(source, root / "dataset", config, destination,
                                            env={"PATH": "/usr/bin"})
            self.assertTrue(report["complete"])

    def test_regrade_explicit_retry_parallelism_remains_four(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.json"
            source.write_text("{}", encoding="utf-8")
            destination = root / "retry"

            def fake_run(command, **kwargs):
                config = json.loads(destination.with_name("retry.regrade.config.json").read_text())
                self.assertEqual(config["parallelism"], 4)
                self.assertEqual(kwargs["env"]["TRAJECTORY_EVALUATION_PARALLELISM"], "4")
                destination.mkdir()
                (destination / "report.json").write_text(json.dumps({"complete": True, "trials": []}))
                return __import__("subprocess").CompletedProcess(command, 0, "", "")

            with patch.object(app.subprocess, "run", side_effect=fake_run):
                report = app.regrade_report(source, root / "dataset",
                    {"parallelism": 16, "judge": {"enabled": True}}, destination,
                    env={}, parallelism=4)
            self.assertTrue(report["complete"])


if __name__ == "__main__":
    unittest.main()


def test_native_review_trace_is_counted_by_the_same_business_reader():
    """Use the business production parser; host repetition diagnostics are not loop scores."""
    import importlib.util
    source = ROOT / "tools/trajectory_evolution/experiments/business-coverage/progress_report.py"
    spec = importlib.util.spec_from_file_location("business_loop_contract_reference", source)
    business = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(business)
    reviewed = {
        "status": "complete", "observed_iterations": 4,
        "wasted_iterations": [
            {"iteration": 2, "evidence_event_ids": ["action-2", "tool-2"],
             "reason": "Repeated failed operation without a new correction"},
            {"iteration": 3, "evidence_event_ids": ["action-3", "tool-3"],
             "reason": "Repeated unchanged failed operation"},
        ],
    }
    row = trial(reviewed)
    row["evidence"]["trace"] = {"events": [
        {"event_id": "start", "kind": "run.start"},
        {"event_id": "action-1", "kind": "action.request",
         "action": {"name": "appworld.execute", "actual_input": {"code": "files = apis.file_system.show_directory(directory_path='/')"}}},
        {"event_id": "tool-1", "kind": "tool.completed", "output": {"observation": "authentication required"}},
        {"event_id": "action-2", "kind": "action.request",
         "action": {"name": "appworld.execute", "actual_input": {"code": "files = apis.file_system.show_directory(directory_path='/')"}}},
        {"event_id": "tool-2", "kind": "tool.completed", "output": {"observation": "authentication required"}},
        {"event_id": "action-3", "kind": "action.request",
         "action": {"name": "appworld.execute", "actual_input": {"code": "files = apis.file_system.show_directory(directory_path='/')"}}},
        {"event_id": "tool-3", "kind": "tool.completed", "output": {"observation": "authentication required"}},
        {"event_id": "end", "kind": "run.end"},
    ]}
    row["evidence"]["trajectory"] = {"summary": {"invalid_loop_lower_bound": 99}}
    row["evidence"]["output"]["final"]["repeated_code_count"] = 88
    assert app._invalid_loops(row) == business._invalid_loops(row)
    assert app.metrics([row])["avg_invalid_loop"] == business._invalid_loops(row)
    compact_row = compact.compact_row(row, "production-contract")
    assert compact_row["invalid_loop_count"] == business._invalid_loops(row)
    assert compact_row["invalid_loop_basis"] == "model_review"
    # Every task has a measurable zero if reviewed as useful. An uncertain
    # review remains unknown in both readers even when raw diagnostics exist.
    for review in ({"status": "complete", "wasted_iterations": []},
                   {"status": "uncertain", "wasted_iterations": []}):
        row["evidence"]["loop_review"] = review
        assert app._invalid_loops(row) == business._invalid_loops(row)


def test_business_and_appworld_metrics_use_identical_native_task_fields():
    import copy

    first = trial({"status": "complete", "observed_iterations": 3,
                   "count": 0, "wasted_iterations": []})
    first.update(task_id="fixture-a", duration_ms=100,
                 metrics={"tokens": 10, "llm_calls": 2, "loop_count": 3})
    first["evidence"]["output"]["final"].update(tokens=9999, model_calls=9999, latency_ms=9999)
    second = copy.deepcopy(first)
    second.update(task_id="fixture-b", duration_ms=300, verdict="failed",
                  metrics={"tokens": 30, "llm_calls": 6, "loop_count": 5})
    second["evidence"]["assessment"].update(quality=.5, quality_upper=.5)
    second["evidence"]["loop_review"].update(count=2, observed_iterations=5,
        wasted_iterations=[{"iteration_id": "loop-2"}, {"iteration_id": "loop-3"}])
    for row, actual in ((first, ["mail", "files"]), (second, ["mail", "calendar"])):
        row["evidence"]["intent_observations"] = [{
            "status": "measured", "turn": 1, "conditional": False,
            "expected_targets": ["mail", "files"], "actual_targets": actual,
            "matched": [], "extra": [], "missing": []}]
    rows = [first, second]
    canonical = app._business_metrics._aggregate(rows, planned=2)
    measured = app.metrics(rows, planned=2)
    for field in ("quality", "strict_passed", "invalid_loop", "latency_ms", "tokens", "llm_calls", "intent_f1"):
        assert measured[field] == canonical[field]
    assert measured["tokens"] == 20 and measured["model_calls"] == 4
    assert measured["latency_ms"] == 200
    assert measured["total_tokens"] == 40 and measured["total_model_calls"] == 8
    assert measured["intent_counts"] == [3, 1, 1]
    assert measured["quality_status"] == measured["invalid_loop_status"] == "complete"


def test_errors_and_missing_tasks_cannot_make_scored_subset_complete():
    healthy = trial({"status": "complete", "count": 0})
    errored = {"task_id": "fixture-error", "execution_status": "environment_error",
               "verdict": "error", "error_code": "runner_environment_error", "evidence": {}}
    for rows, planned in (([healthy, errored], 2), ([healthy], 2)):
        measured = app.metrics(rows, planned=planned)
        assert measured["quality_status"] == "partial"
        assert measured["invalid_loop_status"] == "partial"
        assert measured["quality"] == 1 and measured["avg_invalid_loop"] == 0
        assert measured["quality_coverage"] == measured["invalid_loop_coverage"] == 1


def test_incomplete_objectives_and_contradictory_review_remain_unmeasured():
    row = trial({"status": "complete", "count": 2, "observed_iterations": 1,
                 "iteration_reviews": [{"iteration_id": "loop-1", "decision": "keep"}]})
    row["evidence"]["assessment"].update(quality=.5, quality_upper=1, coverage=.5)
    assert app.assessed_quality(row) is None
    assert app._invalid_loops(row) is None
    assert not app._has_complete_loop_review(row)
    measured = app.metrics([row])
    assert measured["scored"] == 0
    assert measured["quality_status"] == measured["invalid_loop_status"] == "unavailable"
