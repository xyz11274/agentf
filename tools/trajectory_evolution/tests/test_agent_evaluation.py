"""Version-bound background evaluation and production-path isolation."""
import json
from contextlib import closing
import hashlib
import http.server
import os
from pathlib import Path
import select
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.request
import urllib.error
import uuid

BUILD = Path(sys.argv.pop(1)).resolve()
PROTOC = sys.argv.pop(1) if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else shutil.which("protoc")
SOURCE = Path(__file__).resolve().parents[1]
CLI = BUILD / "trajectory_evolution"
WORK = Path(tempfile.mkdtemp(prefix="agent-evaluation-tests.", dir=BUILD))
TOKEN = "agent-evaluation-test-user-a-token"
TOKEN_B = "agent-evaluation-test-user-b-token"


def write(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")


class AgentEvaluationTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="case.", dir=WORK))
        self.env = os.environ.copy()
        self.env.update(EVAL_USER_A_TOKEN=TOKEN, EVAL_USER_B_TOKEN=TOKEN_B)
        task = dict(task_id="sum", group_id="sum", task_input="生成视频时缺少图片素材，计算总量20。",
                    expected_behavior="返回20", initial_state={}, followup_inputs=["再次确认结果。"],
                    difficulty="easy", category="data", split="train", review_status="reviewed")
        write(self.root / "task.jsonl", task)
        dataset = json.loads((SOURCE / "config/dataset.json").read_text())
        dataset["sources"] = [dict(kind="golden", path=str(self.root / "task.jsonl"))]
        write(self.root / "dataset.json", dataset)
        result = subprocess.run([str(CLI), "dataset", "build", "--config", str(self.root / "dataset.json"),
                                 "--output-root", str(self.root / "dataset")], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.evaluation = dict(schema_version=1, splits=["train"], repetitions=2, parallelism=1, timeout_ms=10000,
            require_trace=True, variants=[dict(id="host", version="configured-host", pass_env=[],
                command=[sys.executable, "-B", str(SOURCE / "tests/agent_evaluation_host.py"), "good",
                         str(BUILD / "libtrajectory_evolution_local.so")])],
            checks={"*": [dict(id="total", path="/state/value", op="equals", value=20)]}, judge=dict(enabled=False))
        write(self.root / "evaluation.json", self.evaluation)
        write(self.root / "agent-evaluation.json", dict(schema_version=1, dataset="dataset", evaluation_config="evaluation.json",
            agent=dict(revision="fixture-agent-v1"), min_interval_ms=0, max_jobs_per_hour=100,
            gate=dict(min_trials=2, min_coverage=1, min_pass_rate=1, max_pass_rate_drop=0)))
        self.config = json.loads((SOURCE / "config/service.json").read_text())
        self.config.update(agent_id="eval-test", port=0, max_level="L3", auto_evaluate=False,
            data_dir=str(self.root / "production"), sharing=dict(enabled=False),
            principals=[dict(user_id="user-a", token_env="EVAL_USER_A_TOKEN"), dict(user_id="user-b", token_env="EVAL_USER_B_TOKEN")],
            runtime_config=str(SOURCE / "config/standard-runtime.json"),
            experience_template_file=str(SOURCE / "config/prompts/runtime-experience.txt"),
            policy_evaluation_file="agent-evaluation.json")
        write(self.root / "service.json", self.config)
        self.process = None

    def start(self):
        self.process = subprocess.Popen([str(CLI), "serve", "--config", str(self.root / "service.json")],
                                        env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        ready, _, _ = select.select([self.process.stdout], [], [], 10)
        self.assertTrue(ready, "service did not start")
        line = self.process.stdout.readline()
        self.assertIn("listening=", line, line + (self.process.stderr.read() if self.process.poll() is not None else ""))
        self.url = "http://" + line.strip().split("listening=")[1] + "/v1/invoke"

    def call(self, op, token=TOKEN, **fields):
        body = dict(operation=op, request_id=uuid.uuid4().hex, **fields)
        request = urllib.request.Request(self.url, data=json.dumps(body).encode(),
            headers={"Content-Type": "application/json", "Authorization": "Bearer " + token})
        try:
            with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(request, timeout=3) as response:
                result = json.load(response)
        except urllib.error.HTTPError as error:
            return json.load(error)
        self.assertTrue(result["ok"], result)
        return result["result"]

    def wait_jobs(self, token=TOKEN):
        deadline = time.monotonic() + 40
        while time.monotonic() < deadline:
            result = self.call("agent_evaluation.status", token=token)
            if len(result["jobs"]) >= 4 and all(x["status"] not in ("pending", "running") for x in result["jobs"].values()):
                return result
            time.sleep(0.05)
        self.fail("evaluation did not finish: " + json.dumps(result))

    def add_memory(self):
        folder = self.root / "production/by-user/user-a/memory"
        folder.mkdir(parents=True, exist_ok=True)
        source = '''schema_version: 1 store_kind: "trajectory_evolution_memory" store_version: "evolution-memory-store-v1" revision: 1
entries { memory_id: "test-experience" status: EVOLUTION_MEMORY_STATUS_CANDIDATE
 candidate { schema_version: 1 candidate_id: "test-candidate" generator_version: "scoped-experience-generator-v1"
 target_type: CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION target_scope: "agent:eval-test/actor:assistant" status: EVOLUTION_CANDIDATE_STATUS_PROPOSED
 trigger_pattern: "生成视频时缺少图片素材" observed_problem: "素材缺失时重复执行"
 proposed_change: "缺少图片素材时先补充素材，独立的计算任务继续。" support_user_count: 1
 support_evidence { evidence_id: "test-evidence" source { user_id: "user-a" dialogue_id: "source-run" } }
 }}'''
        protoc = PROTOC
        self.assertTrue(protoc, "pass the configured protoc executable after the build directory")
        result = subprocess.run([protoc, "--encode=agentflow.trajectory.evaluation.EvolutionMemoryStore",
            "-I" + str(SOURCE / "proto"), "-I" + str(Path(protoc).resolve().parent.parent / "include"),
            "hybrid_evaluation.proto"], input=source.encode(), capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        (folder / "evolution-memory.pb").write_bytes(result.stdout)

    def prepare_checkpoint_retry(self, invalid_json=False):
        requests = []
        self.pause_resume_judge = False
        self.resume_judge_entered = threading.Event()
        self.resume_judge_release = threading.Event()
        owner = self

        class Judge(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                requests.append(body)
                if owner.pause_resume_judge and len(requests) >= 4:
                    owner.resume_judge_entered.set()
                    owner.resume_judge_release.wait(5)
                packet = json.loads(body["messages"][1]["content"])
                reviews = [dict(iteration_id=k, decision="keep", reason="该轮提供完成任务所需的结果。",
                    evidence_event_ids=[v.get("anchor_event_id", v["event_ids"][0])])
                    for k, v in packet.get("loop_inventory", {}).items()]
                judgment = dict(verdict="passed", reason="实际输出满足任务要求。", evidence_paths=["/output"],
                    loop_review=dict(status="complete", iteration_reviews=reviews))
                content = "{" if invalid_json and len(requests) == 2 else json.dumps(judgment)
                payload = json.dumps(dict(choices=[dict(finish_reason="stop", message=dict(content=content))])).encode()
                self.send_response(503 if len(requests) == 2 and not invalid_json else 200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                try:
                    self.wfile.write(payload)
                except OSError:
                    pass  # Cancellation closes the in-flight grading socket.

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Judge)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close); self.addCleanup(server.shutdown)
        self.addCleanup(self.resume_judge_release.set)
        shutil.copytree(SOURCE / "config", self.root / "model-config")
        runtime_path = self.root / "model-config/standard-runtime.json"
        runtime = json.loads(runtime_path.read_text())
        runtime["model"].update(endpoint=f"http://127.0.0.1:{server.server_port}/chat/completions",
            model_id="checkpoint-judge", no_api_key=True, max_retries=0, timeout_ms=3000)
        write(runtime_path, runtime)
        self.config.update(max_level="L0", principals=self.config["principals"][:1])
        write(self.root / "service.json", self.config)
        self.evaluation["repetitions"] = 3
        self.evaluation["variants"][0]["command"][-2] = "resume"
        self.evaluation["variants"][0]["pass_env"] = ["EVOLUTION_TEST_EXECUTIONS"]
        self.env["EVOLUTION_TEST_EXECUTIONS"] = str(self.root / "executions.txt")
        self.evaluation["artifacts"] = {"*": [dict(path="proof.txt", media_type="text/plain")]}
        self.evaluation["judge"] = dict(enabled=True, runtime_config=str(runtime_path),
            prompt=str(SOURCE / "config/prompts/task-judge-system.txt"),
            loop_review_prompt=str(SOURCE / "config/prompts/task-loop-review.txt"), max_repair_attempts=0)
        write(self.root / "evaluation.json", self.evaluation)
        self.start()
        job_id, job = self.wait_checkpoint_job()
        self.assertEqual(job["status"], "inconclusive", job)
        self.assertEqual(len(requests), 3)
        folder = self.root / "production/agent-evaluation" / job_id
        return job_id, folder, requests

    def wait_checkpoint_job(self):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            state = self.call("agent_evaluation.status")
            if len(state["jobs"]) == 1:
                job_id, job = next(iter(state["jobs"].items()))
                if job["status"] not in ("pending", "running"):
                    return job_id, job
            time.sleep(.02)
        self.fail("checkpoint job did not finish")

    def test_automatic_level_selection_only_schedules_l2(self):
        self.add_memory()
        self.config.update(max_level="L2", evaluation_levels=["L2"])
        write(self.root / "service.json", self.config)
        self.start()
        deadline = time.monotonic() + 15
        result = {}
        while time.monotonic() < deadline:
            result = self.call("agent_evaluation.status")
            if result["jobs"] and all(x["status"] not in ("pending", "running") for x in result["jobs"].values()):
                break
            time.sleep(0.05)
        self.assertTrue(result["jobs"], result)
        self.assertEqual({x["level"] for x in result["jobs"].values()}, {"L2"})
        for job in result["jobs"].values():
            self.assertNotIn(job["status"], ("pending", "running", "error"), job)

    def test_empty_automatic_levels_leaves_background_evaluation_idle(self):
        self.add_memory()
        self.config.update(evaluation_levels=[])
        write(self.root / "service.json", self.config)
        self.start()
        for _ in range(5):
            self.assertFalse(self.call("agent_evaluation.status")["jobs"])
            time.sleep(0.1)

    def test_retry_reuses_execution_and_only_regrades_failed_judgment(self):
        job_id, folder, requests = self.prepare_checkpoint_retry()
        original = (folder / "attempt-1/report.json").read_bytes()
        first = json.loads(original)
        self.call("agent_evaluation.retry", job_id=job_id)
        _, job = self.wait_checkpoint_job()
        self.assertEqual(job["status"], "passed", job)
        resumed = json.loads((folder / "attempt-2/report.json").read_text())
        self.assertTrue(resumed["complete"])
        self.assertEqual(resumed["summary"]["reused_trials"], 2)
        self.assertEqual(resumed["summary"]["regraded_trials"], 1)
        self.assertEqual(resumed["summary"]["model_calls"], 1)
        self.assertEqual(resumed["summary"]["resume_source"]["report_sha256"], hashlib.sha256(original).hexdigest())
        self.assertEqual(len(requests), 4)
        self.assertEqual(len((self.root / "executions.txt").read_text().splitlines()), 3)
        self.assertEqual((folder / "attempt-1/report.json").read_bytes(), original)
        for before, after in zip(first["trials"], resumed["trials"]):
            self.assertEqual(before["trial_id"], after["trial_id"])
            self.assertEqual(before["duration_ms"], after["duration_ms"])
            self.assertEqual(before["evidence"]["trace"], after["evidence"]["trace"])
            if before["verdict"] == "passed":
                self.assertEqual(before, after)
            name = after["evidence"]["artifacts"]["proof.txt"]["stored_file"]
            self.assertEqual((folder / "attempt-2/artifacts" / after["trial_id"] / name).read_text(), "verified execution")

    def test_retry_rejects_changed_report_without_executing_agent(self):
        job_id, folder, requests = self.prepare_checkpoint_retry()
        path = folder / "attempt-1/report.json"
        path.write_bytes(path.read_bytes() + b" ")
        self.call("agent_evaluation.retry", job_id=job_id)
        _, job = self.wait_checkpoint_job()
        self.assertEqual(job["error"], "evaluation_checkpoint_changed")
        self.assertEqual(len(requests), 3)
        self.assertEqual(len((self.root / "executions.txt").read_text().splitlines()), 3)

    def test_successful_regrade_clears_previous_validation_diagnostics(self):
        job_id, folder, requests = self.prepare_checkpoint_retry(invalid_json=True)
        first = json.loads((folder / "attempt-1/report.json").read_text())
        failed = next(t for t in first["trials"] if t.get("error_code") == "evaluation_judge_failed")
        self.assertEqual(failed["evidence"]["judge_validation_error"]["code"], "invalid_json")
        self.call("agent_evaluation.retry", job_id=job_id)
        _, job = self.wait_checkpoint_job()
        self.assertEqual(job["status"], "passed", job)
        resumed = json.loads((folder / "attempt-2/report.json").read_text())
        retried = next(t for t in resumed["trials"] if t["trial_id"] == failed["trial_id"])
        self.assertNotIn("judge_validation_error", retried["evidence"])
        self.assertEqual(retried["evidence"]["loop_review"]["status"], "complete")
        self.assertEqual(len(requests), 4)
        self.assertEqual(len((self.root / "executions.txt").read_text().splitlines()), 3)

    def test_cancelled_regrade_keeps_checkpoints_for_another_retry(self):
        job_id, folder, _ = self.prepare_checkpoint_retry()
        original = json.loads((folder / "attempt-1/report.json").read_text())
        self.pause_resume_judge = True
        self.call("agent_evaluation.retry", job_id=job_id)
        self.assertTrue(self.resume_judge_entered.wait(5))
        self.call("agent_evaluation.cancel", job_id=job_id)
        self.resume_judge_release.set()
        _, job = self.wait_checkpoint_job()
        self.assertEqual(job["error"], "evaluation_cancelled", job)
        cancelled = json.loads((folder / "attempt-2/report.json").read_text())
        self.assertFalse(cancelled["complete"])
        self.assertEqual({t["trial_id"] for t in cancelled["trials"]}, {t["trial_id"] for t in original["trials"]})
        self.pause_resume_judge = False
        self.call("agent_evaluation.retry", job_id=job_id)
        _, job = self.wait_checkpoint_job()
        self.assertEqual(job["status"], "passed", job)
        self.assertEqual(len((self.root / "executions.txt").read_text().splitlines()), 3)

    def test_retry_rejects_changed_grading_context(self):
        job_id, folder, requests = self.prepare_checkpoint_retry()
        path = folder / "attempt-1/report.json"
        report = json.loads(path.read_text()); report["config_fingerprint"] = "0" * 64; write(path, report)
        manifest_path = folder / "attempt-1/manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["files"]["report.json"] = hashlib.sha256(path.read_bytes()).hexdigest(); write(manifest_path, manifest)
        self.call("agent_evaluation.retry", job_id=job_id)
        _, job = self.wait_checkpoint_job()
        self.assertEqual(job["error"], "evaluation_checkpoint_context_mismatch")
        self.assertEqual(len(requests), 3)

    def test_retry_rejects_changed_deliverable(self):
        job_id, folder, requests = self.prepare_checkpoint_retry()
        first = json.loads((folder / "attempt-1/report.json").read_text())["trials"][0]
        name = first["evidence"]["artifacts"]["proof.txt"]["stored_file"]
        (folder / "attempt-1/artifacts" / first["trial_id"] / name).write_text("changed")
        self.call("agent_evaluation.retry", job_id=job_id)
        _, job = self.wait_checkpoint_job()
        self.assertEqual(job["error"], "evaluation_checkpoint_artifact_changed")
        self.assertEqual(len(requests), 3)

    def test_bundle_feedback_does_not_archive_each_memory(self):
        self.add_memory()
        self.config["auto_approve_experiences"] = True
        write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "bad_candidate"
        write(self.root / "evaluation.json", self.evaluation)
        self.start(); self.wait_jobs()
        deadline = time.monotonic() + 5
        path = self.root / "production/by-user/user-a/memory/evolution-memory.json"
        memory = {}
        while time.monotonic() < deadline:
            if path.exists():
                memory = json.loads(path.read_text())
                if memory.get("entries", [{}])[0].get("evaluation_feedback"):
                    break
            time.sleep(.05)
        self.assertTrue(memory.get("entries"), self.call("agent_evaluation.status"))
        entry = memory["entries"][0]
        self.assertEqual(entry["status"], "EVOLUTION_MEMORY_STATUS_CANDIDATE")
        self.assertEqual(entry["evaluation_feedback"]["status"], "review_needed")
        self.assertEqual(int(entry["evaluation_feedback"].get("negative_trial_count", 0)), 0)
        self.assertEqual(self.call("agent_evaluation.feedback", token=TOKEN_B), {})

    def test_single_memory_regression_is_archived(self):
        self.config["auto_approve_experiences"] = True
        write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "memory_bad"
        write(self.root / "evaluation.json", self.evaluation)
        self.start(); self.wait_jobs(); self.tearDown()
        self.add_memory(); self.start()
        deadline = time.monotonic() + 35
        path = self.root / "production/by-user/user-a/memory/evolution-memory.json"
        memory = {}
        while time.monotonic() < deadline:
            if path.exists():
                memory = json.loads(path.read_text())
                if memory.get("entries") and memory["entries"][0]["status"] == "EVOLUTION_MEMORY_STATUS_ARCHIVED":
                    break
            time.sleep(.05)
        self.assertEqual(memory["entries"][0]["status"], "EVOLUTION_MEMORY_STATUS_ARCHIVED", self.call("agent_evaluation.status"))
        self.assertGreaterEqual(int(memory["entries"][0]["evaluation_feedback"]["negative_trial_count"]), 2)

    def test_slow_background_does_not_wait_in_run_begin(self):
        self.evaluation["variants"][0]["command"][-2] = "slow"
        write(self.root / "evaluation.json", self.evaluation)
        self.start()
        started = time.monotonic()
        run = self.call("run.begin", input="foreground", level="L3", capabilities=["model", "actions", "loop"])
        self.assertLess(time.monotonic() - started, 1)
        self.assertEqual(run["level"], "L0")
        state = self.call("agent_evaluation.status")
        for job_id, job in state["jobs"].items():
            if job["status"] in ("pending", "running"):
                self.call("agent_evaluation.cancel", job_id=job_id)

    def test_single_memory_improvement_is_validated(self):
        self.config["auto_approve_experiences"] = True
        write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "memory_good"
        write(self.root / "evaluation.json", self.evaluation)
        settings = json.loads((self.root / "agent-evaluation.json").read_text())
        settings["gate"]["min_pass_rate"] = 0
        write(self.root / "agent-evaluation.json", settings)
        self.start(); self.wait_jobs(); self.tearDown()
        self.add_memory(); self.start()
        deadline = time.monotonic() + 35
        path = self.root / "production/by-user/user-a/memory/evolution-memory.json"
        memory = {}
        while time.monotonic() < deadline:
            if path.exists():
                memory = json.loads(path.read_text())
                if memory.get("entries") and memory["entries"][0]["status"] == "EVOLUTION_MEMORY_STATUS_VALIDATED":
                    break
            time.sleep(.05)
        self.assertEqual(memory.get("entries", [{}])[0].get("status"), "EVOLUTION_MEMORY_STATUS_VALIDATED",
                         self.call("agent_evaluation.status"))
        self.assertGreaterEqual(int(memory["entries"][0]["evaluation_feedback"]["positive_trial_count"]), 2)

    def tearDown(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.communicate()

    def test_all_levels_publish_and_reload(self):
        self.start()
        result = self.wait_jobs()
        material = ((self.root / "agent-evaluation.json").read_text() + (self.root / "evaluation.json").read_text()
                    + json.loads((self.root / "dataset/manifest.json").read_text())["metadata"]["dataset_version"])
        old_context = hashlib.sha256(material.encode()).hexdigest()
        context = hashlib.sha256(("boundary-current-request-v1\n" + material).encode()).hexdigest()
        self.assertNotEqual(context, old_context)
        self.assertTrue(all(job["context"] == context for job in result["jobs"].values()),
                        "execution-semantic upgrade must not reuse the old evaluation context")
        self.assertEqual(set(result["publications"]), {"L0", "L1", "L2", "L3"}, result)
        bundles = {}
        for job_id, job in result["jobs"].items():
            self.assertEqual(job["status"], "passed", job)
            self.assertEqual(job["gate"]["candidate"]["host_reported_metrics"]["llm_calls"]["mean"], 2)
            folder = self.root / "production/agent-evaluation" / job_id
            path = folder / "published-version.json"
            bundle = json.loads(path.read_text())
            self.assertEqual(bundle["artifact"], json.loads((folder / "candidate.json").read_text()))
            self.assertEqual(bundle["version"], job["version"])
            self.assertEqual(bundle["evaluation"], dict(job_id=job_id, context=job["context"], gate=job["gate"]))
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            bundles[path] = path.read_bytes()
        run = self.call("run.begin", input="test", level="L3", capabilities=["model", "actions", "loop"])
        self.assertEqual(run["level"], "L3", run)
        self.assertEqual(run["policy_gate"], "published")
        self.assertIn("agent-version-v1:", run["agent_version_id"])
        self.call("run.end", run_id=run["run_id"], status="completed", output="done")
        count = len(result["jobs"])
        self.tearDown()
        # Simulate a pre-fix installation: published state exists, exports do not.
        for path in bundles:
            path.unlink()
        self.start()
        restored = self.wait_jobs()
        self.assertEqual(len(restored["jobs"]), count)
        for job_id, job in result["jobs"].items():
            self.assertEqual(restored["jobs"][job_id]["attempt"], job["attempt"])
        for path, content in bundles.items():
            self.assertEqual(path.read_bytes(), content)
        mtimes = {path: path.stat().st_mtime_ns for path in bundles}
        self.tearDown(); self.start(); self.wait_jobs()
        self.assertEqual({path: path.stat().st_mtime_ns for path in bundles}, mtimes)

    def test_fixed_candidate_bundle_runs_offline_without_memory(self):
        self.add_memory()
        self.config["actors"].append("worker/secondary")
        self.config["auto_approve_experiences"] = True
        write(self.root / "service.json", self.config)
        self.start()
        state = self.wait_jobs()
        job_id, job = next((key, value) for key, value in state["jobs"].items() if value["level"] == "L3")
        self.assertTrue(job["published"], job)
        self.tearDown()
        self.config.pop("policy_evaluation_file")
        self.config.update(policy_version_file=str(self.root / "production/agent-evaluation" / job_id / "published-version.json"),
                           data_dir=str(self.root / "offline"))
        # A deployed bundle needs no training runtime, prompt files or memory.
        for key in ("runtime_config", "experience_template_file", "shadow_options", "runtime_options"):
            self.config.pop(key, None)
        write(self.root / "service.json", self.config)
        self.start()
        run = self.call("run.begin", input="生成视频时缺少图片素材，计算总量20。", level="L3",
                        capabilities=["model", "actions", "loop"])
        self.assertEqual(run.get("agent_version_id"), job["version"], run)
        activity = self.call("activity.begin", run_id=run["run_id"], kind="model", name="offline-model",
            input={"messages": [{"role": "user", "content": "生成视频时缺少图片素材，计算总量20。"}]},
            binding={"pointer": "/messages", "mode": "system_message"})
        self.assertIn("缺少图片素材时先补充素材", json.dumps(activity["input"], ensure_ascii=False))
        self.call("activity.end", run_id=run["run_id"], activity_id=activity["activity_id"],
                  status="completed", output="20", actual_input=activity["input"], executed=True)
        self.call("run.end", run_id=run["run_id"], status="completed", output="20")
        other = self.call("run.begin", token=TOKEN_B, input="生成视频时缺少图片素材。", level="L3",
                          capabilities=["model", "actions", "loop"])
        self.assertNotIn("agent_version_id", other)
        self.call("run.end", token=TOKEN_B, run_id=other["run_id"], status="completed", output="done")
        self.assertFalse((self.root / "offline/agent-evaluation").exists())
        self.assertFalse((self.root / "offline/by-user/user-a/memory").exists())

    def test_offline_native_bundle_ignores_unused_files_and_checks_authority(self):
        self.add_memory()
        self.config["actors"].append("worker/secondary")
        self.config["auto_approve_experiences"] = True
        write(self.root / "service.json", self.config)
        self.start()
        state = self.wait_jobs()
        self.tearDown()
        job_id, job = next((key, value) for key, value in state["jobs"].items() if value["level"] == "L3")
        self.config.pop("policy_evaluation_file")
        self.config.update(policy_version_file=str(self.root / "production/agent-evaluation" / job_id / "published-version.json"),
                           data_dir=str(self.root / "offline-native"), runtime_config="deleted-runtime.json",
                           experience_template_file="deleted-template.txt", shadow_options={},
                           runtime_options=dict(request_context_pointer="/messages", request_context_mode="system_message"))
        old_memory = self.root / "offline-native/by-user/user-a/memory/evolution-memory.pb"
        old_memory.parent.mkdir(parents=True)
        old_memory.write_bytes(b"unused malformed historical memory")
        write(self.root / "service.json", self.config)
        sys.path.insert(0, str(SOURCE / "clients/python"))
        from evolution_local import LocalEvolution
        os.environ.update(EVAL_USER_A_TOKEN=TOKEN, EVAL_USER_B_TOKEN=TOKEN_B)
        with closing(LocalEvolution(BUILD / "libtrajectory_evolution_local.so", self.root / "service.json", TOKEN)) as client:
            run = client.invoke("run.begin", dict(conversation_id="native-offline", input="生成视频时缺少图片素材。",
                                level="L3", capabilities=["model", "actions", "loop"]))
            self.assertEqual(run.get("agent_version_id"), job["version"], run)
            self.assertEqual(run.get("policy_gate"), "offline_version", run)
            client.invoke("run.end", dict(run_id=run["run_id"], status="completed", output="done"))
            missing = client.invoke("run.begin", dict(conversation_id="missing-boundary", input="test",
                                    level="L3", capabilities=["loop"]))
            self.assertTrue(missing.get("degraded"), missing)
            self.assertEqual(missing.get("reason"), "offline_policy_capability_missing", missing)
            client.drain()
        self.assertEqual(old_memory.read_bytes(), b"unused malformed historical memory")
        self.config["runtime_options"] = dict(loop_rules=[dict(rule_id="new-strict-limit", actor_id="assistant",
            max_iterations=1, no_progress_decision="stop", reason="current deployment budget")])
        write(self.root / "service.json", self.config)
        with closing(LocalEvolution(BUILD / "libtrajectory_evolution_local.so", self.root / "service.json", TOKEN)) as client:
            denied = client.invoke("run.begin", dict(conversation_id="strict-deployment", input="test",
                                   level="L3", capabilities=["model", "actions", "loop"]))
            self.assertTrue(denied.get("degraded"), denied)
            self.assertEqual(denied.get("reason"), "offline_policy_limits_changed", denied)

    def test_restart_rejects_changed_export(self):
        self.start()
        state = self.wait_jobs()
        self.tearDown()
        job_id = next(key for key, value in state["jobs"].items() if value["level"] == "L3")
        path = self.root / "production/agent-evaluation" / job_id / "published-version.json"
        bundle = json.loads(path.read_text())
        bundle["evaluation"]["gate"]["passed"] = False
        write(path, bundle)
        before = path.read_bytes()
        result = subprocess.run([str(CLI), "serve", "--config", str(self.root / "service.json")],
                                env=self.env, capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(path.read_bytes(), before)

    def test_backfill_old_context_does_not_approve_current_context(self):
        self.start()
        state = self.wait_jobs()
        self.tearDown()
        paths = {key: self.root / "production/agent-evaluation" / key / "published-version.json"
                 for key in state["jobs"]}
        original = {key: path.read_bytes() for key, path in paths.items()}
        for path in paths.values():
            path.unlink()
        evaluation = json.loads((self.root / "agent-evaluation.json").read_text())
        evaluation["gate"]["max_pass_rate_drop"] = 0.01
        write(self.root / "agent-evaluation.json", evaluation)
        self.config["evaluation_levels"] = []
        write(self.root / "service.json", self.config)
        self.start()
        current = self.call("agent_evaluation.status")
        self.assertEqual(current["publications"], {})
        self.assertEqual(set(current["jobs"]), set(state["jobs"]))
        for key, path in paths.items():
            self.assertEqual(path.read_bytes(), original[key])
            self.assertEqual(current["jobs"][key]["attempt"], state["jobs"][key]["attempt"])

    def test_native_management_uses_existing_background_service(self):
        sys.path.insert(0, str(SOURCE / "clients/python"))
        from evolution_local import LocalEvolution
        os.environ["EVAL_USER_A_TOKEN"] = TOKEN
        os.environ["EVAL_USER_B_TOKEN"] = TOKEN_B
        with self.assertRaises(RuntimeError):
            # An invalid deployment does not create an unmanaged runtime.
            LocalEvolution(BUILD / "libtrajectory_evolution_local.so", self.root / "missing.json", TOKEN)
        evo = LocalEvolution(BUILD / "libtrajectory_evolution_local.so", self.root / "service.json", TOKEN)
        try:
            result = evo.manage("agent_evaluation.status")
            self.assertIn("jobs", result)
            with self.assertRaisesRegex(RuntimeError, "management_operation_required"):
                evo.manage("run.begin", {"input": "not an administrative operation"})
            run = evo.invoke("run.begin", {"input": "test", "level": "L0"})
            self.assertFalse(run.get("degraded", False), run)
            evo.invoke("run.end", {"run_id": run["run_id"], "status": "completed", "output": "done"})
        finally:
            evo.close()

    def test_failed_candidates_leave_baseline_and_user_isolation(self):
        self.evaluation["variants"][0]["command"][-2] = "bad_candidate"
        write(self.root / "evaluation.json", self.evaluation)
        self.start()
        result = self.wait_jobs()
        self.assertEqual(set(result["publications"]), {"L0"}, result)
        for job_id, job in result["jobs"].items():
            exported = self.root / "production/agent-evaluation" / job_id / "published-version.json"
            self.assertEqual(exported.exists(), job["level"] == "L0")
        run = self.call("run.begin", input="test", level="L3", capabilities=["model", "actions", "loop"])
        self.assertEqual(run["level"], "L0", run)
        denied = self.call("agent_evaluation.retry", token=TOKEN_B, job_id=next(iter(result["jobs"])))
        self.assertFalse(denied["ok"])
        self.assertEqual(denied["error"], "evaluation_job_not_found")
        rejected = self.call("agent_evaluation.submit", artifact={"revision": TOKEN})
        self.assertEqual(rejected["error"], "credential_value_rejected")

    def test_zero_baseline_is_not_a_positive_loop_reduction(self):
        config = json.loads((self.root / "agent-evaluation.json").read_text())
        config["gate"]["min_invalid_loop_reduction"] = .3
        write(self.root / "agent-evaluation.json", config)
        self.start()
        result = self.wait_jobs()
        self.assertEqual(set(result["publications"]), {"L0"})
        for job in result["jobs"].values():
            if job["level"] != "L0":
                self.assertIn("loop_baseline_has_no_observed_waste", job["gate"]["reasons"])

    def test_wrong_loaded_version_is_rejected(self):
        self.evaluation["variants"][0]["command"][-2] = "wrong_version"
        write(self.root / "evaluation.json", self.evaluation)
        self.start()
        result = self.wait_jobs()
        self.assertEqual(result["publications"], {}, result)
        reports = list((self.root / "production/agent-evaluation").glob("*/attempt-*/report.json"))
        self.assertTrue(reports)
        for path in reports:
            for trial in json.loads(path.read_text())["trials"]:
                self.assertEqual(trial["error_code"], "evaluation_loaded_version_mismatch")


if __name__ == "__main__":
    unittest.main()
