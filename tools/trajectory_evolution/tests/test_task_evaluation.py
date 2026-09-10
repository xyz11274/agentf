"""Offline trial execution, deterministic grading and model-boundary regression."""

import http.server
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest


BUILD = Path(sys.argv.pop(1)).resolve()
SOURCE = Path(__file__).resolve().parents[1]
CLI = BUILD / "trajectory_evolution"
WORK = Path(tempfile.mkdtemp(prefix="task-evaluation-tests.", dir=BUILD))
KEY = "eval-test-opaque-credential-987654321"
REQUESTS = []


class Provider(http.server.BaseHTTPRequestHandler):
    behavior = "passed"

    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        REQUESTS.append(body)
        judgment = {"verdict": Provider.behavior, "reason": "实际输出与给定标准一致。", "evidence_paths": ["/state"]}
        if Provider.behavior == "reference":
            judgment.update(verdict="passed", evidence_paths=["/not-present"])
        if Provider.behavior == "leak":
            judgment.update(verdict="passed", reason=KEY)
        if Provider.behavior in ("fence", "repair"):
            judgment["verdict"] = "passed"
        content = json.dumps(judgment)
        if Provider.behavior == "fence":
            content = "```json\n" + content + "\n```"
        if Provider.behavior == "repair" and "validation_feedback" not in json.loads(body["messages"][1]["content"]):
            content = "invalid response"
        result = {"choices": [{"finish_reason": "length" if Provider.behavior == "length" else "stop",
                                "message": {"content": content}}]}
        encoded = json.dumps(result).encode()
        self.send_response(500 if Provider.behavior == "http_error" else 200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)


SERVER = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
threading.Thread(target=SERVER.serve_forever, daemon=True).start()


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")


class TrialTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="case.", dir=WORK))
        self.env = os.environ.copy()
        self.env["EVAL_TEST_API_KEY"] = KEY
        self.env["EVAL_UNRELATED_SECRET"] = "never-pass-this-to-the-child"
        self.config = dict(schema_version=1, splits=["train"], repetitions=1, parallelism=2,
                           timeout_ms=1000, max_output_bytes=65536,
                           variants=[dict(id="baseline", version="test-v1", command=[sys.executable, "-B", str(SOURCE / "tests/evaluation_host.py"), "good"],
                                          pass_env=["EVAL_TEST_API_KEY"])],
                           checks={"*": [{"id": "value", "path": "/state/value", "op": "equals", "value": 20}]})
        Provider.behavior = "passed"
        self.start_requests = len(REQUESTS)
        self.make_dataset()

    def call(self, *args, code=0):
        result = subprocess.run([str(CLI), "dataset", *map(str, args)], env=self.env,
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        self.assertNotIn(KEY, result.stdout + result.stderr)
        return result

    def make_dataset(self, cases=None, reviewed=True):
        if cases is None:
            cases = [dict(task_id="one", group_id="one", task_input="计算已知数据的总量。",
                          expected_behavior="总量为20，不编造缺失的数据。", difficulty="easy", category="data",
                          initial_state={"value": 20}, followup_inputs=["请再次确认。"], split="train",
                          review_status="reviewed" if reviewed else "pending")]
        source = self.root / "tasks.jsonl"
        source.write_text("\n".join(json.dumps(x, ensure_ascii=False) for x in cases) + "\n")
        config = json.loads((SOURCE / "config/dataset.json").read_text())
        config["sources"] = [{"kind": "golden", "path": str(source)}]
        config["budget"]["timeout_ms"] = 5000
        write_json(self.root / "dataset.json", config)
        self.dataset = self.root / ("dataset-" + str(len(list(self.root.glob("dataset-*")))))
        self.call("build", "--config", self.root / "dataset.json", "--output-root", self.dataset)

    def mode(self, value, index=0):
        self.config["variants"][index]["command"][-1] = value

    def model(self):
        runtime = json.loads((SOURCE / "config/standard-runtime.json").read_text())
        runtime["model"].update(endpoint=f"http://127.0.0.1:{SERVER.server_port}/chat/completions",
                                model_id="task-eval-fake", api_key_env="EVAL_TEST_API_KEY", thinking_mode="disabled",
                                timeout_ms=2000, connect_timeout_ms=500, max_retries=0)
        shutil.copytree(SOURCE / "config/prompts", self.root / "prompts")
        write_json(self.root / "runtime.json", runtime)
        self.config["judge"] = dict(enabled=True, runtime_config="runtime.json", prompt="prompts/task-judge-system.txt", max_model_calls=10)

    def evaluate(self, code=0, extra=(), name="evaluation"):
        write_json(self.root / "evaluation.json", self.config)
        result = self.call("run", self.dataset, "--config", self.root / "evaluation.json",
                           "--output-root", self.root / name, *extra, code=code)
        if result.returncode == 2:
            return None
        if not (self.root / name / "report.json").exists():
            return None
        report = json.loads((self.root / name / "report.json").read_text())
        self.assertTrue(report["complete"])
        for path in (self.root / name).rglob("*"):
            if path.is_file():
                self.assertNotIn(KEY.encode(), path.read_bytes())
                self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertFalse((self.root / name / ".work").exists())
        self.assertEqual(json.loads(result.stdout)["trials"], len(report["trials"]))
        return report

    def test_rule_pass_multiturn_and_files(self):
        report = self.evaluate()
        self.assertEqual(report["summary"]["passed"], 1)
        self.assertEqual(report["trials"][0]["evidence"]["state"]["turns"], 2)
        self.assertEqual(report["summary"]["model_calls"], 0)
        self.assertTrue((self.root / "evaluation/report.pb").is_file())

    def test_component_owns_instrumented_evaluation(self):
        config = json.loads((SOURCE / "config/boundary-service.json").read_text())
        config["runtime_config"] = str(SOURCE / "config/standard-runtime.json")
        config["experience_template_file"] = str(SOURCE / "config/prompts/runtime-experience.txt")
        config["instrumentation"] = {"level": "L0", "capabilities": ["model", "actions", "loop"],
                                     "binding": {"pointer": "", "mode": "text"}}
        write_json(self.root / "instrumentation.json", config)
        self.config["variants"][0].update(
            command=[sys.executable, "-B", str(SOURCE / "tests/instrumented_agent.py"), str(BUILD / "libtrajectory_evolution_local.so")],
            instrumentation_config=str(self.root / "instrumentation.json"))
        self.config.update(timeout_ms=5000, require_trace=True, max_output_bytes=1048576)
        for transport in ("native", "http", "http-async"):
            with self.subTest(transport=transport):
                if transport == "http":
                    self.config["variants"][0]["instrumentation_transport"] = "http"
                    self.config["variants"][0]["command"].append("--http")
                if transport == "http-async":
                    self.config["variants"][0]["command"].append("--dispatch")
                    config["http_persistence"] = {"mode": "buffered"}
                    write_json(self.root / "instrumentation.json", config)
                report = self.evaluate(name="evaluation-" + transport)
                trial = report["trials"][0]
                self.assertEqual(trial["verdict"], "passed")
                self.assertEqual(trial["metrics"]["llm_calls"], 2)
                self.assertEqual(trial["metrics"]["tool_calls"], 1)
                self.assertEqual(trial["metrics"]["tokens"], 84)
                self.assertTrue(trial["evidence"]["trace"]["attributes"]["capture_complete"])
                self.assertIn("agent_version_id", trial["evidence"]["trace"]["events"][0]["attributes"])

    def test_business_failure_is_not_batch_crash(self):
        self.mode("wrong")
        report = self.evaluate(code=1)
        self.assertEqual(report["trials"][0]["execution_status"], "completed")
        self.assertEqual(report["trials"][0]["verdict"], "failed")

    def test_failed_instrumented_runner_preserves_safe_capture(self):
        config = json.loads((SOURCE / "config/boundary-service.json").read_text())
        config["runtime_config"] = str(SOURCE / "config/standard-runtime.json")
        config["experience_template_file"] = str(SOURCE / "config/prompts/runtime-experience.txt")
        config["instrumentation"] = {"level": "L0", "capabilities": ["model", "actions", "loop"],
                                     "binding": {"pointer": "", "mode": "text"}}
        write_json(self.root / "instrumentation.json", config)
        command = [sys.executable, "-B", str(SOURCE / "tests/instrumented_agent.py"),
                   str(BUILD / "libtrajectory_evolution_local.so")]
        self.config["variants"][0].update(instrumentation_config=str(self.root / "instrumentation.json"))
        self.config.update(timeout_ms=5000, require_trace=True, max_output_bytes=1048576)
        for transport in ("native", "http"):
            for mode in ("--exit-after-run", "--exit-open-run", "--exit-before-run", "--corrupt-capture"):
                if transport == "http" and mode == "--corrupt-capture":
                    continue  # The HTTP service, not the child, owns the capture files.
                with self.subTest(transport=transport, mode=mode):
                    flags = ["--exit-after-run", mode] if mode == "--corrupt-capture" else [mode]
                    self.config["variants"][0]["command"] = command + flags + (["--http"] if transport == "http" else [])
                    self.config["variants"][0]["instrumentation_transport"] = transport
                    trial = self.evaluate(code=3, name=transport + mode)["trials"][0]
                    self.assertEqual(trial["error_code"], "runner_nonzero_exit")
                    self.assertEqual(trial["verdict"], "error")
                    if mode == "--exit-after-run":
                        self.assertEqual(trial["metrics"]["llm_calls"], 2)
                        self.assertEqual(trial["metrics"]["tool_calls"], 1)
                        self.assertEqual(trial["metrics"]["tokens"], 84)
                        self.assertIn("trace", trial["evidence"])
                    elif mode == "--exit-open-run":
                        self.assertFalse(trial["evidence"]["failure_capture"]["complete"])
                        self.assertNotIn("metrics", trial)
                        self.assertIn("failure_capture_incomplete", trial["warnings"])
                    else:
                        self.assertNotIn("evidence", trial)
                        self.assertTrue(any(w.startswith("failure_evidence_unavailable") for w in trial["warnings"]))

    def test_variant_pairing_and_failure_isolation(self):
        bad = dict(self.config["variants"][0], id="bad", command=self.config["variants"][0]["command"][:-1] + ["wrong"])
        self.config["variants"].append(bad)
        self.config["repetitions"] = 3
        report = self.evaluate(code=1)
        self.assertEqual(report["summary"]["trials"], 6)
        self.assertEqual(report["comparisons"]["bad"]["losses"], 3)
        self.assertEqual(report["comparisons"]["bad"]["pass_rate_delta"], -1)

    def test_timeout(self):
        self.mode("sleep")
        self.config["timeout_ms"] = 100
        report = self.evaluate(code=1)
        self.assertEqual(report["trials"][0]["error_code"], "runner_timeout")

    def test_environment_error_unscored(self):
        self.mode("environment")
        report = self.evaluate(code=3)
        self.assertIsNone(report["summary"]["pass_rate"])
        self.assertEqual(report["summary"]["coverage"], 0)

    def test_protocol_errors(self):
        for mode in ("wrong_id", "bad_json", "nonzero", "oversize"):
            with self.subTest(mode=mode):
                self.mode(mode)
                self.assertEqual(self.evaluate(code=3, name=mode)["trials"][0]["verdict"], "error")

    def test_credential_rejected_before_persistence(self):
        self.mode("leak")
        report = self.evaluate(code=1)
        self.assertEqual(report["trials"][0]["error_code"], "evaluation_sensitive_output_rejected")
        self.assertNotIn("evidence", report["trials"][0])

    def test_environment_allowlist(self):
        self.mode("env")
        report = self.evaluate()
        self.assertFalse(report["trials"][0]["evidence"]["state"]["unexpected_env"])

    def test_budget_and_unknown_metrics(self):
        self.mode("budget")
        self.evaluate(code=1, name="budget")
        self.mode("no_metrics")
        report = self.evaluate(name="unknown")
        self.assertIn("step_or_tool_budget_unobserved", report["trials"][0]["warnings"])
        self.assertEqual(report["summary"]["host_reported_metrics"], {})

    def test_artifact_is_read_from_disk(self):
        self.mode("artifact")
        self.config["checks"]["*"].append(dict(id="file", path="/artifacts/report.json/json/total", op="equals", value=20))
        report = self.evaluate()
        self.assertEqual(report["trials"][0]["evidence"]["artifacts"]["report.json"]["json"]["total"], 20)

    def test_missing_and_sensitive_artifacts(self):
        for mode in ("missing", "artifact_leak"):
            self.mode(mode)
            self.evaluate(code=1, name=mode)
            self.assertFalse(list((self.root / mode / "artifacts").rglob("*.json")))

    def test_trace_validation_and_identity(self):
        self.config["require_trace"] = True
        self.mode("trace")
        report = self.evaluate(name="valid")
        self.assertIn("trajectory", report["trials"][0]["evidence"])
        for mode in ("good", "trace_wrong_id", "trace_open"):
            self.mode(mode)
            self.evaluate(code=3, name=mode)

    def test_execution_status_does_not_replace_business_acceptance(self):
        self.mode("failed")
        report = self.evaluate()
        self.assertEqual(report["trials"][0]["execution_status"], "failed")
        self.assertEqual(report["trials"][0]["verdict"], "passed")

    def test_actual_completion_order_does_not_change_identity(self):
        cases = []
        for i, delay in enumerate((.15, .01, .06)):
            cases.append(dict(task_id=f"t{i}", group_id=f"g{i}", task_input=f"测试任务{i}", expected_behavior="值为20",
                              difficulty="easy", category="order", initial_state={"value": 20, "delay": delay},
                              split="train", review_status="reviewed"))
        self.make_dataset(cases)
        self.mode("order")
        self.config["parallelism"] = 3
        self.config["repetitions"] = 2
        report = self.evaluate()
        self.assertEqual(len({x["trial_id"] for x in report["trials"]}), 6)
        for trial in report["trials"]:
            self.assertEqual(trial["task_id"], trial["evidence"]["state"]["task_id"])

    def test_symlink_artifact_cannot_read_outside_workspace(self):
        target = self.root / "private.json"
        write_json(target, {"total": KEY})
        self.mode("symlink")
        self.config["variants"][0]["command"].append(str(target))
        report = self.evaluate(code=1)
        self.assertEqual(report["trials"][0]["error_code"], "evaluation_artifact_symlink")

    def test_descendants_are_stopped_at_timeout(self):
        pid_file = self.root / "child.pid"
        self.mode("child")
        self.config["variants"][0]["command"].append(str(pid_file))
        self.config["timeout_ms"] = 200
        self.evaluate(code=1)
        pid = int(pid_file.read_text())
        stat = Path(f"/proc/{pid}/stat")
        for _ in range(100):
            if not stat.exists() or stat.read_text().split()[2] == "Z":
                break
            time.sleep(.01)
        else:
            self.fail("descendant process survived evaluation timeout")

    def test_trial_limit_before_start(self):
        self.config.update(repetitions=2, max_trials=1)
        self.evaluate(code=2)
        self.assertFalse((self.root / "evaluation").exists())

    def test_review_gate_and_explicit_override(self):
        self.make_dataset(reviewed=False)
        self.evaluate(code=2, name="blocked")
        self.assertFalse((self.root / "blocked").exists())
        report = self.evaluate(extra=("--allow-unreviewed",))
        self.assertTrue(report["configuration"]["allow_unreviewed"])

    def test_invalid_grader_and_limits_before_run(self):
        self.config["checks"] = {}
        self.evaluate(code=2, name="no-grader")
        self.assertFalse((self.root / "no-grader").exists())
        self.config["checks"] = {"*": [dict(id="x", path="/bad~3path", op="exists")]}
        self.evaluate(code=2, name="bad-pointer")

    def test_model_pass_and_prompt_separation(self):
        self.model()
        report = self.evaluate()
        self.assertEqual(report["summary"]["model_calls"], 1)
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        self.assertIn("expected_behavior", packet)
        self.assertEqual(REQUESTS[-1]["thinking"]["type"], "disabled")
        self.assertNotIn(KEY, json.dumps(packet))

    def test_model_cannot_override_hard_failure(self):
        self.model()
        self.mode("wrong")
        self.evaluate(code=1)
        self.assertEqual(len(REQUESTS), self.start_requests)

    def test_enclosing_json_fence_and_bounded_semantic_repair(self):
        self.model()
        Provider.behavior = "fence"
        self.evaluate(name="fence")
        Provider.behavior = "repair"
        report = self.evaluate(name="repaired")
        self.assertEqual(report["summary"]["model_calls"], 2)
        self.assertEqual(report["trials"][0]["checks"][-1]["attempts"], 2)
        self.assertEqual(len(report["trials"]), 1)

    def test_semantic_repair_can_be_disabled(self):
        self.model()
        Provider.behavior = "repair"
        self.config["judge"]["max_repair_attempts"] = 0
        report = self.evaluate(code=3)
        self.assertEqual(report["summary"]["model_calls"], 1)

    def test_model_uncertain_error_and_forged_reference(self):
        self.model()
        for mode in ("uncertain", "reference", "leak", "length", "http_error"):
            Provider.behavior = mode
            report = self.evaluate(code=3, name=mode)
            self.assertNotEqual(report["trials"][0]["verdict"], "passed")

    def test_model_budget_does_not_fake_success(self):
        self.model()
        self.config["repetitions"] = 2
        self.config["judge"]["max_model_calls"] = 1
        report = self.evaluate(code=3)
        self.assertEqual(report["summary"]["passed"], 1)
        self.assertEqual(report["summary"]["errors"], 1)

    def test_no_overwrite_before_execution_or_model(self):
        self.evaluate()
        self.model()
        self.evaluate(code=2)
        self.assertEqual(len(REQUESTS), self.start_requests)

    def test_tampered_dataset_refused(self):
        with (self.dataset / "train.jsonl").open("a") as stream:
            stream.write("\n")
        self.evaluate(code=2)

    def test_cancellation_records_trials(self):
        self.mode("sleep")
        self.config["repetitions"] = 3
        self.config["timeout_ms"] = 5000
        write_json(self.root / "evaluation.json", self.config)
        process = subprocess.Popen([str(CLI), "dataset", "run", str(self.dataset), "--config", str(self.root / "evaluation.json"),
                                    "--output-root", str(self.root / "cancel")], env=self.env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        for _ in range(200):
            if (self.root / "cancel/started.json").exists():
                break
            time.sleep(.01)
        process.send_signal(signal.SIGINT)
        stdout, stderr = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 3, stdout + stderr)
        report = json.loads((self.root / "cancel/report.json").read_text())
        self.assertEqual(len(report["trials"]), 3)
        self.assertEqual(report["summary"]["errors"], 3)


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(TrialTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    write_json(WORK / "test-summary.json", dict(tests=result.testsRun, failures=len(result.failures), errors=len(result.errors),
                                               fake_model_requests=len(REQUESTS), paid_model_requests=0))
    SERVER.shutdown()
    print(f"Test output: {WORK}")
    sys.exit(not result.wasSuccessful())
