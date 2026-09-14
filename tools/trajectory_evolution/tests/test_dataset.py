"""Offline dataset contract tests; the only model endpoint is a local fake."""

import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest


BUILD = Path(sys.argv.pop(1)).resolve()
SOURCE = Path(__file__).resolve().parents[1]
CLI = BUILD / "trajectory_evolution"
WORK = Path(tempfile.mkdtemp(prefix="dataset-tests.", dir=BUILD))
REQUESTS = []
FAKE_KEY = "dataset-test-opaque-credential-987654321"


class Provider(http.server.BaseHTTPRequestHandler):
    behavior = "normal"

    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        REQUESTS.append(request)
        evidence = json.loads(request["messages"][1]["content"])
        count = int(evidence["requested_count"])
        cases = [dict(task_input=f"请处理测试任务 {len(REQUESTS)}-{i}，说明缺失条件。",
                      followup_inputs=[], expected_behavior="正确说明需要补充的条件，不编造结果。",
                      category="precondition", difficulty="medium", initial_state={})
                 for i in range(count)]
        behavior = Provider.behavior
        if behavior == "forge":
            cases[0].update(review_status="reviewed", split="holdout")
        if behavior == "leak":
            cases[0]["expected_behavior"] = FAKE_KEY
        if behavior == "short":
            cases = []
        text = "not-json" if behavior == "invalid" else json.dumps({"cases": cases}, ensure_ascii=False)
        if behavior == "fenced":
            text = "```json\r\n" + text + "\r\n```"
        response = {"choices": [{"finish_reason": "length" if behavior == "length" else "stop",
                                  "message": {"content": text}}]}
        body = json.dumps(response).encode()
        self.send_response(500 if behavior == "http_error" else 200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


SERVER = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
THREAD = threading.Thread(target=SERVER.serve_forever, daemon=True)
THREAD.start()


def dump(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")


def tasks(directory):
    return [json.loads(line) for split in ("train", "val", "holdout")
            for line in (directory / f"{split}.jsonl").read_text().splitlines()]


def case(identifier="one", **changes):
    value = dict(task_id=identifier, group_id=identifier, task_input=f"回答问题 {identifier}。",
                 expected_behavior="根据已知事实回答，不编造缺失信息。", difficulty="easy",
                 category="normal", initial_state={}, review_status="pending")
    value.update(changes)
    return value


class DatasetTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="case.", dir=WORK))
        self.config = json.loads((SOURCE / "config/dataset.json").read_text())
        self.config["generation_prompt"] = str(SOURCE / "config/prompts/dataset-system.txt")
        self.config["runtime_config"] = "runtime.json"
        self.config["sources"] = [{"kind": "golden", "path": "tasks.jsonl"}]
        self.env = os.environ.copy()
        self.env["DATASET_TEST_API_KEY"] = FAKE_KEY
        self.env["OPENAI_BASE_URL"] = "https://example.invalid/do-not-use"
        self.env["OPENAI_MODEL"] = "do-not-use"
        self.scope = self.root / "scope.txt"
        self.scope.write_text("支持中文问答、并行工具和正确的停止条件。", encoding="utf-8")
        shutil.copytree(SOURCE / "config/prompts", self.root / "prompts")
        runtime = json.loads((SOURCE / "config/standard-runtime.json").read_text())
        runtime["model"].update(endpoint=f"http://127.0.0.1:{SERVER.server_port}/chat/completions",
                                model_id="dataset-local-fake", api_key_env="DATASET_TEST_API_KEY",
                                thinking_mode="disabled", timeout_ms=2000, connect_timeout_ms=500,
                                max_retries=0)
        dump(self.root / "runtime.json", runtime)
        Provider.behavior = "normal"
        self.start_requests = len(REQUESTS)
        self.write_cases([case()])

    def write_cases(self, values):
        (self.root / "tasks.jsonl").write_text(
            "\n".join(json.dumps(v, ensure_ascii=False) for v in values) + "\n", encoding="utf-8")

    def cli(self, *args, code=0):
        process = subprocess.run([str(CLI), "dataset", *map(str, args)], env=self.env,
                                 text=True, capture_output=True, timeout=20)
        self.assertEqual(process.returncode, code, process.stdout + process.stderr)
        self.assertNotIn(FAKE_KEY, process.stdout + process.stderr)
        return process

    def build(self, name="output", code=0):
        dump(self.root / "dataset.json", self.config)
        process = self.cli("build", "--config", self.root / "dataset.json",
                           "--output-root", self.root / name, code=code)
        return self.root / name, json.loads(process.stdout) if process.stdout else None

    def synthetic(self, count=3, batch=2):
        self.config["sources"] = [{"kind": "synthetic", "path": "scope.txt", "count": count, "batch_size": batch}]

    def test_starter_and_roundtrip(self):
        self.config["sources"][0]["path"] = str(SOURCE / "examples/evaluation-dataset/tasks.jsonl")
        output, summary = self.build()
        self.assertEqual(summary["task_count"], 24)
        self.assertEqual(summary["splits"], {"train": 14, "val": 5, "holdout": 5})
        self.assertFalse(summary["l4_ready"])
        self.assertEqual(summary["pending_review_count"], 24)
        self.assertEqual(len(REQUESTS), self.start_requests)
        self.cli("check", output)
        self.cli("check", output, "--require-reviewed", code=3)
        self.assertEqual(output.stat().st_mode & 0o777, 0o700)
        for path in output.iterdir():
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    def test_stable_version_and_review_gate(self):
        self.write_cases([case(review_status="reviewed", followup_inputs=["请更正前轮的范围。"],
                               attributes={"custom": {"数字": 12}})])
        first, a = self.build("first")
        second, b = self.build("second")
        self.assertEqual(a["dataset_version"], b["dataset_version"])
        self.assertEqual(tasks(first), tasks(second))
        self.cli("check", first, "--require-reviewed")
        self.assertFalse(a["l4_ready"])

    def test_no_overwrite_before_model_call(self):
        output, _ = self.build()
        self.synthetic()
        self.build(code=2)
        self.assertEqual(len(REQUESTS), self.start_requests)
        self.cli("check", output)

    def test_tampered_split_is_rejected(self):
        output, _ = self.build()
        path = output / "train.jsonl"
        path.write_text(path.read_text() + "\n")
        self.cli("check", output, code=2)

    def test_missing_manifest_is_not_complete(self):
        output, _ = self.build()
        (output / "manifest.json").unlink()
        self.cli("check", output, code=2)

    def test_artifact_symlink_rejected(self):
        output, _ = self.build()
        path = output / "val.jsonl"
        old = path.read_bytes()
        path.unlink()
        (self.root / "replacement").write_bytes(old)
        path.symlink_to(self.root / "replacement")
        self.cli("check", output, code=2)

    def test_duplicate_merges_groups(self):
        self.write_cases([case("a", group_id="family-a"),
                          case("b", group_id="family-b", task_input="回答问题 a。"),
                          case("c", group_id="family-b")])
        output, summary = self.build()
        self.assertEqual(summary["task_count"], 2)
        self.assertEqual(len({x["group_id"] for x in tasks(output)}), 1)
        self.assertEqual(len({x["split"] for x in tasks(output)}), 1)

    def test_duplicate_rubric_conflict_rejected(self):
        self.write_cases([case("a"), case("b", task_input="回答问题 a。", expected_behavior="与前一条冲突的标准。")])
        self.build(code=2)

    def test_conflicting_group_split_rejected(self):
        self.write_cases([case("a", group_id="same", split="train"), case("b", group_id="same", split="holdout")])
        self.build(code=2)

    def test_exposure_cannot_enter_holdout(self):
        self.write_cases([case(split="holdout", provenance={"kind": "golden", "learning_exposed": True})])
        self.build(code=2)

    def test_exposure_propagates_to_entire_family(self):
        self.write_cases([case("a", group_id="same", provenance={"kind": "golden", "learning_exposed": True}),
                          case("b", group_id="same")])
        output, _ = self.build()
        self.assertTrue(all(x["split"] == "train" and x["provenance"]["learning_exposed"] for x in tasks(output)))

    def test_configured_learning_groups_are_isolated(self):
        self.config["learning_groups"] = ["seen"]
        self.write_cases([case(group_id="seen")])
        output, _ = self.build()
        self.assertEqual(tasks(output)[0]["split"], "train")
        self.write_cases([case(group_id="seen", split="val")])
        self.build("conflict", code=2)

    def test_stable_assignment_when_adding_unrelated_cases(self):
        self.write_cases([case("a"), case("b")])
        first, _ = self.build("first")
        before = {x["task_id"]: x["split"] for x in tasks(first)}
        self.write_cases([case("b"), case("c"), case("a")])
        second, _ = self.build("second")
        self.assertEqual(before, {x["task_id"]: x["split"] for x in tasks(second) if x["task_id"] in before})

    def test_bad_row_does_not_remove_valid_sibling(self):
        self.write_cases([case("good"), case("bad", difficulty="unknown")])
        output, summary = self.build(code=3)
        self.assertEqual(summary["task_count"], 1)
        self.assertEqual(summary["rejected_count"], 1)
        self.cli("check", output, code=3)

    def test_duplicate_json_key_rejected(self):
        with (self.root / "tasks.jsonl").open("a") as stream:
            stream.write('{"task_input":"x","task_input":"y"}\n')
        _, summary = self.build(code=3)
        self.assertEqual(summary["task_count"], 1)

    def test_input_credentials_never_persist(self):
        self.write_cases([case("good"), case("bad", task_input=FAKE_KEY),
                          case("nested", initial_state={"headers": {"Authorization": "not-a-real-key"}})])
        output, summary = self.build(code=3)
        self.assertEqual(summary["task_count"], 1)
        for path in output.iterdir():
            self.assertNotIn(FAKE_KEY.encode(), path.read_bytes())

    def test_private_path_rejected(self):
        self.write_cases([case(task_input="请读取 /root/private-user/notes.txt")])
        output, summary = self.build(code=3)
        self.assertEqual(summary["task_count"], 0)
        self.assertNotIn(b"private-user", (output / "manifest.json").read_bytes())

    def test_config_unknown_and_invalid_ratio(self):
        self.config["unknown"] = 1
        self.build("unknown", code=2)
        del self.config["unknown"]
        self.config["split_percent"] = [100, 0, 0]
        self.build("ratio", code=2)

    def test_invalid_budget_and_token_overflow_rejected_before_model(self):
        self.synthetic()
        self.config["budget"]["timeout_ms"] = 0
        self.build("budget", code=2)
        self.config["budget"]["timeout_ms"] = 1800000
        self.config["generation_max_output_tokens"] = 4294975488
        self.build("overflow", code=2)
        self.assertEqual(len(REQUESTS), self.start_requests)

    def test_synthetic_model_and_runtime_settings(self):
        self.synthetic()
        output, summary = self.build()
        self.assertEqual(summary["task_count"], 3)
        self.assertEqual(summary["model_calls"], 2)
        self.assertTrue(all(x["split"] == "train" and x["review_status"] == "pending" for x in tasks(output)))
        for request in REQUESTS[self.start_requests:]:
            self.assertEqual(request["model"], "dataset-local-fake")
            self.assertEqual(request["thinking"], {"type": "disabled"})
            self.assertEqual(request["max_tokens"], 8192)
            self.assertNotIn(FAKE_KEY, json.dumps(request))
        self.cli("check", output)

    def test_fenced_generation_keeps_the_review_gate(self):
        self.synthetic()
        Provider.behavior = "fenced"
        output, summary = self.build()
        self.assertEqual(summary["task_count"], 3)
        self.assertEqual(summary["model_calls"], 2)
        self.assertTrue(all(x["split"] == "train" and x["review_status"] == "pending" for x in tasks(output)))
        self.cli("check", output)

    def test_model_cannot_approve_or_choose_split(self):
        self.synthetic(2, 2)
        Provider.behavior = "forge"
        _, summary = self.build(code=3)
        self.assertEqual(summary["task_count"], 1)

    def test_model_secret_response_rejected(self):
        self.synthetic(1, 1)
        Provider.behavior = "leak"
        output, summary = self.build(code=3)
        self.assertEqual(summary["task_count"], 0)
        for path in output.iterdir():
            self.assertNotIn(FAKE_KEY.encode(), path.read_bytes())

    def test_model_failure_modes_are_explicit(self):
        self.synthetic(1, 1)
        for mode in ("invalid", "short", "length", "http_error"):
            Provider.behavior = mode
            _, summary = self.build(mode, code=3)
            self.assertEqual(summary["task_count"], 0)
            self.assertGreater(summary["rejected_count"], 0)
            if mode == "length":
                manifest = json.loads((self.root / mode / "manifest.json").read_text())
                self.assertIn("dataset_model_output_token_limit_reached",
                              [x["code"] for x in manifest["metadata"]["issues"]])

    def test_model_budget_stops_without_repeating(self):
        self.synthetic(5, 2)
        self.config["max_model_calls"] = 1
        _, summary = self.build(code=3)
        self.assertEqual(summary["task_count"], 2)
        self.assertEqual(len(REQUESTS) - self.start_requests, 1)

    def test_zero_model_budget_sends_nothing(self):
        self.synthetic(1, 1)
        self.config["max_model_calls"] = 0
        _, summary = self.build(code=3)
        self.assertEqual(summary["model_calls"], 0)
        self.assertEqual(len(REQUESTS), self.start_requests)

    def test_failed_initialization_does_not_crash_next_source(self):
        self.synthetic(1, 1)
        self.config["sources"] *= 2
        self.config["generation_prompt"] = "missing.txt"
        _, summary = self.build(code=3)
        self.assertEqual(summary["task_count"], 0)
        self.assertEqual(len(REQUESTS), self.start_requests)

    def test_trace_mining_uses_sanitized_content_and_conversation_group(self):
        base = json.loads((SOURCE / "examples/standard-agent/one.trace.json").read_text())
        base["events"][0]["input"] = "请查询发票，诊断参考值 " + FAKE_KEY
        for i in range(2):
            base["identity"]["run_id"] = f"test-run-{i}"
            dump(self.root / f"{i}.trace.json", base)
        # Explicit files avoid picking up unrelated runtime/config JSON.
        self.config["sources"] = [{"kind": "traces", "path": f"{i}.trace.json"} for i in range(2)]
        output, summary = self.build()
        self.assertEqual(summary["task_count"], 2)
        self.assertEqual(summary["group_count"], 1)
        for request in REQUESTS[self.start_requests:]:
            content = request["messages"][1]["content"]
            self.assertIn("发票", content)
            self.assertNotIn(FAKE_KEY, content)
            self.assertIn("REDACTED", content)
        self.assertTrue(all(x["split"] == "train" for x in tasks(output)))

    def test_invalid_trace_does_not_affect_valid_trace(self):
        base = json.loads((SOURCE / "examples/standard-agent/one.trace.json").read_text())
        dump(self.root / "good.trace.json", base)
        base["events"][1]["event_id"] = base["events"][0]["event_id"]
        dump(self.root / "bad.trace.json", base)
        self.config["sources"] = [{"kind": "traces", "path": f"{name}.trace.json"} for name in ("bad", "good")]
        _, summary = self.build(code=3)
        self.assertEqual(summary["task_count"], 1)
        self.assertEqual(len(REQUESTS) - self.start_requests, 1)

    def test_masked_structured_credentials_do_not_block_safe_mining(self):
        base = json.loads((SOURCE / "examples/standard-agent/one.trace.json").read_text())
        base["events"][0]["input"] = {"request": "查询发票", "api_key": FAKE_KEY}
        dump(self.root / "masked.trace.json", base)
        self.config["sources"] = [{"kind": "traces", "path": "masked.trace.json"}]
        _, summary = self.build()
        self.assertEqual(summary["task_count"], 1)
        content = REQUESTS[-1]["messages"][1]["content"]
        self.assertNotIn(FAKE_KEY, content)
        self.assertNotIn('"api_key"', content)

    def test_legacy_sessionlog_mining(self):
        header = dict(type="dialogue.header", version=2, dialogue_id="d-content",
                      conversation_id="legacy-conversation", user_id="legacy-user")
        values = [("dialogue.start", 0, {"input": "请直接回答你好"}),
                  ("coordinator.activation", 0, {"activation_index": 1}),
                  ("llm.request", 1, {"task_type": "coordinator", "activation_index": 1,
                                      "messages": [{"role": "user", "content": "你好"}]}),
                  ("llm.response", 1, {"response": {"choices": [{"finish_reason": "stop",
                                                                    "message": {"content": "你好"}}]}}),
                  ("loop.control", 1, {"scope": "coordinator", "action": "stop", "reason": "final_response"}),
                  ("dialogue.end", 0, {"status": "normal", "response_text": "你好"})]
        lines = [header]
        for i, (kind, round_i, data) in enumerate(values, 1):
            lines.append(dict(schema_version=2, event_id=f"p:{i}", idempotency_key=f"p:{i}",
                              type=kind, producer_id="p", producer_sequence=i, timestamp_us=5000+i,
                              dialogue_id="d-content", node_id="coordinator_decide", round=round_i, data=data))
        path = self.root / "legacy.jsonl"
        path.write_text("\n".join(json.dumps(x, ensure_ascii=False) for x in lines) + "\n", encoding="utf-8")
        self.config["sources"] = [{"kind": "traces", "path": "legacy.jsonl"}]
        output, summary = self.build()
        self.assertEqual(summary["task_count"], 1)
        self.assertEqual(tasks(output)[0]["split"], "train")

    def test_trace_file_limit_exact_boundary(self):
        folder = self.root / "traces"
        folder.mkdir()
        shutil.copyfile(SOURCE / "examples/standard-agent/one.trace.json", folder / "one.trace.json")
        (folder / "ignored.txt").write_text("not a trace")
        self.config["sources"] = [{"kind": "traces", "path": "traces", "max_files": 1}]
        _, summary = self.build()
        self.assertEqual(summary["task_count"], 1)
        shutil.copyfile(folder / "one.trace.json", folder / "two.trace.json")
        before = len(REQUESTS)
        _, summary = self.build("over-limit", code=3)
        self.assertEqual(summary["model_calls"], 0)
        self.assertEqual(len(REQUESTS), before)

    def test_provenance_survives_import(self):
        self.synthetic(1, 1)
        original, _ = self.build("original")
        imported = tasks(original)[0]
        imported["review_status"] = "reviewed"
        self.write_cases([imported])
        self.config["sources"] = [{"kind": "golden", "path": "tasks.jsonl"}]
        output, _ = self.build("imported")
        task = tasks(output)[0]
        self.assertEqual(task["provenance"]["kind"], "synthetic")
        self.assertTrue(task["provenance"]["learning_exposed"])
        self.assertEqual(task["split"], "train")
        self.assertEqual(len(task["provenance"]["source_fingerprints"]), 2)


if __name__ == "__main__":
    try:
        result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(DatasetTests))
        summary = {"tests": result.testsRun, "failures": len(result.failures), "errors": len(result.errors),
                   "fake_model_requests": len(REQUESTS), "paid_model_requests": 0}
        dump(WORK / "test-summary.json", summary)
        print(json.dumps({"summary": str(WORK / "test-summary.json"), **summary}))
        raise SystemExit(0 if result.wasSuccessful() else 1)
    finally:
        SERVER.shutdown()
        SERVER.server_close()
        THREAD.join()
