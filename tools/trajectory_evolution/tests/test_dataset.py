# test_dataset 回归验证：检查包内协议、边界条件与失败处理。
# 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
# 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

"""离线数据集契约测试，唯一模型端点是本地测试桩。"""

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

    # 测试辅助函数：log_message。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def log_message(self, *_):
        pass

    # 测试辅助函数：do_POST。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        REQUESTS.append(request)
        evidence = json.loads(request["messages"][1]["content"])
        count = int(evidence["requested_count"])
        cases = [
            dict(
                task_input=f"请处理测试任务 {len(REQUESTS)}-{i}，说明缺失条件。",
                followup_inputs=[],
                expected_behavior="正确说明需要补充的条件，不编造结果。",
                category="precondition",
                difficulty="medium",
                initial_state={},
            )
            for i in range(count)
        ]
        behavior = Provider.behavior
        if behavior == "forge":
            cases[0].update(review_status="reviewed", split="holdout")
        if behavior == "leak":
            cases[0]["expected_behavior"] = FAKE_KEY
        if behavior == "short":
            cases = []
        text = (
            "not-json"
            if behavior == "invalid"
            else json.dumps({"cases": cases}, ensure_ascii=False)
        )
        if behavior == "fenced":
            text = "```json\r\n" + text + "\r\n```"
        response = {
            "choices": [
                {
                    "finish_reason": "length" if behavior == "length" else "stop",
                    "message": {"content": text},
                }
            ]
        }
        body = json.dumps(response).encode()
        self.send_response(500 if behavior == "http_error" else 200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


SERVER = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
THREAD = threading.Thread(target=SERVER.serve_forever, daemon=True)
THREAD.start()


# 序列化当前对象为紧凑 JSON，供协议传输或持久化使用。
def dump(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")


# 测试辅助函数：tasks。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def tasks(directory):
    return [
        json.loads(line)
        for split in ("train", "val", "holdout")
        for line in (directory / f"{split}.jsonl").read_text().splitlines()
    ]


# 测试辅助函数：case。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def case(identifier="one", **changes):
    value = dict(
        task_id=identifier,
        group_id=identifier,
        task_input=f"回答问题 {identifier}。",
        expected_behavior="根据已知事实回答，不编造缺失信息。",
        difficulty="easy",
        category="normal",
        initial_state={},
        review_status="pending",
    )
    value.update(changes)
    return value


class DatasetTests(unittest.TestCase):
    # 测试辅助函数：setUp。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="case.", dir=WORK))
        self.config = json.loads((SOURCE / "config/dataset.json").read_text())
        self.config["generation_prompt"] = str(
            SOURCE / "config/prompts/dataset-system.txt"
        )
        self.config["runtime_config"] = "runtime.json"
        self.config["sources"] = [{"kind": "golden", "path": "tasks.jsonl"}]
        self.env = os.environ.copy()
        self.env["DATASET_TEST_API_KEY"] = FAKE_KEY
        self.env["OPENAI_BASE_URL"] = "https://example.invalid/do-not-use"
        self.env["OPENAI_MODEL"] = "do-not-use"
        self.scope = self.root / "scope.txt"
        self.scope.write_text(
            "支持中文问答、并行工具和正确的停止条件。", encoding="utf-8"
        )
        shutil.copytree(SOURCE / "config/prompts", self.root / "prompts")
        runtime = json.loads((SOURCE / "config/standard-runtime.json").read_text())
        runtime["model"].update(
            endpoint=f"http://127.0.0.1:{SERVER.server_port}/chat/completions",
            model_id="dataset-local-fake",
            api_key_env="DATASET_TEST_API_KEY",
            thinking_mode="disabled",
            timeout_ms=2000,
            connect_timeout_ms=500,
            max_retries=0,
        )
        dump(self.root / "runtime.json", runtime)
        Provider.behavior = "normal"
        self.start_requests = len(REQUESTS)
        self.write_cases([case()])

    # 测试辅助函数：write_cases。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def write_cases(self, values):
        (self.root / "tasks.jsonl").write_text(
            "\n".join(json.dumps(v, ensure_ascii=False) for v in values) + "\n",
            encoding="utf-8",
        )

    # 测试辅助函数：cli。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def cli(self, *args, code=0):
        process = subprocess.run(
            [str(CLI), "dataset", *map(str, args)],
            env=self.env,
            text=True,
            capture_output=True,
            timeout=20,
        )
        self.assertEqual(process.returncode, code, process.stdout + process.stderr)
        self.assertNotIn(FAKE_KEY, process.stdout + process.stderr)
        return process

    # 测试辅助函数：build。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def build(self, name="output", code=0):
        dump(self.root / "dataset.json", self.config)
        process = self.cli(
            "build",
            "--config",
            self.root / "dataset.json",
            "--output-root",
            self.root / name,
            code=code,
        )
        return self.root / name, json.loads(process.stdout) if process.stdout else None

    # 测试辅助函数：synthetic。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def synthetic(self, count=3, batch=2):
        self.config["sources"] = [
            {
                "kind": "synthetic",
                "path": "scope.txt",
                "count": count,
                "batch_size": batch,
            }
        ]

    # 回归用例：test_starter_and_roundtrip。
    # 重点核验字段或诊断：pending_review_count；异常路径不能被当成成功。
    def test_starter_and_roundtrip(self):
        self.config["sources"][0]["path"] = str(
            SOURCE / "examples/evaluation-dataset/tasks.jsonl"
        )
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

    # 回归用例：test_stable_version_and_review_gate。
    # 关键断言：self.assertEqual(a["dataset_version"], b["dataset_version"])；此处只验证测试构造的场景。
    def test_stable_version_and_review_gate(self):
        self.write_cases(
            [
                case(
                    review_status="reviewed",
                    followup_inputs=["请更正前轮的范围。"],
                    attributes={"custom": {"数字": 12}},
                )
            ]
        )
        first, a = self.build("first")
        second, b = self.build("second")
        self.assertEqual(a["dataset_version"], b["dataset_version"])
        self.assertEqual(tasks(first), tasks(second))
        self.cli("check", first, "--require-reviewed")
        self.assertFalse(a["l4_ready"])

    # 回归用例：test_no_overwrite_before_model_call。
    # 关键断言：self.assertEqual(len(REQUESTS), self.start_requests)；此处只验证测试构造的场景。
    def test_no_overwrite_before_model_call(self):
        output, _ = self.build()
        self.synthetic()
        self.build(code=2)
        self.assertEqual(len(REQUESTS), self.start_requests)
        self.cli("check", output)

    # 回归用例：test_tampered_split_is_rejected。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_tampered_split_is_rejected(self):
        output, _ = self.build()
        path = output / "train.jsonl"
        path.write_text(path.read_text() + "\n")
        self.cli("check", output, code=2)

    # 回归用例：test_missing_manifest_is_not_complete。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_missing_manifest_is_not_complete(self):
        output, _ = self.build()
        (output / "manifest.json").unlink()
        self.cli("check", output, code=2)

    # 回归用例：test_artifact_symlink_rejected。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_artifact_symlink_rejected(self):
        output, _ = self.build()
        path = output / "val.jsonl"
        old = path.read_bytes()
        path.unlink()
        (self.root / "replacement").write_bytes(old)
        path.symlink_to(self.root / "replacement")
        self.cli("check", output, code=2)

    # 回归用例：test_duplicate_merges_groups。
    # 关键断言：self.assertEqual(summary["task_count"], 2)；此处只验证测试构造的场景。
    def test_duplicate_merges_groups(self):
        self.write_cases(
            [
                case("a", group_id="family-a"),
                case("b", group_id="family-b", task_input="回答问题 a。"),
                case("c", group_id="family-b"),
            ]
        )
        output, summary = self.build()
        self.assertEqual(summary["task_count"], 2)
        self.assertEqual(len({x["group_id"] for x in tasks(output)}), 1)
        self.assertEqual(len({x["split"] for x in tasks(output)}), 1)

    # 回归用例：test_duplicate_rubric_conflict_rejected。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_duplicate_rubric_conflict_rejected(self):
        self.write_cases(
            [
                case("a"),
                case(
                    "b",
                    task_input="回答问题 a。",
                    expected_behavior="与前一条冲突的标准。",
                ),
            ]
        )
        self.build(code=2)

    # 回归用例：test_turn_contract_roundtrip_and_count_validation。
    # 关键断言：self.assertEqual(summary["rejected_count"], 1)；此处只验证测试构造的场景。
    def test_turn_contract_roundtrip_and_count_validation(self):
        expected = [
            dict(turn=1, expected_behavior="保留首轮条件", expected_targets=None),
            dict(
                turn=2,
                expected_behavior="执行新要求",
                expected_targets=["left", "right"],
                expected_intents=["request", "request"],
                expected_effects=["write", "write"],
                conditional=True,
            ),
        ]
        self.write_cases(
            [
                case(
                    "good",
                    followup_inputs=["修改一下"],
                    attributes={"turn_expectations": expected},
                ),
                case("bad", attributes={"turn_expectations": expected}),
            ]
        )
        output, summary = self.build(code=3)
        self.assertEqual(summary["rejected_count"], 1)
        self.assertEqual(tasks(output)[0]["attributes"]["turn_expectations"], expected)

    # 回归用例：test_duplicate_turn_rubric_conflict_is_not_silently_merged。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_duplicate_turn_rubric_conflict_is_not_silently_merged(self):
        self.write_cases(
            [
                case(
                    "a",
                    attributes={
                        "turn_expectations": [dict(turn=1, expected_behavior="执行 A")]
                    },
                ),
                case(
                    "b",
                    task_input="回答问题 a。",
                    attributes={
                        "turn_expectations": [dict(turn=1, expected_behavior="执行 B")]
                    },
                ),
            ]
        )
        self.build(code=2)

    # 回归用例：test_conflicting_group_split_rejected。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_conflicting_group_split_rejected(self):
        self.write_cases(
            [
                case("a", group_id="same", split="train"),
                case("b", group_id="same", split="holdout"),
            ]
        )
        self.build(code=2)

    # 回归用例：test_exposure_cannot_enter_holdout。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_exposure_cannot_enter_holdout(self):
        self.write_cases(
            [
                case(
                    split="holdout",
                    provenance={"kind": "golden", "learning_exposed": True},
                )
            ]
        )
        self.build(code=2)

    # 回归用例：test_exposure_propagates_to_entire_family。
    # 关键断言：self.assertTrue(；此处只验证测试构造的场景。
    def test_exposure_propagates_to_entire_family(self):
        self.write_cases(
            [
                case(
                    "a",
                    group_id="same",
                    provenance={"kind": "golden", "learning_exposed": True},
                ),
                case("b", group_id="same"),
            ]
        )
        output, _ = self.build()
        self.assertTrue(
            all(
                x["split"] == "train" and x["provenance"]["learning_exposed"]
                for x in tasks(output)
            )
        )

    # 回归用例：test_configured_learning_groups_are_isolated。
    # 关键断言：self.assertEqual(tasks(output)[0]["split"], "train")；此处只验证测试构造的场景。
    def test_configured_learning_groups_are_isolated(self):
        self.config["learning_groups"] = ["seen"]
        self.write_cases([case(group_id="seen")])
        output, _ = self.build()
        self.assertEqual(tasks(output)[0]["split"], "train")
        self.write_cases([case(group_id="seen", split="val")])
        self.build("conflict", code=2)

    # 回归用例：test_stable_assignment_when_adding_unrelated_cases。
    # 关键断言：self.assertEqual(；此处只验证测试构造的场景。
    def test_stable_assignment_when_adding_unrelated_cases(self):
        self.write_cases([case("a"), case("b")])
        first, _ = self.build("first")
        before = {x["task_id"]: x["split"] for x in tasks(first)}
        self.write_cases([case("b"), case("c"), case("a")])
        second, _ = self.build("second")
        self.assertEqual(
            before,
            {x["task_id"]: x["split"] for x in tasks(second) if x["task_id"] in before},
        )

    # 回归用例：test_bad_row_does_not_remove_valid_sibling。
    # 关键断言：self.assertEqual(summary["task_count"], 1)；此处只验证测试构造的场景。
    def test_bad_row_does_not_remove_valid_sibling(self):
        self.write_cases([case("good"), case("bad", difficulty="unknown")])
        output, summary = self.build(code=3)
        self.assertEqual(summary["task_count"], 1)
        self.assertEqual(summary["rejected_count"], 1)
        self.cli("check", output, code=3)

    # 回归用例：test_duplicate_json_key_rejected。
    # 关键断言：self.assertEqual(summary["task_count"], 1)；此处只验证测试构造的场景。
    def test_duplicate_json_key_rejected(self):
        with (self.root / "tasks.jsonl").open("a") as stream:
            stream.write('{"task_input":"x","task_input":"y"}\n')
        _, summary = self.build(code=3)
        self.assertEqual(summary["task_count"], 1)

    # 回归用例：test_input_credentials_never_persist。
    # 关键断言：self.assertEqual(summary["task_count"], 1)；此处只验证测试构造的场景。
    def test_input_credentials_never_persist(self):
        self.write_cases(
            [
                case("good"),
                case("bad", task_input=FAKE_KEY),
                case(
                    "nested",
                    initial_state={"headers": {"Authorization": "not-a-real-key"}},
                ),
            ]
        )
        output, summary = self.build(code=3)
        self.assertEqual(summary["task_count"], 1)
        for path in output.iterdir():
            self.assertNotIn(FAKE_KEY.encode(), path.read_bytes())

    # 回归用例：test_private_path_rejected。
    # 关键断言：self.assertEqual(summary["task_count"], 0)；此处只验证测试构造的场景。
    def test_private_path_rejected(self):
        self.write_cases([case(task_input="请读取 /root/private-user/notes.txt")])
        output, summary = self.build(code=3)
        self.assertEqual(summary["task_count"], 0)
        self.assertNotIn(b"private-user", (output / "manifest.json").read_bytes())

    # 回归用例：test_config_unknown_and_invalid_ratio。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_config_unknown_and_invalid_ratio(self):
        self.config["unknown"] = 1
        self.build("unknown", code=2)
        del self.config["unknown"]
        self.config["split_percent"] = [100, 0, 0]
        self.build("ratio", code=2)

    # 回归用例：test_invalid_budget_and_token_overflow_rejected_before_model。
    # 重点核验字段或诊断：generation_max_output_tokens；异常路径不能被当成成功。
    def test_invalid_budget_and_token_overflow_rejected_before_model(self):
        self.synthetic()
        self.config["budget"]["timeout_ms"] = 0
        self.build("budget", code=2)
        self.config["budget"]["timeout_ms"] = 1800000
        self.config["generation_max_output_tokens"] = 4294975488
        self.build("overflow", code=2)
        self.assertEqual(len(REQUESTS), self.start_requests)

    # 回归用例：test_synthetic_model_and_runtime_settings。
    # 关键断言：self.assertEqual(summary["task_count"], 3)；此处只验证测试构造的场景。
    def test_synthetic_model_and_runtime_settings(self):
        self.synthetic()
        output, summary = self.build()
        self.assertEqual(summary["task_count"], 3)
        self.assertEqual(summary["model_calls"], 2)
        self.assertTrue(
            all(
                x["split"] == "train" and x["review_status"] == "pending"
                for x in tasks(output)
            )
        )
        for request in REQUESTS[self.start_requests :]:
            self.assertEqual(request["model"], "dataset-local-fake")
            self.assertEqual(request["thinking"], {"type": "disabled"})
            self.assertEqual(request["max_tokens"], 8192)
            self.assertNotIn(FAKE_KEY, json.dumps(request))
        self.cli("check", output)

    # 回归用例：test_fenced_generation_keeps_the_review_gate。
    # 关键断言：self.assertEqual(summary["task_count"], 3)；此处只验证测试构造的场景。
    def test_fenced_generation_keeps_the_review_gate(self):
        self.synthetic()
        Provider.behavior = "fenced"
        output, summary = self.build()
        self.assertEqual(summary["task_count"], 3)
        self.assertEqual(summary["model_calls"], 2)
        self.assertTrue(
            all(
                x["split"] == "train" and x["review_status"] == "pending"
                for x in tasks(output)
            )
        )
        self.cli("check", output)

    # 回归用例：test_model_cannot_approve_or_choose_split。
    # 关键断言：self.assertEqual(summary["task_count"], 1)；此处只验证测试构造的场景。
    def test_model_cannot_approve_or_choose_split(self):
        self.synthetic(2, 2)
        Provider.behavior = "forge"
        _, summary = self.build(code=3)
        self.assertEqual(summary["task_count"], 1)

    # 回归用例：test_model_secret_response_rejected。
    # 关键断言：self.assertEqual(summary["task_count"], 0)；此处只验证测试构造的场景。
    def test_model_secret_response_rejected(self):
        self.synthetic(1, 1)
        Provider.behavior = "leak"
        output, summary = self.build(code=3)
        self.assertEqual(summary["task_count"], 0)
        for path in output.iterdir():
            self.assertNotIn(FAKE_KEY.encode(), path.read_bytes())

    # 回归用例：test_model_failure_modes_are_explicit。
    # 重点核验字段或诊断：dataset_model_output_token_limit_reached；异常路径不能被当成成功。
    def test_model_failure_modes_are_explicit(self):
        self.synthetic(1, 1)
        for mode in ("invalid", "short", "length", "http_error"):
            Provider.behavior = mode
            _, summary = self.build(mode, code=3)
            self.assertEqual(summary["task_count"], 0)
            self.assertGreater(summary["rejected_count"], 0)
            if mode == "length":
                manifest = json.loads((self.root / mode / "manifest.json").read_text())
                self.assertIn(
                    "dataset_model_output_token_limit_reached",
                    [x["code"] for x in manifest["metadata"]["issues"]],
                )

    # 回归用例：test_model_budget_stops_without_repeating。
    # 重点核验字段或诊断：max_model_calls；异常路径不能被当成成功。
    def test_model_budget_stops_without_repeating(self):
        self.synthetic(5, 2)
        self.config["max_model_calls"] = 1
        _, summary = self.build(code=3)
        self.assertEqual(summary["task_count"], 2)
        self.assertEqual(len(REQUESTS) - self.start_requests, 1)

    # 回归用例：test_zero_model_budget_sends_nothing。
    # 重点核验字段或诊断：max_model_calls；异常路径不能被当成成功。
    def test_zero_model_budget_sends_nothing(self):
        self.synthetic(1, 1)
        self.config["max_model_calls"] = 0
        _, summary = self.build(code=3)
        self.assertEqual(summary["model_calls"], 0)
        self.assertEqual(len(REQUESTS), self.start_requests)

    # 回归用例：test_failed_initialization_does_not_crash_next_source。
    # 关键断言：self.assertEqual(summary["task_count"], 0)；此处只验证测试构造的场景。
    def test_failed_initialization_does_not_crash_next_source(self):
        self.synthetic(1, 1)
        self.config["sources"] *= 2
        self.config["generation_prompt"] = "missing.txt"
        _, summary = self.build(code=3)
        self.assertEqual(summary["task_count"], 0)
        self.assertEqual(len(REQUESTS), self.start_requests)

    # 回归用例：test_trace_mining_uses_sanitized_content_and_conversation_group。
    # 关键断言：self.assertEqual(summary["task_count"], 2)；此处只验证测试构造的场景。
    def test_trace_mining_uses_sanitized_content_and_conversation_group(self):
        base = json.loads(
            (SOURCE / "examples/standard-agent/one.trace.json").read_text()
        )
        base["events"][0]["input"] = "请查询发票，诊断参考值 " + FAKE_KEY
        for i in range(2):
            base["identity"]["run_id"] = f"test-run-{i}"
            dump(self.root / f"{i}.trace.json", base)
        # 显式选择文件，避免误读无关运行时或配置 JSON。
        self.config["sources"] = [
            {"kind": "traces", "path": f"{i}.trace.json"} for i in range(2)
        ]
        output, summary = self.build()
        self.assertEqual(summary["task_count"], 2)
        self.assertEqual(summary["group_count"], 1)
        for request in REQUESTS[self.start_requests :]:
            content = request["messages"][1]["content"]
            self.assertIn("发票", content)
            self.assertNotIn(FAKE_KEY, content)
            self.assertIn("REDACTED", content)
        self.assertTrue(all(x["split"] == "train" for x in tasks(output)))

    # 回归用例：test_invalid_trace_does_not_affect_valid_trace。
    # 关键断言：self.assertEqual(summary["task_count"], 1)；此处只验证测试构造的场景。
    def test_invalid_trace_does_not_affect_valid_trace(self):
        base = json.loads(
            (SOURCE / "examples/standard-agent/one.trace.json").read_text()
        )
        dump(self.root / "good.trace.json", base)
        base["events"][1]["event_id"] = base["events"][0]["event_id"]
        dump(self.root / "bad.trace.json", base)
        self.config["sources"] = [
            {"kind": "traces", "path": f"{name}.trace.json"} for name in ("bad", "good")
        ]
        _, summary = self.build(code=3)
        self.assertEqual(summary["task_count"], 1)
        self.assertEqual(len(REQUESTS) - self.start_requests, 1)

    # 回归用例：test_masked_structured_credentials_do_not_block_safe_mining。
    # 关键断言：self.assertEqual(summary["task_count"], 1)；此处只验证测试构造的场景。
    def test_masked_structured_credentials_do_not_block_safe_mining(self):
        base = json.loads(
            (SOURCE / "examples/standard-agent/one.trace.json").read_text()
        )
        base["events"][0]["input"] = {"request": "查询发票", "api_key": FAKE_KEY}
        dump(self.root / "masked.trace.json", base)
        self.config["sources"] = [{"kind": "traces", "path": "masked.trace.json"}]
        _, summary = self.build()
        self.assertEqual(summary["task_count"], 1)
        content = REQUESTS[-1]["messages"][1]["content"]
        self.assertNotIn(FAKE_KEY, content)
        self.assertNotIn('"api_key"', content)

    # 回归用例：test_legacy_sessionlog_mining。
    # 关键断言：self.assertEqual(summary["task_count"], 1)；此处只验证测试构造的场景。
    def test_legacy_sessionlog_mining(self):
        header = dict(
            type="dialogue.header",
            version=2,
            dialogue_id="d-content",
            conversation_id="legacy-conversation",
            user_id="legacy-user",
        )
        values = [
            ("dialogue.start", 0, {"input": "请直接回答你好"}),
            ("coordinator.activation", 0, {"activation_index": 1}),
            (
                "llm.request",
                1,
                {
                    "task_type": "coordinator",
                    "activation_index": 1,
                    "messages": [{"role": "user", "content": "你好"}],
                },
            ),
            (
                "llm.response",
                1,
                {
                    "response": {
                        "choices": [
                            {"finish_reason": "stop", "message": {"content": "你好"}}
                        ]
                    }
                },
            ),
            (
                "loop.control",
                1,
                {"scope": "coordinator", "action": "stop", "reason": "final_response"},
            ),
            ("dialogue.end", 0, {"status": "normal", "response_text": "你好"}),
        ]
        lines = [header]
        for i, (kind, round_i, data) in enumerate(values, 1):
            lines.append(
                dict(
                    schema_version=2,
                    event_id=f"p:{i}",
                    idempotency_key=f"p:{i}",
                    type=kind,
                    producer_id="p",
                    producer_sequence=i,
                    timestamp_us=5000 + i,
                    dialogue_id="d-content",
                    node_id="coordinator_decide",
                    round=round_i,
                    data=data,
                )
            )
        path = self.root / "legacy.jsonl"
        path.write_text(
            "\n".join(json.dumps(x, ensure_ascii=False) for x in lines) + "\n",
            encoding="utf-8",
        )
        self.config["sources"] = [{"kind": "traces", "path": "legacy.jsonl"}]
        output, summary = self.build()
        self.assertEqual(summary["task_count"], 1)
        self.assertEqual(tasks(output)[0]["split"], "train")

    # 回归用例：test_trace_file_limit_exact_boundary。
    # 关键断言：self.assertEqual(summary["task_count"], 1)；此处只验证测试构造的场景。
    def test_trace_file_limit_exact_boundary(self):
        folder = self.root / "traces"
        folder.mkdir()
        shutil.copyfile(
            SOURCE / "examples/standard-agent/one.trace.json", folder / "one.trace.json"
        )
        (folder / "ignored.txt").write_text("not a trace")
        self.config["sources"] = [{"kind": "traces", "path": "traces", "max_files": 1}]
        _, summary = self.build()
        self.assertEqual(summary["task_count"], 1)
        shutil.copyfile(folder / "one.trace.json", folder / "two.trace.json")
        before = len(REQUESTS)
        _, summary = self.build("over-limit", code=3)
        self.assertEqual(summary["model_calls"], 0)
        self.assertEqual(len(REQUESTS), before)

    # 回归用例：test_provenance_survives_import。
    # 关键断言：self.assertEqual(task["provenance"]["kind"], "synthetic")；此处只验证测试构造的场景。
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

    # GEPA 视图只引用原 val/holdout；原 train 不出现，原数据集也不被改写。
    def test_gepa_view_reuses_val_and_holdout(self):
        self.write_cases([
            case("training", split="train"),
            case("search", split="val"),
            case("selection", split="val"),
            case("final", split="holdout"),
        ])
        source, _ = self.build()
        original = {task["task_id"]: task for task in tasks(source)}
        view_file = self.root / "gepa-view.json"
        view = {
            "schema_version": 1,
            "source": str(source),
            "source_version": json.loads((source / "manifest.json").read_text())[
                "metadata"]["dataset_version"],
            "search_task_ids": ["search"],
            "selection_task_ids": ["selection"],
        }
        dump(view_file, view)
        summary = json.loads(self.cli("check", view_file, "--require-reviewed").stdout)
        self.assertEqual(summary["splits"], {"train": 1, "val": 1, "holdout": 1})
        self.assertEqual(summary["task_count"], 3)
        self.assertEqual(original["training"]["split"], "train")
        self.assertEqual(original["search"]["review_status"], "pending")
        view["search_task_ids"] = ["training"]
        dump(view_file, view)
        failed = subprocess.run([str(CLI), "dataset", "check", str(view_file)],
                                capture_output=True, text=True, env=self.env)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("dataset_view", failed.stderr)

    # 构造带真实逐轮契约的最小视图；覆盖文件只保存新增子目标，不复制任务正文。
    def goal_view_fixture(self, final_has_turns=True):
        single_turn = {"turn_expectations": [{
            "turn": 1, "expected_behavior": "查询并如实报告结果。",
            "criteria": [{"id": "query", "expected_behavior": "查询并如实报告结果。"}]}]}
        self.write_cases([
            case("training", split="train"),
            case("search", split="val", initial_state={"value": 20},
                 followup_inputs=["请确认第二项。"], attributes={
                     "turn_expectations": [
                         {"turn": 1, "expected_behavior": "完成第一项。",
                          "expected_targets": ["query"], "conditional": False},
                         {"turn": 2, "expected_behavior": "完成第二项。", "criteria": [
                             {"id": "second", "expected_behavior": "第二项正确。"}]}]}),
            case("selection", split="val", initial_state={"value": 20}, attributes=single_turn),
            case("final", split="holdout", initial_state={"value": 20},
                 attributes=single_turn if final_has_turns else {}),
        ])
        source, _ = self.build()
        original = {task["task_id"]: task for task in tasks(source)}
        version = json.loads((source / "manifest.json").read_text())["metadata"]["dataset_version"]
        view = {"schema_version": 1, "source": str(source), "source_version": version,
                "search_task_ids": ["search"], "selection_task_ids": ["selection"]}
        contracts = {"schema_version": 1, "source_version": version, "tasks": [{
            "task_id": "search", "task_version": original["search"]["task_version"],
            "turn_expectations": [{"turn": 1, "criteria": [
                {"id": "first-a", "expected_behavior": "第一项的查询正确。", "weight": 1},
                {"id": "first-b", "expected_behavior": "第一项的结果如实报告。", "weight": 2},
            ]}, {"turn": 2, "criteria": [
                {"id": "second", "expected_behavior": "第二项正确。"}]}]}]}
        for task_id in ("selection", "final"):
            contracts["tasks"].append({
                "task_id": task_id, "task_version": original[task_id]["task_version"],
                "turn_expectations": [{"turn": 1, "criteria":
                    single_turn["turn_expectations"][0]["criteria"]}]})
        return source, original, view, contracts

    # 覆盖改变内存任务和视图的版本；子目标内容没变的任务版本与原数据集所有字节保持不变。
    # 使用本地确定性宿主读取正式评测报告，不调用模型，也不改写源数据集。
    def test_gepa_view_goal_contracts_versions_and_source_isolation(self):
        source, original, view, contracts = self.goal_view_fixture()
        source_bytes = {p.name: p.read_bytes() for p in source.iterdir() if p.is_file()}
        view_file = self.root / "gepa-view.json"
        goals_file = self.root / "goals.json"
        evaluation = {
            "schema_version": 1, "splits": ["train", "val", "holdout"],
            "repetitions": 1, "parallelism": 2, "timeout_ms": 3000,
            "max_output_bytes": 65536, "judge": {"enabled": False},
            "variants": [{"id": "baseline", "version": "test-v1", "command": [
                sys.executable, "-B", str(SOURCE / "tests/evaluation_host.py"), "good"]}],
            "checks": {"*": [{"id": "value", "path": "/state/value",
                               "op": "equals", "value": 20}]},
        }
        config = self.root / "evaluation.json"
        dump(config, evaluation)

        def evaluate(name):
            dump(view_file, view)
            # 此测试只检查冻结任务身份，不让 state.value 检查冒充完整自然语言验收；
            # 禁用模型且缺少逐目标裁判时，正式入口应返回 uncertain（退出码 3）。
            self.cli("run", view_file, "--config", config,
                     "--output-root", self.root / name, code=3)
            report = json.loads((self.root / name / "report.json").read_text())
            self.assertTrue(all(row["verdict"] == "uncertain" for row in report["trials"]))
            return report

        baseline = evaluate("before-goals")
        original_summary = json.loads(self.cli("check", view_file).stdout)
        dump(goals_file, contracts)
        view["goal_contracts_file"] = goals_file.name
        updated = evaluate("with-goals")
        new_summary = json.loads(self.cli("check", view_file).stdout)
        self.assertNotEqual(original_summary["dataset_version"], new_summary["dataset_version"])
        before = {row["task_id"]: row["task_version"] for row in baseline["trials"]}
        after = {row["task_id"]: row["task_version"] for row in updated["trials"]}
        self.assertNotEqual(before["search"], after["search"])
        for task_id in ("selection", "final"):
            self.assertEqual(before[task_id], after[task_id])
        self.assertEqual(source_bytes, {p.name: p.read_bytes() for p in source.iterdir() if p.is_file()})
        self.assertEqual(original, {task["task_id"]: task for task in tasks(source)})

        # 路径名称不是评分语义；同内容换文件名不改变版本，修改子目标内容必须改变。
        dump(self.root / "same-goals.json", contracts)
        view["goal_contracts_file"] = "same-goals.json"
        dump(view_file, view)
        self.assertEqual(new_summary["dataset_version"],
                         json.loads(self.cli("check", view_file).stdout)["dataset_version"])
        contracts["tasks"][0]["turn_expectations"][0]["criteria"][0]["weight"] = 3
        dump(self.root / "same-goals.json", contracts)
        self.assertNotEqual(new_summary["dataset_version"],
                            json.loads(self.cli("check", view_file).stdout)["dataset_version"])
        view.pop("goal_contracts_file")
        dump(view_file, view)
        self.assertEqual(original_summary, json.loads(self.cli("check", view_file).stdout))
        self.assertEqual(len(REQUESTS), self.start_requests)

    # 在读取视图时拒绝越权字段、旧版本、未知或重复身份及非法子目标，不等到启动 Agent。
    def test_gepa_view_goal_contracts_reject_invalid_data(self):
        _, original, view, contracts = self.goal_view_fixture()
        view_file = self.root / "gepa-view.json"
        goals_file = self.root / "goals.json"
        view["goal_contracts_file"] = goals_file.name
        dump(view_file, view)

        def changed(mutate):
            value = json.loads(json.dumps(contracts))
            mutate(value)
            return value

        invalid = {
            "schema": changed(lambda x: x.update(schema_version=2)),
            "source": changed(lambda x: x.update(source_version="old-dataset")),
            "unknown-task": changed(lambda x: x["tasks"][0].update(task_id="unknown")),
            "duplicate-task": changed(lambda x: x["tasks"].append(x["tasks"][0])),
            "old-task": changed(lambda x: x["tasks"][0].update(task_version="old-task")),
            "training-task": changed(lambda x: x["tasks"][0].update(
                task_id="training", task_version=original["training"]["task_version"])),
            "missing-task": changed(lambda x: x["tasks"].pop()),
            "missing-turn": changed(lambda x: x["tasks"][0]["turn_expectations"].pop()),
            "change-input": changed(lambda x: x["tasks"][0].update(task_input="changed")),
            "change-split": changed(lambda x: x["tasks"][0].update(split="train")),
            "empty-turns": changed(lambda x: x["tasks"][0].update(turn_expectations=[])),
            "duplicate-turn": changed(lambda x: x["tasks"][0]["turn_expectations"].append(
                x["tasks"][0]["turn_expectations"][0])),
        }
        for turn in (0, 3, 1.5, "1", True):
            invalid[f"turn-{turn!r}"] = changed(
                lambda x, turn=turn: x["tasks"][0]["turn_expectations"][0].update(turn=turn))
        for name, fields in {
            "change-expected": {"expected_behavior": "changed"},
            "empty-criteria": {"criteria": []},
            "null-criteria": {"criteria": None},
            "unknown-criterion-field": {"criteria": [{"expected_behavior": "结果正确。", "x": True}]},
            "zero-weight": {"criteria": [{"expected_behavior": "结果正确。", "weight": 0}]},
            "duplicate-goal-id": {"criteria": [{"id": "second", "expected_behavior": "结果正确。"}]},
            "wrong-deadline": {"criteria": [{"expected_behavior": "结果正确。", "due_turn": 3}]},
        }.items():
            invalid[name] = changed(
                lambda x, fields=fields: x["tasks"][0]["turn_expectations"][0].update(fields))
        for name, value in invalid.items():
            with self.subTest(name=name):
                dump(goals_file, value)
                self.cli("check", view_file, code=2)
        self.assertEqual(len(REQUESTS), self.start_requests)

    # 即使外部文件声称已覆盖整题，也不能给源数据里不存在的逐轮契约发明新轮次。
    def test_gepa_view_goal_contracts_require_original_turns(self):
        _, _, view, contracts = self.goal_view_fixture(final_has_turns=False)
        view["goal_contracts_file"] = "goals.json"
        dump(self.root / "goals.json", contracts)
        dump(self.root / "gepa-view.json", view)
        failed = self.cli("check", self.root / "gepa-view.json", code=2)
        self.assertIn("dataset_view_goal_turn_invalid", failed.stderr)

    # 覆盖文件受有界读取和逐层符号链接校验约束；不能借父目录链接绕过叶文件检查。
    def test_gepa_view_goal_contracts_paths_and_size(self):
        _, _, view, contracts = self.goal_view_fixture()
        view_file = self.root / "gepa-view.json"
        goals_file = self.root / "goals.json"
        dump(goals_file, contracts)
        (self.root / "goal-link.json").symlink_to(goals_file)
        (self.root / "goal-dir-link").symlink_to(self.root, target_is_directory=True)
        oversized = self.root / "oversized-goals.json"
        with oversized.open("wb") as stream:
            stream.truncate(64 * 1024 * 1024 + 1)
        for reference in (None, "", str(goals_file), "goal-link.json",
                          "goal-dir-link/goals.json", oversized.name):
            with self.subTest(reference=reference):
                view["goal_contracts_file"] = reference
                dump(view_file, view)
                self.cli("check", view_file, code=2)
        self.assertEqual(len(REQUESTS), self.start_requests)

if __name__ == "__main__":
    try:
        result = unittest.TextTestRunner(verbosity=2).run(
            unittest.defaultTestLoader.loadTestsFromTestCase(DatasetTests)
        )
        summary = {
            "tests": result.testsRun,
            "failures": len(result.failures),
            "errors": len(result.errors),
            "fake_model_requests": len(REQUESTS),
            "paid_model_requests": 0,
        }
        dump(WORK / "test-summary.json", summary)
        print(json.dumps({"summary": str(WORK / "test-summary.json"), **summary}))
        raise SystemExit(0 if result.wasSuccessful() else 1)
    finally:
        SERVER.shutdown()
        SERVER.server_close()
        THREAD.join()
