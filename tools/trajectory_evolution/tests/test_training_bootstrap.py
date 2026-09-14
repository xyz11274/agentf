"""Automatic seed preparation reuses the evaluation host and learning queue."""
import hashlib
import json
import subprocess
import time
import unittest

import test_gepa_search as gepa
from test_gepa_search import fixture


class TrainingBootstrapTests(unittest.TestCase):
    start = fixture.AgentEvaluationTests.start
    call = fixture.AgentEvaluationTests.call
    tearDown = gepa.GepaSearchTests.tearDown

    def add_memory(self):
        # The evaluation-only fixture uses convenient IDs. Learning additionally
        # validates content-derived candidate/memory IDs, so seed that contract.
        candidate = dict(schema_version=1, generator_version="scoped-experience-generator-v1",
            target_type="CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION", target_scope="agent:eval-test/actor:assistant",
            trigger_pattern="生成视频时缺少图片素材", observed_problem="素材缺失时重复执行",
            proposed_change="缺少图片素材时先补充素材，独立的计算任务继续。", support_user_count=1,
            status="EVOLUTION_CANDIDATE_STATUS_PROPOSED",
            support_evidence=[dict(evidence_id="test-evidence", source=dict(user_id="user-a", dialogue_id="source-run"))])
        digest = lambda text: hashlib.sha256(text.encode()).hexdigest()
        candidate["proposal_fingerprint"] = digest(candidate["proposed_change"])
        candidate["candidate_id"] = "candidate:" + digest("|".join(["2", candidate["generator_version"],
            candidate["target_scope"], candidate["trigger_pattern"], candidate["proposal_fingerprint"]]))
        memory = dict(schema_version=1, store_kind="trajectory_evolution_memory", store_version="evolution-memory-store-v1",
            revision=1, entries=[dict(memory_id="memory:" + candidate["candidate_id"], candidate=candidate,
                                     status="EVOLUTION_MEMORY_STATUS_CANDIDATE")])
        path = self.root / "production/by-user/user-a/memory/evolution-memory.pb"
        path.parent.mkdir(parents=True, exist_ok=True)
        encoded = subprocess.run([str(fixture.BUILD / "test_shared_memory"), "--encode-memory"],
            input=json.dumps(memory).encode(), capture_output=True, check=True).stdout
        path.write_bytes(encoded)

    def setUp(self):
        gepa.GepaSearchTests.setUp(self)
        self.config.update(auto_evaluate=True, principals=self.config["principals"][:1])
        self.config["learning"].update(min_new_runs=1, min_interval_ms=0, max_wait_ms=1000)
        fixture.write(self.root / "service.json", self.config)
        self.assessment["training"] = dict(min_experiences=3, batch_size=1, max_batches=2)
        fixture.write(self.root / "agent-evaluation.json", self.assessment)

    def wait_training(self, states=("training_exhausted", "budget_exhausted")):
        deadline = time.monotonic() + 25
        status = {}
        while time.monotonic() < deadline:
            status = self.call("agent_evaluation.status")
            if status.get("training", {}).get("status") in states:
                return status
            time.sleep(.05)
        self.fail("training did not finish: " + json.dumps(status))

    def test_nonempty_but_insufficient_memory_trains_and_resumes_without_replay(self):
        self.start()
        status = self.wait_training()
        self.assertEqual(status["training"]["usable_experiences"], 1)
        jobs = [job for job in status["jobs"].values() if job.get("training")]
        self.assertEqual(len(jobs), 2, status)
        self.assertEqual({t for job in jobs for t in job["task_ids"]}, {"train-a", "train-b"})
        self.assertTrue(all(job["training_import"]["ok"] for job in jobs), jobs)
        runs = [json.loads(path.read_text()) for path in (self.root / "production/runs").glob("*.json")]
        self.assertEqual(len(runs), 2)
        self.assertTrue(all(run["evaluation_status"] == "completed" for run in runs), runs)
        self.assertEqual({run["training_source"]["task_id"] for run in runs}, {"train-a", "train-b"})
        self.assertFalse(status["publications"], status)
        self.assertFalse(self.requests, "GEPA/reflection must not run with insufficient seed memory")
        fixture.AgentEvaluationTests.tearDown(self)
        self.start()
        restored = self.wait_training()
        self.assertEqual(set(restored["jobs"]), set(status["jobs"]))
        self.assertEqual(len(list((self.root / "production/runs").glob("*.json"))), 2)

    def test_empty_memory_uses_the_same_training_path(self):
        (self.root / "production/by-user/user-a/memory/evolution-memory.pb").unlink()
        self.assessment["training"]["max_batches"] = 1
        fixture.write(self.root / "agent-evaluation.json", self.assessment)
        self.start()
        status = self.wait_training()
        self.assertEqual(status["training"]["usable_experiences"], 0)
        self.assertEqual(status["training"]["batches"], 1)

    def prepare_waiting_batch(self, status="pending", error=None):
        self.assessment["training"]["max_batches"] = 1
        fixture.write(self.root / "agent-evaluation.json", self.assessment)
        state_path = self.root / "production/agent-evaluation/state.json"
        state_path.parent.mkdir(parents=True, exist_ok=True)
        fixture.write(state_path, dict(schema_version=1, jobs={}, latest={}, publications={},
            feedback={}, starts=[], next_due_ms=int(time.time() * 1000) + 3600000))
        self.start()
        before = self.wait_training(("training",))
        fixture.AgentEvaluationTests.tearDown(self)
        state = json.loads(state_path.read_text())
        job_id = before["training"]["job_id"]
        state["jobs"][job_id]["status"] = status
        if error is not None:
            state["jobs"][job_id]["error"] = error
        state["next_due_ms"] = 0
        fixture.write(state_path, state)
        return before, job_id

    def assert_stale_batch_replaced(self, status):
        before, old_id = self.prepare_waiting_batch(status,
            "training_report_incomplete" if status == "error" else None)
        folder = self.root / "production/agent-evaluation" / old_id / "attempt-1"
        folder.mkdir()
        fixture.write(folder / "manifest.json", dict(old_checkpoint="must not be reused"))
        checkpoint = (folder / "manifest.json").read_bytes()
        self.evaluation["timeout_ms"] += 1000
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        after = self.wait_training()
        new_id = after["training"]["job_id"]
        self.assertNotEqual(new_id, old_id)
        self.assertEqual(after["jobs"][old_id]["status"], "superseded")
        self.assertEqual(after["jobs"][new_id]["supersedes"], old_id)
        self.assertEqual(after["jobs"][new_id]["task_ids"], before["jobs"][old_id]["task_ids"])
        self.assertNotEqual(after["jobs"][new_id]["context"], before["jobs"][old_id]["context"])
        self.assertEqual(after["jobs"][new_id]["status"], "completed")
        self.assertTrue(after["jobs"][new_id]["training_import"]["ok"])
        self.assertEqual(after["training"]["batches"], before["training"]["batches"])
        self.assertEqual(after["training"]["tasks"], before["training"]["tasks"])
        self.assertEqual((folder / "manifest.json").read_bytes(), checkpoint)
        fixture.AgentEvaluationTests.tearDown(self)
        self.start()
        restored = self.wait_training()
        self.assertEqual(set(restored["jobs"]), set(after["jobs"]))
        self.assertEqual(restored["jobs"][new_id]["attempt"], 1)

    def test_changed_context_requeues_pending_training_without_new_budget(self):
        self.assert_stale_batch_replaced("pending")

    def test_changed_context_requeues_interrupted_training(self):
        self.assert_stale_batch_replaced("running")

    def test_changed_context_requeues_failed_training(self):
        self.assert_stale_batch_replaced("error")

    def test_unchanged_context_does_not_automatically_retry_failure(self):
        before, job_id = self.prepare_waiting_batch("error", "training_report_incomplete")
        self.start()
        after = self.wait_training(("training_failed",))
        self.assertEqual(set(after["jobs"]), set(before["jobs"]))
        self.assertEqual(after["jobs"][job_id]["status"], "error")

    def test_changed_context_does_not_restart_cancelled_training(self):
        before, job_id = self.prepare_waiting_batch("error", "evaluation_cancelled")
        self.evaluation["timeout_ms"] += 1000
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        after = self.wait_training(("training_failed",))
        self.assertEqual(set(after["jobs"]), set(before["jobs"]))
        self.assertEqual(after["jobs"][job_id]["error"], "evaluation_cancelled")

    def test_changed_context_does_not_repeat_completed_training(self):
        self.assessment["training"]["max_batches"] = 1
        fixture.write(self.root / "agent-evaluation.json", self.assessment)
        self.start()
        before = self.wait_training()
        fixture.AgentEvaluationTests.tearDown(self)
        self.evaluation["timeout_ms"] += 1000
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        after = self.wait_training()
        self.assertEqual(set(after["jobs"]), set(before["jobs"]))
        self.assertEqual(after["training"]["batches"], 1)

    def test_filtered_memory_is_not_counted_as_a_seed(self):
        path = self.root / "production/by-user/user-a/memory/evolution-memory.pb"
        args = [fixture.PROTOC, "-I" + str(fixture.SOURCE / "proto"),
                "-I" + str(fixture.Path(fixture.PROTOC).resolve().parent.parent / "include"), "hybrid_evaluation.proto"]
        decoded = subprocess.run(args[:1] + ["--decode=agentflow.trajectory.evaluation.EvolutionMemoryStore"] + args[1:],
                                 input=path.read_bytes(), capture_output=True, check=True).stdout
        decoded = decoded.replace(b"EVOLUTION_MEMORY_STATUS_CANDIDATE", b"EVOLUTION_MEMORY_STATUS_ARCHIVED")
        encoded = subprocess.run(args[:1] + ["--encode=agentflow.trajectory.evaluation.EvolutionMemoryStore"] + args[1:],
                                 input=decoded, capture_output=True, check=True).stdout
        path.write_bytes(encoded)
        self.start()
        status = self.wait_training()
        self.assertEqual(status["training"]["usable_experiences"], 0)
        self.assertTrue(any(job.get("training") for job in status["jobs"].values()))

    def test_sufficient_memory_skips_training(self):
        self.assessment["training"]["min_experiences"] = 1
        fixture.write(self.root / "agent-evaluation.json", self.assessment)
        self.start()
        status = self.wait_training(("ready",))
        self.assertFalse(any(job.get("training") for job in status["jobs"].values()), status)

    def test_learning_import_failure_does_not_launch_another_batch(self):
        self.config["agent_id"] = "different-application"
        fixture.write(self.root / "service.json", self.config)
        self.start()
        status = self.wait_training(("import_failed",))
        self.assertEqual(status["training"]["batches"], 1)
        self.assertTrue(any(not job.get("training_import", {}).get("ok", True)
                            for job in status["jobs"].values()))

    def test_archived_learning_run_does_not_become_a_failure(self):
        self.config.update(max_stored_runs=1, max_active_runs=1)
        fixture.write(self.root / "service.json", self.config)
        self.start()
        status = self.wait_training()
        self.assertEqual(status["training"]["batches"], 2)
        self.assertTrue(list((self.root / "production/archive/runs").rglob("*.json")))

    def test_learning_error_stops_before_next_training_batch(self):
        fixture.AgentEvaluationTests.add_memory(self)  # Invalid learning IDs, deliberately.
        self.start()
        status = self.wait_training(("learning_failed",))
        self.assertEqual(status["training"]["batches"], 1)

    def test_offline_learn_waits_for_training_and_reports_insufficient_seed(self):
        result = subprocess.run([str(fixture.CLI), "learn", "--config", str(self.root / "service.json")],
            env=self.env, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        status = json.loads(result.stdout)
        self.assertEqual(status["training_failed_users"], 1)
        self.assertEqual(status["training_pending_users"], 0)
        self.assertEqual(status["agent_evaluation_pending_jobs"], 0)
        self.assertEqual(status["run_outcomes"].get("completed"), 2)


if __name__ == "__main__":
    unittest.main()
