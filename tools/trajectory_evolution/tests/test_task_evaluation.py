"""Offline trial execution, deterministic grading and model-boundary regression."""

import http.server
import hashlib
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
        if Provider.behavior.startswith("loops"):
            packet = json.loads(body["messages"][1]["content"])
            inventory = packet["loop_inventory"]
            wasted = []
            if inventory:
                key = next(iter(inventory))
                wasted.append(dict(iteration_id=key, reason="已有结果且没有新任务，本轮为重复执行。",
                    evidence_event_ids=[inventory[key]["event_ids"][0]]))
            review = dict(status="complete" if inventory else "uncertain", wasted_iterations=wasted)
            if Provider.behavior == "loops_duplicate":
                wasted.append(wasted[0])
            if Provider.behavior == "loops_unknown":
                wasted[0]["iteration_id"] = "not-an-iteration"
            if Provider.behavior == "loops_repair" and "validation_feedback" not in packet:
                wasted[0]["iteration_id"] = "not-an-iteration"
            if Provider.behavior == "loops_other_event":
                other = next(k for k in inventory if k != key)
                wasted[0]["evidence_event_ids"] = [inventory[other]["event_ids"][0]]
            if Provider.behavior in ("loops_prior_event", "loops_future_event", "loops_anchor_repair"):
                events = packet["evidence"]["trace"]["events"]
                order = {e["event_id"]: int(e["timestamp_us"]) for e in events}
                rounds = sorted(inventory, key=lambda k: min(order[e] for e in inventory[k]["event_ids"]))
                current, other = (rounds[-1], rounds[0]) if Provider.behavior != "loops_future_event" else (rounds[0], rounds[-1])
                wasted[0].update(iteration_id=current, evidence_event_ids=[
                    inventory[current]["event_ids"][0], inventory[other]["event_ids"][0]])
                if Provider.behavior == "loops_anchor_repair":
                    wasted[0]["evidence_event_ids"].pop(0)
                    if "validation_feedback" in packet:
                        reference = packet["validation_feedback"]["loop_review_error"]["reference"]
                        wasted[0]["evidence_event_ids"] = [reference["allowed_iteration_event_ids"][0]]
            if Provider.behavior.startswith("loops_join_"):
                current = next(k for k, v in inventory.items()
                               if v["execution_id"] == "left" and str(v["iteration"]) == "1")
                cited = {"loops_join_child": "right-2-action.end", "loops_join_without_anchor": "right-2-action.end",
                         "loops_join_unrelated": "right-1-action.end", "loops_join_future": "left-2-action.end"}
                refs = [cited[Provider.behavior]]
                if Provider.behavior != "loops_join_without_anchor":
                    refs.insert(0, inventory[current]["event_ids"][0])
                wasted[0].update(iteration_id=current, evidence_event_ids=refs)
            if Provider.behavior == "loops_uncertain":
                review = dict(status="uncertain", wasted_iterations=[])
            if Provider.behavior.startswith("loops_detailed"):
                reviews = [dict(iteration_id=k, reason="该轮首次交付结果，必须保留。", decision="keep",
                                evidence_event_ids=[v["event_ids"][0]]) for k, v in inventory.items()]
                reviews[0].update(reason="此前已完成同一操作，本轮重复执行且无其他必要工作。", decision="waste")
                review = dict(status="complete", iteration_reviews=reviews)
                if Provider.behavior == "loops_detailed_missing":
                    reviews.pop()
                if Provider.behavior == "loops_detailed_duplicate":
                    reviews[1] = dict(reviews[0])
                if Provider.behavior == "loops_detailed_unknown":
                    reviews[1]["iteration_id"] = "not-an-iteration"
                if Provider.behavior == "loops_detailed_invalid_decision":
                    reviews[1]["decision"] = "maybe"
                if Provider.behavior in ("loops_detailed_uncertain", "loops_detailed_uncertain_complete"):
                    reviews[1]["decision"] = "uncertain"
                if Provider.behavior == "loops_detailed_uncertain":
                    review["status"] = "uncertain"
                if Provider.behavior == "loops_detailed_both":
                    review["wasted_iterations"] = wasted
            judgment.update(verdict="passed", loop_review=review)
        if Provider.behavior == "reference":
            judgment.update(verdict="passed", evidence_paths=["/not-present"])
        if Provider.behavior in ("path_repair", "syntax_repair"):
            packet = json.loads(body["messages"][1]["content"])
            path = "/state/missing" if Provider.behavior == "path_repair" else "/state/~2bad"
            judgment.update(verdict="passed", evidence_paths=["/state"] if "validation_feedback" in packet else ["/output", path])
        references = {"child_reference": "/state/value", "array_reference": "/trace/events/0/kind",
                      "missing_child_reference": "/state/missing", "prefix_reference": "/stateful/value",
                      "encoded_root_reference": "/state~1value", "packet_reference": "/evidence_text_refs/0",
                      "missing_array_reference": "/trace/events/999/kind"}
        if Provider.behavior in references:
            judgment.update(verdict="passed", evidence_paths=[references[Provider.behavior]])
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

    def test_report_budget_covers_full_report_not_half_the_trial_evidence(self):
        host = self.config["variants"][0]["command"]
        self.config["variants"][0]["command"] = [sys.executable, "-c",
            "import json, subprocess, sys\n"
            f"response=json.loads(subprocess.run({host!r},input=sys.stdin.read(),capture_output=True,text=True,check=True).stdout)\n"
            "response['state']['details']='verified payload '*1400\nprint(json.dumps(response))\n"]
        self.config["max_report_bytes"] = 40000
        report = self.evaluate()
        trial_bytes = len(json.dumps(report["trials"][0], ensure_ascii=False,
                                     separators=(",", ":")).encode())
        report_bytes = (self.root / "evaluation/report.json").stat().st_size
        self.assertGreater(trial_bytes, self.config["max_report_bytes"] // 2)
        self.assertLessEqual(report_bytes, self.config["max_report_bytes"])

        # Trial evidence fits, but summary and metadata must also fit the limit.
        self.config["max_report_bytes"] = trial_bytes + 1024
        self.assertLess(self.config["max_report_bytes"], report_bytes)
        self.evaluate(code=2, name="limited")
        self.assertTrue(list((self.root / "limited/trials").glob("*.json")))
        self.assertFalse((self.root / "limited/report.json").exists())

    def counted_actions(self, skipped=False):
        host = self.config["variants"][0]["command"][:-1] + ["loop_trace"]
        wrapper = (
            "import json, subprocess, sys\n"
            f"response=json.loads(subprocess.run({host!r},input=sys.stdin.read(),capture_output=True,text=True,check=True).stdout)\n"
            "for event in response['trace']['events']:\n"
            " if event['kind']=='action.end':\n"
            "  event.setdefault('attributes', {})['executed']=True\n"
            "  if event['execution_id']=='left' and event['iteration']==1:\n"
            "   event['status']='failed'\n"
            f"   event['attributes']['executed']={not skipped!r}\n"
            "print(json.dumps(response))\n"
        )
        self.config["variants"][0]["command"] = [sys.executable, "-c", wrapper]

    def test_action_count_covers_parallel_failures_and_model_cannot_override(self):
        self.model(); self.counted_actions()
        self.config["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
        Provider.behavior = "loops_detailed"
        self.config["checks"]["*"].append(dict(id="calls", op="action_count", operation="calculate", max=3))
        report = self.evaluate(code=1)
        trial = report["trials"][0]
        count = next(c for c in trial["checks"] if c["check_id"] == "calls")
        self.assertEqual(count["status"], "failed")
        self.assertIn("calculate 4 次", count["reason"])
        self.assertEqual(trial["checks"][-1]["check_id"], "model")
        self.assertEqual(trial["checks"][-1]["status"], "passed")
        self.assertEqual(trial["evidence"]["loop_review"]["status"], "complete")
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        self.assertIn(dict(op="action_count", operation="calculate", max=3), packet["required_checks"])
        self.assertTrue(all("id" not in check for check in packet["required_checks"]))

    def test_judge_receives_observed_action_counts_without_inventing_observations(self):
        self.model()
        host = self.config["variants"][0]["command"].copy()
        for name, skipped, expected in (("all", False, 4), ("skipped", True, 3),
                                        ("unknown", None, None)):
            with self.subTest(name=name):
                self.config["variants"][0]["command"] = host.copy()
                if skipped is None:
                    self.mode("loop_trace")
                else:
                    self.counted_actions(skipped=skipped)
                report = self.evaluate(name=name)
                packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
                self.assertEqual(packet["observed_action_counts"], {"calculate": expected})
                self.assertNotIn("observed_action_counts", report["trials"][0]["evidence"])
        self.config["variants"][0]["command"] = host
        self.evaluate(name="no-trace")
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        self.assertNotIn("observed_action_counts", packet)

    def test_judge_receives_only_current_task_and_global_check_constraints(self):
        self.make_dataset(cases=[dict(task_id=name, group_id=name, task_input=f"计算{name}任务的总量20。",
            expected_behavior="总量为20。", initial_state=dict(value=20), difficulty="easy", category="data",
            split="train", review_status="reviewed") for name in ("one", "unrelated")])
        self.config["task_ids"] = ["one"]
        self.model(); self.counted_actions()
        self.config["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
        Provider.behavior = "loops_detailed"
        self.config["checks"].update(one=[dict(id="calls", op="action_count", operation="calculate", max=4)],
            unrelated=[dict(id="not_this_task", op="action_count", operation="unrelated", max=0)])
        report = self.evaluate()
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        self.assertIn(dict(op="action_count", operation="calculate", max=4), packet["required_checks"])
        self.assertIn(dict(path="/state/value", op="equals", value=20), packet["required_checks"])
        self.assertFalse(any(c.get("operation") == "unrelated" for c in packet["required_checks"]))
        budgets = {c["path"] for c in packet["required_checks"] if c.get("op") == "at_most"}
        self.assertEqual(budgets, {"/metrics/steps", "/metrics/tool_calls"})
        self.assertNotIn("required_checks", report["trials"][0]["evidence"])

    def test_action_count_filters_status_and_excludes_unexecuted_calls(self):
        self.counted_actions(skipped=True)
        self.config["checks"]["*"].extend([
            dict(id="calls", op="action_count", operation="calculate", min=3, max=3),
            dict(id="failed", op="action_count", operation="calculate", status="failed", max=0),
            dict(id="other", op="action_count", operation="another-operation", max=0)])
        self.evaluate()

    def test_action_count_unknown_observation_is_error_not_zero(self):
        self.config["checks"]["*"].append(dict(id="calls", op="action_count", operation="calculate", max=100))
        for mode in ("good", "loop_trace"):
            self.mode(mode)
            report = self.evaluate(code=3, name=mode)
            self.assertEqual(report["trials"][0]["verdict"], "error")
            self.assertEqual(report["trials"][0]["error_code"], "evaluation_check_evidence_invalid")

    def test_recheck_adds_rules_without_reexecuting_or_replacing_source(self):
        self.model(); self.counted_actions()
        first = self.evaluate()
        source = self.root / "evaluation/report.json"
        original = source.read_bytes()
        self.config["variants"][0]["command"] = ["/bin/false"]
        self.config["checks"]["*"].append(dict(id="calls", op="action_count", operation="calculate", max=1))
        model_only = self.evaluate(name="model-only", extra=("--regrade-report", source))
        self.assertEqual(model_only["trials"][0]["verdict"], "passed")
        report = self.evaluate(code=1, name="rechecked", extra=("--regrade-report", source, "--recheck"))
        trial = report["trials"][0]
        self.assertEqual(trial["verdict"], "failed")
        self.assertEqual(trial["trial_id"], first["trials"][0]["trial_id"])
        self.assertEqual(trial["duration_ms"], first["trials"][0]["duration_ms"])
        self.assertEqual(trial["evidence"]["trace"], first["trials"][0]["evidence"]["trace"])
        self.assertEqual(trial["checks"][-1]["status"], "passed")
        self.assertTrue(report["configuration"]["rechecked_configured_checks"])
        self.assertFalse(report["configuration"]["model_only_regrade"])
        self.assertNotEqual(report["config_fingerprint"], model_only["config_fingerprint"])
        self.assertEqual(source.read_bytes(), original)

    def test_recheck_replaces_matching_rule_and_preserves_artifact_failure(self):
        self.model(); self.mode("missing")
        self.config["checks"]["*"][0]["value"] = -1
        self.evaluate(code=1)
        self.config["checks"]["*"][0]["value"] = 20
        report = self.evaluate(code=1, name="rechecked", extra=("--regrade-report", self.root / "evaluation/report.json", "--recheck"))
        checks = {c["check_id"]: c["status"] for c in report["trials"][0]["checks"]}
        self.assertEqual(checks["artifact"], "failed")
        self.assertEqual(checks["value"], "passed")
        self.assertEqual(checks["model"], "passed")

    def test_action_count_configuration_is_checked_before_execution(self):
        for index, spec in enumerate([
            dict(operation="calculate"), dict(operation="calculate", min=3, max=1),
            dict(operation="", max=1), dict(operation="calculate", max=-1),
            dict(operation="calculate", max=1.5), dict(operation="calculate", max=1, status="invented"),
            dict(operation="calculate", max=1, path="/output")]):
            self.config["checks"] = {"*": [dict(id="calls", op="action_count", **spec)]}
            self.assertIsNone(self.evaluate(code=2, name="invalid-count-" + str(index)))
        self.config["checks"] = {"*": [dict(id="artifact", op="exists", path="/output")]}
        self.assertIsNone(self.evaluate(code=2, name="reserved-id"))
        self.config["checks"] = {"*": [dict(id="output", op="exists", path="/output")]}
        self.assertIsNone(self.evaluate(code=2, name="missing-regrade", extra=("--recheck",)))

    def test_internal_contract_check_id_is_reserved_before_execution(self):
        self.config["checks"] = {"*": [dict(id="contract.schedule_success", op="exists", path="/output")]}
        self.assertIsNone(self.evaluate(code=2, name="reserved-contract-id"))
        self.assertFalse((self.root / "reserved-contract-id").exists())

    def test_cli_resume_reuses_execution_and_regrades_only_judge_errors(self):
        self.model()
        host = self.config["variants"][0]["command"][:-1] + ["trace"]
        counter = self.root / "host-count"
        wrapper = (
            "import pathlib, subprocess, sys\n"
            f"counter=pathlib.Path({str(counter)!r})\n"
            "counter.write_text(str(int(counter.read_text())+1) if counter.exists() else '1')\n"
            f"subprocess.run({host!r},input=sys.stdin.read(),text=True,check=True)\n"
        )
        self.config["variants"][0]["command"] = [sys.executable, "-c", wrapper]
        Provider.behavior = "http_error"
        first = self.evaluate(code=3)
        source = self.root / "evaluation/report.json"
        original = source.read_bytes()
        self.assertEqual(first["trials"][0]["error_code"], "evaluation_judge_failed")
        Provider.behavior = "passed"
        second = self.evaluate(name="resumed", extra=("--resume-report", source))
        before, after = first["trials"][0], second["trials"][0]
        self.assertEqual(after["verdict"], "passed")
        for key in ("trial_id", "duration_ms", "execution_status", "metrics"):
            self.assertEqual(before.get(key), after.get(key))
        self.assertEqual(before["evidence"]["trace"], after["evidence"]["trace"])
        self.assertEqual(second["summary"]["regraded_trials"], 1)
        self.assertEqual(second["summary"]["model_calls"], 1)
        third = self.evaluate(name="resumed-again", extra=("--resume-report", self.root / "resumed/report.json"))
        self.assertEqual(third["summary"]["reused_trials"], 1)
        self.assertEqual(third["summary"]["model_calls"], 0)
        self.assertEqual(counter.read_text(), "1")
        self.assertEqual(source.read_bytes(), original)
        self.evaluate(code=2, name="conflict", extra=("--resume-report", source, "--regrade-report", source))
        self.config["variants"][0]["version"] = "changed"
        self.evaluate(code=2, name="changed", extra=("--resume-report", source))
        self.assertEqual(counter.read_text(), "1")

    def test_model_only_regrade_preserves_execution_and_source(self):
        self.model()
        self.mode("trace")
        first = self.evaluate()
        source = self.root / "evaluation/report.json"
        original = source.read_bytes()
        prompt = self.root / "prompts/task-judge-system.txt"
        prompt.write_text(prompt.read_text() + "\n重新核对实际证据。\n")
        self.mode("nonzero")  # Any accidental host execution would fail.
        Provider.behavior = "failed"
        second = self.evaluate(code=1, name="regraded", extra=("--regrade-report", source))
        before, after = first["trials"][0], second["trials"][0]
        self.assertEqual(after["verdict"], "failed")
        for key in ("trial_id", "duration_ms", "execution_status", "metrics"):
            self.assertEqual(before.get(key), after.get(key))
        self.assertEqual(before["evidence"]["trace"], after["evidence"]["trace"])
        self.assertEqual(second["summary"]["model_calls"], 1)
        self.assertEqual(second["summary"]["regraded_trials"], 1)
        self.assertEqual(second["summary"]["reused_trials"], 0)
        self.assertFalse(second["summary"]["agent_reexecuted"])
        self.assertNotEqual(first["config_fingerprint"], second["config_fingerprint"])
        self.assertNotEqual(first["run_id"], second["run_id"])
        provenance = second["configuration"]["execution_source"]
        self.assertEqual(provenance["report_sha256"], hashlib.sha256(original).hexdigest())
        self.assertEqual(provenance["config_fingerprint"], first["config_fingerprint"])
        self.assertEqual(source.read_bytes(), original)

    def test_model_only_regrade_explicit_subset_of_paused_batch(self):
        self.config["parallelism"] = 1
        self.make_dataset(cases=[dict(task_id=name, group_id=name, task_input=f"计算任务{name}的总量20。",
            expected_behavior="总量为20。", initial_state=dict(value=20), difficulty="easy", category="data",
            split="train", review_status="reviewed") for name in ("a", "b", "c")])
        self.model(); self.mode("trace")
        host = self.config["variants"][0]["command"].copy()
        entered = self.root / "second-task-entered"
        wrapper = (
            "import json, pathlib, subprocess, sys, time\n"
            "request=sys.stdin.read()\n"
            "if json.loads(request)['task']['task_id']=='b':\n"
            f" pathlib.Path({str(entered)!r}).touch()\n"
            " time.sleep(30)\n"
            f"subprocess.run({host!r},input=request,text=True,check=True)\n"
        )
        self.config["variants"][0]["command"] = [sys.executable, "-c", wrapper]
        write_json(self.root / "evaluation.json", self.config)
        process = subprocess.Popen([str(CLI), "dataset", "run", str(self.dataset),
            "--config", str(self.root / "evaluation.json"), "--output-root", str(self.root / "evaluation")],
            env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 5
            while not entered.exists() and process.poll() is None and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertTrue(entered.exists())
            process.send_signal(signal.SIGINT)
            stdout, stderr = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 3, stdout + stderr)
        finally:
            if process.poll() is None:
                process.terminate(); process.communicate(timeout=10)
        first = json.loads((self.root / "evaluation/report.json").read_text())
        self.assertFalse(first["complete"])
        self.assertEqual(first["trials"][0]["task_id"], "a")
        self.assertEqual(first["trials"][0]["execution_status"], "completed")
        self.assertEqual(first["trials"][1]["execution_status"], "cancelled")
        source = self.root / "evaluation/report.json"
        original = source.read_bytes()
        self.config["variants"][0]["command"] = host[:-1] + ["nonzero"]
        count = len(REQUESTS)
        self.assertIsNone(self.evaluate(code=2, name="unselected", extra=("--regrade-report", source)))
        self.config["task_ids"] = ["b"]
        self.assertIsNone(self.evaluate(code=2, name="unfinished", extra=("--regrade-report", source)))
        self.config["task_ids"] = ["c"]
        self.assertIsNone(self.evaluate(code=2, name="absent", extra=("--regrade-report", source)))
        self.assertEqual(len(REQUESTS), count)
        self.config["task_ids"] = ["a"]
        second = self.evaluate(name="subset", extra=("--regrade-report", source))
        self.assertTrue(second["complete"])
        self.assertEqual(second["summary"]["planned_trials"], 1)
        self.assertEqual(second["summary"]["regraded_trials"], 1)
        self.assertFalse(second["summary"]["agent_reexecuted"])
        before, after = first["trials"][0], second["trials"][0]
        for key in ("trial_id", "duration_ms", "execution_status", "evidence"):
            self.assertEqual(before[key], after[key])
        self.assertEqual(source.read_bytes(), original)

    def test_model_only_regrade_keeps_hard_checks_and_verifies_artifacts(self):
        self.model()
        self.mode("artifact")
        self.config["checks"]["*"][0]["value"] = -1
        first = self.evaluate(code=1)
        source = self.root / "evaluation/report.json"
        self.config["checks"]["*"][0]["value"] = 20
        self.mode("nonzero")
        second = self.evaluate(code=1, name="regraded", extra=("--regrade-report", source))
        before, after = first["trials"][0], second["trials"][0]
        self.assertEqual([c for c in before["checks"] if c["check_id"] != "model"],
                         [c for c in after["checks"] if c["check_id"] != "model"])
        self.assertEqual(after["verdict"], "failed")
        name = before["evidence"]["artifacts"]["report.json"]["stored_file"]
        relative = Path("artifacts") / before["trial_id"] / name
        self.assertEqual((self.root / "evaluation" / relative).read_bytes(),
                         (self.root / "regraded" / relative).read_bytes())
        (self.root / "evaluation" / relative).write_text("changed artifact")
        count = len(REQUESTS)
        self.assertIsNone(self.evaluate(code=2, name="changed-artifact", extra=("--regrade-report", source)))
        self.assertEqual(len(REQUESTS), count)

    def test_model_only_regrade_rejects_changed_context_and_source(self):
        self.model()
        self.evaluate()
        source = self.root / "evaluation/report.json"
        count = len(REQUESTS)
        self.config["variants"][0]["version"] = "different-agent-version"
        self.assertIsNone(self.evaluate(code=2, name="changed-version", extra=("--regrade-report", source)))
        self.config["variants"][0]["version"] = "test-v1"
        self.config["judge"]["enabled"] = False
        self.assertIsNone(self.evaluate(code=2, name="disabled-judge", extra=("--regrade-report", source)))
        self.config["judge"]["enabled"] = True
        source.write_bytes(source.read_bytes() + b" ")
        self.assertIsNone(self.evaluate(code=2, name="changed-report", extra=("--regrade-report", source)))
        self.assertEqual(len(REQUESTS), count)

    def test_model_only_regrade_budget_does_not_reuse_old_passes(self):
        self.model()
        self.config.update(repetitions=3, parallelism=1)
        self.evaluate()
        self.mode("nonzero")
        self.config["judge"]["max_model_calls"] = 1
        self.config["judge"]["max_repair_attempts"] = 0
        second = self.evaluate(code=3, name="regraded", extra=("--regrade-report", self.root / "evaluation/report.json"))
        self.assertEqual(second["summary"]["model_calls"], 1)
        self.assertEqual(second["summary"]["passed"], 1)
        self.assertEqual(second["summary"]["errors"], 2)
        self.assertEqual(second["summary"]["regraded_trials"], 3)

    def test_model_only_regrade_cancellation_does_not_mix_old_grades(self):
        self.model()
        self.config.update(repetitions=3, parallelism=1)
        self.evaluate()
        entered, release = threading.Event(), threading.Event()

        class PausedProvider(Provider):
            def do_POST(self):
                entered.set()
                release.wait(5)
                try:
                    super().do_POST()
                except (BrokenPipeError, ConnectionResetError):
                    pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), PausedProvider)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        process = None
        try:
            runtime_path = self.root / "runtime.json"
            runtime = json.loads(runtime_path.read_text())
            runtime["model"]["endpoint"] = f"http://127.0.0.1:{server.server_port}/chat/completions"
            write_json(runtime_path, runtime)
            self.mode("nonzero")
            write_json(self.root / "evaluation.json", self.config)
            process = subprocess.Popen([str(CLI), "dataset", "run", str(self.dataset),
                "--config", str(self.root / "evaluation.json"), "--output-root", str(self.root / "cancelled"),
                "--regrade-report", str(self.root / "evaluation/report.json")], env=self.env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.assertTrue(entered.wait(5))
            process.terminate()
            stdout, stderr = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 3, stdout + stderr)
            report = json.loads((self.root / "cancelled/report.json").read_text())
            self.assertFalse(report["complete"])
            self.assertEqual(report["summary"]["not_started_trials"], 2)
            self.assertEqual(report["summary"]["reused_trials"], 0)
            self.assertEqual(len(report["trials"]), 1)
            self.assertNotEqual(report["trials"][0]["verdict"], "passed")
        finally:
            release.set()
            if process is not None and process.poll() is None:
                process.terminate(); process.communicate(timeout=10)
            server.shutdown(); server.server_close(); thread.join()

    def test_execution_view_omits_only_typed_model_requests_not_captured_evidence(self):
        self.model()
        host = self.config["variants"][0]["command"][:-1] + ["loop_trace"]
        wrapper = (
            "import json, subprocess, sys\n"
            f"response=json.loads(subprocess.run({host!r},input=sys.stdin.read(),capture_output=True,text=True,check=True).stdout)\n"
            "for event in response['trace']['events']:\n"
            " if event.get('execution_id') == 'left':\n"
            "  event.setdefault('attributes', {})['semantic']='model'\n"
            "  if 'input' in event: event['input']={'messages':[{'role':'system','content':'request-only-data' * 2000}], 'tools':[{'name':'placeholder'}]}\n"
            " elif 'input' in event: event['input']={'messages':'business-field-must-stay','value':20}\n"
            "events=response['trace']['events']; terminal=events[-1]\n"
            "events.insert(-1, dict(event_id='handoff-start', kind='step.start', producer_id='coordinator', "
            "actor_id='coordinator', execution_id='coordinator', step_id='handoff', sequence=1, "
            "timestamp_us='100', caused_by=['right-2-step.end'], "
            "input={'role':'tool','tool_call_id':'dispatch-1','content':'cross-boundary-result'}))\n"
            "events.insert(-1, dict(event_id='handoff-end', kind='step.end', producer_id='coordinator', "
            "actor_id='coordinator', execution_id='coordinator', step_id='handoff', sequence=2, "
            "timestamp_us='101', caused_by=['handoff-start'], status='completed', "
            "output={'content':'cross-boundary-result'}))\n"
            "terminal['caused_by'].append('handoff-end')\n"
            "print(json.dumps(response))\n"
        )
        self.config["variants"][0]["command"] = [sys.executable, "-c", wrapper]
        self.config["max_output_bytes"] = 1048576
        self.evaluate()
        full_bytes = len(REQUESTS[-1]["messages"][1]["content"].encode())
        self.config["judge"]["include_model_requests"] = False
        report = self.evaluate(name="execution-view")
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        self.assertEqual(packet["evidence_view"], "model_requests_omitted")
        self.assertLess(len(REQUESTS[-1]["messages"][1]["content"].encode()), full_bytes / 2)
        original = report["trials"][0]["evidence"]["trace"]["events"]
        compact = packet["evidence"]["trace"]["events"]
        self.assertTrue(any(e.get("kind") == "action.end" for e in compact))
        self.assertFalse(any(e.get("kind") == "run.start" for e in compact))
        action_steps = {e.get("step_id") for e in compact if e.get("kind") in ("action.start", "action.end")}
        self.assertFalse(any(e.get("kind") in ("step.start", "step.end") and
                             e.get("step_id") in action_steps for e in compact))
        self.assertFalse(any(e.get("kind") == "action.start" and
                             e.get("attributes", {}).get("semantic") == "model" for e in compact))
        self.assertFalse(any('request-only-data' in json.dumps(e.get('input')) for e in compact))
        self.assertTrue(any('business-field-must-stay' in json.dumps(e.get('input')) for e in compact))
        self.assertTrue(any(e.get('input', {}).get('content') == 'cross-boundary-result'
                            for e in compact if isinstance(e.get('input'), dict)))
        self.assertTrue(any(e.get("event_id") for e in compact))
        self.assertTrue(any(e.get("caused_by") for e in compact))
        self.assertTrue(any('request-only-data' in json.dumps(e.get('input')) for e in original))
        self.config["judge"]["include_model_requests"] = "false"
        count = len(REQUESTS)
        self.assertIsNone(self.evaluate(code=2, name="invalid-view"))
        self.assertEqual(len(REQUESTS), count)

    def test_execution_view_omits_retrieval_payload_but_keeps_decisions(self):
        self.model()
        host = self.config["variants"][0]["command"][:-1] + ["loop_trace"]
        wrapper = (
            "import json, subprocess, sys\n"
            f"response=json.loads(subprocess.run({host!r},input=sys.stdin.read(),capture_output=True,text=True,check=True).stdout)\n"
            "for index, kind in enumerate(('policy.query', 'application.query'), 1):\n"
            " response['trace']['events'].insert(-1, dict(event_id='query-'+str(index), kind='custom', "
            "actor_id='agent', producer_id='diagnostic', sequence=index, timestamp_us='1', "
            "attributes=dict(type=kind, decision=dict(query='retrieval-only-'*1000, context='context-only-'*1000, "
            "experiences=['experience-only-'*1000], policy_id='policy-diagnostic', status='ready', "
            "actions=[dict(decision='deny', reason='already-completed')], progress=dict(decision='stop')))))\n"
            "print(json.dumps(response))\n"
        )
        self.config["variants"][0]["command"] = [sys.executable, "-c", wrapper]
        self.config["max_output_bytes"] = 1048576
        self.config["judge"]["share_repeated_text"] = False
        self.evaluate()
        full = json.loads(REQUESTS[-1]["messages"][1]["content"])
        self.config["judge"]["include_model_requests"] = False
        report = self.evaluate(name="retrieval-view")
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        raw = {e["event_id"]: e for e in report["trials"][0]["evidence"]["trace"]["events"]}
        compact = {e["event_id"]: e for e in packet["evidence"]["trace"]["events"]}
        self.assertNotIn("query-1", compact)
        self.assertEqual(compact["query-2"]["attributes"], raw["query-2"]["attributes"])
        self.assertIn("query", raw["query-1"]["attributes"]["decision"])
        self.assertTrue(any(e.get("attributes", {}).get("decision", {}).get("query")
                            for e in full["evidence"]["trace"]["events"]))
        self.assertLess(len(json.dumps(packet)), len(json.dumps(full)) - 40000)

    def entry_mode(self, *flags):
        for key in ("run_layout", "instrumentation_transport"):
            self.config["variants"][0].pop(key, None)
        config = json.loads((SOURCE / "config/boundary-service.json").read_text())
        config.update(runtime_config=str(SOURCE / "config/standard-runtime.json"),
                      experience_template_file=str(SOURCE / "config/prompts/runtime-experience.txt"))
        config["instrumentation"] = {"level": "L0", "capabilities": ["model", "actions", "loop"],
                                     "binding": {"pointer": "", "mode": "text"}}
        config["http_persistence"] = {"mode": "buffered"}
        write_json(self.root / "instrumentation.json", config)
        self.config["variants"][0].update(
            command=[sys.executable, "-B", str(SOURCE / "tests/entry_agent.py"), *map(str, flags)],
            instrumentation_config=str(self.root / "instrumentation.json"),
            entry={"input": {k: {"$eval": v} for k, v in {
                "text": "input", "user": "user_id", "conversation": "conversation_id", "key": "run_key",
                "turn": "turn", "initial": "initial_state", "history": "messages"}.items()}, "output_pointer": "/answer"})
        self.config.update(timeout_ms=5000, require_trace=True, max_output_bytes=1048576)

    def test_entry_receivers_no_host_protocol(self):
        for label, flags in (("sync", ()), ("async", ("--async",)),
                             ("dispatch", ("--dispatch",)), ("persistent", ("--stay-alive",)),
                             ("cabi", ("--cabi", BUILD / "libtrajectory_evolution_local.so")),
                             ("native-points", ("--native", BUILD / "libtrajectory_evolution_local.so")),
                             ("business", ("--business-failure",))):
            with self.subTest(label=label):
                self.entry_mode(*flags)
                report = self.evaluate(name="entry-" + label)
                trial = report["trials"][0]
                self.assertEqual(trial["verdict"], "passed", trial)
                self.assertEqual(len(trial["evidence"]["traces"]), 2)
                self.assertEqual(trial["metrics"]["llm_calls"], 2)
                identities = [trace["identity"] for trace in trial["evidence"]["traces"]]
                self.assertEqual(identities[0]["conversation_id"], identities[1]["conversation_id"])
                self.assertNotEqual(identities[0]["run_id"], identities[1]["run_id"])

    def test_training_import_requires_verified_l0_train_report(self):
        self.entry_mode()
        report = self.evaluate()
        env = dict(self.env)
        service = json.loads((self.root / "instrumentation.json").read_text())
        service.update(data_dir=str(self.root / "learning"), auto_evaluate=False, sharing={"enabled": False})
        for principal in service["principals"]:
            env[principal["token_env"]] = "training-import-test-principal-000001"
        write_json(self.root / "learning.json", service)
        args = [str(CLI), "learn", "--config", str(self.root / "learning.json"), "--enqueue-only"]
        training = ["--training-evaluation", str(self.root / "evaluation"), "--dataset", str(self.dataset)]
        first = subprocess.run([*args, *training], env=env, capture_output=True, text=True, timeout=15)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(json.loads(first.stdout)["imported"], 2)
        again = subprocess.run([*args, *training], env=env, capture_output=True, text=True, timeout=15)
        self.assertEqual(again.returncode, 0, again.stderr)
        self.assertEqual(json.loads(again.stdout)["reused"], 2)
        runs = list((self.root / "learning/runs").glob("*.json"))
        self.assertEqual(len(runs), 2)
        trial = report["trials"][0]
        task = json.loads((self.dataset / "train.jsonl").read_text().splitlines()[0])
        original = {trace["identity"]["run_id"]: trace for trace in trial["evidence"]["traces"]}
        for path in runs:
            value = json.loads(path.read_text())
            self.assertEqual(value["training_source"]["kind"], "baseline_training")
            self.assertEqual(value["training_source"]["task_id"], "one")
            source_id = value["import_source_id"]
            trace_path = (self.root / "learning/by-user" / value["identity"]["user_id"] / "traces"
                          / value["identity"]["conversation_id"] / (source_id + ".trace.json"))
            imported = json.loads(trace_path.read_text())
            context = imported["attributes"]["training_evaluation"]
            self.assertEqual(context["expected_behavior"], task["expected_behavior"])
            self.assertEqual(context["constraints"], task.get("constraints", []))
            self.assertEqual(context["verdict"], trial["verdict"])
            self.assertEqual(context["checks"], trial["checks"])
            self.assertEqual(context["run_ids"], list(original))
            self.assertEqual(imported["events"], original[source_id]["events"])
            self.assertNotIn("training_evaluation", original[source_id].get("attributes", {}))

        # Older imports are reused, not silently rewritten or analyzed twice.
        legacy_path = runs[0]
        legacy = json.loads(legacy_path.read_text())
        legacy_trace_path = (self.root / "learning/by-user" / legacy["identity"]["user_id"] / "traces"
                             / legacy["identity"]["conversation_id"] / (legacy["import_source_id"] + ".trace.json"))
        legacy_trace = json.loads(legacy_trace_path.read_text())
        del legacy_trace["attributes"]["training_evaluation"]
        write_json(legacy_trace_path, legacy_trace)
        legacy["import_content_fingerprint"] = hashlib.sha256(legacy_trace_path.read_bytes()).hexdigest()
        legacy["import_evidence_fingerprint"] = legacy["training_source"].pop("source_evidence_fingerprint")
        write_json(legacy_path, legacy)
        reused = subprocess.run([*args, *training], env=env, capture_output=True, text=True, timeout=15)
        self.assertEqual(reused.returncode, 0, reused.stderr)
        self.assertEqual(json.loads(reused.stdout)["reused"], 2)
        self.assertEqual(json.loads(legacy_trace_path.read_text()), legacy_trace)
        self.assertEqual(json.loads(legacy_path.read_text()), legacy)
        raw = self.root / "raw.trace.json"
        write_json(raw, report["trials"][0]["evidence"]["traces"][0])
        ordinary = subprocess.run([*args, str(raw)], env=env, capture_output=True, text=True, timeout=15)
        self.assertEqual(ordinary.returncode, 3)
        self.assertIn("evaluation_trace_not_learning_source", ordinary.stderr)

        # Re-sign the report to exercise semantic guards independently of hashes.
        for change, expected in (("split", "training_task_not_admissible"),
                                 ("task_version", "training_task_not_admissible"),
                                 ("policy", "training_requires_unmodified_baseline"),
                                 ("trace_version", "training_trace_version_mismatch"),
                                 ("context_credential", "credential_value_rejected"),
                                 ("hash", "training_report_invalid")):
            modified = json.loads(json.dumps(report))
            trial = modified["trials"][0]
            if change == "split": trial["split"] = "holdout"
            elif change == "task_version": trial["task_version"] = "another-version"
            elif change == "policy": trial["evidence"]["loaded_version"] = "candidate-policy"
            elif change == "trace_version":
                trial["evidence"]["traces"][1]["events"][0]["attributes"]["evaluation_trial_id"] = "different-trial"
            elif change == "context_credential": trial["checks"][0]["reason"] = KEY
            else: modified["run_id"] = "modified-report"
            path = self.root / "evaluation/report.json"
            write_json(path, modified)
            manifest_path = self.root / "evaluation/manifest.json"
            manifest = json.loads(manifest_path.read_text())
            if change != "hash":
                manifest["files"]["report.json"] = hashlib.sha256(path.read_bytes()).hexdigest()
                write_json(manifest_path, manifest)
            rejected = subprocess.run([*args, *training], env=env, capture_output=True, text=True, timeout=15)
            self.assertEqual(rejected.returncode, 2, (change, rejected.stdout, rejected.stderr))
            self.assertIn(expected, rejected.stderr)
            self.assertNotIn(KEY, rejected.stdout + rejected.stderr)
            self.assertEqual(len(list((self.root / "learning/runs").glob("*.json"))), 2)

    def test_partial_timeout_does_not_block_other_training_traces(self):
        self.entry_mode()
        slow = json.loads(json.dumps(self.config["variants"][0]))
        slow["id"] = "timed-out"
        slow["command"].append("--missing-end")
        self.config["variants"].append(slow)
        self.config["timeout_ms"] = 1500
        report = self.evaluate(code=1)
        timeout = next(t for t in report["trials"] if t["variant_id"] == "timed-out")
        self.assertEqual(timeout["execution_status"], "timeout")
        self.assertFalse(timeout["evidence"]["failure_capture"]["complete"])
        service = json.loads((self.root / "instrumentation.json").read_text())
        service.update(data_dir=str(self.root / "learning"), auto_evaluate=False, sharing={"enabled": False})
        env = dict(self.env)
        for principal in service["principals"]:
            env[principal["token_env"]] = "training-import-test-principal-000001"
        write_json(self.root / "learning.json", service)
        result = subprocess.run([str(CLI), "learn", "--config", str(self.root / "learning.json"),
            "--enqueue-only", "--training-evaluation", str(self.root / "evaluation"),
            "--dataset", str(self.dataset)], env=env, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 3, result.stderr)
        counts = json.loads(result.stdout)
        self.assertEqual(counts["imported"], 2)
        self.assertEqual(counts["rejected"], 1)
        self.assertEqual(len(list((self.root / "learning/runs").glob("*.json"))), 2)

    def test_entry_failure_and_missing_end_are_not_success(self):
        for label, flags, expected in (("handler", ("--error",), "evaluation_entry_handler_failed"),
                                       ("no-run", ("--no-run",), "runner_timeout"),
                                       ("missing-end", ("--missing-end",), "runner_timeout")):
            with self.subTest(label=label):
                self.entry_mode(*flags)
                self.config["timeout_ms"] = 1500
                report = self.evaluate(code=3 if label == "handler" else 1, name="entry-" + label)
                trial = report["trials"][0]
                self.assertIn(trial["error_code"], (expected, "runner_nonzero_exit"))
                self.assertNotEqual(trial["verdict"], "passed")

    def test_entry_configuration_rejected_before_execution(self):
        for label, mutation in (("variable", {"entry": {"input": {"$eval": "rubric"}}}),
                                ("layout", {"run_layout": "single"}),
                                ("transport", {"instrumentation_transport": "native"}),
                                ("pointer", {"entry": {"input": {"$eval": "input"}, "output_pointer": "bad"}})):
            with self.subTest(label=label):
                self.entry_mode()
                self.config["variants"][0].update(mutation)
                self.evaluate(code=2, name="entry-invalid-" + label)
        self.entry_mode()
        self.config["variants"][0]["entry"]["input"] = {"$eval": "input"}
        report = self.evaluate(code=3, name="entry-unbound-state")
        self.assertEqual(report["trials"][0]["error_code"], "evaluation_entry_initial_state_unbound")

    @unittest.skipUnless(shutil.which("node"), "Node is not installed")
    def test_javascript_entry_uses_same_protocol(self):
        self.entry_mode()
        self.config["variants"][0]["command"] = [shutil.which("node"), str(SOURCE / "tests/clients/entry.mjs")]
        trial = self.evaluate(name="javascript-entry")["trials"][0]
        self.assertEqual(len(trial["evidence"]["traces"]), 2)

    def test_entry_is_inert_in_normal_application(self):
        env = {k: v for k, v in self.env.items() if not k.startswith("TRAJECTORY_EVOLUTION_")}
        env["PYTHONPATH"] = str(SOURCE / "clients/python")
        code = "from evolution_entry import evaluate, aevaluate; import asyncio; assert evaluate(None) is False; assert asyncio.run(aevaluate(None)) is False"
        result = subprocess.run([sys.executable, "-c", code], env=env, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_entry_missing_output_and_extra_runs_are_errors(self):
        self.entry_mode()
        self.config["variants"][0]["entry"]["output_pointer"] = "/missing"
        trial = self.evaluate(code=3, name="missing-answer")["trials"][0]
        self.assertIn(trial["error_code"], ("evaluation_entry_output_missing", "runner_nonzero_exit", "evaluation_entry_handler_failed"))
        self.entry_mode("--extra-run")
        trial = self.evaluate(code=3, name="unexpected-run")["trials"][0]
        self.assertIn(trial["error_code"], ("evaluation_entry_unexpected_run", "runner_nonzero_exit", "evaluation_entry_handler_failed"))

    def test_published_policy_file_is_bound_to_fresh_evaluation(self):
        self.entry_mode()
        variant = self.config["variants"][0]
        variant.pop("version")
        variant["policy_version_file"] = "published-version.json"
        owner = json.loads((self.root / "instrumentation.json").read_text())["principals"][0]["user_id"]
        artifact = dict(schema_version=1.0, agent=dict(name="test", revision="fresh-host-1"),
                        user_id=owner, level="L0", policy=None)
        def publish():
            version = "agent-version-v1:" + hashlib.sha256(json.dumps(
                artifact, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
            write_json(self.root / "published-version.json", dict(schema_version=1, version=version,
                       artifact=artifact, evaluation=dict(gate=dict(passed=True))))
            return version
        first_version = publish()
        first = self.evaluate(name="published-first")
        self.assertEqual(first["configuration"]["variants"][0]["version"], first_version)
        self.assertEqual(first["trials"][0]["evidence"]["loaded_version"], first_version)
        artifact["agent"]["revision"] = "fresh-host-2"
        second_version = publish()
        second = self.evaluate(name="published-second")
        self.assertNotEqual(first_version, second_version)
        self.assertNotEqual(first["config_fingerprint"], second["config_fingerprint"])
        self.assertNotEqual(first["trials"][0]["trial_id"], second["trials"][0]["trial_id"])

    def test_published_policy_file_rejects_invalid_or_ambiguous_source(self):
        self.entry_mode()
        variant = self.config["variants"][0]
        variant.pop("version")
        variant["policy_version_file"] = "published-version.json"
        for label, bundle in (("rejected", dict(schema_version=1, evaluation=dict(gate=dict(passed=False)))),
                              ("changed", dict(schema_version=1, version="changed", artifact=dict(
                                  schema_version=1, agent=dict(revision="v1"), user_id="user-a", level="L0", policy=None),
                                  evaluation=dict(gate=dict(passed=True))))):
            write_json(self.root / "published-version.json", bundle)
            self.assertIsNone(self.evaluate(code=2, name=label))
            self.assertFalse((self.root / label).exists())
        variant["artifact"] = None
        self.assertIsNone(self.evaluate(code=2, name="ambiguous"))

    def test_component_owns_instrumented_evaluation(self):
        config = json.loads((SOURCE / "config/boundary-service.json").read_text())
        config["runtime_config"] = str(SOURCE / "config/standard-runtime.json")
        config["experience_template_file"] = str(SOURCE / "config/prompts/runtime-experience.txt")
        config["instrumentation"] = {"level": "L0", "capabilities": ["model", "actions", "loop"],
                                     "binding": {"pointer": "", "mode": "text"}}
        write_json(self.root / "instrumentation.json", config)
        self.config["variants"][0].update(
            command=[sys.executable, "-B", str(SOURCE / "tests/instrumented_agent.py"), str(BUILD / "libtrajectory_evolution_local.so"), "--file"],
            instrumentation_config=str(self.root / "instrumentation.json"))
        self.config["artifacts"] = {"one": [dict(path="report.json", media_type="application/json")]}
        self.config["checks"]["*"].append(dict(id="actual-file", path="/artifacts/report.json/json/total", op="equals", value=20))
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
                self.assertEqual(trial["evidence"]["artifacts"]["report.json"]["json"]["total"], 20)

    def test_configured_artifacts_are_validated_before_execution(self):
        for name, files in (("unknown-task", {"missing-task": []}),
                            ("unsafe", {"one": [{"path": "../outside.txt"}]}),
                            ("duplicate", {"one": [{"path": "a.txt"}, {"path": "a.txt"}]}),
                            ("unsupported", {"one": [{"path": "a.bin", "media_type": "application/octet-stream"}]})):
            with self.subTest(name=name):
                self.config["artifacts"] = files
                self.evaluate(code=2, name=name)
                self.assertFalse((self.root / name).exists())
        self.config["artifacts"] = {"one": [{"path": "missing.txt"}]}
        report = self.evaluate(code=1, name="missing-file")
        self.assertEqual(report["trials"][0]["error_code"], "evaluation_file_not_regular")

    def test_business_failure_is_not_batch_crash(self):
        self.mode("wrong")
        report = self.evaluate(code=1)
        self.assertEqual(report["trials"][0]["execution_status"], "completed")
        self.assertEqual(report["trials"][0]["verdict"], "failed")

    def test_multiple_real_runs_in_one_trial(self):
        config = json.loads((SOURCE / "config/boundary-service.json").read_text())
        config["runtime_config"] = str(SOURCE / "config/standard-runtime.json")
        config["experience_template_file"] = str(SOURCE / "config/prompts/runtime-experience.txt")
        config["instrumentation"] = {"level": "L0", "capabilities": ["model", "actions", "loop"],
                                     "binding": {"pointer": "", "mode": "text"}}
        write_json(self.root / "instrumentation.json", config)
        command = [sys.executable, "-B", str(SOURCE / "tests/instrumented_agent.py"),
                   str(BUILD / "libtrajectory_evolution_local.so"), "--separate-runs"]
        self.config["variants"][0].update(instrumentation_config=str(self.root / "instrumentation.json"), run_layout="per_input")
        self.config.update(timeout_ms=5000, require_trace=True, max_output_bytes=2097152)
        for mode in ("native", "http", "async", "missing-metrics", "exit-after-run", "exit-open-second", "exit-between-runs", "wrong-owner"):
            with self.subTest(mode=mode):
                flags = [] if mode == "native" else (["--http", "--dispatch", "--between-run-delay"] if mode == "async" else ["--" + mode])
                self.config["variants"][0].update(command=command + flags,
                    instrumentation_transport="http" if mode in ("http", "async") else "native")
                error = mode in ("exit-after-run", "exit-open-second", "exit-between-runs", "wrong-owner")
                trial = self.evaluate(name="multi-" + mode, code=3 if error else 0)["trials"][0]
                if mode == "wrong-owner":
                    self.assertEqual(trial["error_code"], "evaluation_loaded_version_mismatch")
                    self.assertNotIn("evidence", trial)
                elif mode in ("exit-open-second", "exit-between-runs"):
                    capture = trial["evidence"]["failure_capture"]
                    self.assertFalse(capture["complete"])
                    self.assertEqual([r["complete"] for r in capture["runs"]], [True, False] if mode == "exit-open-second" else [True])
                    self.assertEqual(capture["expected_runs"], 2)
                    self.assertNotIn("metrics", trial)
                else:
                    self.assertEqual(trial["verdict"], "error" if error else "passed")
                    self.assertEqual(trial["metrics"]["llm_calls"], 2)
                    self.assertEqual(trial["metrics"]["tool_calls"], 2)
                    if mode == "missing-metrics":
                        self.assertNotIn("tokens", trial["metrics"])
                    else:
                        self.assertEqual(trial["metrics"]["tokens"], 84)
                    traces = trial["evidence"]["traces"]
                    self.assertEqual(len(traces), 2)
                    self.assertEqual([t["events"][-1]["output"]["state"]["turn"] for t in traces], [0, 1])
                    self.assertEqual(trial["evidence"]["trajectory"]["summary"]["run_count"], 2)
        self.config["variants"][0].update(command=command, instrumentation_transport="native")
        self.config["artifacts"] = {"one": [{"path": "missing.txt"}]}
        trial = self.evaluate(name="multi-missing-artifact", code=1)["trials"][0]
        self.assertEqual(trial["error_code"], "evaluation_file_not_regular")
        self.assertEqual(len(trial["evidence"]["traces"]), 2)
        self.assertEqual(trial["evidence"]["metrics"]["llm_calls"], 2)

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

    def test_judge_receives_artifact_facts_and_their_post_run_provenance(self):
        self.mode("png")
        self.model()
        self.config["artifacts"] = {"*": [{"pattern": "output/*/assets/*.png", "media_type": "image/png"}]}
        self.evaluate()
        request = REQUESTS[-1]
        prompt = request["messages"][0]["content"]
        self.assertIn("评测器在 Agent 执行后的产物检查", prompt)
        self.assertIn("产物正确不能补足缺失动作", prompt)
        packet = json.loads(request["messages"][1]["content"])
        artifact = packet["evidence"]["artifacts"]["output/run/assets/image0.png"]
        self.assertEqual(artifact["width"], 2)
        self.assertEqual(artifact["height"], 1)

    def test_missing_and_sensitive_artifacts(self):
        for mode in ("missing", "artifact_leak"):
            self.mode(mode)
            self.evaluate(code=1, name=mode)
            self.assertFalse(list((self.root / mode / "artifacts").rglob("*.json")))

    def test_artifact_patterns_collect_once_and_allow_zero_matches(self):
        self.mode("png_declared")
        self.config["artifacts"] = {"*": [{"pattern": "output/*/assets/*.png", "media_type": "image/png"}],
                                    "one": [{"path": "output/run/assets/image0.png", "media_type": "image/png"}]}
        trial = self.evaluate(name="matched")["trials"][0]
        files = trial["evidence"]["artifacts"]
        self.assertEqual(list(files), ["output/run/assets/image0.png"])
        self.assertEqual(files["output/run/assets/image0.png"]["width"], 2)
        self.mode("good")
        del self.config["artifacts"]["one"]
        self.assertEqual(self.evaluate(name="optional")["trials"][0]["evidence"]["artifacts"], {})

    def test_artifact_patterns_reject_unsafe_paths_symlinks_and_limit(self):
        for index, declaration in enumerate(({"pattern": "../*.png"}, {"pattern": "output/**/*.png"},
                                              {"pattern": "/output/*.png"}, {"pattern": "output/[ab].png"},
                                              {"path": "a.png", "pattern": "*.png"})):
            self.config["artifacts"] = {"*": [declaration]}
            self.evaluate(code=2, name=f"invalid-pattern-{index}")
        self.config["artifacts"] = {"*": [{"pattern": "output/*/assets/*.png", "media_type": "image/png"}]}
        private = self.root / "private.txt"
        private.write_text(KEY)
        for mode, error in (("png_symlink", "evaluation_artifact_symlink"),
                            ("png_parent_symlink", "evaluation_artifact_symlink"),
                            ("png_many", "evaluation_artifact_count_limit")):
            self.config["variants"][0]["command"] = [sys.executable, "-B", str(SOURCE / "tests/evaluation_host.py"), mode, str(private)]
            trial = self.evaluate(code=1, name=mode)["trials"][0]
            self.assertEqual(trial["error_code"], error)
            self.assertFalse(list((self.root / mode / "artifacts").rglob("*.png")))

    def test_png_decode_rejects_corrupt_truncated_and_expanding_images(self):
        self.mode("png")
        self.config["artifacts"] = {"*": [{"pattern": "output/*/assets/*.png", "media_type": "image/png"}]}
        trial = self.evaluate(name="valid-png")["trials"][0]
        if trial["evidence"]["artifacts"]["output/run/assets/image0.png"]["validation"] == "header_only":
            self.skipTest("libpng decoding is not enabled in this build")
        for mode, error in (("png_corrupt", "evaluation_image_decode_failed"),
                            ("png_truncated", "evaluation_image_decode_failed"),
                            ("png_no_end", "evaluation_image_decode_failed"),
                            ("png_huge", "evaluation_image_decoded_bytes_limit")):
            self.mode(mode)
            trial = self.evaluate(code=1, name=mode)["trials"][0]
            self.assertEqual(trial["error_code"], error)
            self.assertFalse(list((self.root / mode / "artifacts").rglob("*.png")))

    def test_configured_artifact_media_conflict_fails(self):
        self.mode("png_declared")
        self.config["artifacts"] = {"*": [{"pattern": "output/*/assets/*.png", "media_type": "text/plain"}]}
        trial = self.evaluate(code=1)["trials"][0]
        self.assertEqual(trial["error_code"], "evaluation_artifact_type_conflict")

    def test_artifact_failure_still_reviews_execution_efficiency(self):
        self.mode("loop_trace")
        self.model()
        Provider.behavior = "loops_detailed"
        self.config["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
        self.config["artifacts"] = {"*": [{"path": "missing.png", "media_type": "image/png"}]}
        report = self.evaluate(code=1)
        trial = report["trials"][0]
        self.assertEqual(trial["verdict"], "failed")
        self.assertEqual(trial["error_code"], "evaluation_file_not_regular")
        self.assertEqual(report["summary"]["model_calls"], 1)
        self.assertEqual(trial["evidence"]["loop_review"]["status"], "complete")
        self.assertEqual(trial["evidence"]["artifact_error"], "evaluation_file_not_regular")
        self.assertFalse(list((self.root / "evaluation/artifacts").rglob("*.png")))

    def test_intentional_corrupt_input_uses_declared_media_not_suffix(self):
        self.mode("invalid_png_fixture")
        self.config["artifacts"] = {"*": [dict(pattern="output/*/assets/broken.png", media_type="text/plain"),
                                           dict(path="report.json", media_type="application/json")]}
        report = self.evaluate()
        files = report["trials"][0]["evidence"]["artifacts"]
        self.assertEqual(files["output/run/assets/broken.png"]["text"], "NOT_AN_IMAGE")
        self.assertFalse(files["report.json"]["json"]["is_valid_png"])

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

    def test_detached_descendants_are_reaped_before_workspace_cleanup(self):
        pid_file = self.root / "detached.pid"
        self.mode("detached_child")
        self.config["variants"][0]["command"].append(str(pid_file))
        self.config["timeout_ms"] = 500
        sentinel = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(20)"], start_new_session=True)
        try:
            report = self.evaluate(code=1)
            self.assertEqual(report["trials"][0]["error_code"], "runner_timeout")
            self.assertTrue(pid_file.exists(), "detached worker never started")
            self.assertFalse(Path(f"/proc/{int(pid_file.read_text())}").exists(), "detached worker survived")
            time.sleep(.7)
            self.assertFalse((self.root / "evaluation/.work").exists(), "late worker recreated the workspace")
            self.assertIsNone(sentinel.poll(), "cleanup touched an unrelated process")
        finally:
            sentinel.terminate(); sentinel.wait(timeout=5)

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
        constraints = ["只使用本次工作区。", "失败后停止，不得重试或伪造成功。"]
        self.make_dataset(cases=[dict(task_id="one", group_id="one", task_input="计算已知数据的总量。",
                              expected_behavior="总量为20，不编造缺失的数据。", constraints=constraints,
                              difficulty="easy", category="data", initial_state={"value": 20},
                              followup_inputs=["请再次确认。"], split="train", review_status="reviewed")])
        self.model()
        report = self.evaluate()
        self.assertEqual(report["summary"]["model_calls"], 1)
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        self.assertIn("expected_behavior", packet)
        self.assertEqual(packet["constraints"], constraints)
        self.assertIn("禁止项", packet["contract"]["reason"])
        self.assertIn("业务验收标准仅来自本题 expected_behavior 与 constraints", REQUESTS[-1]["messages"][0]["content"])
        self.assertIn("required_checks", REQUESTS[-1]["messages"][0]["content"])
        self.assertIn("内部经验、策略、工具说明和输出不能新增验收禁令", REQUESTS[-1]["messages"][0]["content"])
        self.assertIn("先检查明确缺失的必需项或违规", REQUESTS[-1]["messages"][0]["content"])
        self.assertIn("A && B || echo", REQUESTS[-1]["messages"][0]["content"])
        self.assertIn("后续正确产物也不能补足未执行的必需步骤", REQUESTS[-1]["messages"][0]["content"])
        self.assertIn("目标工具未再次执行，不算目标操作重试", REQUESTS[-1]["messages"][0]["content"])
        self.assertIn("可计循环浪费，不能仅据此判业务失败", REQUESTS[-1]["messages"][0]["content"])
        self.assertIn("除非标准禁止该调度尝试", REQUESTS[-1]["messages"][0]["content"])
        self.assertIn("failed 的 reason 须先引用本题", REQUESTS[-1]["messages"][0]["content"])
        loop_prompt = (SOURCE / "config/prompts/task-loop-review.txt").read_text(encoding="utf-8")
        self.assertIn("浪费可与业务 passed 同时成立", loop_prompt)
        self.assertNotIn("违规仍使业务 failed", loop_prompt)
        self.assertEqual(packet["contract"]["evidence_paths"], ["/output"])
        self.assertLess(len(REQUESTS[-1]["messages"][0]["content"].encode("utf-8")), 6000)
        self.assertNotIn("evidence_text_refs", packet)
        self.assertEqual(REQUESTS[-1]["thinking"]["type"], "disabled")
        self.assertNotIn(KEY, json.dumps(packet))

        self.mode("loop_trace")
        self.evaluate(name="with-trace")
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        self.assertEqual(packet["contract"]["evidence_paths"], ["/trace"])

    def test_json_object_output_is_opt_in(self):
        self.model()
        runtime_path = self.root / "runtime.json"
        runtime = json.loads(runtime_path.read_text())
        for enabled in (None, False, True):
            with self.subTest(enabled=enabled):
                if enabled is None:
                    runtime["model"].pop("json_object_output", None)
                else:
                    runtime["model"]["json_object_output"] = enabled
                write_json(runtime_path, runtime)
                report = self.evaluate(name=f"json-output-{enabled}")
                self.assertEqual(report["summary"]["model_calls"], 1)
                self.assertEqual(REQUESTS[-1].get("response_format"),
                                 {"type": "json_object"} if enabled else None)
                self.assertEqual(REQUESTS[-1]["thinking"]["type"], "disabled")

    def test_json_object_output_rejects_non_boolean_before_execution(self):
        self.model()
        runtime_path = self.root / "runtime.json"
        runtime = json.loads(runtime_path.read_text())
        for index, invalid in enumerate(("true", 1, None, {"type": "json_object"})):
            with self.subTest(invalid=invalid):
                runtime["model"]["json_object_output"] = invalid
                write_json(runtime_path, runtime)
                name = f"json-output-invalid-{index}"
                self.evaluate(code=2, name=name)
                self.assertFalse((self.root / name).exists())
                self.assertEqual(len(REQUESTS), self.start_requests)

    def test_judge_shares_repeated_text_without_losing_evidence(self):
        self.model()
        self.config["judge"]["share_repeated_text"] = True
        self.mode("repeated_evidence")
        host = self.config["variants"][0]["command"]
        wrapper = (
            "import json, subprocess, sys\n"
            f"response=json.loads(subprocess.run({host!r},input=sys.stdin.read(),capture_output=True,text=True,check=True).stdout)\n"
            "for event in response['trace']['events']:\n"
            " event['attributes']={'custom_receipt':{'text':'完整回执。'*100, 'nested/~':[1, True, None]}}\n"
            "print(json.dumps(response, ensure_ascii=False))\n"
        )
        self.config["variants"][0]["command"] = [sys.executable, "-c", wrapper]
        repeated = "这是必须保留的中文完整证据。" * 80
        details = {"nested/~": [repeated, None, {"again/~": repeated}],
                   "copies": [repeated] * 10, "unique": "不重复的证据。" * 80,
                   "spoof": {"$evidence_ref": "/not-real"}}
        self.make_dataset([dict(task_id="shared", group_id="shared", task_input="核验总量。",
            expected_behavior="总量为20，保留完整证据。", difficulty="easy", category="data",
            initial_state=dict(value=20, details=details), split="train", review_status="reviewed")])
        self.config["max_output_bytes"] = 1048576
        self.config["judge"]["max_input_bytes"] = 131072
        report = self.evaluate()
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        original = report["trials"][0]["evidence"]
        def model_view(raw):
            view = json.loads(json.dumps(raw))
            for field in ("trajectory", "execution_run_id", "loaded_version", "loop_review"):
                view.pop(field, None)
            traces = view.get("traces", [view["trace"]] if "trace" in view else [])
            for trace in traces:
                for field in ("schema_version", "trace_kind"):
                    trace.pop(field, None)
                for field in ("application_id", "user_id", "conversation_id"):
                    trace.get("identity", {}).pop(field, None)
                for event in trace["events"]:
                    for field in ("producer_id", "sequence"):
                        event.pop(field, None)
                    for field in ("action_id", "execution_id", "step_id", "loop_id", "operation", "status", "decision", "reason"):
                        if event.get(field) == "":
                            event.pop(field)
                    for field in ("protocol", "agent_version_id", "evaluation_trial_id"):
                        event.get("attributes", {}).pop(field, None)
            return view
        self.assertEqual(original["state"]["details"], details)
        self.assertEqual(report["summary"]["model_calls"], 1)
        self.assertTrue(packet["evidence_text_refs"])
        self.assertLessEqual(len(REQUESTS[-1]["messages"][1]["content"].encode()), 131072)
        self.assertGreater(len(json.dumps(original, ensure_ascii=False).encode()), 131072)
        restored = packet["evidence"]
        self.assertEqual(restored["output"], original["output"])
        self.assertEqual(restored["state"], original["state"])
        for event, raw in zip(restored["trace"]["events"], original["trace"]["events"]):
            self.assertEqual(event.get("output"), raw.get("output"))
        def locate(path):
            parts = [p.replace("~1", "/").replace("~0", "~") for p in path[1:].split("/")]
            parent = restored
            for part in parts[:-1]:
                parent = parent[int(part)] if isinstance(parent, list) else parent[part]
            return parent, int(parts[-1]) if isinstance(parent, list) else parts[-1]
        seen = set()
        for item in packet["evidence_text_refs"]:
            source, key = locate(item["source"])
            text = source[key]
            self.assertIsInstance(text, str)
            for path in item["paths"]:
                self.assertNotIn(path, seen)
                seen.add(path)
                parent, key = locate(path)
                self.assertEqual(parent[key], {"$evidence_ref": item["source"]})
                parent[key] = text
        self.assertEqual(restored, model_view(original))

        previous_bytes = len(REQUESTS[-1]["messages"][1]["content"].encode())
        self.config["judge"]["share_repeated_text"] = False
        self.config["judge"]["share_repeated_inputs"] = True
        report = self.evaluate(name="shared-inputs")
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        packed = packet["evidence"]
        aliases = {}
        for item in packet["evidence_text_refs"]:
            for path in item["paths"]:
                self.assertNotIn(path, aliases)
                aliases[path] = item["source"]

        def value_at(path):
            value = packed
            for part in path[1:].split("/"):
                part = part.replace("~1", "/").replace("~0", "~")
                value = value[int(part)] if isinstance(value, list) else value[part]
            return value

        def restore(value, path="", active=frozenset()):
            if path in aliases:
                self.assertNotIn(path, active)
                source = aliases[path]
                self.assertEqual(value, {"$evidence_ref": source})
                return restore(value_at(source), source, active | {path})
            if isinstance(value, list):
                return [restore(v, path + "/" + str(i), active) for i, v in enumerate(value)]
            if isinstance(value, dict):
                return {k: restore(v, path + "/" + k.replace("~", "~0").replace("/", "~1"), active)
                        for k, v in value.items()}
            return value

        self.assertTrue(any(isinstance(value_at(source), (dict, list)) for source in aliases.values()))
        original = report["trials"][0]["evidence"]
        self.assertEqual(restore(packed), model_view(original))
        self.assertEqual(packed["output"], original["output"])
        self.assertEqual(packed["state"], original["state"])
        self.assertTrue(any("/output" in path for path in aliases))
        self.assertTrue(any(path.endswith("/attributes") or "/attributes/" in path
                            for path in aliases))
        self.assertLess(len(REQUESTS[-1]["messages"][1]["content"].encode()), previous_bytes)

    def test_minimum_judge_input_is_the_default(self):
        self.model()
        self.mode("repeated_evidence")
        self.make_dataset([dict(task_id="minimal", group_id="minimal", task_input="核验总量。",
            expected_behavior="总量为20。", difficulty="easy", category="data",
            initial_state=dict(value=20, details={"copies": ["必要中文证据。" * 120] * 10}),
            split="train", review_status="reviewed")])
        self.config["max_output_bytes"] = 1048576
        self.config["judge"]["max_input_bytes"] = 131072
        report = self.evaluate()
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        self.assertTrue(packet["evidence_text_refs"])
        self.assertNotIn("trajectory", packet["evidence"])
        self.assertNotIn("loaded_version", packet["evidence"])
        self.assertNotIn("/trajectory", packet["allowed_evidence_paths"])
        self.assertIn("trajectory", report["trials"][0]["evidence"])
        self.assertEqual(report["summary"]["model_calls"], 1)

    def test_http_grading_uses_configured_parallel_workers(self):
        condition = threading.Condition()
        state = dict(active=0, entered=0, peak=0)

        class ConcurrentProvider(Provider):
            def do_POST(self):
                with condition:
                    state["active"] += 1
                    state["entered"] += 1
                    state["peak"] = max(state["peak"], state["active"])
                    condition.notify_all()
                    condition.wait_for(lambda: state["entered"] >= 2, timeout=1)
                try:
                    super().do_POST()
                finally:
                    with condition:
                        state["active"] -= 1

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), ConcurrentProvider)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            self.model()
            runtime_path = self.root / "runtime.json"
            runtime = json.loads(runtime_path.read_text())
            runtime["model"]["endpoint"] = f"http://127.0.0.1:{server.server_port}/chat/completions"
            write_json(runtime_path, runtime)
            self.config.update(repetitions=2, parallelism=2)
            report = self.evaluate()
            self.assertEqual(report["summary"]["model_calls"], 2)
            self.assertEqual(state["peak"], 2, "HTTP grading was serialized across parallel workers")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_http_retries_share_one_timeout_budget(self):
        self.model()
        runtime_path = self.root / "runtime.json"
        runtime = json.loads(runtime_path.read_text())
        for scenario, calls, reason in (("timeout", 3, "evaluation_judge_transport_28"),
                                        ("remaining", 3, "evaluation_judge_transport_28"),
                                        ("stalled_recovered", 2, None),
                                        ("no_retry", 1, "evaluation_judge_transport_28"),
                                        ("recovered", 2, None),
                                        ("unauthorized", 1, "evaluation_judge_http_401")):
            with self.subTest(scenario=scenario):
                entered = []
                release = threading.Event()

                class BudgetProvider(Provider):
                    def do_POST(self):
                        entered.append(time.monotonic())
                        if scenario in ("recovered", "stalled_recovered") and len(entered) > 1:
                            return super().do_POST()
                        self.rfile.read(int(self.headers["Content-Length"]))
                        if scenario == "unauthorized" or (scenario in ("remaining", "recovered") and len(entered) == 1):
                            if scenario == "remaining":
                                time.sleep(.2)
                            self.send_response(401 if scenario == "unauthorized" else 503)
                            self.send_header("Content-Length", "0")
                            self.end_headers()
                        else:
                            release.wait(10)

                server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), BudgetProvider)
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                try:
                    runtime["model"].update(endpoint=f"http://127.0.0.1:{server.server_port}/chat/completions",
                                            timeout_ms=1200, max_retries=0 if scenario == "no_retry" else 2)
                    write_json(runtime_path, runtime)
                    report = self.evaluate(code=0 if reason is None else 3, name="http-" + scenario)
                    self.assertEqual(len(entered), calls, "retries renewed the network timeout")
                    self.assertLess(time.monotonic() - entered[0], 2.0,
                                    "transport retries exceeded the shared budget and cleanup allowance")
                    if len(entered) > 1:
                        self.assertLess(entered[-1] - entered[0], 1.2,
                                        "transport retry started after the total budget")
                    self.assertEqual(report["summary"]["model_calls"], 1,
                                     "transport failure incorrectly entered semantic recovery")
                    check = report["trials"][0]["checks"][-1]
                    self.assertEqual(check["status"], "passed" if reason is None else "error")
                    if reason is not None:
                        self.assertEqual(check["reason"], reason)
                finally:
                    release.set()
                    server.shutdown()
                    server.server_close()
                    thread.join()

    def test_failed_completion_envelope_is_not_a_valid_judgment(self):
        self.model()
        mode = "content_filter"

        class EnvelopeProvider(Provider):
            def do_POST(self):
                self.rfile.read(int(self.headers["Content-Length"]))
                answer = json.dumps(dict(verdict="passed", reason="实际输出满足要求。", evidence_paths=["/state"]))
                choice = dict(finish_reason="stop", message=dict(content=answer))
                value = dict(choices=[choice])
                if mode == "content_filter": choice["finish_reason"] = "content_filter"
                elif mode == "error": value["error"] = dict(message="provider error body must remain private")
                elif mode == "multiple": value["choices"].append(dict(choice))
                elif mode == "refusal": choice["message"]["refusal"] = "not a completed judgment"
                encoded = json.dumps(value).encode()
                self.send_response(200); self.send_header("Content-Length", str(len(encoded))); self.end_headers()
                self.wfile.write(encoded)

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), EnvelopeProvider)
        thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
        try:
            runtime = json.loads((self.root / "runtime.json").read_text())
            runtime["model"]["endpoint"] = f"http://127.0.0.1:{server.server_port}/chat/completions"
            write_json(self.root / "runtime.json", runtime)
            for mode in ("content_filter", "error", "multiple", "refusal"):
                with self.subTest(mode=mode):
                    report = self.evaluate(code=3, name="envelope-" + mode)
                    self.assertEqual(report["trials"][0]["checks"][-1]["reason"], "evaluation_judge_invalid_api_response")
                    self.assertEqual(report["summary"]["model_calls"], 1)
        finally:
            server.shutdown(); server.server_close(); thread.join()

    def test_model_prompt_is_not_silently_cut_at_nul(self):
        self.model()
        path = self.root / self.config["judge"]["prompt"]
        path.write_text(path.read_text() + "\n\0正文仍须保留。", encoding="utf-8")
        self.evaluate()
        self.assertTrue(REQUESTS[-1]["messages"][0]["content"].endswith("\0正文仍须保留。"))

    def test_ambiguous_model_envelope_is_rejected(self):
        self.model()
        mode = "root"

        class AmbiguousProvider(Provider):
            def do_POST(self):
                self.rfile.read(int(self.headers["Content-Length"]))
                answer = json.dumps(dict(verdict="passed", reason="实际输出满足要求。", evidence_paths=["/state"]))
                content = json.dumps(answer)
                if mode == "stream":
                    encoded = ('data: {"choices":[{"index":0,"delta":{"content":' + content
                               + ',"content":""},"finish_reason":"stop"}]}\n\ndata: [DONE]\n\n').encode()
                else:
                    message = '{"content":' + content + (',"content":""' if mode == "message" else '') + '}'
                    choice = '{"finish_reason":"stop","message":' + message
                    if mode == "choice": choice += ',"finish_reason":"content_filter"'
                    if mode == "index": choice += ',"index":1'
                    encoded = ('{"choices":[' + choice + '}]' + (',"choices":[]' if mode == "root" else '') + '}').encode()
                self.send_response(200); self.send_header("Content-Length", str(len(encoded))); self.end_headers()
                self.wfile.write(encoded)

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), AmbiguousProvider)
        thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
        try:
            runtime_path = self.root / "runtime.json"
            runtime = json.loads(runtime_path.read_text())
            for mode in ("root", "choice", "message", "index", "stream"):
                with self.subTest(mode=mode):
                    runtime["model"].update(endpoint=f"http://127.0.0.1:{server.server_port}/chat/completions", stream=mode == "stream")
                    write_json(runtime_path, runtime)
                    report = self.evaluate(code=3, name="ambiguous-" + mode)
                    self.assertEqual(report["trials"][0]["checks"][-1]["reason"], "evaluation_judge_invalid_api_response")
                    self.assertEqual(report["summary"]["model_calls"], 1)
        finally:
            server.shutdown(); server.server_close(); thread.join()

    def test_streamed_model_responses_are_complete_and_bounded(self):
        self.model()
        runtime_path = self.root / "runtime.json"
        runtime = json.loads(runtime_path.read_text())
        mode = "normal"
        observed = []

        class StreamProvider(Provider):
            def do_POST(self):
                if mode == "json_fallback":
                    return super().do_POST()
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                observed.append(body)
                answer = json.dumps(dict(verdict="passed", reason="实际结果满足全部要求。", evidence_paths=["/state"]), ensure_ascii=False)
                if mode == "oversized":
                    answer += " " * 2000
                frames = []
                for start in range(0, len(answer), 5):
                    frames.append({"choices": [{"index": 0, "delta": {"content": answer[start:start + 5]}, "finish_reason": None}]})
                frames.append({"choices": [{"index": 0, "delta": {}, "finish_reason": "length" if mode == "length" else "stop"}]})
                frames.append({"choices": [], "usage": {"completion_tokens": 20}})
                if mode == "malformed":
                    frames.append({"error": {"message": "do not persist provider diagnostics"}})
                if mode == "null_error":
                    frames[0]["error"] = None
                if mode == "refusal":
                    frames[0]["choices"][0]["delta"]["refusal"] = "not a completed judgment"
                if mode == "multiple_choices":
                    frames[0]["choices"].append({"index": 1, "delta": {"content": "other"}})
                data = "\ufeff:keepalive\r\n\r\n"
                for frame in frames:
                    # Multi-line SSE data and split UTF-8 network chunks.
                    raw = json.dumps(frame, ensure_ascii=False)
                    data += "data: {\r\ndata: " + raw[1:] + "\r\n\r\n"
                if mode != "missing_done":
                    data += "data: [DONE]\r\n\r\n"
                encoded = data.encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Content-Length", str(len(encoded)))
                self.end_headers()
                try:
                    for start in range(0, len(encoded), 29):
                        self.wfile.write(encoded[start:start + 29])
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), StreamProvider)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            for mode in ("normal", "json_fallback", "null_error", "refusal", "length", "missing_done", "malformed", "multiple_choices", "oversized"):
                with self.subTest(mode=mode):
                    runtime["model"].update(endpoint=f"http://127.0.0.1:{server.server_port}/chat/completions",
                                            stream=True, idle_timeout_ms=1500, max_response_bytes=1024)
                    write_json(runtime_path, runtime)
                    ok = mode in ("normal", "json_fallback", "null_error")
                    report = self.evaluate(code=0 if ok else 3, name="stream-" + mode)
                    check = report["trials"][0]["checks"][-1]
                    self.assertEqual(check["status"], "passed" if ok else "error")
                    self.assertEqual(report["summary"]["model_calls"], 1)
                    if mode == "length":
                        self.assertEqual(check["reason"], "evaluation_judge_output_token_limit_reached")
            self.assertTrue(all(r["stream"] for r in observed))
        finally:
            server.shutdown(); server.server_close(); thread.join()

    def test_stream_idle_retry_does_not_wait_for_total_budget(self):
        self.model()
        runtime_path = self.root / "runtime.json"
        runtime = json.loads(runtime_path.read_text())
        entered = []
        release = threading.Event()

        class IdleProvider(Provider):
            def do_POST(self):
                entered.append(time.monotonic())
                if len(entered) > 1:
                    return super().do_POST()
                self.rfile.read(int(self.headers["Content-Length"]))
                release.wait(8)

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), IdleProvider)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            runtime["model"].update(endpoint=f"http://127.0.0.1:{server.server_port}/chat/completions",
                                    stream=True, idle_timeout_ms=300, timeout_ms=6000, max_retries=1)
            write_json(runtime_path, runtime)
            report = self.evaluate()
            self.assertEqual(len(entered), 2)
            self.assertLess(entered[1] - entered[0], 2.0)
            self.assertEqual(report["trials"][0]["checks"][-1]["status"], "passed")
            self.assertEqual(report["summary"]["model_calls"], 1)
        finally:
            release.set(); server.shutdown(); server.server_close(); thread.join()

    def test_sequential_model_calls_reuse_connection(self):
        self.model()
        self.config.update(parallelism=1, repetitions=2)
        addresses = []

        class KeepAliveProvider(Provider):
            protocol_version = "HTTP/1.1"
            def do_POST(self):
                addresses.append(self.client_address)
                return super().do_POST()

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), KeepAliveProvider)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            path = self.root / "runtime.json"
            runtime = json.loads(path.read_text())
            runtime["model"]["endpoint"] = f"http://127.0.0.1:{server.server_port}/chat/completions"
            write_json(path, runtime)
            report = self.evaluate()
            self.assertEqual(report["summary"]["model_calls"], 2)
            self.assertEqual(len(addresses), 2)
            self.assertEqual(addresses[0], addresses[1], "sequential requests recreated the TCP connection")
        finally:
            server.shutdown(); server.server_close(); thread.join()

    def test_stream_options_are_validated_before_agent_execution(self):
        self.model()
        path = self.root / "runtime.json"
        runtime = json.loads(path.read_text())
        for settings in ({"stream": "yes"}, {"stream": False, "idle_timeout_ms": 100},
                         {"stream": True, "idle_timeout_ms": -1}):
            with self.subTest(settings=settings):
                altered = json.loads(json.dumps(runtime))
                altered["model"].update(settings)
                write_json(path, altered)
                self.assertIsNone(self.evaluate(code=2))
                self.assertEqual(len(REQUESTS), self.start_requests)

    def test_repeated_credentials_are_rejected_before_model_packing(self):
        self.model()
        self.config["judge"]["share_repeated_text"] = True
        self.mode("repeated_leak")
        report = self.evaluate(code=1)
        self.assertEqual(report["trials"][0]["error_code"], "evaluation_sensitive_output_rejected")
        self.assertNotIn("evidence", report["trials"][0])
        self.assertEqual(len(REQUESTS), self.start_requests)

    def test_text_sharing_option_is_validated_before_execution(self):
        self.model()
        for option in ("share_repeated_text", "share_repeated_inputs"):
            with self.subTest(option=option):
                self.config["judge"][option] = "true"
                self.evaluate(code=2, name="invalid-" + option)
                self.assertFalse((self.root / ("invalid-" + option)).exists())
                self.assertEqual(len(REQUESTS), self.start_requests)
                self.config["judge"].pop(option)

    def test_container_sharing_keeps_privacy_checks_before_model(self):
        self.model()
        self.config["judge"]["share_repeated_text"] = False
        self.config["judge"]["share_repeated_inputs"] = True
        self.mode("repeated_leak")
        report = self.evaluate(code=1)
        self.assertEqual(report["trials"][0]["error_code"], "evaluation_sensitive_output_rejected")
        self.assertNotIn("evidence", report["trials"][0])
        self.assertEqual(len(REQUESTS), self.start_requests)

    def test_loop_review_reuses_judge_and_counts_distinct_iterations(self):
        self.model(); self.mode("loop_trace")
        Provider.behavior = "loops"
        self.config["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
        report = self.evaluate()
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        inventory = packet["loop_inventory"]
        self.assertEqual(len(inventory), 4)  # Two branches, two iterations, not 16 step/action events.
        self.assertEqual({v["execution_id"] for v in inventory.values()}, {"left", "right"})
        events = {e["event_id"]: e for e in packet["evidence"]["trace"]["events"]}
        for item in inventory.values():
            anchor = item["anchor_event_id"]
            self.assertIn(anchor, item["event_ids"])
            self.assertEqual(events[anchor]["kind"], "action.end")
            self.assertTrue(events[anchor].get("attributes", {}).get("executed", True))
            for field in ("execution_id", "loop_id"):
                self.assertEqual(events[anchor][field], item[field])
            self.assertEqual(int(events[anchor]["iteration"]), int(item["iteration"]))
        self.assertEqual(report["summary"]["model_calls"], 1)
        self.assertNotIn("/loop_review", packet["allowed_evidence_paths"])
        self.assertNotIn("loop_review", packet["evidence"])
        evidence = report["trials"][0]["evidence"]
        self.assertEqual(evidence["loop_review"]["count"], 1)
        self.assertEqual(evidence["trajectory"]["summary"]["invalid_loop_lower_bound"], 0)
        self.assertEqual(report["summary"]["invalid_loop_assessment"],
                         dict(method="model_review", mean=1, observations=1))

    def test_loop_review_uses_short_refs_and_persists_canonical_ids(self):
        self.model(); self.mode("loop_trace")
        self.config["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
        Provider.behavior = "loops_detailed"
        report = self.evaluate()
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        inventory = packet["loop_inventory"]
        self.assertEqual(set(inventory), {"i1", "i2", "i3", "i4"})
        canonical = {
            hashlib.sha256(json.dumps([item["run_id"], item["execution_id"],
                item["loop_id"], str(item["iteration"])], separators=(",", ":")).encode()).hexdigest(): item
            for item in inventory.values()
        }
        review = report["trials"][0]["evidence"]["loop_review"]
        self.assertEqual({item["iteration_id"] for item in review["iteration_reviews"]}, set(canonical))
        for item in review["iteration_reviews"] + review["wasted_iterations"]:
            self.assertIn(item["evidence_event_ids"][0], canonical[item["iteration_id"]]["event_ids"])
        self.assertEqual(report["summary"]["model_calls"], 1)

    def test_loop_review_short_refs_follow_observations_not_hashes(self):
        self.model(); self.mode("loop_trace")
        command = self.config["variants"][0]["command"]
        # Twelve observed iterations cross the i9/i10 lexical boundary without
        # merging the six independently observed execution branches.
        wrapper = ("import json,subprocess,sys\n"
                   "value=json.loads(subprocess.check_output(sys.argv[1:],input=sys.stdin.buffer.read()))\n"
                   "trace=value['trace']; original=trace['events']; events=[original[0]]; ends=[]\n"
                   "for run in range(3):\n"
                   " for raw in original[1:-1]:\n"
                   "  item=json.loads(json.dumps(raw))\n"
                   "  for field in ('event_id','execution_id','loop_id','step_id','action_id','producer_id'):\n"
                   "   if field in item: item[field]=f'{run}-'+item[field]\n"
                   "  item['caused_by']=[key if key=='start' else f'{run}-'+key for key in item['caused_by']]\n"
                   "  item['timestamp_us']=str(len(events)+1); events.append(item)\n"
                   " ends.extend(f'{run}-'+key for key in original[-1]['caused_by'])\n"
                   "end=original[-1]; end['caused_by']=ends; end['timestamp_us']=str(len(events)+1)\n"
                   "events.append(end); trace['events']=events\nprint(json.dumps(value))\n")
        self.config["variants"][0]["command"] = [sys.executable, "-c", wrapper, *command]
        self.config["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
        Provider.behavior = "loops_detailed"
        report = self.evaluate()
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        inventory = packet["loop_inventory"]
        self.assertEqual(list(inventory), [f"i{i:02}" for i in range(1, 13)])
        trace = packet["evidence"]["trace"]
        order = {(trace["identity"]["run_id"], event["event_id"]): index
                 for index, event in enumerate(trace["events"])}
        ranks = [order[(item["run_id"], item["anchor_event_id"])] for item in inventory.values()]
        self.assertEqual(ranks, sorted(ranks))
        reviews = report["trials"][0]["evidence"]["loop_review"]["iteration_reviews"]
        self.assertEqual(len({item["iteration_id"] for item in reviews}), 12)
        self.assertTrue(all(len(item["iteration_id"]) == 64 for item in reviews))
        prompt = REQUESTS[-1]["messages"][0]["content"]
        self.assertIn("最后综合核验输出 verdict", prompt)
        self.assertIn("不表示并发分支之间存在依赖", prompt)

    def test_loop_review_rejects_duplicates_unknown_ids_and_other_loop_evidence(self):
        self.model(); self.mode("loop_trace")
        self.config["judge"].update(loop_review_prompt="prompts/task-loop-review.txt", max_repair_attempts=0)
        for behavior in ("loops_duplicate", "loops_unknown", "loops_other_event"):
            with self.subTest(behavior=behavior):
                Provider.behavior = behavior
                report = self.evaluate(code=3, name=behavior)
                self.assertEqual(report["trials"][0]["error_code"], "evaluation_judge_failed")
                self.assertIsNone(report["summary"]["invalid_loop_assessment"]["mean"])
                self.assertEqual(report["summary"]["invalid_loop_assessment"]["observations"], 0)

    def test_detailed_loop_review_counts_decisions_and_keeps_all_reasons(self):
        self.model(); self.mode("loop_trace")
        self.config["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
        Provider.behavior = "loops_detailed"
        report = self.evaluate()
        review = report["trials"][0]["evidence"]["loop_review"]
        self.assertEqual(len(review["iteration_reviews"]), 4)
        self.assertEqual(review["count"], 1)
        self.assertEqual([r["iteration_id"] for r in review["wasted_iterations"]],
                         [r["iteration_id"] for r in review["iteration_reviews"] if r["decision"] == "waste"])
        self.assertEqual(report["summary"]["model_calls"], 1)
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        self.assertIn("iteration_reviews", packet["contract"]["loop_review"])
        self.assertNotIn("wasted_iterations", packet["contract"]["loop_review"])

    def test_detailed_loop_review_requires_coverage_and_valid_decisions(self):
        self.model(); self.mode("loop_trace")
        self.config["judge"].update(loop_review_prompt="prompts/task-loop-review.txt", max_repair_attempts=0)
        for suffix, constraint in (("missing", "missing_iteration_reviews"), ("duplicate", "duplicate_iteration"),
                ("unknown", "unknown_iteration"), ("invalid_decision", "invalid_iteration_decision"),
                ("uncertain_complete", "uncertain_iteration_in_complete_review"), ("both", "review_forms_exclusive")):
            with self.subTest(suffix=suffix):
                Provider.behavior = "loops_detailed_" + suffix
                report = self.evaluate(code=3, name=suffix)
                self.assertEqual(report["trials"][0]["checks"][-1]["reason"], "evaluation_loop_review_invalid:" + constraint)
                self.assertIsNone(report["summary"]["invalid_loop_assessment"]["mean"])

    def test_detailed_uncertain_review_never_becomes_a_partial_or_zero_count(self):
        self.model(); self.mode("loop_trace")
        self.config["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
        Provider.behavior = "loops_detailed_uncertain"
        report = self.evaluate()
        review = report["trials"][0]["evidence"]["loop_review"]
        self.assertEqual(review["status"], "uncertain")
        self.assertIsNone(review["count"])
        self.assertEqual(review["wasted_iterations"], [])
        self.assertEqual(len(review["iteration_reviews"]), 4)
        self.assertEqual(report["summary"]["invalid_loop_assessment"]["observations"], 0)

    def test_loop_review_preserves_deterministic_failure(self):
        self.model(); self.mode("loop_trace")
        self.config["checks"]["*"][0]["value"] = 99
        self.config["judge"].update(loop_review_prompt="prompts/task-loop-review.txt", max_repair_attempts=0)
        for behavior, expected in (("loops", 1), ("loops_uncertain", None), ("loops_unknown", None)):
            with self.subTest(behavior=behavior):
                Provider.behavior = behavior
                report = self.evaluate(code=1, name=behavior)
                trial = report["trials"][0]
                self.assertEqual(trial["verdict"], "failed")
                self.assertEqual(report["summary"]["pass_rate"], 0)
                self.assertEqual(report["summary"]["model_calls"], 1)
                self.assertEqual(trial["evidence"]["loop_review"]["count"], expected)

    def test_loop_review_allows_prior_causes_with_anchor_but_not_future_evidence(self):
        self.model(); self.mode("loop_trace")
        self.config["judge"].update(loop_review_prompt="prompts/task-loop-review.txt", max_repair_attempts=0)
        for behavior, code in (("loops_prior_event", 0), ("loops_future_event", 3)):
            with self.subTest(behavior=behavior):
                Provider.behavior = behavior
                report = self.evaluate(code=code, name=behavior)
                trial = report["trials"][0]
                self.assertEqual(trial["checks"][-1]["status"], "passed" if code == 0 else "error")
                self.assertEqual(report["summary"]["model_calls"], 1)
                self.assertEqual(trial["evidence"]["loop_review"]["count"], 1 if code == 0 else None)

    def test_loop_review_uses_causal_order_for_unordered_trace_input(self):
        self.model(); self.mode("loop_trace_unordered")
        self.config["judge"].update(loop_review_prompt="prompts/task-loop-review.txt", max_repair_attempts=0)
        for behavior, code in (("loops_prior_event", 0), ("loops_future_event", 3)):
            Provider.behavior = behavior
            report = self.evaluate(code=code, name=behavior)
            self.assertEqual(report["trials"][0]["evidence"]["loop_review"]["count"], 1 if code == 0 else None)

    def test_loop_review_accepts_joined_child_but_not_unrelated_or_future_events(self):
        self.model()
        self.config["judge"].update(loop_review_prompt="prompts/task-loop-review.txt", max_repair_attempts=0)
        for mode in ("loop_trace_join", "loop_trace_join_unordered"):
            self.mode(mode)
            for behavior, constraint in (("loops_join_child", None),
                    ("loops_join_without_anchor", "missing_iteration_anchor"),
                    ("loops_join_unrelated", "event_not_in_iteration_or_prior_context"),
                    ("loops_join_future", "event_not_in_iteration_or_prior_context")):
                with self.subTest(mode=mode, behavior=behavior):
                    Provider.behavior = behavior
                    report = self.evaluate(code=3 if constraint else 0, name=mode + behavior)
                    trial = report["trials"][0]
                    review = trial["evidence"]["loop_review"]
                    self.assertEqual(review["count"], None if constraint else 1)
                    inventory = json.loads(REQUESTS[-1]["messages"][1]["content"])["loop_inventory"]
                    self.assertEqual(len(inventory), 4)
                    if constraint:
                        self.assertEqual(review["validation_error"]["constraint"], constraint)
                    else:
                        current = next(item for item in inventory.values() if
                            hashlib.sha256(json.dumps([item["run_id"], item["execution_id"], item["loop_id"],
                                str(item["iteration"])], separators=(",", ":")).encode()).hexdigest()
                            == review["wasted_iterations"][0]["iteration_id"])
                        self.assertEqual(review["wasted_iterations"][0]["evidence_event_ids"],
                                         [current["event_ids"][0], "right-2-action.end"])
                    self.assertEqual(report["summary"]["model_calls"], 1)

    def test_uncertain_or_unobserved_loop_review_is_not_zero(self):
        self.model()
        Provider.behavior = "loops_uncertain"
        self.config["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
        for mode in ("loop_trace", "trace"):
            self.mode(mode)
            report = self.evaluate(name=mode)
            self.assertEqual(report["trials"][0]["verdict"], "passed")
            self.assertIsNone(report["trials"][0]["evidence"]["loop_review"]["count"])
            self.assertEqual(report["summary"]["invalid_loop_assessment"]["observations"], 0)

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
        feedback = json.loads(REQUESTS[-1]["messages"][1]["content"])["validation_feedback"]
        self.assertEqual(feedback["code"], "invalid_json")
        self.assertEqual(feedback["json_error"], dict(parser_code=3, description="Invalid value.",
                                                   offset=0, response_bytes=len("invalid response")))
        self.assertNotIn("previous_response", feedback)
        self.assertNotIn("judge_validation_error", report["trials"][0]["evidence"])

    def test_semantic_repair_can_be_disabled(self):
        self.model()
        Provider.behavior = "repair"
        self.config["judge"]["max_repair_attempts"] = 0
        report = self.evaluate(code=3)
        self.assertEqual(report["summary"]["model_calls"], 1)
        diagnostic = report["trials"][0]["evidence"]["judge_validation_error"]
        self.assertEqual(diagnostic["code"], "invalid_json")
        self.assertEqual(diagnostic["json_error"], dict(parser_code=3, description="Invalid value.",
                                                     offset=0, response_bytes=len("invalid response")))
        self.assertNotIn("invalid response", json.dumps(diagnostic))

    def test_model_uncertain_error_and_forged_reference(self):
        self.model()
        for mode in ("uncertain", "reference", "leak", "length", "http_error"):
            Provider.behavior = mode
            report = self.evaluate(code=3, name=mode)
            self.assertNotEqual(report["trials"][0]["verdict"], "passed")

    def test_model_can_cite_existing_descendants_but_not_missing_or_foreign_paths(self):
        self.model()
        self.mode("trace")
        self.config["judge"]["max_repair_attempts"] = 0
        for mode, valid in (("child_reference", True), ("array_reference", True),
                            ("missing_child_reference", False), ("prefix_reference", False),
                            ("encoded_root_reference", False), ("packet_reference", False),
                            ("missing_array_reference", False)):
            with self.subTest(mode=mode):
                Provider.behavior = mode
                report = self.evaluate(code=0 if valid else 3, name=mode)
                check = report["trials"][0]["checks"][-1]
                self.assertEqual(check["status"], "passed" if valid else "error")
                if not valid:
                    self.assertEqual(check["reason"], "evaluation_judge_evidence_invalid")
                    diagnostic = report["trials"][0]["evidence"]["judge_validation_error"]
                    self.assertEqual(diagnostic["code"], check["reason"])
                    self.assertEqual(len(diagnostic["invalid_evidence_paths"]), 1)
                    self.assertNotIn("previous_response", diagnostic)
                else:
                    self.assertNotIn("judge_validation_error", report["trials"][0]["evidence"])
                self.assertEqual(report["summary"]["model_calls"], 1)

    def test_path_recovery_reports_specific_rejected_paths(self):
        self.model()
        for behavior, path in (("path_repair", "/state/missing"), ("syntax_repair", "/state/~2bad")):
            with self.subTest(behavior=behavior):
                Provider.behavior = behavior
                report = self.evaluate(name=behavior)
                feedback = json.loads(REQUESTS[-1]["messages"][1]["content"])["validation_feedback"]
                self.assertEqual(feedback["code"], "evaluation_judge_evidence_invalid")
                self.assertEqual(feedback["invalid_evidence_paths"], [path])
                self.assertEqual(feedback["previous_response"]["evidence_paths"], ["/output", path])
                self.assertEqual(report["summary"]["model_calls"], 2)
                self.assertEqual(report["trials"][0]["checks"][-1]["evidence_paths"], ["/state"])
                self.assertNotIn("judge_validation_error", report["trials"][0]["evidence"])

    def test_loop_recovery_receives_previous_safe_judgment(self):
        self.model(); self.mode("loop_trace")
        self.config["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
        Provider.behavior = "loops_repair"
        report = self.evaluate()
        feedback = json.loads(REQUESTS[-1]["messages"][1]["content"])["validation_feedback"]
        self.assertEqual(feedback["code"], "evaluation_loop_review_invalid")
        self.assertEqual(feedback["previous_response"]["loop_review"]["wasted_iterations"][0]["iteration_id"], "not-an-iteration")
        self.assertEqual(report["summary"]["model_calls"], 2)
        self.assertEqual(report["trials"][0]["evidence"]["loop_review"]["count"], 1)

    def test_loop_recovery_identifies_constraint_and_item(self):
        self.model(); self.mode("loop_trace")
        self.config["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
        for behavior, constraints, index in (
                ("loops_unknown", {"unknown_iteration"}, 0),
                ("loops_duplicate", {"duplicate_iteration"}, 1),
                ("loops_future_event", {"event_not_in_iteration_or_prior_context"}, 0),
                ("loops_other_event", {"event_not_in_iteration_or_prior_context", "missing_iteration_anchor"}, 0)):
            with self.subTest(behavior=behavior):
                Provider.behavior = behavior
                report = self.evaluate(code=3, name=behavior)
                feedback = json.loads(REQUESTS[-1]["messages"][1]["content"])["validation_feedback"]
                self.assertEqual(feedback["code"], "evaluation_loop_review_invalid")
                detail = feedback["loop_review_error"]
                self.assertIn(detail["constraint"], constraints)
                self.assertEqual(detail["wasted_iteration_index"], index)
                self.assertEqual(report["summary"]["model_calls"], 2)
                self.assertEqual(report["trials"][0]["checks"][-1]["reason"],
                                 "evaluation_loop_review_invalid:" + detail["constraint"])
                self.assertEqual(report["trials"][0]["evidence"]["loop_review"]["validation_error"], detail)
                if behavior == "loops_future_event":
                    packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
                    previous = feedback["previous_response"]["loop_review"]["wasted_iterations"][index]
                    reference = detail["reference"]
                    self.assertEqual(reference["iteration_id"], previous["iteration_id"])
                    self.assertEqual(reference["decision"], "waste")
                    self.assertEqual(reference["evidence_event_index"], 1)
                    self.assertEqual(reference["event_id"], previous["evidence_event_ids"][1])
                    self.assertEqual(reference["allowed_iteration_event_ids"],
                                     packet["loop_inventory"][previous["iteration_id"]]["event_ids"])
                    self.assertNotIn(reference["event_id"], reference["allowed_iteration_event_ids"])

    def test_missing_loop_anchor_recovery_identifies_current_events(self):
        self.model(); self.mode("loop_trace")
        self.config["judge"].update(loop_review_prompt="prompts/task-loop-review.txt", max_repair_attempts=0)
        Provider.behavior = "loops_anchor_repair"
        report = self.evaluate(code=3, name="missing-anchor")
        review = report["trials"][0]["evidence"]["loop_review"]
        self.assertEqual(review["validation_error"]["constraint"], "missing_iteration_anchor")
        self.assertIsNone(review["count"])
        self.assertEqual(report["summary"]["invalid_loop_assessment"]["observations"], 0)
        self.config["judge"]["max_repair_attempts"] = 1
        report = self.evaluate(name="recovered-anchor")
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        feedback = packet["validation_feedback"]
        detail = feedback["loop_review_error"]
        previous = feedback["previous_response"]["loop_review"]["wasted_iterations"][0]
        reference = detail["reference"]
        self.assertEqual(detail["constraint"], "missing_iteration_anchor")
        self.assertEqual(detail["wasted_iteration_index"], 0)
        self.assertEqual(reference["iteration_id"], previous["iteration_id"])
        self.assertEqual(reference["decision"], "waste")
        self.assertEqual(reference["allowed_iteration_event_ids"],
                         packet["loop_inventory"][previous["iteration_id"]]["event_ids"])
        self.assertFalse(set(reference["allowed_iteration_event_ids"]) & set(previous["evidence_event_ids"]))
        self.assertNotIn("reason", reference)
        self.assertEqual(report["summary"]["model_calls"], 2)
        review = report["trials"][0]["evidence"]["loop_review"]
        self.assertEqual(review["count"], 1)
        self.assertNotIn("validation_error", review)

    def test_loop_diagnostics_persist_without_retry_and_do_not_survive_recovery(self):
        self.model(); self.mode("loop_trace")
        self.config["judge"].update(loop_review_prompt="prompts/task-loop-review.txt", max_repair_attempts=0)
        Provider.behavior = "loops_future_event"
        report = self.evaluate(code=3, name="no-retry")
        review = report["trials"][0]["evidence"]["loop_review"]
        self.assertEqual(review["status"], "not_reviewed")
        self.assertIsNone(review["count"])
        self.assertEqual(review["validation_error"]["reference"]["evidence_event_index"], 1)
        self.assertEqual(report["summary"]["model_calls"], 1)
        self.assertEqual(report["summary"]["invalid_loop_assessment"]["observations"], 0)
        self.config["judge"]["max_repair_attempts"] = 1
        Provider.behavior = "loops_repair"
        report = self.evaluate(name="recovered")
        self.assertNotIn("validation_error", report["trials"][0]["evidence"]["loop_review"])
        self.assertEqual(report["summary"]["model_calls"], 2)

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
        slow = json.loads(json.dumps(self.config["variants"][0]))
        slow["id"] = "slow"
        slow["command"][-1] = "sleep"
        self.config["variants"].append(slow)
        self.config["parallelism"] = 1
        self.config["repetitions"] = 3
        self.config["timeout_ms"] = 5000
        write_json(self.root / "evaluation.json", self.config)
        process = subprocess.Popen([str(CLI), "dataset", "run", str(self.dataset), "--config", str(self.root / "evaluation.json"),
                                    "--output-root", str(self.root / "cancel")], env=self.env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            for _ in range(200):
                finished = list((self.root / "cancel/trials").glob("*.json"))
                if finished:
                    break
                time.sleep(.01)
            self.assertEqual(len(finished), 1)
            original = finished[0].read_bytes()
            self.assertEqual(json.loads(original)["verdict"], "passed")
            process.send_signal(signal.SIGINT)
            stdout, stderr = process.communicate(timeout=5)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
        self.assertEqual(process.returncode, 3, stdout + stderr)
        report = json.loads((self.root / "cancel/report.json").read_text())
        self.assertFalse(report["complete"])
        self.assertLess(len(report["trials"]), 6)
        self.assertEqual(report["summary"]["passed"], 1)
        self.assertEqual(report["summary"]["planned_trials"], 6)
        self.assertEqual(report["summary"]["not_started_trials"], 6 - len(report["trials"]))
        self.assertEqual(finished[0].read_bytes(), original)
        self.assertEqual(len(list((self.root / "cancel/trials").glob("*.json"))), len(report["trials"]))
        self.assertFalse(json.loads((self.root / "cancel/manifest.json").read_text())["complete"])


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(TrialTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    write_json(WORK / "test-summary.json", dict(tests=result.testsRun, failures=len(result.failures), errors=len(result.errors),
                                               fake_model_requests=len(REQUESTS), paid_model_requests=0))
    SERVER.shutdown()
    print(f"Test output: {WORK}")
    sys.exit(not result.wasSuccessful())
