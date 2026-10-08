"""No-model tests; no live processes are signaled and no evaluation API runs."""
from contextlib import nullcontext
from datetime import datetime, timezone
import importlib.util
import io
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import Mock, patch

SPEC = importlib.util.spec_from_file_location("deadline_finish", Path(__file__).with_name("deadline_finish.py"))
d = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(d)
JOB = "a" * 64


def node(number, quality):
    return {"id": number, "version": f"v{number}", "validation": {
        f"task-{i}": {"quality": quality, "strict_pass_rate": float(quality > .5),
                      "invalid_loops": 2, "tokens": 100, "duration_ms": 10}
        for i in range(50)}}


def view(line="business", alive=True):
    return {"line": line, "job_id": JOB, "runner_pid": 42 if alive else 0,
            "runner_status": "running" if alive else "paused",
            "tree": {"nodes": [node(0, .5), node(1, .7)], "status": "searching"}}


class DeadlineTests(unittest.TestCase):
    def setUp(self):
        self.cutoff = d.utc(d.CUTOFF)
        self.deadline = d.utc(d.DEADLINE)
        self.after = d.utc("2026-09-29T19:01:00Z")

    def test_time_stage_and_seed_guards(self):
        current = view()
        self.assertEqual(d.action(current, d.utc("2026-09-29T17:59:00Z"), self.cutoff,
                                  self.deadline, True), "waiting_cutoff")
        self.assertEqual(d.action(current, self.after, self.cutoff, self.deadline, True), "pause_search")
        current["tree"]["nodes"][0]["validation"].pop("task-0")
        self.assertEqual(d.action(current, self.after, self.cutoff, self.deadline, True), "waiting_seed_validation")
        self.assertEqual(d.action(current, self.after, self.cutoff, self.deadline, False), "needs_seed_validation")
        self.assertEqual(d.action(view(), self.deadline, self.cutoff, self.deadline, True), "deadline_reached")

    def test_active_final_and_delivery_are_never_stopped(self):
        for marker in ({"holdout_started": True}, {"final_holdout": {"status": "running"}}):
            current = view()
            current["tree"].update(marker)
            self.assertEqual(d.action(current, self.after, self.cutoff, self.deadline, True),
                             "waiting_final_evaluation")
        self.assertEqual(d.action(view(), self.after, self.cutoff, self.deadline, True, True),
                         "waiting_final_evaluation")
        current = view()
        current["runner_status"] = "published"
        self.assertEqual(d.action(current, self.after, self.cutoff, self.deadline, True),
                         "waiting_final_evaluation")

    def test_training_does_not_block_business_delivery(self):
        training = dict(view("bfcl", False), job_id="", tree={}, training_status="running")
        self.assertEqual(d.action(training, self.after, self.cutoff, self.deadline, False), "waiting_training")
        self.assertEqual(d.action(view(alive=False), self.after, self.cutoff, self.deadline, False),
                         "start_final_delivery")

    def test_snapshot_verifies_job_owner(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            out = root / d.LINES["business"]["output"]
            d.write(out / "gepa/status.json", {"pid": 42, "progress": {"job_id": JOB}})
            state = out / "gepa/workspace/agent-evaluation/state.json"
            d.write(state, {"jobs": {JOB: {"user": "business-training", "level": "L3"}}})
            self.assertEqual(d.snapshot("business", root=root)["job_id"], JOB)
            with self.assertRaisesRegex(RuntimeError, "job_changed"):
                d.snapshot("business", "b" * 64, root=root)
            d.write(state, {"jobs": {JOB: {"user": "another-user", "level": "L3"}}})
            with self.assertRaisesRegex(RuntimeError, "owner_unverified"):
                d.snapshot("business", root=root)

    def test_runner_identity_command_cwd_and_bfcl_split(self):
        with tempfile.TemporaryDirectory() as tmp:
            proc = Path(tmp)
            folder = proc / "42"
            folder.mkdir()
            fields = ["S"] + ["0"] * 18 + ["123456"]
            (folder / "stat").write_text("42 (name with spaces) " + " ".join(fields))
            (folder / "cwd").symlink_to(d.ROOT, target_is_directory=True)
            script = d.HERE / "bfcl-v3/bfcl_experiment.py"
            (folder / "cmdline").write_bytes(f"python3\0{script}\0--resume\0".encode())
            (folder / "environ").write_bytes(b"BFCL_SPLIT_ID=seen-scenarios-v1\0")
            self.assertEqual(d.runner_identity("bfcl", 42, proc_root=proc),
                             {"pid": 42, "start_ticks": "123456"})
            (folder / "environ").write_bytes(b"BFCL_SPLIT_ID=another-split\0")
            with self.assertRaisesRegex(RuntimeError, "split_unverified"):
                d.runner_identity("bfcl", 42, proc_root=proc)
            (folder / "cmdline").write_bytes(b"python3\0unrelated.py\0")
            with self.assertRaisesRegex(RuntimeError, "command_unverified"):
                d.runner_identity("business", 42, proc_root=proc)
            with self.assertRaisesRegex(RuntimeError, "identity_unverified"):
                d.runner_identity("business", 42, proc_root=proc, root=proc)

    def test_pidfd_stage_recheck_prevents_signal(self):
        current = view()
        changed = view()
        changed["tree"]["holdout_started"] = True
        with patch.object(d.os, "pidfd_open", return_value=99, create=True), \
                patch.object(d.os, "close") as close, \
                patch.object(d.signal, "pidfd_send_signal") as send, \
                patch.object(d, "runner_identity", return_value={"pid": 42, "start_ticks": "1"}), \
                patch.object(d, "snapshot", return_value=changed):
            with self.assertRaisesRegex(RuntimeError, "stage_changed"):
                d.pause_search(current, "")
            send.assert_not_called()
            close.assert_called_once_with(99)

    def test_readonly_plan_has_no_writes_signals_or_workers(self):
        with tempfile.TemporaryDirectory() as tmp, \
                patch.object(d, "snapshot", side_effect=lambda line, job: view(line, False)), \
                patch.object(d, "external_delivery", return_value=False), \
                patch.object(d, "write") as write, patch.object(d, "pause_search") as stop, \
                patch.object(d.subprocess, "Popen") as launch, patch("sys.stdout", new_callable=io.StringIO):
            self.assertEqual(d.main(["--state-dir", tmp, "--cutoff", "2000-01-01T00:00:00Z",
                                     "--deadline", "2100-01-01T00:00:00Z"]), 0)
            write.assert_not_called()
            stop.assert_not_called()
            launch.assert_not_called()
            self.assertEqual(list(Path(tmp).iterdir()), [])

    def test_independent_launch_and_fast_receipt_not_overwritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            def launch(argv, **kwargs):
                receipt = Path(argv[argv.index("--receipt") + 1])
                d.write(receipt, {"status": "finished", "job_id": JOB, "pid": 987})
                return SimpleNamespace(pid=987, poll=lambda: 0)
            with patch.object(d, "snapshot", side_effect=lambda line, job: view(line, False)), \
                    patch.object(d, "external_delivery", return_value=False), \
                    patch.object(d, "process", return_value=None), \
                    patch.object(d.os, "pidfd_open", return_value=99, create=True), \
                    patch.object(d.subprocess, "Popen", side_effect=launch) as spawn, \
                    patch("sys.stdout", new_callable=io.StringIO):
                self.assertEqual(d.main(["--execute", "--once", "--state-dir", tmp,
                    "--cutoff", "2000-01-01T00:00:00Z", "--deadline", "2100-01-01T00:00:00Z"]), 0)
                self.assertEqual(spawn.call_count, 2)
                for line in d.LINES:
                    self.assertEqual(d.read(Path(tmp) / f"{line}-delivery.json")["status"], "finished")

    def test_finished_receipt_prevents_duplicate_launch(self):
        with tempfile.TemporaryDirectory() as tmp:
            for line in d.LINES:
                d.write(Path(tmp) / f"{line}-delivery.json", {"status": "finished", "job_id": JOB})
            with patch.object(d, "snapshot", side_effect=lambda line, job: view(line, False)), \
                    patch.object(d, "external_delivery", return_value=False), \
                    patch.object(d.os, "pidfd_open", return_value=99, create=True), \
                    patch.object(d.subprocess, "Popen") as spawn, \
                    patch("sys.stdout", new_callable=io.StringIO):
                self.assertEqual(d.main(["--execute", "--once", "--state-dir", tmp,
                    "--cutoff", "2000-01-01T00:00:00Z", "--deadline", "2100-01-01T00:00:00Z"]), 0)
                spawn.assert_not_called()

    def fake_best(self, root, current):
        best = SimpleNamespace(OUTPUT=root / "output", JOB_ROOT=root / "jobs")
        best.BEST_FILE = best.OUTPUT / "published/best-strategy.json"
        best.COMPARISON_FILE = best.OUTPUT / "published/best-strategy-comparison.json"
        for item in current["tree"]["nodes"]:
            d.write(best.JOB_ROOT / JOB / f"search/versions/{item['id']}.json", {"version": item["version"]})
        def metrics(item):
            rows = list(item["validation"].values())
            return {"quality": sum(row["quality"] for row in rows)/len(rows),
                    "strict_pass_rate": sum(row["strict_pass_rate"] for row in rows)/len(rows)}
        def candidates(*args, **kwargs):
            return [{"job_id": JOB, "node": item["id"], "version": item["version"],
                     "artifact": {"version": item["version"]}, "validation": metrics(item)}
                    for item in current["tree"]["nodes"] if item["id"] != 0]
        best.ROOT = root
        d.write(best.JOB_ROOT / JOB / "baseline.json", {"version": "L0"})
        best.discover_external_seed_candidates = lambda job: []
        best._baseline400_rows = lambda *args: None
        def freeze_artifact(job_root, freeze_dir, item):
            path = freeze_dir / "artifacts" / (item["version"] + ".json")
            d.write(path, item["artifact"])
            return {"artifact_path": str(path)}
        best._freeze_candidate_artifact = freeze_artifact
        best.discover_completed_val_audits = lambda job: []
        best._independent_val_candidates = Mock(return_value=[])
        best._cached_val_report = lambda *args: None
        best._node_metrics = metrics
        best._candidate_pool = candidates
        best._completed_pool_candidates = lambda *args: []
        best._artifact_version = lambda artifact: artifact["version"]
        best._showcase_key = lambda item: (item["quality"], item.get("strict_passed", 0))
        best._final_validation = Mock(return_value={"status": "complete", "complete_trials": 400, "metrics": {}})
        best.run_comparison = Mock(return_value={"status": "complete", "complete_trials": 400, "metrics": {}})
        return best

    def test_external_seed_is_frozen_and_restored_without_fake_node(self):
        with tempfile.TemporaryDirectory() as tmp:
            current = view(alive=False)
            best = self.fake_best(Path(tmp), current)
            external = {"job_id": JOB, "node": None, "external_candidate_id": "seed96",
                        "version": "external-v", "artifact": {"version": "external-v"},
                        "validation": {"quality": .95, "strict_passed": 40},
                        "screening_task_ids": sorted(current["tree"]["nodes"][0]["validation"]),
                        "selection_source": "external_seed_registry"}
            best.discover_external_seed_candidates = Mock(return_value=[external])
            best._independent_val_candidates.return_value = [external]
            chosen, pool = d.business_selection(best, current)
            self.assertIsNone(chosen["node"])
            self.assertEqual(chosen["external_candidate_id"], "seed96")
            chosen, _ = d.business_selection(best, current)
            self.assertEqual(chosen["artifact"]["version"], "external-v")
            self.assertFalse((best.JOB_ROOT / JOB / "search/versions/None.json").exists())

    def test_deadline_holdout_uses_complete_l0_cache_without_new_l0(self):
        with tempfile.TemporaryDirectory() as tmp:
            current = view(alive=False)
            best = self.fake_best(Path(tmp), current)
            best._baseline400_rows = Mock(return_value=([{"task_id": str(i)} for i in range(200)], "l0-index"))
            best._aggregate = Mock(return_value={"quality_complete": True, "scored": 200})
            best.run_comparison.return_value = {"status": "complete", "complete_trials": 200,
                                                "metrics": {"best": {"holdout": {"quality_complete": True}}}}
            result = d.business_delivery(best, current)
            self.assertFalse(best.run_comparison.call_args_list[0].kwargs["include_l0"])
            self.assertEqual(result["comparison"]["complete_trials"], 800)
            self.assertEqual(result["comparison"]["holdout"]["new_l0_tasks"], 0)

    def test_business_freeze_ignores_holdout_incomplete_val_and_remains_stable(self):
        with tempfile.TemporaryDirectory() as tmp:
            current = view(alive=False)
            current["tree"]["nodes"] += [node(2, .6), node(3, .99)]
            current["tree"]["nodes"][3]["validation"].pop("task-0")
            current["tree"]["nodes"][2]["holdout"] = {"quality": 1.0}
            best = self.fake_best(Path(tmp), current)
            chosen, pool = d.business_selection(best, current)
            self.assertEqual(chosen["node"], 1)
            self.assertEqual([item["node"] for item in pool], [1, 2])
            best.run_comparison.assert_not_called()
            current["tree"]["nodes"][2] = node(2, 1.0)
            chosen, _ = d.business_selection(best, current)
            self.assertEqual(chosen["node"], 1)
            result = d.business_delivery(best, current)
            self.assertEqual(result["selection"]["node"], 1)
            self.assertFalse(result["holdout_used_for_ranking"])
            self.assertEqual(result["status"], "shortfall")
            self.assertEqual(result["comparison"]["complete_trials"], 800)
            best._final_validation.assert_called_once()
            self.assertEqual(best.run_comparison.call_args_list[0].kwargs["splits"], ("holdout",))
            self.assertEqual(best.run_comparison.call_count, 2)
            self.assertEqual(best.run_comparison.call_args_list[1].kwargs["splits"], ("holdout",))

    def test_complete_independent_audit_joins_freeze_without_repair(self):
        with tempfile.TemporaryDirectory() as tmp:
            current = view(alive=False)
            current["tree"]["nodes"].append(node(2, .4))
            best = self.fake_best(Path(tmp), current)
            supplied = {"node": 2, "version": "v2"}
            best.discover_completed_val_audits = Mock(return_value=[supplied])
            audit = {"job_id": JOB, "node": 2, "version": "v2", "artifact": {"version": "v2"},
                     "validation": {"quality": .9, "strict_passed": 49},
                     "screening_task_ids": list(current["tree"]["nodes"][0]["validation"]),
                     "screening_report_path": "independent-report.json",
                     "validation_evidence_source": "independent_val_audit"}
            best._independent_val_candidates.return_value = [audit]
            chosen, pool = d.business_selection(best, current)
            self.assertEqual(chosen["node"], 2)
            self.assertEqual(chosen["screening_report_path"], "independent-report.json")
            self.assertEqual(len(pool), 2)
            best._independent_val_candidates.assert_called_once_with(
                JOB, [supplied], set(current["tree"]["nodes"][0]["validation"]), repair_errors=False)
            best.run_comparison.assert_not_called()
            best._final_validation.assert_not_called()

    def test_seed_only_is_labeled_and_does_not_require_three_candidates(self):
        with tempfile.TemporaryDirectory() as tmp:
            current = view(alive=False)
            current["tree"]["nodes"] = [node(0, .5)]
            best = self.fake_best(Path(tmp), current)
            result = d.business_delivery(best, current)
            self.assertTrue(result["evaluation_complete"])
            self.assertEqual(result["selection"]["selection_source"], "seed_validation_fallback")
            self.assertEqual(result["shortfall_candidate_count"], 2)
            best.run_comparison.assert_called_once()

    def test_bfcl_worker_uses_real_signature_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            gepa = SimpleNamespace(lifecycle_lock=nullcontext, environment=lambda: {})
            best = SimpleNamespace(build_best_observed=Mock(return_value={"status": "shortfall"}))
            original_path = list(d.sys.path)
            try:
                with patch.object(d.importlib, "import_module", side_effect=[gepa, best]), \
                        patch.object(d, "lock", return_value=nullcontext()), \
                        patch.object(d, "snapshot", return_value=view("bfcl", False)), \
                        patch.dict(d.os.environ):
                    self.assertEqual(d.worker("bfcl", JOB, Path(tmp) / "receipt.json"), 0)
                    best.build_best_observed.assert_called_once_with(JOB)
            finally:
                d.sys.path[:] = original_path


if __name__ == "__main__":
    unittest.main()
