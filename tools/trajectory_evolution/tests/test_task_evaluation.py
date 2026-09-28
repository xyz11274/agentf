# test_task_evaluation 回归验证：检查包内协议、边界条件与失败处理。
# 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
# 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

"""离线任务执行、确定性验收和模型边界回归测试。"""

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

    # 屏蔽本地 HTTP 桩的访问日志，避免请求内容混入测试输出。
    def log_message(self, *_):
        pass

    # 按测试指定的行为返回裁判响应，并记录请求供断言检查。
    # 包含格式损坏、证据越界、未知目标等分支；所有响应均在本机生成。
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        REQUESTS.append(body)
        judgment = {
            "verdict": Provider.behavior,
            "reason": "实际输出与给定标准一致。",
            "evidence_paths": ["/state"],
        }
        if Provider.behavior.startswith("loops"):
            packet = json.loads(body["messages"][1]["content"])
            inventory = packet["loop_inventory"]
            wasted = []
            if inventory:
                key = next(iter(inventory))
                wasted.append(
                    dict(
                        iteration_id=key,
                        reason="已有结果且没有新任务，本轮为重复执行。",
                        evidence_event_ids=[inventory[key]["event_ids"][0]],
                    )
                )
            review = dict(
                status="complete" if inventory else "uncertain",
                wasted_iterations=wasted,
            )
            if Provider.behavior == "loops_duplicate":
                wasted.append(wasted[0])
            if Provider.behavior == "loops_unknown":
                wasted[0]["iteration_id"] = "not-an-iteration"
            if (
                Provider.behavior == "loops_repair"
                and "validation_feedback" not in packet
            ):
                wasted[0]["iteration_id"] = "not-an-iteration"
            if Provider.behavior == "loops_other_event":
                other = next(k for k in inventory if k != key)
                wasted[0]["evidence_event_ids"] = [inventory[other]["event_ids"][0]]
            if Provider.behavior in (
                "loops_prior_event",
                "loops_future_event",
                "loops_anchor_repair",
            ):
                events = packet["evidence"]["trace"]["events"]
                order = {e["event_id"]: int(e["timestamp_us"]) for e in events}
                rounds = sorted(
                    inventory,
                    key=lambda k: min(order[e] for e in inventory[k]["event_ids"]),
                )
                current, other = (
                    (rounds[-1], rounds[0])
                    if Provider.behavior != "loops_future_event"
                    else (rounds[0], rounds[-1])
                )
                wasted[0].update(
                    iteration_id=current,
                    evidence_event_ids=[
                        inventory[current]["event_ids"][0],
                        inventory[other]["event_ids"][0],
                    ],
                )
                if Provider.behavior == "loops_anchor_repair":
                    wasted[0]["evidence_event_ids"].pop(0)
                    if "validation_feedback" in packet:
                        reference = packet["validation_feedback"]["loop_review_error"][
                            "reference"
                        ]
                        wasted[0]["evidence_event_ids"] = [
                            reference["allowed_iteration_event_ids"][0]
                        ]
            if Provider.behavior.startswith("loops_join_"):
                current = next(
                    k
                    for k, v in inventory.items()
                    if v["execution_id"] == "left" and str(v["iteration"]) == "1"
                )
                cited = {
                    "loops_join_child": "right-2-action.end",
                    "loops_join_without_anchor": "right-2-action.end",
                    "loops_join_unrelated": "right-1-action.end",
                    "loops_join_future": "left-2-action.end",
                }
                refs = [cited[Provider.behavior]]
                if Provider.behavior != "loops_join_without_anchor":
                    refs.insert(0, inventory[current]["event_ids"][0])
                wasted[0].update(iteration_id=current, evidence_event_ids=refs)
            if Provider.behavior == "loops_uncertain":
                review = dict(status="uncertain", wasted_iterations=[])
            if Provider.behavior.startswith("loops_detailed"):
                reviews = [
                    dict(
                        iteration_id=k,
                        reason="该轮首次交付结果，必须保留。",
                        decision="keep",
                        evidence_event_ids=[v["event_ids"][0]],
                    )
                    for k, v in inventory.items()
                ]
                reviews[0].update(
                    reason="此前已完成同一操作，本轮重复执行且无其他必要工作。",
                    decision="waste",
                )
                review = dict(status="complete", iteration_reviews=reviews)
                if Provider.behavior == "loops_detailed_missing":
                    reviews.pop()
                if Provider.behavior == "loops_detailed_duplicate":
                    reviews[1] = dict(reviews[0])
                if Provider.behavior == "loops_detailed_unknown":
                    reviews[1]["iteration_id"] = "not-an-iteration"
                if Provider.behavior == "loops_detailed_invalid_decision":
                    reviews[1]["decision"] = "maybe"
                if Provider.behavior in (
                    "loops_detailed_uncertain",
                    "loops_detailed_uncertain_complete",
                ):
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
            path = (
                "/state/missing"
                if Provider.behavior == "path_repair"
                else "/state/~2bad"
            )
            judgment.update(
                verdict="passed",
                evidence_paths=["/state"]
                if "validation_feedback" in packet
                else ["/output", path],
            )
        references = {
            "child_reference": "/state/value",
            "array_reference": "/trace/events/0/kind",
            "missing_child_reference": "/state/missing",
            "prefix_reference": "/stateful/value",
            "encoded_root_reference": "/state~1value",
            "packet_reference": "/evidence_text_refs/0",
            "missing_array_reference": "/trace/events/999/kind",
        }
        if Provider.behavior in references:
            judgment.update(
                verdict="passed", evidence_paths=[references[Provider.behavior]]
            )
        if Provider.behavior == "leak":
            judgment.update(verdict="passed", reason=KEY)
        if Provider.behavior in ("fence", "repair"):
            judgment["verdict"] = "passed"
        if Provider.behavior.startswith("turn_"):
            packet = json.loads(body["messages"][1]["content"])
            judgment.update(
                verdict="passed",
                turn_verdicts=[
                    {
                        "turn": row["turn"],
                        "verdict": "failed"
                        if Provider.behavior == "turn_failed" and row["turn"] == 2
                        else "passed",
                        "reason": "本轮依据真实执行结果判定。",
                    }
                    for row in packet["turn_expectations"]
                ],
            )
            if Provider.behavior in ("turn_missing", "turn_repair") and (
                Provider.behavior == "turn_missing"
                or "validation_feedback" not in packet
            ):
                judgment["turn_verdicts"].pop()
            if Provider.behavior in ("turn_goal_status", "turn_goal_status_repair", "turn_private_status"):
                diagnostic = packet.get("validation_feedback", {}).get("turn_verdict_error", {})
                if Provider.behavior == "turn_goal_status_repair" and diagnostic.get("allowed_verdicts") == [
                        "passed", "failed", "uncertain"]:
                    judgment["turn_verdicts"][1]["verdict"] = "failed"
                    judgment["turn_verdicts"][1]["reason"] = "该轮实际目标未完成，格式恢复不能放行。"
                else:
                    judgment["turn_verdicts"][1]["verdict"] = (
                        "诊断不可回显此状态" if Provider.behavior == "turn_private_status" else "not_applicable")
            if Provider.behavior == "turn_string_number":
                judgment["turn_verdicts"][0]["turn"] = "1"
            if Provider.behavior == "turn_wrong_order":
                judgment["turn_verdicts"].reverse()
            if Provider.behavior == "turn_long_reason":
                judgment["turn_verdicts"][0]["reason"] = "诊断不可回显此内容" * 300
        # 目标模式的本地裁判桩：验证拆批、恢复、未知和引用协议，不调用外部 API。
        packet = json.loads(body["messages"][1]["content"])
        if "assessment" in packet:
            scope = packet["assessment"]
            rows = []
            for goal in scope["goals"]:
                status = "passed"
                if Provider.behavior == "goal_consistency_repair" and packet.get(
                        "validation_feedback", {}).get("reason_conflicts"):
                    status = "failed"
                if Provider.behavior == "goal_partial" and goal["turn"] == 2:
                    status = "failed"
                if Provider.behavior in ("goal_failed", "goal_failed_unknown",
                                         "goal_consistency_failed_waiver",
                                         "goal_consistency_failed_unjustified",
                                         "goal_consistency_mixed") and goal["turn"] == 1:
                    status = "failed"
                if Provider.behavior in ("goal_unknown", "goal_failed_unknown") and goal["turn"] == 2:
                    status = "uncertain"
                if Provider.behavior.startswith(("goal_preview", "goal_tool_preview")):
                    resolved = packet["evidence"].get("context_evidence", [])
                    full_answer = any(
                        ref.get("source") == "current"
                        and ref.get("path", "").endswith(
                            "/output/text" if Provider.behavior.startswith("goal_tool_preview") else "/answer")
                        and isinstance(ref.get("value"), str)
                        and "中段验收证据完整" in ref["value"]
                        for ref in resolved
                    )
                    status = "uncertain" if not full_answer or Provider.behavior in ("goal_preview_unknown", "goal_tool_preview_unknown") else (
                        "failed" if Provider.behavior in ("goal_preview_failed", "goal_tool_preview_failed") else "passed"
                    )
                if (
                    Provider.behavior in ("goal_recovery", "goal_critical")
                    and scope["turn_end"] == 1
                ):
                    status = "failed"
                rows.append(
                    dict(
                        id=goal["id"],
                        status=status,
                        reason="本目标按可见证据独立验收。",
                        evidence_paths=["/traces"],
                    )
                )
            if Provider.behavior.startswith("goal_status_") and (
                Provider.behavior == "goal_status_exhausted" or "validation_feedback" not in packet
            ):
                # 非条件目标不能返回 not_applicable，诊断应告诉模型允许的状态。
                rows[0]["status"] = "not_applicable"
            loop_review = judgment.get("loop_review")
            judgment = dict(
                verdict="failed"
                if any(r["status"] == "failed" for r in rows)
                else "passed",
                reason="按本批目标独立核验，其他轮次不视为缺失。",
                evidence_paths=["/traces"],
                goal_verdicts=rows,
            )
            if loop_review is not None:
                judgment["loop_review"] = loop_review
            if "turn_expectations" in packet:
                judgment["turn_verdicts"] = [
                    dict(
                        turn=t["turn"],
                        verdict="uncertain"
                        if Provider.behavior == "goal_turn_uncertain"
                        else "passed",
                        reason="本轮执行证据完整。",
                    )
                    for t in packet["turn_expectations"]
                ]
            # 总结层故意与明细矛盾，用于验证协议恢复而非修改任务成绩。
            if (Provider.behavior.startswith("goal_summary_")
                    or Provider.behavior == "loops_summary_conflict"):
                if Provider.behavior != "goal_summary_repair" or "validation_feedback" not in packet:
                    judgment["verdict"] = "failed"
                if Provider.behavior == "goal_summary_turn_failed":
                    judgment["turn_verdicts"][0]["verdict"] = "failed"
            if (
                Provider.behavior == "goal_lookup"
                and "context_evidence" not in packet["evidence"]
            ):
                judgment = dict(
                    evidence_requests=[dict(source="current", path="/state")]
                )
            if Provider.behavior == "goal_future_lookup":
                judgment = dict(
                    evidence_requests=[
                        dict(source="history", path="/traces/32/events/0")
                    ]
                )
        if packet.get("kind") == "task-judge-consistency.v1":
            conflict = dict(id=packet["goals"][0]["id"], issue="missing_requirement_waived")
            conflicts = []
            if Provider.behavior == "goal_consistency_exhausted" or (
                    Provider.behavior == "goal_consistency_repair"
                    and packet["goals"][0]["reported"]["status"] == "passed"):
                conflicts = [conflict]
            if Provider.behavior == "goal_consistency_unknown":
                conflicts = [dict(conflict, id="not-a-goal")]
            if Provider.behavior == "goal_consistency_failed_waiver":
                conflicts = [conflict]
            if Provider.behavior == "goal_consistency_failed_unjustified":
                conflicts = [dict(conflict, issue="unjustified_requirement")]
            if Provider.behavior == "goal_consistency_mixed":
                conflicts = [conflict, dict(conflict, id=packet["goals"][1]["id"])]
            judgment = dict(conflicts=conflicts)
        content = json.dumps(judgment)
        if Provider.behavior == "fence":
            content = "```json\n" + content + "\n```"
        if Provider.behavior == "repair" and "validation_feedback" not in json.loads(
            body["messages"][1]["content"]
        ):
            content = "invalid response"
        result = {
            "choices": [
                {
                    "finish_reason": "length"
                    if Provider.behavior == "length"
                    else "stop",
                    "message": {"content": content},
                }
            ]
        }
        encoded = json.dumps(result).encode()
        rate_limited = (Provider.behavior == "goal_consistency_rate_limited"
                        and packet.get("kind") == "task-judge-consistency.v1")
        self.send_response(429 if rate_limited else 500 if Provider.behavior == "http_error" else 200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)


SERVER = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
threading.Thread(target=SERVER.serve_forever, daemon=True).start()


# 将测试配置写成 UTF-8 JSON，保留中文以便核对实际输入字节数。
def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")


class TrialTests(unittest.TestCase):
    # 每条回归使用独立目录和伪凭据，默认启动本地宿主并检查 state.value。
    # 测试随后按需修改这一最小配置；未明确启用时不创建模型裁判。
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="case.", dir=WORK))
        self.env = os.environ.copy()
        self.env["EVAL_TEST_API_KEY"] = KEY
        self.env["EVAL_UNRELATED_SECRET"] = "never-pass-this-to-the-child"
        self.config = dict(
            schema_version=1,
            splits=["train"],
            repetitions=1,
            parallelism=2,
            timeout_ms=1000,
            max_output_bytes=65536,
            variants=[
                dict(
                    id="baseline",
                    version="test-v1",
                    command=[
                        sys.executable,
                        "-B",
                        str(SOURCE / "tests/evaluation_host.py"),
                        "good",
                    ],
                    pass_env=["EVAL_TEST_API_KEY"],
                )
            ],
            checks={
                "*": [
                    {"id": "value", "path": "/state/value", "op": "equals", "value": 20}
                ]
            },
        )
        Provider.behavior = "passed"
        self.start_requests = len(REQUESTS)
        self.make_dataset()

    # 执行 dataset 子命令并核对退出码，同时确认诊断没有泄漏伪凭据。
    def call(self, *args, code=0):
        result = subprocess.run(
            [str(CLI), "dataset", *map(str, args)],
            env=self.env,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        self.assertNotIn(KEY, result.stdout + result.stderr)
        return result

    # 从给定 golden 用例构建真实格式的数据集，不使用模型生成题目。
    # 每次选择新的输出目录，允许同一测试比较多种任务契约。
    def make_dataset(self, cases=None, reviewed=True):
        if cases is None:
            cases = [
                dict(
                    task_id="one",
                    group_id="one",
                    task_input="计算已知数据的总量。",
                    expected_behavior="总量为20，不编造缺失的数据。",
                    difficulty="easy",
                    category="data",
                    initial_state={"value": 20},
                    followup_inputs=["请再次确认。"],
                    split="train",
                    review_status="reviewed" if reviewed else "pending",
                )
            ]
        source = self.root / "tasks.jsonl"
        source.write_text(
            "\n".join(json.dumps(x, ensure_ascii=False) for x in cases) + "\n"
        )
        config = json.loads((SOURCE / "config/dataset.json").read_text())
        config["sources"] = [{"kind": "golden", "path": str(source)}]
        config["budget"]["timeout_ms"] = 5000
        write_json(self.root / "dataset.json", config)
        self.dataset = self.root / (
            "dataset-" + str(len(list(self.root.glob("dataset-*"))))
        )
        self.call(
            "build",
            "--config",
            self.root / "dataset.json",
            "--output-root",
            self.dataset,
        )

    # 替换本地测试宿主的行为参数，用于模拟成功、失败、超时和异常轨迹。
    def mode(self, value, index=0):
        self.config["variants"][index]["command"][-1] = value

    # 将裁判指向本机 Provider 桩，复制提示词并配置伪凭据环境变量。
    # 只设置配置；实际裁判请求由随后执行的 dataset run 触发。
    def model(self):
        runtime = json.loads((SOURCE / "config/standard-runtime.json").read_text())
        runtime["model"].update(
            endpoint=f"http://127.0.0.1:{SERVER.server_port}/chat/completions",
            model_id="task-eval-fake",
            api_key_env="EVAL_TEST_API_KEY",
            thinking_mode="disabled",
            timeout_ms=2000,
            connect_timeout_ms=500,
            max_retries=0,
        )
        shutil.copytree(SOURCE / "config/prompts", self.root / "prompts")
        write_json(self.root / "runtime.json", runtime)
        self.config["judge"] = dict(
            enabled=True,
            runtime_config="runtime.json",
            prompt="prompts/task-judge-system.txt",
            max_model_calls=10,
        )

    # 调用正式评测命令，读取报告并检查文件权限、凭据过滤和工作目录收尾。
    # extra 用于断点续跑或仅重新评分，name 用于区分同一测试的多次产物。
    def evaluate(self, code=0, extra=(), name="evaluation"):
        write_json(self.root / "evaluation.json", self.config)
        result = self.call(
            "run",
            self.dataset,
            "--config",
            self.root / "evaluation.json",
            "--output-root",
            self.root / name,
            *extra,
            code=code,
        )
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

    # 回归用例：test_rule_pass_multiturn_and_files。
    # 关键断言：self.assertEqual(report["summary"]["passed"], 1)；此处只验证测试构造的场景。
    def test_rule_pass_multiturn_and_files(self):
        report = self.evaluate()
        self.assertEqual(report["summary"]["passed"], 1)
        self.assertEqual(report["trials"][0]["evidence"]["state"]["turns"], 2)
        self.assertEqual(report["summary"]["model_calls"], 0)
        self.assertTrue((self.root / "evaluation/report.pb").is_file())

    # 测试辅助函数：turn_dataset。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def turn_dataset(self):
        task = json.loads((self.dataset / "train.jsonl").read_text().splitlines()[0])
        task["attributes"] = {
            "turn_expectations": [
                {
                    "turn": 1,
                    "expected_behavior": "第一轮验收标记，仅评分器可见。",
                    "expected_targets": ["left", "right"],
                },
                {"turn": 2, "expected_behavior": "第二轮新要求不得追溯惩罚第一轮。"},
            ]
        }
        self.make_dataset([task])
        self.entry_mode()

    # 回归用例：test_turn_grading_one_request_and_local_failure_not_execution_failure。
    # 关键断言：self.assertEqual(trial["execution_status"], "completed")；此处只验证测试构造的场景。
    def test_turn_grading_one_request_and_local_failure_not_execution_failure(self):
        self.turn_dataset()
        self.model()
        Provider.behavior = "turn_failed"
        report = self.evaluate(code=1)
        trial = report["trials"][0]
        self.assertEqual(trial["execution_status"], "completed")
        self.assertEqual(trial["verdict"], "failed")
        self.assertEqual(
            [v["verdict"] for v in trial["evidence"]["turn_evaluation"]],
            ["passed", "failed"],
        )
        self.assertEqual(len(REQUESTS) - self.start_requests, 1)
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        self.assertEqual(len(packet["turn_expectations"]), 2)
        self.assertEqual(
            [row["turn"] for row in packet["contract"]["turn_verdicts"]], [1, 2]
        )
        self.assertNotIn("intent_observations", packet["evidence"])
        self.assertNotIn(
            "第一轮验收标记",
            json.dumps(trial["evidence"]["traces"], ensure_ascii=False),
        )
        self.assertEqual(report["summary"]["turn_acceptance"]["passed_turns"], 1)

    # 回归用例：test_missing_turn_verdict_is_error_not_success。
    # 重点核验字段或诊断：judge_validation_error、turn_verdict_error、turn_count_mismatch；异常路径不能被当成成功。
    def test_missing_turn_verdict_is_error_not_success(self):
        self.turn_dataset()
        self.model()
        Provider.behavior = "turn_missing"
        report = self.evaluate(code=3)
        self.assertEqual(report["trials"][0]["verdict"], "error")
        diagnostic = report["trials"][0]["evidence"]["judge_validation_error"][
            "turn_verdict_error"
        ]
        self.assertEqual(
            diagnostic,
            {
                "constraint": "turn_count_mismatch",
                "expected_count": 2,
                "received_count": 1,
            },
        )

    # 构造逐轮验收目标并启用目标评分；默认每批一轮，便于定位跨批次错误。
    # criteria 可替换首轮目标，覆盖延迟完成和关键约束；宿主仍由正式入口驱动。
    def objective_mode(self, turns=2, criteria=None):
        task = json.loads((self.dataset / "train.jsonl").read_text().splitlines()[0])
        task["followup_inputs"] = [
            f"第 {i} 轮继续验证结果。" for i in range(2, turns + 1)
        ]
        task["attributes"] = {
            "turn_expectations": [
                dict(turn=i, expected_behavior=f"第 {i} 轮应给出可靠结果。")
                for i in range(1, turns + 1)
            ]
        }
        if criteria:
            task["attributes"]["turn_expectations"][0]["criteria"] = criteria
        self.make_dataset([task])
        self.entry_mode()
        if not (self.root / "prompts").exists():
            self.model()
        self.config["judge"].update(
            assessment_mode="objectives",
            max_batch_turns=1,
            max_model_calls=80,
            batch_input_bytes=49152,
        )
        self.config["timeout_ms"] = 15000
        self.config["checks"] = {}

    def test_objective_default_answer_budget_is_independent_of_tool_preview(self):
        """未设置答复预算时采用已声明的 4096 字节默认值，不跟随工具预览缩到 256 字节。"""
        self.objective_mode(turns=1)
        self.entry_mode("--long-answer")
        self.config["judge"].update(evidence_field_bytes=256)
        self.assertNotIn("answer_field_bytes", self.config["judge"])
        self.evaluate()
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        answer = packet["evidence"]["output"]["turns"][0]["answer"]
        self.assertTrue(answer["preview_only"])
        self.assertGreater(answer["original_bytes"], 4096)
        self.assertGreaterEqual(len(answer["preview"].encode()), 4090)
        self.assertLessEqual(len(answer["preview"].encode()), 4096)
        self.assertEqual(answer["path"], "/output/turns/0/answer")

    def test_objective_repeated_final_answer_is_shared_without_losing_turns(self):
        """聚合输出只共享完全相同的值；还原后每轮答案、原始轨迹和最终答复保持一致。"""
        self.objective_mode(turns=2)
        self.entry_mode("--long-answer")
        # 固定宿主续轮只接受历史答案“20”；用既有输出指针选择数值，
        # 待验收的长答复仍完整保存在每轮 run.end，不能因此从证据中移除。
        self.config["variants"][0]["entry"]["output_pointer"] = "/state/value"
        self.config["judge"].update(
            max_batch_turns=2, evidence_field_bytes=256, answer_field_bytes=32768,
            batch_input_bytes=262144, share_repeated_inputs=False, share_repeated_text=False,
        )
        first = self.evaluate()
        before = json.loads(REQUESTS[-1]["messages"][1]["content"])
        before_bytes = len(REQUESTS[-1]["messages"][1]["content"].encode())
        self.config["judge"]["share_repeated_inputs"] = True
        second = self.evaluate(name="shared-answers")
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        compact = packet["evidence"]
        aliases = {path: ref["source"] for ref in packet["evidence_text_refs"] for path in ref["paths"]}
        self.assertTrue(any(path.startswith("/output/") for path in aliases))

        def at(path):
            value = compact
            for key in path[1:].split("/"):
                key = key.replace("~1", "/").replace("~0", "~")
                value = value[int(key)] if isinstance(value, list) else value[key]
            return value

        def restore(value, path, active=frozenset()):
            if path in aliases:
                self.assertNotIn(path, active)
                source = aliases[path]
                self.assertEqual(value, {"$evidence_ref": source})
                return restore(at(source), source, active | {path})
            if isinstance(value, list):
                return [restore(v, path + "/" + str(i), active) for i, v in enumerate(value)]
            if isinstance(value, dict):
                return {k: restore(v, path + "/" + k.replace("~", "~0").replace("/", "~1"), active)
                        for k, v in value.items()}
            return value

        restored = restore(compact["output"], "/output")
        self.assertEqual(restored, before["evidence"]["output"])
        self.assertEqual(len(restored["turns"]), 2)
        self.assertEqual(restored["final"], restored["turns"][-1])
        self.assertIn("中段验收证据完整", restored["turns"][0]["answer"])
        self.assertEqual(second["trials"][0]["evidence"]["output"],
                         first["trials"][0]["evidence"]["output"])
        self.assertEqual(second["summary"]["model_calls"], 1)
        self.assertLess(len(REQUESTS[-1]["messages"][1]["content"].encode()), before_bytes)

    def test_goal_contract_lists_exact_ids_and_only_applicable_states(self):
        """每批模板完整列出真实目标；局部轮号不能替代跨批目标 ID 或放宽到期规则。"""
        self.objective_mode(
            turns=2,
            criteria=[
                dict(id="query", expected_behavior="查询已执行。"),
                dict(id="deliver", expected_behavior="第二轮交付结果。", due_turn=2),
                dict(id="optional", expected_behavior="若有结果则整理。", conditional=True),
            ],
        )
        self.evaluate()
        packets = [json.loads(r["messages"][1]["content"]) for r in REQUESTS[self.start_requests:]]
        self.assertEqual(len(packets), 2)
        for packet in packets:
            goals = packet["assessment"]["goals"]
            template = packet["contract"]["goal_verdicts"]
            self.assertEqual([r["id"] for r in template], [r["id"] for r in goals])
            for row, goal in zip(template, goals):
                allowed = ["passed", "failed", "uncertain"]
                if goal["conditional"]:
                    allowed.append("not_applicable")
                if goal["due_turn"] > packet["assessment"]["turn_end"]:
                    allowed.append("pending")
                self.assertEqual(row["status"].split("|"), allowed)
        self.assertEqual(
            [r["id"] for r in packets[1]["contract"]["goal_verdicts"]],
            ["deliver", "turn-2-1"],
        )

    def test_separator_only_turn_standard_uses_its_user_request(self):
        """空单元格拼出的分号不能豁免用户任务；跨批次仍取正确一轮，原数据不变。"""
        self.objective_mode(turns=2)
        task = json.loads((self.dataset / "train.jsonl").read_text().splitlines()[0])
        task["task_input"] = "请创建上午的服药提醒。"
        task["followup_inputs"] = ["Save the second record."]
        for turn in task["attributes"]["turn_expectations"]:
            turn["expected_behavior"] = " ；；, | 。 "
        self.make_dataset([task])
        source_bytes = (self.dataset / "train.jsonl").read_bytes()
        Provider.behavior = "goal_failed"
        self.evaluate(code=1)
        packets = [json.loads(r["messages"][1]["content"]) for r in REQUESTS[self.start_requests:]]
        self.assertEqual(len(packets), 2)
        for packet, request in zip(packets, [task["task_input"], *task["followup_inputs"]]):
            self.assertEqual(packet["assessment"]["goals"][0]["expected_behavior"], request)
            self.assertEqual(packet["turn_expectations"][0]["expected_behavior"], request)
        self.assertEqual((self.dataset / "train.jsonl").read_bytes(), source_bytes)

    def test_explicit_criteria_are_not_replaced_by_user_request(self):
        """显式子目标保留自身验收范围；不因外层是占位标点就扩大评分范围。"""
        standard = "只核对本轮查询是否执行。"
        self.objective_mode(turns=1, criteria=[dict(id="query-only", expected_behavior=standard)])
        task = json.loads((self.dataset / "train.jsonl").read_text().splitlines()[0])
        task["attributes"]["turn_expectations"][0]["expected_behavior"] = "；"
        self.make_dataset([task])
        self.evaluate()
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        self.assertEqual(packet["assessment"]["goals"][0]["expected_behavior"], standard)

    def test_goal_status_diagnostic_repairs_without_relaxing_contract(self):
        """非条件目标被误跳过时，有界修复携带允许状态；成功后不遗留旧错误。"""
        self.objective_mode(turns=1)
        self.config["judge"]["max_repair_attempts"] = 1
        Provider.behavior = "goal_status_repair"
        report = self.evaluate()
        self.assertEqual(report["summary"]["model_calls"], 2)
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        diagnostic = packet["validation_feedback"]["goal_verdict_error"]
        self.assertEqual(diagnostic["constraint"], "goal_status_not_allowed")
        self.assertEqual(diagnostic["expected_id"], "turn-1-1")
        self.assertEqual(diagnostic["allowed_statuses"], ["passed", "failed", "uncertain"])
        self.assertNotIn("validation_error", report["trials"][0]["evidence"]["judge_batches"][0])

    def test_goal_status_exhaustion_retains_bounded_diagnostic(self):
        """耗尽修复后维持评测错误，并在批次里保留可定位的静态诊断。"""
        self.objective_mode(turns=1)
        self.config["judge"]["max_repair_attempts"] = 1
        Provider.behavior = "goal_status_exhausted"
        report = self.evaluate(code=3)
        self.assertEqual(report["summary"]["model_calls"], 2)
        trial = report["trials"][0]
        self.assertEqual(trial["verdict"], "error")
        diagnostic = trial["evidence"]["judge_batches"][0]["validation_error"]["goal_verdict_error"]
        self.assertEqual(diagnostic["constraint"], "goal_status_not_allowed")
        self.assertEqual(diagnostic["goal_verdict_index"], 0)
        self.assertNotIn("previous_response", diagnostic)
        self.assertEqual(trial["evidence"]["assessment"]["coverage"], 0)

    def test_execution_view_binds_requests_without_reordering_parallel_events(self):
        """参数按真实调用 ID 配对；裁判视图不改原轨迹，也不按并发返回顺序配对。"""
        self.model()
        self.mode("loop_trace")
        self.config["judge"].update(
            include_model_requests=False, share_repeated_text=False,
            loop_review_prompt="prompts/task-loop-review.txt",
        )
        # 组合路径必须覆盖循环观测：派生配对字段不能进入标准轨迹的 Protobuf 转换。
        Provider.behavior = "loops"
        report = self.evaluate()
        self.assertEqual(report["summary"]["invalid_loop_assessment"]["mean"], 1)
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        self.assertEqual(len(packet["loop_inventory"]), 4)
        original = report["trials"][0]["evidence"]["trace"]["events"]
        visible = packet["evidence"]["trace"]["events"]
        starts = {
            event["action_id"]: event for event in original
            if event["kind"] == "action.start"
        }
        paired = 0
        for event in visible:
            if event["kind"] != "action.end" or "request" not in event:
                continue
            source = starts[event["action_id"]]
            self.assertEqual(event["request"]["event_id"], source["event_id"])
            self.assertEqual(event["request"]["input"], source["input"])
            paired += 1
        self.assertGreater(paired, 1)
        self.assertTrue(all("request" not in event for event in original))
        visible_ids = {event["event_id"] for event in visible}
        self.assertEqual(
            [event["event_id"] for event in visible],
            [event["event_id"] for event in original if event["event_id"] in visible_ids],
        )

    def test_judge_request_reference_maps_back_to_original_start(self):
        """正式入口接收派生请求引用后，保存原始开始事件的路径，而不是虚构字段。"""
        self.model()
        self.mode("loop_trace")
        self.config["judge"].update(include_model_requests=False, share_repeated_text=False)
        captured = []

        class RequestProvider(Provider):
            def do_POST(self):
                # 本地裁判桩只返回精确引用，用于验证程序的来源映射，不做语义判分。
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                packet = json.loads(body["messages"][1]["content"])
                for index, event in enumerate(packet["evidence"]["trace"]["events"]):
                    if event.get("request", {}).get("event_id"):
                        path = f"/trace/events/{index}/request/input"
                        captured.append(event["request"]["event_id"])
                        break
                result = dict(
                    choices=[dict(
                        finish_reason="stop",
                        message=dict(content=json.dumps(dict(
                            verdict="passed",
                            reason="已按同一调用的原始实参与结果核对。",
                            evidence_paths=[path],
                        ), ensure_ascii=False)),
                    )]
                )
                encoded = json.dumps(result).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), RequestProvider)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            path = self.root / "runtime.json"
            runtime = json.loads(path.read_text())
            runtime["model"]["endpoint"] = f"http://127.0.0.1:{server.server_port}/chat/completions"
            write_json(path, runtime)
            report = self.evaluate()
            trial = report["trials"][0]
            raw = trial["evidence"]["trace"]["events"]
            check = next(check for check in trial["checks"] if check["check_id"] == "model")
            source = next(i for i, event in enumerate(raw) if event["event_id"] == captured[0])
            self.assertEqual(check["evidence_paths"], [f"/trace/events/{source}/input"])
            self.assertEqual(report["summary"]["model_calls"], 1)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

    def test_goal_quote_review_keeps_source_standards_and_protocol(self):
        """逐项引用只约束裁判读证据的方法，不改原标准、状态集合或请求次数。"""
        standards = [
            "查询附近机构并展示结果；如无匹配则如实告知。",
            "比较两个方案，并询问用户偏好。",
            "建议一个地点，不要求实时核验。",
        ]
        self.objective_mode(
            turns=1,
            criteria=[
                dict(id=f"requirement-{i}", expected_behavior=text)
                for i, text in enumerate(standards, 1)
            ],
        )
        source = (self.dataset / "train.jsonl").read_bytes()
        Provider.behavior = "goal_failed"
        report = self.evaluate(code=1)

        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        prompt = REQUESTS[-1]["messages"][0]["content"]
        self.assertEqual(
            [goal["expected_behavior"] for goal in packet["assessment"]["goals"]],
            standards,
        )
        for instruction in (
            "要求原文→对应答复原句或工具事实→满足/缺失",
            "不能承认漏项却给 passed",
            "整体结论不能增加各项目标未要求的成功条件",
            "返回无关条目不是已经确认无匹配",
            "错误参数、占位任务和无关查询不适用",
            "turn_verdicts 每项先 turn、reason，再 verdict",
            "当轮真实违规仍须指出原文与事实",
            "不能仅因工具结果标注为测试、模拟或快照而判失败",
            "明确要求的时效、字段、成功操作与禁止行为仍必须满足",
        ):
            self.assertIn(instruction, prompt)
        self.assertEqual((self.dataset / "train.jsonl").read_bytes(), source)
        self.assertEqual(report["summary"]["model_calls"], 1)
        self.assertEqual(report["trials"][0]["verdict"], "failed")
        for row in packet["contract"]["goal_verdicts"]:
            self.assertEqual(set(row), {"id", "reason", "status", "evidence_paths"})
            self.assertEqual(row["status"], "passed|failed|uncertain")
        # 本地桩只验证输入和兼容协议，真实模型准确性另以保留轨迹复评验证。
        self.assertEqual(REQUESTS[-1]["thinking"]["type"], "disabled")

    def test_goal_prompt_change_regrades_without_rerunning_host(self):
        """目标核验提示词也绑定判分身份；只改裁判时仍可复用原始执行证据。"""
        self.objective_mode(turns=1)
        first = self.evaluate()
        source = self.root / "evaluation/report.json"
        prompt = self.root / "prompts/task-goals.txt"
        prompt.write_text(prompt.read_text() + "\n按实际回执核对查询对象。\n")

        # 故意令宿主命令不可执行；重评若偷偷重跑 Agent，该回归会失败。
        self.config["variants"][0]["command"] = [str(self.root / "must-not-execute")]
        second = self.evaluate(name="regraded-goals", extra=("--regrade-report", source))
        self.assertNotEqual(
            first["configuration"]["judge_fingerprint"],
            second["configuration"]["judge_fingerprint"],
        )
        self.assertEqual(second["summary"]["model_calls"], 1)
        for field in ("trial_id", "duration_ms", "execution_status", "metrics"):
            self.assertEqual(first["trials"][0].get(field), second["trials"][0].get(field))
        for field in ("trace", "traces"):
            self.assertEqual(
                first["trials"][0]["evidence"].get(field),
                second["trials"][0]["evidence"].get(field),
            )

    def test_conditional_goal_prompt_preserves_required_failure(self):
        """条件说明进入外置裁判提示词；真实失败仍失败，不在程序中强行放行。"""
        standard = "先校验数据；若有匹配项则输出操作计划，否则说明查询为空。"
        self.objective_mode(turns=1, criteria=[dict(
            id="conditional", expected_behavior=standard, conditional=True)])
        Provider.behavior = "goal_failed"
        report = self.evaluate(code=1)
        self.assertEqual(report["trials"][0]["verdict"], "failed")
        self.assertEqual(report["summary"]["model_calls"], 1)
        request = REQUESTS[-1]
        packet = json.loads(request["messages"][1]["content"])
        self.assertEqual(packet["assessment"]["goals"][0]["expected_behavior"], standard)
        self.assertTrue(packet["assessment"]["goals"][0]["conditional"])
        prompt = request["messages"][0]["content"]
        for text in ("条件未知不等于不成立", "后置分支未触发也不豁免", "环境限制和如实报告不能豁免"):
            self.assertIn(text, prompt)

    def test_objective_summary_conflict_repairs_without_changing_evidence(self):
        """明细全部通过但总结失败时，仅修复裁判响应，不重跑宿主或直接放行。"""
        self.objective_mode(turns=1)
        self.config["judge"]["max_repair_attempts"] = 1
        Provider.behavior = "goal_summary_repair"
        report = self.evaluate()
        self.assertEqual(report["summary"]["model_calls"], 2)
        self.assertEqual(report["trials"][0]["verdict"], "passed")
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        self.assertEqual(packet["validation_feedback"]["code"],
                         "evaluation_judge_summary_conflict")
        self.assertEqual(packet["validation_feedback"]["previous_response"]["verdict"], "failed")

    def test_objective_summary_conflict_exhaustion_is_error_not_business_failure(self):
        """持续矛盾的判分记为评测错误，不作为策略反证，也不保留虚假的满分。"""
        self.objective_mode(turns=1)
        self.config["judge"]["max_repair_attempts"] = 1
        Provider.behavior = "goal_summary_conflict"
        report = self.evaluate(code=3)
        trial = report["trials"][0]
        self.assertEqual(report["summary"]["model_calls"], 2)
        self.assertEqual(trial["execution_status"], "completed")
        self.assertEqual(trial["verdict"], "error")
        self.assertEqual(trial["evidence"]["assessment"]["coverage"], 0)
        check = next(c for c in trial["checks"] if c["check_id"] == "model")
        self.assertTrue(check["reason"].startswith("evaluation_judge_summary_conflict"))
        self.assertEqual(trial["evidence"]["judge_batches"][0]["status"], "error")

    def test_objective_invalid_summary_discards_loop_judgment(self):
        """同一响应的总结无效时，不把其循环判定作为已完成评分落盘。"""
        task = json.loads((self.dataset / "train.jsonl").read_text().splitlines()[0])
        task["followup_inputs"] = []
        self.make_dataset([task])
        self.model()
        self.mode("loop_trace")
        self.config["checks"] = {}
        self.config["judge"].update(
            assessment_mode="objectives",
            loop_review_prompt="prompts/task-loop-review.txt",
            max_repair_attempts=0,
        )
        Provider.behavior = "loops_summary_conflict"
        report = self.evaluate(code=3)
        trial = report["trials"][0]
        self.assertEqual(trial["verdict"], "error")
        self.assertEqual(trial["execution_status"], "completed")
        review = trial["evidence"]["loop_review"]
        self.assertEqual(review["status"], "uncertain")
        self.assertEqual(review["observed_iterations"], 4)
        self.assertIsNone(review["count"])
        self.assertEqual(review["wasted_iterations"], [])
        self.assertIsNone(report["summary"]["invalid_loop_assessment"]["mean"])

    def test_objective_summary_keeps_independent_hard_failure(self):
        """独立硬检查未通过时，总结失败仍合法，不能因目标完成而自动放行。"""
        self.objective_mode(turns=1)
        self.config["checks"] = {"*": [dict(
            id="missing-required-state", path="/state/required_external_field",
            op="equals", value="required")]}
        Provider.behavior = "goal_summary_conflict"
        report = self.evaluate(code=1)
        self.assertEqual(report["summary"]["model_calls"], 1)
        self.assertEqual(report["trials"][0]["verdict"], "failed")
        self.assertNotIn("judge_validation_error", report["trials"][0]["evidence"])

    def test_objective_summary_keeps_actual_turn_failure(self):
        """目标最终完成不抹除当轮真实错误；严格验收继续保留失败。"""
        self.objective_mode(turns=1)
        Provider.behavior = "goal_summary_turn_failed"
        report = self.evaluate(code=1)
        self.assertEqual(report["summary"]["model_calls"], 1)
        self.assertEqual(report["trials"][0]["verdict"], "failed")
        self.assertEqual(report["trials"][0]["evidence"]["assessment"]["quality"], 1)

    def test_objective_answer_budget_keeps_middle_in_first_request(self):
        """独立答复预算经正式入口生效，不靠多调一次裁判才看到中段事实。"""
        self.objective_mode(turns=1)
        self.entry_mode("--long-answer")
        self.config["judge"].update(evidence_field_bytes=256, answer_field_bytes=32768,
                                    batch_input_bytes=131072)
        Provider.behavior = "goal_ok"
        report = self.evaluate()
        self.assertEqual(report["summary"]["model_calls"], 1)
        request = REQUESTS[-1]
        packet = json.loads(request["messages"][1]["content"])
        answer = packet["evidence"]["output"]["turns"][0]["answer"]
        self.assertIsInstance(answer, str)
        self.assertIn("中段验收证据完整", answer)
        self.assertLessEqual(len(answer.encode()), 32768)
        self.assertNotIn("context_evidence", packet["evidence"])
        self.assertLessEqual(sum(len(m["content"].encode()) for m in request["messages"]),
                             131072)

    def test_model_only_regrade_completed_batch_subset(self):
        """从完整多题报告中选一题重评；未选题不执行，原轨迹与计时不可变化。"""
        self.make_dataset([dict(
            task_id=name, group_id=name, task_input=f"任务{name}计算总量20。",
            expected_behavior="总量为20。", initial_state=dict(value=20),
            difficulty="easy", category="data", split="train", review_status="reviewed")
            for name in ("a", "b", "c")])
        self.model()
        self.mode("trace")
        first = self.evaluate()
        source = self.root / "evaluation/report.json"
        original = source.read_bytes()
        self.config["task_ids"] = ["b"]
        self.mode("nonzero")
        second = self.evaluate(name="selected", extra=("--regrade-report", source))
        self.assertEqual(second["summary"]["regraded_trials"], 1)
        self.assertFalse(second["summary"]["agent_reexecuted"])
        before = next(row for row in first["trials"] if row["task_id"] == "b")
        after = second["trials"][0]
        for field in ("task_id", "trial_id", "duration_ms", "execution_status", "metrics"):
            self.assertEqual(before.get(field), after.get(field))
        self.assertEqual(before["evidence"]["trace"], after["evidence"]["trace"])
        self.assertEqual(source.read_bytes(), original)

    # 一轮失败仍保留另一轮的完成分；严格失败、逐轮记录和批次数分别验证。
    def test_objective_partial_success_is_not_zero(self):
        self.objective_mode()
        Provider.behavior = "goal_partial"
        trial = self.evaluate(code=1)["trials"][0]
        self.assertEqual(trial["verdict"], "failed")
        self.assertEqual(trial["evidence"]["assessment"]["quality"], 0.5)
        self.assertEqual(trial["evidence"]["assessment"]["coverage"], 1)
        self.assertEqual(
            [r["turn"] for r in trial["evidence"]["turn_evaluation"]], [1, 2]
        )
        self.assertEqual(len(trial["evidence"]["judge_batches"]), 2)

    # 未知目标保留在分母中，只扩大上下界，不计为已完成或确定业务失败。
    def test_objective_unknown_stays_in_denominator(self):
        self.objective_mode()
        Provider.behavior = "goal_unknown"
        value = self.evaluate(code=3)["trials"][0]["evidence"]["assessment"]
        self.assertEqual(
            (value["quality"], value["quality_upper"], value["coverage"]), (0.5, 1, 0.5)
        )

    def test_objective_failed_with_unknown_goal_regrades_without_execution(self):
        """整题失败但某个子目标未知，仍只补裁判；不能丢掉既有业务失败。"""
        self.objective_mode()
        Provider.behavior = "goal_failed_unknown"
        first = self.evaluate()
        before = first["trials"][0]
        source = self.root / "evaluation/report.json"
        original = source.read_bytes()
        self.assertEqual(before["verdict"], "failed")
        self.assertEqual(before["evidence"]["assessment"]["coverage"], 0.5)
        Provider.behavior = "goal_failed"
        second = self.evaluate(name="resolved", code=1, extra=("--resume-report", source))
        after = second["trials"][0]
        self.assertEqual(after["verdict"], "failed")
        self.assertEqual(after["evidence"]["assessment"]["coverage"], 1)
        self.assertEqual(second["summary"]["regraded_trials"], 1)
        for field in ("trial_id", "execution_status", "duration_ms", "metrics"):
            self.assertEqual(before.get(field), after.get(field))
        self.assertEqual(before["evidence"]["traces"], after["evidence"]["traces"])
        self.assertEqual(source.read_bytes(), original)
        third = self.evaluate(
            name="reused", code=1, extra=("--resume-report", self.root / "resolved/report.json"))
        self.assertEqual(third["summary"]["reused_trials"], 1)
        self.assertEqual(third["summary"]["model_calls"], 0)

    # 同一执行证据可从旧二元评分升级到目标评分，不能重新执行宿主或改写原轨迹。
    def test_objective_regrade_upgrades_legacy_without_execution(self):
        self.objective_mode()
        self.config["judge"]["assessment_mode"] = "legacy"
        Provider.behavior = "turn_failed"
        old = self.evaluate(code=1)
        self.config["judge"]["assessment_mode"] = "objectives"
        Provider.behavior = "goal_partial"
        new = self.evaluate(
            code=1,
            name="goal-regrade",
            extra=("--regrade-report", self.root / "evaluation/report.json"),
        )
        self.assertEqual(old["evaluator_version"], "task-evaluator-v1")
        self.assertEqual(new["evaluator_version"], "task-evaluator-v2-goals")
        self.assertFalse(new["summary"]["agent_reexecuted"])
        self.assertEqual(
            old["trials"][0]["evidence"]["traces"],
            new["trials"][0]["evidence"]["traces"],
        )
        self.assertEqual(new["summary"]["goal_completion"]["mean"], 0.5)

    # 在目标期限内恢复可提高完成度，但曾发生的关键违规仍使有效质量归零。
    def test_objective_recovery_does_not_erase_critical_failure(self):
        for critical in (False, True):
            self.objective_mode(
                criteria=[
                    dict(
                        id="recoverable",
                        expected_behavior="在第二轮结束前完成。",
                        due_turn=2,
                        critical=critical,
                    )
                ]
            )
            Provider.behavior = "goal_critical" if critical else "goal_recovery"
            report = self.evaluate(code=1, name="critical" if critical else "recovery")
            trial = report["trials"][0]
            value = trial["evidence"]["assessment"]
            self.assertEqual(value["quality"], 1)
            self.assertEqual(value["recovered_goals"], 1)
            self.assertEqual(value["critical_failures"], int(critical))
            self.assertEqual(
                report["summary"]["goal_completion"]["mean"], 0 if critical else 1
            )

    # 整轮验收未知不应抹去已完整判定的目标；配对比较同时保留两种统计口径。
    def test_objective_pairs_survive_uncertain_strict_verdict(self):
        self.objective_mode()
        other = json.loads(json.dumps(self.config["variants"][0]))
        other["id"] = "candidate"
        self.config["variants"].append(other)
        Provider.behavior = "goal_turn_uncertain"
        report = self.evaluate(code=3)
        self.assertTrue(all(t["verdict"] == "uncertain" for t in report["trials"]))
        comparison = report["comparisons"]["candidate"]
        self.assertEqual(comparison["paired_trials"], 0)
        self.assertEqual(comparison["goal_completion"]["paired_trials"], 1)
        self.assertEqual(comparison["goal_completion"]["quality_delta"], 0)
        self.assertEqual(report["summary"]["goal_completion"]["mean"], 1)

    # 20 轮完整执行后按每批 4 轮验收；请求数、原始轨迹数和字节上限同时检查。
    def test_objective_twenty_turns_are_batched_without_history_replay(self):
        self.objective_mode(turns=20)
        Provider.behavior = "goal_ok"
        self.config["judge"]["max_batch_turns"] = 4
        trial = self.evaluate()["trials"][0]
        self.assertEqual(len(trial["evidence"]["traces"]), 20)
        self.assertEqual(len(trial["evidence"]["judge_batches"]), 5)
        self.assertEqual(len(REQUESTS) - self.start_requests, 5)
        for request in REQUESTS[self.start_requests :]:
            packet = json.loads(request["messages"][1]["content"])
            self.assertLessEqual(len(packet["evidence"]["traces"]), 4)
            self.assertLessEqual(
                len(request["messages"][0]["content"].encode())
                + len(request["messages"][1]["content"].encode()),
                49152,
            )
        self.assertEqual(trial["evidence"]["assessment"]["quality"], 1)

    # 当前证据可以限次补查；历史接口请求尚未发生的轮次必须拒绝且不得得到满分。
    def test_objective_bounded_evidence_lookup_and_future_rejection(self):
        self.objective_mode()
        Provider.behavior = "goal_lookup"
        trial = self.evaluate()["trials"][0]
        self.assertEqual(len(REQUESTS) - self.start_requests, 4)
        self.assertEqual(trial["evidence"]["assessment"]["quality"], 1)
        Provider.behavior = "goal_future_lookup"
        trial = self.evaluate(code=3, name="future")["trials"][0]
        self.assertEqual(trial["verdict"], "error")
        self.assertEqual(trial["evidence"]["assessment"]["coverage"], 0)

    def test_objective_unknown_preview_uses_bounded_tool_lookup(self):
        """工具补查沿原字段提供证据；不得重跑 Agent、扩大预算或把未知自动改成通过。"""
        self.objective_mode(turns=1)
        self.config["judge"]["evidence_field_bytes"] = 256
        for name, behavior, flag, lookups, calls, verdict, code in (
            ("tool-resolved", "goal_tool_preview", "--long-tool", 2, 2, "passed", 0),
            ("tool-failed", "goal_tool_preview_failed", "--long-tool", 2, 2, "failed", 1),
            ("tool-unknown", "goal_tool_preview_unknown", "--long-tool", 2, 2, "uncertain", 3),
            ("tool-disabled", "goal_tool_preview", "--long-tool", 0, 1, "uncertain", 3),
            ("tool-budget", "goal_tool_preview", "--long-tool", 2, 1, "uncertain", 3),
            ("tool-oversize", "goal_tool_preview", "--huge-tool", 2, 1, "uncertain", 3),
        ):
            with self.subTest(name=name):
                self.entry_mode(flag)
                self.config["judge"]["max_evidence_lookups"] = lookups
                self.config["judge"]["max_model_calls"] = 1 if name == "tool-budget" else 80
                Provider.behavior = behavior
                start = len(REQUESTS)
                trial = self.evaluate(code=code, name=name)["trials"][0]
                self.assertEqual(trial["verdict"], verdict)
                self.assertEqual(len(REQUESTS) - start, calls)
                packets = [json.loads(r["messages"][1]["content"]) for r in REQUESTS[start:]]
                self.assertNotIn("context_evidence", packets[0]["evidence"])
                traces = trial["evidence"].get("traces") or [trial["evidence"]["trace"]]
                results = [e for t in traces for e in t["events"]
                           if e["kind"] == "action.end"
                           and e.get("attributes", {}).get("semantic") != "model"]
                self.assertEqual(len(results), 1)
                self.assertIn("中段验收证据完整", results[0]["output"]["text"])
                self.assertEqual(sum(e["kind"] == "run.end" for t in traces for e in t["events"]), 1)
                if calls == 2:
                    refs = packets[-1]["evidence"]["context_evidence"]
                    self.assertEqual(len(refs), 1)
                    self.assertEqual(refs[0]["source"], "current")
                    self.assertTrue(refs[0]["path"].startswith("/traces/0/events/"))
                    self.assertTrue(refs[0]["path"].endswith("/output/text"))
                    self.assertEqual(refs[0]["value"], results[0]["output"]["text"])
                for request in REQUESTS[start:]:
                    self.assertLessEqual(
                        len(request["messages"][0]["content"].encode())
                        + len(request["messages"][1]["content"].encode()), 49152)

    def test_objective_unknown_preview_uses_bounded_answer_lookup(self):
        """补查只提供事实；通过、失败、持续未知及预算不足均须保持真实判定。"""
        self.objective_mode(turns=1)
        self.config["judge"]["evidence_field_bytes"] = 256
        for name, behavior, flag, lookups, calls, verdict, code in (
            ("resolved", "goal_preview", "--long-answer", 2, 2, "passed", 0),
            ("failed", "goal_preview_failed", "--long-answer", 2, 2, "failed", 1),
            ("unknown", "goal_preview_unknown", "--long-answer", 2, 2, "uncertain", 3),
            ("disabled", "goal_preview", "--long-answer", 0, 1, "uncertain", 3),
            ("call-budget", "goal_preview", "--long-answer", 2, 1, "uncertain", 3),
            ("oversize", "goal_preview", "--huge-answer", 2, 1, "uncertain", 3),
        ):
            with self.subTest(name=name):
                self.entry_mode(flag)
                self.config["judge"]["max_evidence_lookups"] = lookups
                self.config["judge"]["max_model_calls"] = 1 if name == "call-budget" else 80
                Provider.behavior = behavior
                start = len(REQUESTS)
                trial = self.evaluate(code=code, name=name)["trials"][0]
                self.assertEqual(trial["verdict"], verdict)
                self.assertEqual(len(REQUESTS) - start, calls)
                packets = [json.loads(r["messages"][1]["content"]) for r in REQUESTS[start:]]
                self.assertNotIn("context_evidence", packets[0]["evidence"])
                if calls == 2:
                    refs = packets[-1]["evidence"]["context_evidence"]
                    self.assertEqual(len(refs), 1)
                    self.assertEqual(refs[0]["source"], "current")
                    self.assertEqual(refs[0]["path"], "/output/turns/0/answer")
                    self.assertIn("中段验收证据完整", refs[0]["value"])
                for request in REQUESTS[start:]:
                    self.assertLessEqual(
                        len(request["messages"][0]["content"].encode())
                        + len(request["messages"][1]["content"].encode()), 49152)
                # 原始轨迹保持完整，不将补查内容伪装为 Agent 的新一次执行。
                traces = trial["evidence"].get("traces") or [trial["evidence"]["trace"]]
                ends = [e for t in traces for e in t["events"]
                        if e["kind"] == "run.end"]
                self.assertEqual(len(ends), 1)
                self.assertIn("中段验收证据完整", ends[0]["output"]["answer"])

    # 新评分模式仍兼容两种 loop 裁判响应，总观测轮数不能取“无效轮次列表”的长度。
    def test_objective_loop_review_keeps_actual_observation_count(self):
        task = json.loads((self.dataset / "train.jsonl").read_text().splitlines()[0])
        task["followup_inputs"] = []
        self.make_dataset([task])
        self.model()
        self.mode("loop_trace")
        self.config["judge"].update(
            assessment_mode="objectives",
            loop_review_prompt="prompts/task-loop-review.txt",
        )
        for behavior in ("loops", "loops_detailed"):
            Provider.behavior = behavior
            report = self.evaluate(name=behavior)
            review = report["trials"][0]["evidence"]["loop_review"]
            self.assertEqual(review["status"], "complete")
            self.assertEqual(review["observed_iterations"], 4)
            self.assertEqual(review["count"], 1)
            self.assertEqual(report["summary"]["invalid_loop_assessment"]["mean"], 1)

    def test_objective_request_pairs_preserve_loop_review_and_raw_trace(self):
        """目标验收与循环评测同时开启时，配对只增强裁判视图，不改变循环数量与原轨迹。"""
        task = json.loads((self.dataset / "train.jsonl").read_text().splitlines()[0])
        task["followup_inputs"] = []
        self.make_dataset([task])
        self.model()
        self.mode("loop_trace")
        self.config["judge"].update(
            assessment_mode="objectives", include_model_requests=False,
            share_repeated_text=False, loop_review_prompt="prompts/task-loop-review.txt",
        )
        for behavior in ("loops", "loops_detailed"):
            Provider.behavior = behavior
            report = self.evaluate(name=behavior)
            evidence = report["trials"][0]["evidence"]
            review = evidence["loop_review"]
            self.assertEqual(review["status"], "complete")
            self.assertEqual(review["observed_iterations"], 4)
            self.assertEqual(review["count"], 1)
            packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
            visible = packet["evidence"].get("traces") or [packet["evidence"]["trace"]]
            original = evidence.get("traces") or [evidence["trace"]]
            self.assertTrue(any("request" in event for trace in visible for event in trace["events"]))
            self.assertFalse(any("request" in event for trace in original for event in trace["events"]))

    # 评测器事后读取的文件只在最后一批可见，不作为早期已经创建文件的执行证据。
    def test_objective_artifact_facts_are_kept_in_final_batch(self):
        self.objective_mode()
        variant = self.config["variants"][0]
        variant["command"] = [
            sys.executable,
            "-B",
            "-c",
            "from pathlib import Path; import runpy; "
            "Path('report.json').write_text('{\"total\":20}'); "
            f"runpy.run_path({str(SOURCE / 'tests/entry_agent.py')!r},run_name='__main__')",
        ]
        self.config["artifacts"] = {
            "*": [{"path": "report.json", "media_type": "application/json"}]
        }
        Provider.behavior = "goal_ok"
        trial = self.evaluate()["trials"][0]
        packets = [
            json.loads(r["messages"][1]["content"])
            for r in REQUESTS[self.start_requests :]
        ]
        self.assertNotIn("artifacts", packets[0]["evidence"])
        self.assertEqual(
            packets[-1]["evidence"]["artifacts"]["report.json"]["json"]["total"], 20
        )
        self.assertEqual(
            packets[-1]["artifact_provenance"],
            "evaluator_after_execution_not_agent_actions",
        )
        self.assertEqual(len(trial["evidence"]["traces"]), 2)

    # 回归用例：test_turn_verdict_diagnostic_repairs_without_changing_acceptance。
    # 重点核验字段或诊断：turn_verdict_error、turn_count_mismatch、judge_validation_error；异常路径不能被当成成功。
    def test_turn_verdict_diagnostic_repairs_without_changing_acceptance(self):
        self.turn_dataset()
        self.model()
        Provider.behavior = "turn_repair"
        trial = self.evaluate()["trials"][0]
        self.assertEqual(len(REQUESTS) - self.start_requests, 2)
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        self.assertEqual(
            packet["validation_feedback"]["turn_verdict_error"]["constraint"],
            "turn_count_mismatch",
        )
        self.assertEqual(
            [row["turn"] for row in trial["evidence"]["turn_evaluation"]], [1, 2]
        )
        self.assertNotIn("judge_validation_error", trial["evidence"])

    def test_turn_status_repair_lists_allowed_values_without_waiving_failure(self):
        """目标级状态误入逐轮结果时给出确切字段约束，恢复仍保留业务失败。"""
        self.turn_dataset()
        self.model()
        Provider.behavior = "turn_goal_status_repair"
        trial = self.evaluate(code=1)["trials"][0]
        self.assertEqual(len(REQUESTS) - self.start_requests, 2)
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        diagnostic = packet["validation_feedback"]["turn_verdict_error"]
        self.assertEqual(diagnostic["allowed_verdicts"], ["passed", "failed", "uncertain"])
        self.assertEqual(diagnostic["verdict_path"], "/turn_verdicts/1/verdict")
        self.assertEqual(trial["verdict"], "failed")
        self.assertEqual(trial["execution_status"], "completed")

    def test_turn_invalid_status_diagnostic_never_copies_response_value(self):
        """持续错误保持为协议错误；诊断仅保留静态合法值，不回显模型状态正文。"""
        self.turn_dataset()
        self.model()
        for behavior in ("turn_goal_status", "turn_private_status"):
            with self.subTest(behavior=behavior):
                Provider.behavior = behavior
                trial = self.evaluate(code=3, name=behavior)["trials"][0]
                diagnostic = trial["evidence"]["judge_validation_error"]["turn_verdict_error"]
                self.assertEqual(diagnostic["allowed_verdicts"], ["passed", "failed", "uncertain"])
                self.assertEqual(diagnostic["verdict_path"], "/turn_verdicts/1/verdict")
                self.assertNotIn("诊断不可回显", json.dumps(diagnostic, ensure_ascii=False))
                self.assertEqual(trial["verdict"], "error")

    # 回归用例：test_turn_verdict_diagnostics_keep_strict_number_order_and_reason_limit。
    # 重点核验字段或诊断：turn_string_number、ordered_numeric_turn_required、turn_wrong_order；异常路径不能被当成成功。
    def test_turn_verdict_diagnostics_keep_strict_number_order_and_reason_limit(self):
        self.turn_dataset()
        self.model()
        for behavior, constraint in [
            ("turn_string_number", "ordered_numeric_turn_required"),
            ("turn_wrong_order", "ordered_numeric_turn_required"),
            ("turn_long_reason", "turn_reason_required_max_4096_bytes"),
        ]:
            with self.subTest(behavior=behavior):
                Provider.behavior = behavior
                trial = self.evaluate(code=3, name=behavior)["trials"][0]
                self.assertEqual(trial["verdict"], "error")
                diagnostic = trial["evidence"]["judge_validation_error"][
                    "turn_verdict_error"
                ]
                self.assertEqual(diagnostic["constraint"], constraint)
                self.assertEqual(diagnostic["turn"], 1)
                self.assertNotIn(
                    "诊断不可回显此内容", json.dumps(diagnostic, ensure_ascii=False)
                )

    # 回归用例：test_ungraded_turn_contract_is_uncertain。
    # 关键断言：self.assertEqual(self.evaluate(code=3)["trials"][0]["verdict"], "uncertain")；此处只验证测试构造的场景。
    def test_ungraded_turn_contract_is_uncertain(self):
        self.turn_dataset()
        self.assertEqual(self.evaluate(code=3)["trials"][0]["verdict"], "uncertain")

    # 冷启动只采集训练轨迹时不请求任务裁判，仍可进入原有分析和记忆队列。
    # 任务结果保持未知；同一报告重复导入必须复用，不能制造新的学习证据。
    def test_ungraded_training_import_preserves_unknown_and_reuses_runs(self):
        self.turn_dataset()
        self.config["checks"] = {}
        # 保留关闭前的模型配置，不应读提示词或要求裁判密钥。
        self.config["judge"] = dict(enabled=False, loop_review_prompt="missing-prompt.txt")
        report = self.evaluate(code=3)
        self.assertEqual(report["summary"]["model_calls"], 0)
        self.assertEqual(report["trials"][0]["verdict"], "uncertain")
        service = json.loads((self.root / "instrumentation.json").read_text())
        service.update(data_dir=str(self.root / "learning"), auto_evaluate=False,
                       sharing={"enabled": False})
        env = dict(self.env)
        for principal in service["principals"]:
            env[principal["token_env"]] = "training-import-test-principal-000001"
        write_json(self.root / "learning.json", service)
        command = [str(CLI), "learn", "--config", str(self.root / "learning.json"),
                   "--enqueue-only", "--training-evaluation", str(self.root / "evaluation"),
                   "--dataset", str(self.dataset)]
        for field in ("imported", "reused"):
            result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)[field], 2)
        paths = list((self.root / "learning/by-user").glob("*/traces/*/*.trace.json"))
        self.assertEqual(len(paths), 2)
        for path in paths:
            context = json.loads(path.read_text())["attributes"]["training_evaluation"]
            self.assertEqual(context["task_assessment_status"], "not_requested")
            self.assertEqual(context["verdict"], "uncertain")
            self.assertEqual(context["trial_verdict"], "uncertain")
            self.assertEqual(context["history"]["prior_turns"], context["turn"] - 1)
            self.assertEqual(context["history"]["scope"], "historical_context_not_current_actions")
            self.assertIn("current_input", context)
        self.assertEqual(len(REQUESTS), self.start_requests)

        # 即使报告文件哈希有效，也不能把请求过裁判的未知结果或异常执行
        # 当成“未评分训练”导入；拒绝时不得增加任何 Run。
        for reason in ("judge_requested", "judge_error", "timeout", "missing_marker", "model_check"):
            modified = json.loads(json.dumps(report))
            trial = modified["trials"][0]
            if reason == "judge_requested":
                modified["configuration"]["model_enabled"] = True
            elif reason == "judge_error":
                trial["error_code"] = "evaluation_judge_failed"
            elif reason == "timeout":
                trial["execution_status"] = "timeout"
            elif reason == "missing_marker":
                trial["warnings"] = []
            else:
                trial["checks"].append(dict(check_id="model", status="uncertain"))
            path = self.root / "evaluation/report.json"
            write_json(path, modified)
            manifest_path = self.root / "evaluation/manifest.json"
            manifest = json.loads(manifest_path.read_text())
            manifest["files"]["report.json"] = hashlib.sha256(path.read_bytes()).hexdigest()
            write_json(manifest_path, manifest)
            result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 2, (reason, result.stdout, result.stderr))
            self.assertIn("training_no_complete_traces", result.stderr)
        self.assertEqual(len(list((self.root / "learning/runs").glob("*.json"))), 2)

    def test_ungraded_training_shares_without_gepa_and_counts_independent_groups(self):
        """正式导入登记冻结语料；本机共享模型不能把同题多轮算成多个用户。"""
        self.turn_dataset()
        first = json.loads((self.dataset / "train.jsonl").read_text())
        second = json.loads(json.dumps(first))
        second.update(task_id="second", group_id="second", task_input="检查视频生成缺少的素材。")
        self.make_dataset([first, second])
        self.config.update(checks={}, judge=dict(enabled=False))
        self.evaluate(code=3)
        shutil.copytree(SOURCE / "config", self.root / "learning-config")
        runtime_path = self.root / "learning-config/standard-runtime.json"
        runtime = json.loads(runtime_path.read_text())
        runtime["evaluation"]["model_selection"] = "none"
        runtime["evolution"].update(enable_meta_reflection=False, enable_memory_semantic_matching=False)
        write_json(runtime_path, runtime)
        service = json.loads((self.root / "instrumentation.json").read_text())
        service.update(data_dir=str(self.root / "learning"), auto_evaluate=True,
                       runtime_config=str(runtime_path), sharing={"enabled": False})
        service["learning"].update(min_new_runs=1, min_interval_ms=0)
        for principal in service["principals"]:
            self.env[principal["token_env"]] = "training-public-test-principal-0001"
        config_path = self.root / "learning.json"
        write_json(config_path, service)
        command = [str(CLI), "learn", "--config", str(config_path)]
        result = subprocess.run(command + ["--training-evaluation", str(self.root / "evaluation"),
                                "--dataset", str(self.dataset)], env=self.env, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        reports = [json.loads(p.read_text()) for p in (self.root / "learning/by-user").glob("*/evaluation/*/*.evaluation.json")]
        self.assertEqual(len(reports), 4)
        user = reports[0]["source"]["user_id"]
        scope = reports[0]["assessments"][0]["evidence"]["scope"]
        condition = "视频生成缺少图片素材"
        encoded = subprocess.run([str(BUILD / "test_shared_memory"), "--seed-reports"],
            input=json.dumps(dict(scope=scope, condition=condition, reports=reports)).encode(),
            capture_output=True, check=True).stdout
        (self.root / "learning/by-user" / user / "memory/evolution-memory.pb").write_bytes(encoded)

        class SharingModel(http.server.BaseHTTPRequestHandler):
            def do_POST(handler):
                packet = json.loads(handler.rfile.read(int(handler.headers["Content-Length"])))
                packet = json.loads(packet["messages"][-1]["content"])
                if "draft" in packet:
                    answer = dict(decision="new", match_id="", privacy_safe=True, generalizable=True,
                                  supported=True, non_overriding=True, confidence=.99)
                elif "source" in packet:
                    answer = dict(shareable=True, target_scope=scope, trigger_pattern=condition,
                                  content="缺少图片素材时先请求补充，独立检索继续。")
                else:
                    answer = dict(relation="duplicate", confidence=.99)
                raw = json.dumps(dict(choices=[dict(message=dict(content=json.dumps(answer)), finish_reason="stop")])).encode()
                handler.send_response(200); handler.send_header("Content-Length", str(len(raw)))
                handler.end_headers(); handler.wfile.write(raw)
            def log_message(self, *_): pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), SharingModel)
        thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
        try:
            runtime["model"].update(endpoint=f"http://127.0.0.1:{server.server_port}/chat/completions",
                                    model_id="local-public", no_api_key=True, api_key_env="", max_retries=0)
            write_json(runtime_path, runtime)
            service["sharing"] = dict(enabled=True, min_interval_ms=10, min_training_groups=2)
            write_json(config_path, service)
            result = subprocess.run(command, env=self.env, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            state = json.loads((self.root / "learning/learning/shared-memory.json").read_text())
            published = [e for e in state["entries"].values() if e["state"] == "published"]
            self.assertEqual(len(published), 1, state)
            self.assertEqual(published[0]["supporting_training_groups"], 2)
            self.assertEqual(published[0]["supporting_users"], 0)
            self.assertNotIn("policy_evaluation_file", service)
            # 冻结来源无法继续验证时撤回支持，不能永久信任一份路径登记。
            manifest = self.dataset / "manifest.json"
            saved = self.dataset / "manifest.unavailable"
            manifest.rename(saved)
            try:
                result = subprocess.run(command, env=self.env, capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stderr)
                public = json.loads((self.root / "learning/memory/evolution-memory.json").read_text())
                self.assertFalse(public.get("experiences"))
            finally:
                saved.rename(manifest)
        finally:
            server.shutdown(); server.server_close(); thread.join()

    # 三个模型开关独立：有无任务评分 × 轨迹模型分析 × 元反思，共八种路径。
    # 执行入口生成真实协议的身份与多轮边界；内部动作替换为已有的固定重复失败
    # 轨迹，用来验证学习链路，不把本地桩的结果当作业务 Agent 能力评测。
    def test_training_memory_model_switch_matrix(self):
        self.turn_dataset()
        self.model()
        self.config["checks"] = {}
        requests = []

        class LearningModel(http.server.BaseHTTPRequestHandler):
            # 本机固定响应只使用请求中已有的事件和种子，校验仍由正式代码执行。
            def do_POST(handler):
                body = json.loads(handler.rfile.read(int(handler.headers["Content-Length"])))
                text = body["messages"][-1]["content"]
                marker = "输入候选：\n" if "输入候选：\n" in text else "输入证据：\n"
                payload, _ = json.JSONDecoder().raw_decode(text.split(marker, 1)[1])
                requests.append(payload)
                if "candidate_seeds" in payload:
                    response = {"candidate_groups": [dict(
                        seed_ids=group["seed_ids"],
                        trigger_pattern="发票读取权限未恢复且相同请求反复失败时",
                        observed_problem="非重试错误后仍然重复调用。",
                        proposed_change="权限未恢复时停止重复请求，说明缺少授权后等待补充。",
                        expected_benefit="减少确定性失败后的无效循环。",
                        confidence=0.95,
                    ) for group in payload["allowed_seed_groups"]]}
                else:
                    event_ids = [item["event_id"] for item in payload["content_events"]
                                 if "event_id" in item]
                    response = dict(
                        task_outcome=dict(status="unknown", confidence=0.9,
                                          reason="动作结果不足以判定整题业务是否通过。",
                                          evidence_event_ids=event_ids[:1]),
                        episode_assessments=[dict(
                            episode_id=episode, verdict="invalid", confidence=0.95,
                            expected_state_change="读取发票结果", observed_state_change="权限仍然缺失",
                            concise_reason="相同非重试错误重复发生。", evidence_event_ids=event_ids[:1],
                        ) for episode in payload["selected_episode_ids"]],
                        reflection=dict(summary="授权恢复前不应重复执行。", root_causes=[],
                                        skill_orchestration_changes=[], experience_injections=[],
                                        stop_condition_changes=[], wasted_episode_ids=[],
                                        evidence_event_ids=event_ids[:1]),
                    )
                encoded = json.dumps({"choices": [{"message": {"role": "assistant",
                    "content": json.dumps(response, ensure_ascii=False)}, "finish_reason": "stop"}]}).encode()
                handler.send_response(200)
                handler.send_header("Content-Type", "application/json")
                handler.send_header("Content-Length", str(len(encoded)))
                handler.end_headers()
                handler.wfile.write(encoded)

            # 禁止把测试请求正文输出到访问日志。
            def log_message(handler, *_):
                pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), LearningModel)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        fixture = json.loads((SOURCE / "examples/standard-agent/one.trace.json").read_text())
        for judge in (False, True):
            self.config["judge"]["enabled"] = judge
            Provider.behavior = "turn_failed"
            name = "scored" if judge else "collected"
            report = self.evaluate(code=1 if judge else 3, name=name)
            # 保留执行器绑定的 L0 版本、身份与训练来源；固定动作提供规则种子。
            for index, trace in enumerate(report["trials"][0]["evidence"]["traces"]):
                start, end = trace["events"][0], trace["events"][-1]
                body = json.loads(json.dumps(fixture["events"][1:-1]))
                for event in body:
                    if "operation" in event:
                        event["operation"] += f"_{index}"
                    if index and "error" in event:
                        event["error"]["code"] = "unsupported_operation"
                body[0]["caused_by"] = [start["event_id"]]
                end["caused_by"] = [start["event_id"], body[-1]["event_id"]]
                trace["attributes"].pop("metrics", None)
                trace["events"] = [start, *body, end]
            report_path = self.root / name / "report.json"
            write_json(report_path, report)
            manifest_path = self.root / name / "manifest.json"
            manifest = json.loads(manifest_path.read_text())
            manifest["files"]["report.json"] = hashlib.sha256(report_path.read_bytes()).hexdigest()
            write_json(manifest_path, manifest)
            for analysis in (False, True):
                for meta in (False, True):
                    with self.subTest(judge=judge, analysis=analysis, meta=meta):
                        label = f"mode-{int(judge)}{int(analysis)}{int(meta)}"
                        root = self.root / label
                        root.mkdir()
                        runtime = json.loads((self.root / "runtime.json").read_text())
                        shutil.copytree(self.root / "prompts", root / "prompts")
                        runtime["evaluation"].update(model_selection="all" if analysis else "none",
                                                     strict_model=True)
                        runtime["evolution"].update(enable_meta_reflection=meta,
                                                   enable_memory_semantic_matching=False)
                        runtime["model"].update(endpoint=f"http://127.0.0.1:{server.server_port}/v1/chat/completions",
                                                 api_key_env="LEARNING_MATRIX_KEY")
                        write_json(root / "runtime.json", runtime)
                        service = json.loads((self.root / "instrumentation.json").read_text())
                        service.update(data_dir=str(root / "workspace"), auto_evaluate=True,
                                       runtime_config=str(root / "runtime.json"), sharing={"enabled": False})
                        write_json(root / "service.json", service)
                        env = dict(self.env)
                        # 全关闭时故意不提供模型凭据，验证不会被残留 endpoint 阻塞。
                        env.pop("LEARNING_MATRIX_KEY", None)
                        if analysis or meta:
                            env["LEARNING_MATRIX_KEY"] = KEY
                        for principal in service["principals"]:
                            env[principal["token_env"]] = "training-import-test-principal-000001"
                        command = [str(CLI), "learn", "--config", str(root / "service.json"),
                                   "--training-evaluation", str(self.root / name), "--dataset", str(self.dataset)]
                        begin = len(requests)
                        first = subprocess.run(command, env=env, capture_output=True, text=True, timeout=30)
                        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
                        state = json.loads(first.stdout)
                        self.assertEqual(state["imported"], 2)
                        self.assertEqual(state["run_outcomes"]["completed"], 2)
                        memories = list((root / "workspace/by-user").glob("*/memory/evolution-memory.json"))
                        self.assertEqual(len(memories), 1)
                        memory = json.loads(memories[0].read_text())
                        self.assertGreater(len(memory["entries"]), 0, label)
                        batches = list((root / "workspace/by-user").glob("*/batches/*/batch.evaluation.json"))
                        self.assertEqual(len(batches), 1)
                        batch = json.loads(batches[0].read_text())
                        self.assertEqual(batch["meta_reflection"]["status"],
                                         "MODEL_CALL_STATUS_SUCCEEDED" if meta else "MODEL_CALL_STATUS_NOT_REQUESTED")
                        sent = requests[begin:]
                        self.assertEqual(sum("selected_episode_ids" in r for r in sent), 2 if analysis else 0)
                        self.assertEqual(sum("candidate_seeds" in r for r in sent), 1 if meta else 0)
                        before = memories[0].read_bytes()
                        count = len(requests)
                        repeated = subprocess.run(command, env=env, capture_output=True, text=True, timeout=15)
                        self.assertEqual(repeated.returncode, 0, repeated.stderr)
                        self.assertEqual(json.loads(repeated.stdout)["reused"], 2)
                        self.assertEqual(memories[0].read_bytes(), before)
                        self.assertEqual(len(requests), count)

    # 未配置评分器的验证和留出任务仍拒绝执行，不把采集模式扩散到策略比较。
    def test_collection_only_is_restricted_to_train(self):
        self.config.update(checks={}, judge=dict(enabled=False))
        task = json.loads((self.dataset / "train.jsonl").read_text().splitlines()[0])
        for split in ("val", "holdout"):
            task["split"] = split
            self.make_dataset([task])
            self.config["splits"] = [split]
            self.evaluate(code=2, name=split)
            self.assertFalse((self.root / split).exists())

    # 回归用例：test_turn_regrade_does_not_reuse_previous_verdicts。
    # 关键断言：self.assertIn("turn_evaluation", first["evidence"])；此处只验证测试构造的场景。
    def test_turn_regrade_does_not_reuse_previous_verdicts(self):
        self.turn_dataset()
        self.model()
        Provider.behavior = "turn_failed"
        first = self.evaluate(code=1)["trials"][0]
        self.assertIn("turn_evaluation", first["evidence"])
        Provider.behavior = "turn_missing"
        trial = self.evaluate(
            code=3,
            name="regraded",
            extra=("--regrade-report", self.root / "evaluation/report.json"),
        )["trials"][0]
        self.assertEqual(trial["evidence"]["traces"], first["evidence"]["traces"])
        self.assertNotIn("turn_evaluation", trial["evidence"])
        for request in REQUESTS[self.start_requests :]:
            packet = json.loads(request["messages"][1]["content"])
            self.assertNotIn("turn_evaluation", packet["evidence"])

    # 回归用例：test_initial_state_file_does_not_require_entry_state_binding。
    # 重点核验字段或诊断：initial_state_transport；异常路径不能被当成成功。
    def test_initial_state_file_does_not_require_entry_state_binding(self):
        self.entry_mode("--initial-file")
        variant = self.config["variants"][0]
        variant["initial_state_transport"] = "file"
        variant["entry"]["input"]["initial"] = {}
        report = self.evaluate()
        self.assertEqual(report["trials"][0]["execution_status"], "completed")
        self.assertEqual(report["trials"][0]["verdict"], "passed")

    # 回归用例：test_training_import_attributes_only_the_current_turn。
    # 重点核验字段或诊断：current_turn_only；异常路径不能被当成成功。
    def test_training_import_attributes_only_the_current_turn(self):
        self.model()
        for graded in (True, False):
            with self.subTest(graded=graded):
                self.turn_dataset()
                Provider.behavior = "turn_failed"
                if not graded:
                    self.config.pop("judge")
                    # 确定性整题检查失败、逐轮未评分：可导入，但不得将失败复制给每轮。
                    self.config["checks"]["*"][0]["value"] = 21
                name = "graded" if graded else "ungraded"
                report = self.evaluate(code=1, name=name)
                service = json.loads((self.root / "instrumentation.json").read_text())
                service.update(
                    data_dir=str(self.root / name / "learning"),
                    auto_evaluate=False,
                    sharing={"enabled": False},
                )
                env = dict(self.env)
                for principal in service["principals"]:
                    env[principal["token_env"]] = (
                        "training-import-test-principal-000001"
                    )
                write_json(self.root / "learning.json", service)
                command = [
                    str(CLI),
                    "learn",
                    "--config",
                    str(self.root / "learning.json"),
                    "--enqueue-only",
                    "--training-evaluation",
                    str(self.root / name),
                    "--dataset",
                    str(self.dataset),
                ]
                imported = subprocess.run(
                    command, env=env, capture_output=True, text=True, timeout=15
                )
                self.assertEqual(imported.returncode, 0, imported.stderr)
                self.assertEqual(json.loads(imported.stdout)["imported"], 2)
                paths = list(
                    (self.root / name / "learning/by-user").glob(
                        "*/traces/*/*.trace.json"
                    )
                )
                contexts = sorted(
                    (
                        json.loads(p.read_text())["attributes"]["training_evaluation"]
                        for p in paths
                    ),
                    key=lambda context: context["turn"],
                )
                self.assertEqual(len(contexts), 2)
                self.assertEqual(
                    [c["verdict"] for c in contexts],
                    ["passed", "failed"] if graded else ["uncertain", "uncertain"],
                )
                for index, context in enumerate(contexts):
                    self.assertEqual(
                        context["trial_verdict"], report["trials"][0]["verdict"]
                    )
                    self.assertEqual(context["attribution"], "current_turn_only")
                    self.assertEqual(context["checks"], [])
                    self.assertEqual(context["turn_count"], 2)
                    self.assertEqual(
                        context["expected_behavior"],
                        json.loads((self.dataset / "train.jsonl").read_text())[
                            "attributes"
                        ]["turn_expectations"][index]["expected_behavior"],
                    )

    # 回归用例：test_report_budget_covers_full_report_not_half_the_trial_evidence。
    # 重点核验字段或诊断：max_report_bytes；异常路径不能被当成成功。
    def test_report_budget_covers_full_report_not_half_the_trial_evidence(self):
        host = self.config["variants"][0]["command"]
        self.config["variants"][0]["command"] = [
            sys.executable,
            "-c",
            "import json, subprocess, sys\n"
            f"response=json.loads(subprocess.run({host!r},input=sys.stdin.read(),capture_output=True,text=True,check=True).stdout)\n"
            "response['state']['details']='verified payload '*1400\nprint(json.dumps(response))\n",
        ]
        self.config["max_report_bytes"] = 40000
        report = self.evaluate()
        trial_bytes = len(
            json.dumps(
                report["trials"][0], ensure_ascii=False, separators=(",", ":")
            ).encode()
        )
        report_bytes = (self.root / "evaluation/report.json").stat().st_size
        self.assertGreater(trial_bytes, self.config["max_report_bytes"] // 2)
        self.assertLessEqual(report_bytes, self.config["max_report_bytes"])

        # 试验轨迹能放下还不够，汇总和元数据也必须满足总上限。
        self.config["max_report_bytes"] = trial_bytes + 1024
        self.assertLess(self.config["max_report_bytes"], report_bytes)
        self.evaluate(code=2, name="limited")
        self.assertTrue(list((self.root / "limited/trials").glob("*.json")))
        self.assertFalse((self.root / "limited/report.json").exists())

    # 容量配置允许大批次，但不预分配整份预算；所有输出保持同一试验顺序和摘要。
    def test_large_report_capacity_keeps_formats_and_hard_limit(self):
        self.config["max_report_bytes"] = 1073741824
        report = self.evaluate(name="large-capacity")
        output = self.root / "large-capacity"
        rows = [json.loads(line) for line in (output / "results.jsonl").read_text().splitlines()]
        self.assertEqual(rows, report["trials"])
        manifest = json.loads((output / "manifest.json").read_text())
        for name, digest in manifest["files"].items():
            self.assertEqual(hashlib.sha256((output / name).read_bytes()).hexdigest(), digest)
        # 用合法 JSON 尾随空白跨过旧 128 MiB 读取上限，避免制造大批假轨迹或调用模型。
        # 文件摘要按实际字节更新；恢复仍须核对原试验身份和配置，不能只测试配置能加载。
        report_path = output / "report.json"
        with report_path.open("ab") as stream:
            for _ in range(128):
                stream.write(b" " * 1048576)
        digest = hashlib.sha256()
        with report_path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1048576), b""):
                digest.update(chunk)
        manifest["files"]["report.json"] = digest.hexdigest()
        (output / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.assertGreater(report_path.stat().st_size, 134217728)
        restored = self.evaluate(
            extra=("--resume-report", str(report_path)), name="large-capacity-resumed"
        )
        self.assertEqual(restored["summary"]["reused_trials"], len(report["trials"]))
        self.assertEqual(restored["trials"], report["trials"])

        self.config["max_report_bytes"] += 1
        self.evaluate(code=2, name="over-capacity")
        self.assertFalse((self.root / "over-capacity" / "report.json").exists())

    # 测试辅助函数：counted_actions。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
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

    # 回归用例：test_action_count_covers_parallel_failures_and_model_cannot_override。
    # 重点核验字段或诊断：loop_review_prompt；异常路径不能被当成成功。
    def test_action_count_covers_parallel_failures_and_model_cannot_override(self):
        self.model()
        self.counted_actions()
        self.config["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
        Provider.behavior = "loops_detailed"
        self.config["checks"]["*"].append(
            dict(id="calls", op="action_count", operation="calculate", max=3)
        )
        report = self.evaluate(code=1)
        trial = report["trials"][0]
        count = next(c for c in trial["checks"] if c["check_id"] == "calls")
        self.assertEqual(count["status"], "failed")
        self.assertIn("calculate 4 次", count["reason"])
        self.assertEqual(trial["checks"][-1]["check_id"], "model")
        self.assertEqual(trial["checks"][-1]["status"], "passed")
        self.assertEqual(trial["evidence"]["loop_review"]["status"], "complete")
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        self.assertIn(
            dict(op="action_count", operation="calculate", max=3),
            packet["required_checks"],
        )
        self.assertTrue(all("id" not in check for check in packet["required_checks"]))

    # 回归用例：test_judge_receives_observed_action_counts_without_inventing_observations。
    # 重点核验字段或诊断：observed_action_counts；异常路径不能被当成成功。
    def test_judge_receives_observed_action_counts_without_inventing_observations(self):
        self.model()
        host = self.config["variants"][0]["command"].copy()
        for name, skipped, expected in (
            ("all", False, 4),
            ("skipped", True, 3),
            ("unknown", None, None),
        ):
            with self.subTest(name=name):
                self.config["variants"][0]["command"] = host.copy()
                if skipped is None:
                    self.mode("loop_trace")
                else:
                    self.counted_actions(skipped=skipped)
                report = self.evaluate(name=name)
                packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
                self.assertEqual(
                    packet["observed_action_counts"], {"calculate": expected}
                )
                self.assertNotIn(
                    "observed_action_counts", report["trials"][0]["evidence"]
                )
        self.config["variants"][0]["command"] = host
        self.evaluate(name="no-trace")
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        self.assertNotIn("observed_action_counts", packet)

    # 回归用例：test_judge_receives_only_current_task_and_global_check_constraints。
    # 重点核验字段或诊断：loop_review_prompt、not_this_task；异常路径不能被当成成功。
    def test_judge_receives_only_current_task_and_global_check_constraints(self):
        self.make_dataset(
            cases=[
                dict(
                    task_id=name,
                    group_id=name,
                    task_input=f"计算{name}任务的总量20。",
                    expected_behavior="总量为20。",
                    initial_state=dict(value=20),
                    difficulty="easy",
                    category="data",
                    split="train",
                    review_status="reviewed",
                )
                for name in ("one", "unrelated")
            ]
        )
        self.config["task_ids"] = ["one"]
        self.model()
        self.counted_actions()
        self.config["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
        Provider.behavior = "loops_detailed"
        self.config["checks"].update(
            one=[dict(id="calls", op="action_count", operation="calculate", max=4)],
            unrelated=[
                dict(
                    id="not_this_task", op="action_count", operation="unrelated", max=0
                )
            ],
        )
        report = self.evaluate()
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        self.assertIn(
            dict(op="action_count", operation="calculate", max=4),
            packet["required_checks"],
        )
        self.assertIn(
            dict(path="/state/value", op="equals", value=20), packet["required_checks"]
        )
        self.assertFalse(
            any(c.get("operation") == "unrelated" for c in packet["required_checks"])
        )
        budgets = {
            c["path"] for c in packet["required_checks"] if c.get("op") == "at_most"
        }
        self.assertEqual(budgets, {"/metrics/steps", "/metrics/tool_calls"})
        self.assertNotIn("required_checks", report["trials"][0]["evidence"])

    # 回归用例：test_action_count_filters_status_and_excludes_unexecuted_calls。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_action_count_filters_status_and_excludes_unexecuted_calls(self):
        self.counted_actions(skipped=True)
        self.config["checks"]["*"].extend(
            [
                dict(
                    id="calls", op="action_count", operation="calculate", min=3, max=3
                ),
                dict(
                    id="failed",
                    op="action_count",
                    operation="calculate",
                    status="failed",
                    max=0,
                ),
                dict(
                    id="other", op="action_count", operation="another-operation", max=0
                ),
            ]
        )
        self.evaluate()

    # 回归用例：test_action_count_unknown_observation_is_error_not_zero。
    # 重点核验字段或诊断：evaluation_check_evidence_invalid；异常路径不能被当成成功。
    def test_action_count_unknown_observation_is_error_not_zero(self):
        self.config["checks"]["*"].append(
            dict(id="calls", op="action_count", operation="calculate", max=100)
        )
        for mode in ("good", "loop_trace"):
            self.mode(mode)
            report = self.evaluate(code=3, name=mode)
            self.assertEqual(report["trials"][0]["verdict"], "error")
            self.assertEqual(
                report["trials"][0]["error_code"], "evaluation_check_evidence_invalid"
            )

    # 回归用例：test_recheck_adds_rules_without_reexecuting_or_replacing_source。
    # 重点核验字段或诊断：rechecked_configured_checks、model_only_regrade；异常路径不能被当成成功。
    def test_recheck_adds_rules_without_reexecuting_or_replacing_source(self):
        self.model()
        self.counted_actions()
        first = self.evaluate()
        source = self.root / "evaluation/report.json"
        original = source.read_bytes()
        self.config["variants"][0]["command"] = ["/bin/false"]
        self.config["checks"]["*"].append(
            dict(id="calls", op="action_count", operation="calculate", max=1)
        )
        model_only = self.evaluate(
            name="model-only", extra=("--regrade-report", source)
        )
        self.assertEqual(model_only["trials"][0]["verdict"], "passed")
        report = self.evaluate(
            code=1, name="rechecked", extra=("--regrade-report", source, "--recheck")
        )
        trial = report["trials"][0]
        self.assertEqual(trial["verdict"], "failed")
        self.assertEqual(trial["trial_id"], first["trials"][0]["trial_id"])
        self.assertEqual(trial["duration_ms"], first["trials"][0]["duration_ms"])
        self.assertEqual(
            trial["evidence"]["trace"], first["trials"][0]["evidence"]["trace"]
        )
        self.assertEqual(trial["checks"][-1]["status"], "passed")
        self.assertTrue(report["configuration"]["rechecked_configured_checks"])
        self.assertFalse(report["configuration"]["model_only_regrade"])
        self.assertNotEqual(
            report["config_fingerprint"], model_only["config_fingerprint"]
        )
        self.assertEqual(source.read_bytes(), original)

    # 回归用例：test_recheck_replaces_matching_rule_and_preserves_artifact_failure。
    # 关键断言：self.assertEqual(checks["artifact"], "failed")；此处只验证测试构造的场景。
    def test_recheck_replaces_matching_rule_and_preserves_artifact_failure(self):
        self.model()
        self.mode("missing")
        self.config["checks"]["*"][0]["value"] = -1
        self.evaluate(code=1)
        self.config["checks"]["*"][0]["value"] = 20
        report = self.evaluate(
            code=1,
            name="rechecked",
            extra=(
                "--regrade-report",
                self.root / "evaluation/report.json",
                "--recheck",
            ),
        )
        checks = {c["check_id"]: c["status"] for c in report["trials"][0]["checks"]}
        self.assertEqual(checks["artifact"], "failed")
        self.assertEqual(checks["value"], "passed")
        self.assertEqual(checks["model"], "passed")

    # 回归用例：test_action_count_configuration_is_checked_before_execution。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_action_count_configuration_is_checked_before_execution(self):
        for index, spec in enumerate(
            [
                dict(operation="calculate"),
                dict(operation="calculate", min=3, max=1),
                dict(operation="", max=1),
                dict(operation="calculate", max=-1),
                dict(operation="calculate", max=1.5),
                dict(operation="calculate", max=1, status="invented"),
                dict(operation="calculate", max=1, path="/output"),
            ]
        ):
            self.config["checks"] = {"*": [dict(id="calls", op="action_count", **spec)]}
            self.assertIsNone(self.evaluate(code=2, name="invalid-count-" + str(index)))
        self.config["checks"] = {
            "*": [dict(id="artifact", op="exists", path="/output")]
        }
        self.assertIsNone(self.evaluate(code=2, name="reserved-id"))
        self.config["checks"] = {"*": [dict(id="output", op="exists", path="/output")]}
        self.assertIsNone(
            self.evaluate(code=2, name="missing-regrade", extra=("--recheck",))
        )

    # 回归用例：test_internal_contract_check_id_is_reserved_before_execution。
    # 关键断言：self.assertFalse((self.root / "reserved-contract-id").exists())；此处只验证测试构造的场景。
    def test_internal_contract_check_id_is_reserved_before_execution(self):
        self.config["checks"] = {
            "*": [dict(id="contract.schedule_success", op="exists", path="/output")]
        }
        self.assertIsNone(self.evaluate(code=2, name="reserved-contract-id"))
        self.assertFalse((self.root / "reserved-contract-id").exists())

    # 回归用例：test_cli_resume_reuses_execution_and_regrades_only_judge_errors。
    # 重点核验字段或诊断：evaluation_judge_failed；异常路径不能被当成成功。
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
        third = self.evaluate(
            name="resumed-again",
            extra=("--resume-report", self.root / "resumed/report.json"),
        )
        self.assertEqual(third["summary"]["reused_trials"], 1)
        self.assertEqual(third["summary"]["model_calls"], 0)
        self.assertEqual(counter.read_text(), "1")
        self.assertEqual(source.read_bytes(), original)
        self.evaluate(
            code=2,
            name="conflict",
            extra=("--resume-report", source, "--regrade-report", source),
        )
        self.config["variants"][0]["version"] = "changed"
        self.evaluate(code=2, name="changed", extra=("--resume-report", source))
        self.assertEqual(counter.read_text(), "1")

    def test_resume_reexecutes_failed_trial_instead_of_caching_infrastructure_failure(self):
        """普通 retry 不复用 failed 执行；同一命令恢复后必须产生一次新的真实 trial。"""
        counter = self.root / "failed-resume-count"
        mode = self.root / "failed-resume-mode"
        mode.write_text("failed")
        host = str(SOURCE / "tests/evaluation_host.py")
        self.config["variants"][0]["command"] = [
            sys.executable,
            "-c",
            "import pathlib, subprocess, sys\n"
            f"counter=pathlib.Path({str(counter)!r})\n"
            "counter.write_text(str(int(counter.read_text())+1) if counter.exists() else '1')\n"
            f"mode=pathlib.Path({str(mode)!r}).read_text()\n"
            f"result=subprocess.run([sys.executable,'-B',{host!r},mode],input=sys.stdin.read(),text=True,capture_output=True)\n"
            "sys.stdout.write(result.stdout); sys.stderr.write(result.stderr); sys.exit(result.returncode)\n",
        ]
        first = self.evaluate()
        self.assertEqual(first["trials"][0]["execution_status"], "failed")
        source = self.root / "evaluation/report.json"
        mode.write_text("good")
        second = self.evaluate(name="resumed", extra=("--resume-report", source))
        self.assertEqual(second["trials"][0]["execution_status"], "completed")
        self.assertEqual(counter.read_text(), "2")
        self.assertEqual(second["summary"].get("reused_trials", 0), 0)

    def test_resume_rejects_duplicate_trial_identity_before_agent_execution(self):
        """报告和 manifest 同时被改写也不能让重复 task/repetition 冒充两个结果。"""
        counter = self.root / "duplicate-resume-count"
        host = self.config["variants"][0]["command"]
        self.config["variants"][0]["command"] = [
            sys.executable,
            "-c",
            "import pathlib, subprocess, sys\n"
            f"counter=pathlib.Path({str(counter)!r})\n"
            "counter.write_text(str(int(counter.read_text())+1) if counter.exists() else '1')\n"
            f"subprocess.run({host!r},input=sys.stdin.read(),text=True,check=True)\n",
        ]
        first = self.evaluate()
        source = self.root / "evaluation/report.json"
        first["trials"].append(dict(first["trials"][0]))
        write_json(source, first)
        manifest_path = source.parent / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["files"]["report.json"] = hashlib.sha256(source.read_bytes()).hexdigest()
        write_json(manifest_path, manifest)
        self.evaluate(code=2, name="duplicate-resume", extra=("--resume-report", source))
        self.assertEqual(counter.read_text(), "1")

    def test_resume_regrades_uncertain_without_reexecuting_agent(self):
        """裁判未知时只重评原轨迹；保持业务执行身份、耗时、指标与原报告不变。"""
        self.model()
        host = self.config["variants"][0]["command"][:-1] + ["trace"]
        counter = self.root / "host-count"
        self.config["variants"][0]["command"] = [sys.executable, "-c",
            "import pathlib, subprocess, sys\n"
            f"counter=pathlib.Path({str(counter)!r})\n"
            "counter.write_text(str(int(counter.read_text())+1) if counter.exists() else '1')\n"
            f"subprocess.run({host!r}, input=sys.stdin.read(), text=True, check=True)\n"]
        Provider.behavior = "uncertain"
        first = self.evaluate(code=3)
        source = self.root / "evaluation/report.json"
        original = source.read_bytes()
        self.assertEqual(first["trials"][0]["verdict"], "uncertain")
        # 真正缺少证据仍须保持未知，不能由恢复分支直接改成通过。
        still_unknown = self.evaluate(
            name="still-unknown", code=3, extra=("--resume-report", source))
        self.assertEqual(still_unknown["trials"][0]["verdict"], "uncertain")
        self.assertEqual(still_unknown["summary"]["regraded_trials"], 1)
        Provider.behavior = "passed"
        resumed = self.evaluate(
            name="resumed", extra=("--resume-report", self.root / "still-unknown/report.json"))
        before, after = first["trials"][0], resumed["trials"][0]
        self.assertEqual(after["verdict"], "passed")
        for field in ("trial_id", "duration_ms", "execution_status", "metrics"):
            self.assertEqual(before.get(field), after.get(field))
        self.assertEqual(before["evidence"]["trace"], after["evidence"]["trace"])
        self.assertEqual(resumed["summary"]["regraded_trials"], 1)
        self.assertEqual(resumed["summary"]["model_calls"], 1)
        self.assertEqual(counter.read_text(), "1")
        self.assertEqual(source.read_bytes(), original)

    def test_resume_reuses_explicitly_ungraded_training_without_agent_calls(self):
        """训练不请求裁判时，续跑复用完整轨迹，结果仍保持未评分。"""
        self.turn_dataset()
        self.config["checks"] = {}
        self.config["judge"] = dict(enabled=False, loop_review_prompt="unused.txt")
        counter = self.root / "capture-count"
        host = self.config["variants"][0]["command"]
        self.config["variants"][0]["command"] = [sys.executable, "-c",
            "import pathlib, subprocess, sys\n"
            f"counter=pathlib.Path({str(counter)!r})\n"
            "counter.write_text(str(int(counter.read_text())+1) if counter.exists() else '1')\n"
            f"subprocess.run({host!r},input=sys.stdin.read(),text=True,check=True)\n"]
        first = self.evaluate(code=3)
        source = self.root / "evaluation/report.json"
        second = self.evaluate(code=3, name="resumed", extra=("--resume-report", source))
        self.assertEqual(counter.read_text(), "1")
        self.assertEqual(second["summary"]["reused_trials"], 1)
        self.assertEqual(first["trials"], second["trials"])
        self.assertEqual(second["trials"][0]["verdict"], "uncertain")
        self.assertEqual(len(REQUESTS), self.start_requests)

    def test_cancelled_complete_capture_resumes_without_agent_calls(self):
        """所有题已完成但整批取消时，原生恢复只重新汇总，不重新执行或评分。"""
        self.turn_dataset()
        self.config['checks'] = {}
        self.config['judge'] = dict(enabled=False)
        counter = self.root / 'execution-count'
        host = self.config['variants'][0]['command']
        self.config['variants'][0]['command'] = [sys.executable, '-c',
            'import pathlib, subprocess, sys\n'
            f'counter=pathlib.Path({str(counter)!r})\n'
            "counter.write_text(str(int(counter.read_text())+1) if counter.exists() else '1')\n"
            f'subprocess.run({host!r}, input=sys.stdin.read(), text=True, check=True)\n']
        first = self.evaluate(code=3)
        source = self.root / 'evaluation/report.json'
        # 构造取消落在最后一题完成之后的合法检查点；保留原始单题、配置及执行身份。
        first['complete'] = False
        write_json(source, first)
        manifest_path = source.parent / 'manifest.json'
        manifest = json.loads(manifest_path.read_text())
        manifest['complete'] = False
        manifest['files']['report.json'] = hashlib.sha256(source.read_bytes()).hexdigest()
        write_json(manifest_path, manifest)
        original = source.read_bytes()
        resumed = self.evaluate(code=3, name='resumed', extra=('--resume-report', source))
        self.assertEqual(resumed['summary']['reused_trials'], 1)
        self.assertEqual(resumed['summary']['model_calls'], 0)
        self.assertEqual(first['trials'], resumed['trials'])
        self.assertEqual(counter.read_text(), '1')
        self.assertEqual(source.read_bytes(), original)
        self.assertEqual(len(REQUESTS), self.start_requests)

    def test_parallel_results_are_complete_when_first_visible(self):
        """持续读取并行产生的大结果；最终文件名出现时必须已是完整、私有的 JSON。"""
        host = self.config['variants'][0]['command']
        self.config['variants'][0]['command'] = [sys.executable, '-c',
            'import json, subprocess, sys\n'
            f'response=json.loads(subprocess.run({host!r}, input=sys.stdin.read(), capture_output=True, text=True, check=True).stdout)\n'
            "response['state']['notes']='已核验的数据。'*12000\nprint(json.dumps(response, ensure_ascii=False))\n"]
        self.config.update(repetitions=16, parallelism=8, timeout_ms=10000,
                           max_output_bytes=1048576, max_report_bytes=33554432)
        write_json(self.root / 'evaluation.json', self.config)
        destination = self.root / 'atomic-results'
        process = subprocess.Popen([str(CLI), 'dataset', 'run', str(self.dataset),
            '--config', str(self.root / 'evaluation.json'), '--output-root', str(destination)],
            env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        seen = set()
        deadline = time.monotonic() + 30
        try:
            while True:
                finished = process.poll() is not None
                paths = list((destination / 'trials').glob('*.json')) + list(destination.glob('*.json'))
                for path in paths:
                    if path in seen:
                        continue
                    value = json.loads(path.read_bytes())
                    self.assertIsInstance(value, dict)
                    self.assertEqual(path.stat().st_mode & 0o777, 0o600)
                    seen.add(path)
                if finished:
                    break
                self.assertLess(time.monotonic(), deadline, '并行写入测试超时')
                time.sleep(0.001)
            stdout, stderr = process.communicate(timeout=5)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
        self.assertEqual(process.returncode, 0, stdout + stderr)
        self.assertEqual(len(list((destination / 'trials').glob('*.json'))), 16)
        self.assertEqual(len([path for path in seen if path.parent.name == 'trials']), 16)
        self.assertFalse(list(destination.rglob('*.pending.*')))
        self.assertTrue(json.loads((destination / 'report.json').read_text())['complete'])

    def test_judge_fingerprint_binds_content_not_paths_or_batch_quota(self):
        """裁判正文改变必须使指纹失效；资源搬家和整批配额不改变已完成评分口径。"""
        self.model()
        self.mode("trace")
        first = self.evaluate()
        source = self.root / "evaluation/report.json"
        stamp = first["configuration"]["judge_fingerprint"]
        self.assertRegex(stamp, r"^[a-f0-9]{64}$")
        prompt = self.root / "prompts/task-judge-system.txt"
        alias = self.root / "same-judge.txt"
        alias.write_text(prompt.read_text())
        self.config["judge"].update(prompt=alias.name, max_model_calls=20)
        self.mode("nonzero")  # 错误地重新执行 Agent 时，测试不能通过。
        same = self.evaluate(name="same-judge", extra=("--regrade-report", source))
        self.assertEqual(same["configuration"]["judge_fingerprint"], stamp)
        alias.write_text(alias.read_text() + "\n核对返回值，不能仅凭成功声明。\n")
        changed = self.evaluate(name="changed-judge", extra=("--regrade-report", source))
        self.assertNotEqual(changed["configuration"]["judge_fingerprint"], stamp)
        for report in (same, changed):
            self.assertEqual(report["summary"]["model_calls"], 1)
            for field in ("trial_id", "duration_ms", "execution_status", "metrics"):
                self.assertEqual(report["trials"][0].get(field), first["trials"][0].get(field))

    def test_legacy_resume_adds_judge_fingerprint_without_reexecution(self):
        """旧报告仅缺裁判标记时按完整指纹升级；已有错误标记不能被覆盖。"""
        self.model()
        self.mode("trace")
        first = self.evaluate()
        source = self.root / "evaluation/report.json"
        stamp = first["configuration"].pop("judge_fingerprint")
        write_json(source, first)
        manifest_path = source.parent / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["files"]["report.json"] = hashlib.sha256(source.read_bytes()).hexdigest()
        write_json(manifest_path, manifest)
        original = source.read_bytes()
        before = len(REQUESTS)
        resumed = self.evaluate(name="resumed", extra=("--resume-report", source))
        self.assertEqual(resumed["configuration"]["judge_fingerprint"], stamp)
        self.assertEqual(resumed["summary"]["reused_trials"], 1)
        self.assertEqual(resumed["summary"]["model_calls"], 0)
        self.assertEqual(resumed["trials"], first["trials"])
        self.assertEqual(source.read_bytes(), original)
        self.assertEqual(len(REQUESTS), before)
        # 即使报告与清单一致，已有的错误裁判标记也不能自动改写成当前标记。
        first["configuration"]["judge_fingerprint"] = "0" * 64
        write_json(source, first)
        manifest["files"]["report.json"] = hashlib.sha256(source.read_bytes()).hexdigest()
        write_json(manifest_path, manifest)
        self.evaluate(code=2, name="mismatch", extra=("--resume-report", source))
        self.assertEqual(len(REQUESTS), before)

    def consistency_mode(self):
        """仅给本地目标模式启用短输入核验；不影响未配置核验的兼容测试。"""
        self.objective_mode(turns=1)
        prompt = self.root / "prompts/task-consistency.txt"
        prompt.write_text((SOURCE / "config/prompts/task-consistency.txt").read_text())
        self.config["judge"]["consistency_prompt"] = "prompts/task-consistency.txt"
        self.config["judge"]["max_repair_attempts"] = 1

    def test_reason_consistency_repairs_without_reexecuting_or_changing_standards(self):
        """依据与结论冲突时原裁判重核；第二次仍失败，核验器不直接改分。"""
        self.consistency_mode()
        Provider.behavior = "goal_consistency_repair"
        report = self.evaluate(code=1)
        self.assertEqual(report["summary"]["model_calls"], 4)
        trial = report["trials"][0]
        self.assertEqual(trial["verdict"], "failed")
        self.assertEqual(trial["execution_status"], "completed")
        self.assertEqual(trial["checks"][-1]["attempts"], 4)
        self.assertEqual(trial["evidence"]["judge_batches"][0]["consistency_review"],
                         {"status": "complete", "reviewed_goals": 1})
        packets = [json.loads(r["messages"][1]["content"]) for r in REQUESTS[self.start_requests:]]
        self.assertEqual(packets[0]["assessment"], packets[2]["assessment"])
        self.assertEqual(packets[0]["evidence"], packets[2]["evidence"])
        self.assertEqual(packets[2]["validation_feedback"]["reason_conflicts"][0]["issue"],
                         "missing_requirement_waived")
        self.assertIn("判分逻辑矛盾，不是 JSON 格式问题",
                      packets[2]["validation_feedback"]["instruction"])
        self.assertIn("不能修改标准或编造已完成事实",
                      packets[2]["validation_feedback"]["instruction"])
        for packet in (packets[1], packets[3]):
            self.assertEqual(set(packet), {"kind", "inputs", "goals"})
            self.assertNotIn("trace", json.dumps(packet))

    def test_reason_consistency_exhaustion_conservatively_fails_waived_success(self):
        """持续承认缺项却判通过时仅单向降级；保留原理由，不丢成基础设施错误。"""
        self.consistency_mode()
        Provider.behavior = "goal_consistency_exhausted"
        report = self.evaluate(code=1, name="goal-consistency-exhausted")
        trial = report["trials"][0]
        self.assertEqual(report["summary"]["model_calls"], 4)
        self.assertEqual(trial["verdict"], "failed")
        self.assertEqual(trial["execution_status"], "completed")
        self.assertEqual(trial["evidence"]["assessment"]["coverage"], 1)
        self.assertEqual(trial["evidence"]["assessment"]["quality"], 0)
        goal = trial["evidence"]["assessment"]["goals"][0]
        self.assertEqual(goal["id"], "turn-1-1")
        self.assertEqual(goal["status"], "failed")
        self.assertEqual(goal["reason"], "本目标按可见证据独立验收。")
        self.assertTrue(goal["consistency_corrected"])
        turn = trial["evidence"]["turn_evaluation"][0]
        self.assertEqual(turn["verdict"], "failed")
        self.assertTrue(turn["consistency_corrected"])
        batch = trial["evidence"]["judge_batches"][0]
        self.assertNotIn("validation_error", batch)
        self.assertTrue(batch["consistency_review"]["consistency_corrected"])
        self.assertEqual(batch["consistency_review"]["corrected_conflicts"], [dict(
            id="turn-1-1", issue="missing_requirement_waived")])
        self.assertNotIn(KEY, json.dumps(trial))

    def test_reason_consistency_unknown_goal_stays_error(self):
        """未知目标 ID 不能触发保守降级，仍按无效复核响应处理。"""
        self.consistency_mode()
        Provider.behavior = "goal_consistency_unknown"
        report = self.evaluate(code=3, name="goal-consistency-unknown")
        trial = report["trials"][0]
        self.assertEqual(trial["verdict"], "error")
        self.assertEqual(trial["evidence"]["assessment"]["coverage"], 0)
        self.assertEqual(report["summary"]["model_calls"], 2)

    def test_failed_goal_cannot_be_accused_of_waived_success(self):
        """复现真实失败：主裁判已经失败，复核误报被放行；保留业务失败并记录无效指控。"""
        self.consistency_mode()
        Provider.behavior = "goal_consistency_failed_waiver"
        report = self.evaluate(code=1)
        trial = report["trials"][0]
        self.assertEqual(trial["verdict"], "failed")
        self.assertEqual(trial["evidence"]["assessment"]["quality"], 0)
        self.assertEqual(trial["evidence"]["assessment"]["coverage"], 1)
        self.assertEqual(report["summary"]["model_calls"], 2)
        review = trial["evidence"]["judge_batches"][0]["consistency_review"]
        self.assertEqual(review["ignored_inapplicable_conflicts"], [dict(
            id="turn-1-1", issue="missing_requirement_waived", reported_status="failed")])

    def test_failed_goal_still_rejects_unsupported_requirements(self):
        """已失败不等于免核验；标准外要求造成的失败仍须纠错，不能吞掉合理复核。"""
        self.consistency_mode()
        Provider.behavior = "goal_consistency_failed_unjustified"
        report = self.evaluate(code=3)
        self.assertEqual(report["summary"]["model_calls"], 4)
        diagnostic = report["trials"][0]["evidence"]["judge_batches"][0]["validation_error"]
        self.assertEqual(diagnostic["reason_conflicts"][0]["issue"], "unjustified_requirement")

    def test_inapplicable_conflict_does_not_hide_another_goals_real_conflict(self):
        """同批目标分别核验：忽略失败目标误报，并保守纠正另一目标的真实缺项。"""
        self.consistency_mode()
        self.objective_mode(turns=2)
        self.config["judge"]["max_batch_turns"] = 2
        Provider.behavior = "goal_consistency_mixed"
        report = self.evaluate(code=1)
        self.assertEqual(report["summary"]["model_calls"], 4)
        trial = report["trials"][0]
        self.assertEqual(trial["verdict"], "failed")
        review = trial["evidence"]["judge_batches"][0]["consistency_review"]
        self.assertEqual(review["ignored_inapplicable_conflicts"], [dict(
            id="turn-1-1", issue="missing_requirement_waived", reported_status="failed")])
        self.assertEqual(review["corrected_conflicts"], [dict(
            id="turn-2-1", issue="missing_requirement_waived")])
        goals = trial["evidence"]["assessment"]["goals"]
        self.assertEqual([goal["status"] for goal in goals], ["failed", "failed"])
        self.assertNotIn("consistency_corrected", goals[0])
        self.assertTrue(goals[1]["consistency_corrected"])

    def test_reason_consistency_preserves_rate_limit_error(self):
        """短核验遇到限流时仍走既有限流暂停链路，不吞成普通业务失败。"""
        self.consistency_mode()
        Provider.behavior = "goal_consistency_rate_limited"
        report = self.evaluate(code=3)
        self.assertEqual(report["trials"][0]["checks"][-1]["reason"], "evaluation_judge_http_429")
        self.assertEqual(report["summary"]["model_calls"], 2)
        self.assertEqual(report["trials"][0]["verdict"], "error")

    def test_reason_consistency_shares_budget_and_fingerprints_prompt(self):
        """不能超额调用；核验提示词更新改变身份，已有合规轨迹仍可模型重评。"""
        self.consistency_mode()
        Provider.behavior = "passed"
        self.config["judge"]["max_model_calls"] = 1
        report = self.evaluate(code=3, name="budget")
        self.assertEqual(report["summary"]["model_calls"], 1)
        self.assertEqual(report["trials"][0]["verdict"], "error")
        self.config["judge"]["max_model_calls"] = 10
        first = self.evaluate()
        self.assertEqual(first["summary"]["model_calls"], 2)
        prompt = self.root / "prompts/task-consistency.txt"
        prompt.write_text(prompt.read_text() + "\n仍然只检查内部矛盾。\n")
        self.mode("nonzero")
        second = self.evaluate(name="regraded", extra=(
            "--regrade-report", self.root / "evaluation/report.json"))
        self.assertNotEqual(first["configuration"]["judge_fingerprint"],
                            second["configuration"]["judge_fingerprint"])
        self.assertFalse(second["summary"]["agent_reexecuted"])
        self.assertEqual(second["summary"]["model_calls"], 2)

    def test_query_goal_accepts_honest_empty_result_without_waiving_real_failure(self):
        """查询与找到条目的验收要求分开，仍保留真实失败，不由程序强行改判。"""
        self.objective_mode(turns=1, criteria=[dict(
            id="query", expected_behavior="查询并展示提醒列表。")])
        Provider.behavior = "goal_failed"
        report = self.evaluate(code=1)
        prompt = REQUESTS[-1]["messages"][0]["content"]
        self.assertIn("可以完成该查询目标", prompt)
        self.assertIn("请求参数错误、查询未执行、数据源不可用和真实查询无匹配", prompt)
        self.assertIn("仅查询到空列表却未创建，仍然失败", prompt)
        self.assertIn("空结果仅可完成纯查询/展示目标", prompt)
        self.assertIn("如实说明未完成均不豁免下游目标", prompt)
        self.assertIn("条件分支只依题目明示前提，不补造条件", prompt)
        self.assertIn("整理资料、生成产物、写入保存等要求不能降格为查询", prompt)
        self.assertIn("完成度不等于行为合理性", prompt)
        self.assertIn("只有题目本身将“追问缺项/说明限制”作为该情形的验收目标时", prompt)
        self.assertEqual(report["trials"][0]["verdict"], "failed")

    # 回归用例：test_model_only_regrade_preserves_execution_and_source。
    # 关键断言：self.assertEqual(after["verdict"], "failed")；此处只验证测试构造的场景。
    def test_model_only_report_resumes_incomplete_judgment_without_execution(self):
        """复评后仍有未决目标时只补评分；确定成绩复用，原始执行始终不重跑。"""
        self.objective_mode()
        Provider.behavior = "goal_failed_unknown"
        self.evaluate(code=1)
        self.mode("nonzero")  # 若恢复路径误启动 Agent，试验应失败。
        first = self.evaluate(code=1, name="regraded",
                              extra=("--regrade-report", self.root / "evaluation/report.json"))
        source = self.root / "regraded/report.json"
        Provider.behavior = "goal_failed"
        second = self.evaluate(code=1, name="resolved", extra=("--resume-report", source))
        self.assertEqual(second["summary"]["regraded_trials"], 1)
        self.assertFalse(second["summary"]["agent_reexecuted"])
        self.assertEqual(second["trials"][0]["evidence"]["assessment"]["coverage"], 1)
        for key in ("trial_id", "metrics", "duration_ms", "execution_status"):
            self.assertEqual(first["trials"][0].get(key), second["trials"][0].get(key))
        self.assertEqual(first["trials"][0]["evidence"]["traces"],
                         second["trials"][0]["evidence"]["traces"])
        self.assertEqual(first["configuration"]["execution_source"],
                         second["configuration"]["execution_source"])
        third = self.evaluate(code=1, name="reused",
                              extra=("--resume-report", self.root / "resolved/report.json"))
        self.assertEqual(third["summary"]["model_calls"], 0)
        self.assertEqual(third["summary"]["reused_trials"], 1)

    def test_model_only_resume_rejects_changed_prompt_or_checks(self):
        """续评不是宽松导入：提示词或硬检查改变后，必须拒绝沿用旧成绩。"""
        self.objective_mode(turns=1)
        self.evaluate()
        self.evaluate(name="regraded", extra=("--regrade-report", self.root / "evaluation/report.json"))
        source = self.root / "regraded/report.json"
        count = len(REQUESTS)
        prompt = self.root / "prompts/task-goals.txt"
        original = prompt.read_text()
        prompt.write_text(original + "\n新增核验口径。\n")
        self.assertIsNone(self.evaluate(code=2, name="changed-prompt",
                                       extra=("--resume-report", source)))
        prompt.write_text(original)
        self.config["checks"] = {"*": [dict(id="new-check", path="/state/value",
                                           op="equals", value=999)]}
        self.assertIsNone(self.evaluate(code=2, name="changed-check",
                                       extra=("--resume-report", source)))
        self.assertEqual(len(REQUESTS), count)

    def test_model_only_regrade_preserves_execution_and_source(self):
        self.model()
        self.mode("trace")
        first = self.evaluate()
        source = self.root / "evaluation/report.json"
        original = source.read_bytes()
        prompt = self.root / "prompts/task-judge-system.txt"
        prompt.write_text(prompt.read_text() + "\n重新核对实际证据。\n")
        self.mode("nonzero")  # 若意外执行真实宿主，本测试应失败。
        Provider.behavior = "failed"
        second = self.evaluate(
            code=1, name="regraded", extra=("--regrade-report", source)
        )
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
        self.assertEqual(
            provenance["report_sha256"], hashlib.sha256(original).hexdigest()
        )
        self.assertEqual(provenance["config_fingerprint"], first["config_fingerprint"])
        self.assertEqual(source.read_bytes(), original)

    # 回归用例：test_model_only_regrade_explicit_subset_of_paused_batch。
    # 关键断言：self.assertTrue(entered.exists())；此处只验证测试构造的场景。
    def test_model_only_regrade_explicit_subset_of_paused_batch(self):
        self.config["parallelism"] = 1
        self.make_dataset(
            cases=[
                dict(
                    task_id=name,
                    group_id=name,
                    task_input=f"计算任务{name}的总量20。",
                    expected_behavior="总量为20。",
                    initial_state=dict(value=20),
                    difficulty="easy",
                    category="data",
                    split="train",
                    review_status="reviewed",
                )
                for name in ("a", "b", "c")
            ]
        )
        self.model()
        self.mode("trace")
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
        process = subprocess.Popen(
            [
                str(CLI),
                "dataset",
                "run",
                str(self.dataset),
                "--config",
                str(self.root / "evaluation.json"),
                "--output-root",
                str(self.root / "evaluation"),
            ],
            env=self.env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            deadline = time.monotonic() + 5
            while (
                not entered.exists()
                and process.poll() is None
                and time.monotonic() < deadline
            ):
                time.sleep(0.01)
            self.assertTrue(entered.exists())
            process.send_signal(signal.SIGINT)
            stdout, stderr = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 3, stdout + stderr)
        finally:
            if process.poll() is None:
                process.terminate()
                process.communicate(timeout=10)
        first = json.loads((self.root / "evaluation/report.json").read_text())
        self.assertFalse(first["complete"])
        self.assertEqual(first["trials"][0]["task_id"], "a")
        self.assertEqual(first["trials"][0]["execution_status"], "completed")
        self.assertEqual(first["trials"][1]["execution_status"], "cancelled")
        source = self.root / "evaluation/report.json"
        original = source.read_bytes()
        self.config["variants"][0]["command"] = host[:-1] + ["nonzero"]
        count = len(REQUESTS)
        self.assertIsNone(
            self.evaluate(code=2, name="unselected", extra=("--regrade-report", source))
        )
        self.config["task_ids"] = ["b"]
        self.assertIsNone(
            self.evaluate(code=2, name="unfinished", extra=("--regrade-report", source))
        )
        self.config["task_ids"] = ["c"]
        self.assertIsNone(
            self.evaluate(code=2, name="absent", extra=("--regrade-report", source))
        )
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

    # 回归用例：test_model_only_regrade_keeps_hard_checks_and_verifies_artifacts。
    # 关键断言：self.assertEqual(；此处只验证测试构造的场景。
    def test_model_only_regrade_keeps_hard_checks_and_verifies_artifacts(self):
        self.model()
        self.mode("artifact")
        self.config["checks"]["*"][0]["value"] = -1
        first = self.evaluate(code=1)
        source = self.root / "evaluation/report.json"
        self.config["checks"]["*"][0]["value"] = 20
        self.mode("nonzero")
        second = self.evaluate(
            code=1, name="regraded", extra=("--regrade-report", source)
        )
        before, after = first["trials"][0], second["trials"][0]
        self.assertEqual(
            [c for c in before["checks"] if c["check_id"] != "model"],
            [c for c in after["checks"] if c["check_id"] != "model"],
        )
        self.assertEqual(after["verdict"], "failed")
        name = before["evidence"]["artifacts"]["report.json"]["stored_file"]
        relative = Path("artifacts") / before["trial_id"] / name
        self.assertEqual(
            (self.root / "evaluation" / relative).read_bytes(),
            (self.root / "regraded" / relative).read_bytes(),
        )
        (self.root / "evaluation" / relative).write_text("changed artifact")
        count = len(REQUESTS)
        self.assertIsNone(
            self.evaluate(
                code=2, name="changed-artifact", extra=("--regrade-report", source)
            )
        )
        self.assertEqual(len(REQUESTS), count)

    # 回归用例：test_model_only_regrade_rejects_changed_context_and_source。
    # 关键断言：self.assertEqual(len(REQUESTS), count)；此处只验证测试构造的场景。
    def test_model_only_regrade_rejects_changed_context_and_source(self):
        self.model()
        self.evaluate()
        source = self.root / "evaluation/report.json"
        count = len(REQUESTS)
        self.config["variants"][0]["version"] = "different-agent-version"
        self.assertIsNone(
            self.evaluate(
                code=2, name="changed-version", extra=("--regrade-report", source)
            )
        )
        self.config["variants"][0]["version"] = "test-v1"
        self.config["judge"]["enabled"] = False
        self.assertIsNone(
            self.evaluate(
                code=2, name="disabled-judge", extra=("--regrade-report", source)
            )
        )
        self.config["judge"]["enabled"] = True
        source.write_bytes(source.read_bytes() + b" ")
        self.assertIsNone(
            self.evaluate(
                code=2, name="changed-report", extra=("--regrade-report", source)
            )
        )
        self.assertEqual(len(REQUESTS), count)

    # 回归用例：test_model_only_regrade_budget_does_not_reuse_old_passes。
    # 重点核验字段或诊断：max_model_calls、max_repair_attempts；异常路径不能被当成成功。
    def test_model_only_regrade_budget_does_not_reuse_old_passes(self):
        self.model()
        self.config.update(repetitions=3, parallelism=1)
        self.evaluate()
        self.mode("nonzero")
        self.config["judge"]["max_model_calls"] = 1
        self.config["judge"]["max_repair_attempts"] = 0
        second = self.evaluate(
            code=3,
            name="regraded",
            extra=("--regrade-report", self.root / "evaluation/report.json"),
        )
        self.assertEqual(second["summary"]["model_calls"], 1)
        self.assertEqual(second["summary"]["passed"], 1)
        self.assertEqual(second["summary"]["errors"], 2)
        self.assertEqual(second["summary"]["regraded_trials"], 3)

    # 回归用例：test_model_only_regrade_cancellation_does_not_mix_old_grades。
    # 重点核验字段或诊断：not_started_trials；异常路径不能被当成成功。
    def test_model_only_regrade_cancellation_does_not_mix_old_grades(self):
        self.model()
        self.config.update(repetitions=3, parallelism=1)
        self.evaluate()
        entered, release = threading.Event(), threading.Event()

        class PausedProvider(Provider):
            # 测试辅助函数：do_POST。
            # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
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
            runtime["model"]["endpoint"] = (
                f"http://127.0.0.1:{server.server_port}/chat/completions"
            )
            write_json(runtime_path, runtime)
            self.mode("nonzero")
            write_json(self.root / "evaluation.json", self.config)
            process = subprocess.Popen(
                [
                    str(CLI),
                    "dataset",
                    "run",
                    str(self.dataset),
                    "--config",
                    str(self.root / "evaluation.json"),
                    "--output-root",
                    str(self.root / "cancelled"),
                    "--regrade-report",
                    str(self.root / "evaluation/report.json"),
                ],
                env=self.env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
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
                process.terminate()
                process.communicate(timeout=10)
            server.shutdown()
            server.server_close()
            thread.join()

    # 回归用例：test_execution_view_omits_only_typed_model_requests_not_captured_evidence。
    # 重点核验字段或诊断：tool_call_id、max_output_bytes、include_model_requests；异常路径不能被当成成功。
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
        self.assertLess(
            len(REQUESTS[-1]["messages"][1]["content"].encode()), full_bytes / 2
        )
        original = report["trials"][0]["evidence"]["trace"]["events"]
        compact = packet["evidence"]["trace"]["events"]
        self.assertTrue(any(e.get("kind") == "action.end" for e in compact))
        self.assertFalse(any(e.get("kind") == "run.start" for e in compact))
        action_steps = {
            e.get("step_id")
            for e in compact
            if e.get("kind") in ("action.start", "action.end")
        }
        self.assertFalse(
            any(
                e.get("kind") in ("step.start", "step.end")
                and e.get("step_id") in action_steps
                for e in compact
            )
        )
        self.assertFalse(
            any(
                e.get("kind") == "action.start"
                and e.get("attributes", {}).get("semantic") == "model"
                for e in compact
            )
        )
        self.assertFalse(
            any("request-only-data" in json.dumps(e.get("input")) for e in compact)
        )
        self.assertTrue(
            any(
                "business-field-must-stay" in json.dumps(e.get("input"))
                for e in compact
            )
        )
        self.assertTrue(
            any(
                e.get("input", {}).get("content") == "cross-boundary-result"
                for e in compact
                if isinstance(e.get("input"), dict)
            )
        )
        self.assertTrue(any(e.get("event_id") for e in compact))
        self.assertTrue(any(e.get("caused_by") for e in compact))
        self.assertTrue(
            any("request-only-data" in json.dumps(e.get("input")) for e in original)
        )
        self.config["judge"]["include_model_requests"] = "false"
        count = len(REQUESTS)
        self.assertIsNone(self.evaluate(code=2, name="invalid-view"))
        self.assertEqual(len(REQUESTS), count)

    # 回归用例：test_execution_view_omits_retrieval_payload_but_keeps_decisions。
    # 重点核验字段或诊断：max_output_bytes、share_repeated_text、include_model_requests；异常路径不能被当成成功。
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
        raw = {
            e["event_id"]: e for e in report["trials"][0]["evidence"]["trace"]["events"]
        }
        compact = {e["event_id"]: e for e in packet["evidence"]["trace"]["events"]}
        self.assertNotIn("query-1", compact)
        self.assertEqual(compact["query-2"]["attributes"], raw["query-2"]["attributes"])
        self.assertIn("query", raw["query-1"]["attributes"]["decision"])
        self.assertTrue(
            any(
                e.get("attributes", {}).get("decision", {}).get("query")
                for e in full["evidence"]["trace"]["events"]
            )
        )
        self.assertLess(len(json.dumps(packet)), len(json.dumps(full)) - 40000)

    # 测试辅助函数：entry_mode。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def entry_mode(self, *flags):
        for key in ("run_layout", "instrumentation_transport"):
            self.config["variants"][0].pop(key, None)
        config = json.loads((SOURCE / "config/boundary-service.json").read_text())
        config.update(
            runtime_config=str(SOURCE / "config/standard-runtime.json"),
            experience_template_file=str(
                SOURCE / "config/prompts/runtime-experience.txt"
            ),
        )
        config["instrumentation"] = {
            "level": "L0",
            "capabilities": ["model", "actions", "loop"],
            "binding": {"pointer": "", "mode": "text"},
        }
        config["http_persistence"] = {"mode": "buffered"}
        write_json(self.root / "instrumentation.json", config)
        self.config["variants"][0].update(
            command=[
                sys.executable,
                "-B",
                str(SOURCE / "tests/entry_agent.py"),
                *map(str, flags),
            ],
            instrumentation_config=str(self.root / "instrumentation.json"),
            entry={
                "input": {
                    k: {"$eval": v}
                    for k, v in {
                        "text": "input",
                        "user": "user_id",
                        "conversation": "conversation_id",
                        "key": "run_key",
                        "turn": "turn",
                        "initial": "initial_state",
                        "history": "messages",
                    }.items()
                },
                "output_pointer": "/answer",
            },
        )
        self.config.update(
            timeout_ms=5000, require_trace=True, max_output_bytes=1048576
        )

    # 回归用例：test_entry_receivers_no_host_protocol。
    # 关键断言：self.assertEqual(trial["verdict"], "passed", trial)；此处只验证测试构造的场景。
    def test_entry_receivers_no_host_protocol(self):
        for label, flags in (
            ("sync", ()),
            ("async", ("--async",)),
            ("dispatch", ("--dispatch",)),
            ("persistent", ("--stay-alive",)),
            ("cabi", ("--cabi", BUILD / "libtrajectory_evolution_local.so")),
            ("native-points", ("--native", BUILD / "libtrajectory_evolution_local.so")),
            ("business", ("--business-failure",)),
        ):
            with self.subTest(label=label):
                self.entry_mode(*flags)
                report = self.evaluate(name="entry-" + label)
                trial = report["trials"][0]
                self.assertEqual(trial["verdict"], "passed", trial)
                self.assertEqual(len(trial["evidence"]["traces"]), 2)
                self.assertEqual(trial["metrics"]["llm_calls"], 2)
                identities = [
                    trace["identity"] for trace in trial["evidence"]["traces"]
                ]
                self.assertEqual(
                    identities[0]["conversation_id"], identities[1]["conversation_id"]
                )
                self.assertNotEqual(identities[0]["run_id"], identities[1]["run_id"])

    # 回归用例：test_training_import_requires_verified_l0_train_report。
    # 重点核验字段或诊断：import_source_id、import_content_fingerprint、import_evidence_fingerprint；异常路径不能被当成成功。
    def test_training_import_requires_verified_l0_train_report(self):
        self.entry_mode()
        report = self.evaluate()
        env = dict(self.env)
        service = json.loads((self.root / "instrumentation.json").read_text())
        service.update(
            data_dir=str(self.root / "learning"),
            auto_evaluate=False,
            sharing={"enabled": False},
        )
        for principal in service["principals"]:
            env[principal["token_env"]] = "training-import-test-principal-000001"
        write_json(self.root / "learning.json", service)
        args = [
            str(CLI),
            "learn",
            "--config",
            str(self.root / "learning.json"),
            "--enqueue-only",
        ]
        training = [
            "--training-evaluation",
            str(self.root / "evaluation"),
            "--dataset",
            str(self.dataset),
        ]
        first = subprocess.run(
            [*args, *training], env=env, capture_output=True, text=True, timeout=15
        )
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(json.loads(first.stdout)["imported"], 2)
        again = subprocess.run(
            [*args, *training], env=env, capture_output=True, text=True, timeout=15
        )
        self.assertEqual(again.returncode, 0, again.stderr)
        self.assertEqual(json.loads(again.stdout)["reused"], 2)
        runs = list((self.root / "learning/runs").glob("*.json"))
        self.assertEqual(len(runs), 2)
        trial = report["trials"][0]
        task = json.loads((self.dataset / "train.jsonl").read_text().splitlines()[0])
        original = {
            trace["identity"]["run_id"]: trace for trace in trial["evidence"]["traces"]
        }
        for path in runs:
            value = json.loads(path.read_text())
            self.assertEqual(value["training_source"]["kind"], "baseline_training")
            self.assertEqual(value["training_source"]["task_id"], "one")
            source_id = value["import_source_id"]
            trace_path = (
                self.root
                / "learning/by-user"
                / value["identity"]["user_id"]
                / "traces"
                / value["identity"]["conversation_id"]
                / (source_id + ".trace.json")
            )
            imported = json.loads(trace_path.read_text())
            context = imported["attributes"]["training_evaluation"]
            self.assertEqual(context["expected_behavior"], task["expected_behavior"])
            self.assertEqual(context["constraints"], task.get("constraints", []))
            self.assertEqual(context["verdict"], trial["verdict"])
            self.assertEqual(context["checks"], trial["checks"])
            self.assertEqual(context["run_ids"], list(original))
            self.assertEqual(imported["events"], original[source_id]["events"])
            self.assertNotIn(
                "training_evaluation", original[source_id].get("attributes", {})
            )

        # 复用旧导入，不静默重写或重复分析。
        legacy_path = runs[0]
        legacy = json.loads(legacy_path.read_text())
        legacy_trace_path = (
            self.root
            / "learning/by-user"
            / legacy["identity"]["user_id"]
            / "traces"
            / legacy["identity"]["conversation_id"]
            / (legacy["import_source_id"] + ".trace.json")
        )
        legacy_trace = json.loads(legacy_trace_path.read_text())
        del legacy_trace["attributes"]["training_evaluation"]
        write_json(legacy_trace_path, legacy_trace)
        legacy["import_content_fingerprint"] = hashlib.sha256(
            legacy_trace_path.read_bytes()
        ).hexdigest()
        legacy["import_evidence_fingerprint"] = legacy["training_source"].pop(
            "source_evidence_fingerprint"
        )
        write_json(legacy_path, legacy)
        reused = subprocess.run(
            [*args, *training], env=env, capture_output=True, text=True, timeout=15
        )
        self.assertEqual(reused.returncode, 0, reused.stderr)
        self.assertEqual(json.loads(reused.stdout)["reused"], 2)
        self.assertEqual(json.loads(legacy_trace_path.read_text()), legacy_trace)
        self.assertEqual(json.loads(legacy_path.read_text()), legacy)
        raw = self.root / "raw.trace.json"
        write_json(raw, report["trials"][0]["evidence"]["traces"][0])
        ordinary = subprocess.run(
            [*args, str(raw)], env=env, capture_output=True, text=True, timeout=15
        )
        self.assertEqual(ordinary.returncode, 3)
        self.assertIn("evaluation_trace_not_learning_source", ordinary.stderr)

        # 重新签署报告，以便独立于哈希校验测试语义约束。
        for change, expected in (
            ("split", "training_task_not_admissible"),
            ("task_version", "training_task_not_admissible"),
            ("policy", "training_requires_unmodified_baseline"),
            ("trace_version", "training_trace_version_mismatch"),
            ("context_credential", "credential_value_rejected"),
            ("hash", "training_report_invalid"),
        ):
            modified = json.loads(json.dumps(report))
            trial = modified["trials"][0]
            if change == "split":
                trial["split"] = "holdout"
            elif change == "task_version":
                trial["task_version"] = "another-version"
            elif change == "policy":
                trial["evidence"]["loaded_version"] = "candidate-policy"
            elif change == "trace_version":
                trial["evidence"]["traces"][1]["events"][0]["attributes"][
                    "evaluation_trial_id"
                ] = "different-trial"
            elif change == "context_credential":
                trial["checks"][0]["reason"] = KEY
            else:
                modified["run_id"] = "modified-report"
            path = self.root / "evaluation/report.json"
            write_json(path, modified)
            manifest_path = self.root / "evaluation/manifest.json"
            manifest = json.loads(manifest_path.read_text())
            if change != "hash":
                manifest["files"]["report.json"] = hashlib.sha256(
                    path.read_bytes()
                ).hexdigest()
                write_json(manifest_path, manifest)
            rejected = subprocess.run(
                [*args, *training], env=env, capture_output=True, text=True, timeout=15
            )
            self.assertEqual(
                rejected.returncode, 2, (change, rejected.stdout, rejected.stderr)
            )
            self.assertIn(expected, rejected.stderr)
            self.assertNotIn(KEY, rejected.stdout + rejected.stderr)
            self.assertEqual(len(list((self.root / "learning/runs").glob("*.json"))), 2)

    # 回归用例：test_training_import_accepts_errored_complete_traces。
    # 裁判错误只要有完整合法轨迹仍可进入私有训练轨迹；空错误记录必须拒绝。
    def test_training_import_accepts_errored_complete_traces(self):
        self.entry_mode()
        report = self.evaluate()
        trial = report["trials"][0]
        trial["verdict"] = "error"
        trial["execution_status"] = "completed"
        trial["error_code"] = "evaluation_judge_failed"

        report_path = self.root / "evaluation/report.json"
        manifest_path = self.root / "evaluation/manifest.json"

        def resign(value):
            write_json(report_path, value)
            manifest = json.loads(manifest_path.read_text())
            manifest["files"]["report.json"] = hashlib.sha256(
                report_path.read_bytes()
            ).hexdigest()
            write_json(manifest_path, manifest)

        resign(report)
        service = json.loads((self.root / "instrumentation.json").read_text())
        service.update(
            data_dir=str(self.root / "learning"),
            auto_evaluate=False,
            sharing={"enabled": False},
        )
        env = dict(self.env)
        for principal in service["principals"]:
            env[principal["token_env"]] = "training-import-test-principal-000001"
        config_path = self.root / "learning.json"
        write_json(config_path, service)
        command = [
            str(CLI),
            "learn",
            "--config",
            str(config_path),
            "--enqueue-only",
            "--training-evaluation",
            str(self.root / "evaluation"),
            "--dataset",
            str(self.dataset),
        ]
        imported = subprocess.run(
            command, env=env, capture_output=True, text=True, timeout=15
        )
        self.assertEqual(imported.returncode, 0, imported.stderr)
        self.assertEqual(json.loads(imported.stdout)["imported"], 2)
        private = list(
            (self.root / "learning/by-user").glob("*/traces/*/*.trace.json")
        )
        self.assertEqual(len(private), 2)
        self.assertTrue(all(self.root / "learning/by-user" in path.parents for path in private))
        for path in private:
            context = json.loads(path.read_text())["attributes"]["training_evaluation"]
            self.assertEqual(context["task_assessment_status"], "error")
            self.assertEqual(context["verdict"], "error")
            self.assertEqual(context["error_code"], "evaluation_judge_failed")

        missing_trace = json.loads(json.dumps(report))
        missing_trace["trials"][0]["evidence"].pop("trace", None)
        missing_trace["trials"][0]["evidence"].pop("traces", None)
        resign(missing_trace)
        rejected = subprocess.run(
            command, env=env, capture_output=True, text=True, timeout=15
        )
        self.assertEqual(rejected.returncode, 2, rejected.stderr)
        self.assertIn("training_no_complete_traces", rejected.stderr)
        self.assertEqual(
            len(list((self.root / "learning/runs").glob("*.json"))), 2
        )

        # 错误试验的第二轮轨迹损坏时，只拒绝该试验；第一轮暂存的 run_id
        # 不能污染后面正常试验的导入。
        mixed = json.loads(json.dumps(report))
        bad = mixed["trials"][0]
        good = json.loads(json.dumps(bad))
        good["verdict"] = "passed"
        good["error_code"] = ""
        bad["trial_id"] += "-bad"
        good["trial_id"] += "-good"
        for event in bad["evidence"]["traces"][0]["events"]:
            if event["kind"] == "run.start":
                event["attributes"]["evaluation_trial_id"] = bad["trial_id"]
        for trace in good["evidence"]["traces"]:
            for event in trace["events"]:
                if event["kind"] == "run.start":
                    event["attributes"]["evaluation_trial_id"] = good["trial_id"]
        bad["evidence"]["traces"][1] = {}
        mixed["trials"].append(good)
        resign(mixed)
        service["data_dir"] = str(self.root / "learning-mixed")
        write_json(config_path, service)
        isolated = subprocess.run(
            command, env=env, capture_output=True, text=True, timeout=15
        )
        self.assertEqual(isolated.returncode, 3, isolated.stderr)
        counts = json.loads(isolated.stdout)
        self.assertEqual((counts["imported"], counts["rejected"]), (2, 1))
        self.assertEqual(
            len(list((self.root / "learning-mixed/runs").glob("*.json"))), 2
        )

    # 回归用例：test_partial_timeout_does_not_block_other_training_traces。
    # 关键断言：self.assertEqual(timeout["execution_status"], "timeout")；此处只验证测试构造的场景。
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
        service.update(
            data_dir=str(self.root / "learning"),
            auto_evaluate=False,
            sharing={"enabled": False},
        )
        env = dict(self.env)
        for principal in service["principals"]:
            env[principal["token_env"]] = "training-import-test-principal-000001"
        write_json(self.root / "learning.json", service)
        result = subprocess.run(
            [
                str(CLI),
                "learn",
                "--config",
                str(self.root / "learning.json"),
                "--enqueue-only",
                "--training-evaluation",
                str(self.root / "evaluation"),
                "--dataset",
                str(self.dataset),
            ],
            env=env,
            capture_output=True,
            text=True,
            timeout=15,
        )
        self.assertEqual(result.returncode, 3, result.stderr)
        counts = json.loads(result.stdout)
        self.assertEqual(counts["imported"], 2)
        self.assertEqual(counts["rejected"], 1)
        self.assertEqual(len(list((self.root / "learning/runs").glob("*.json"))), 2)

    # 回归用例：test_entry_failure_and_missing_end_are_not_success。
    # 重点核验字段或诊断：evaluation_entry_handler_failed、runner_nonzero_exit；异常路径不能被当成成功。
    def test_entry_failure_and_missing_end_are_not_success(self):
        for label, flags, expected in (
            ("handler", ("--error",), "evaluation_entry_handler_failed"),
            ("no-run", ("--no-run",), "runner_timeout"),
            ("missing-end", ("--missing-end",), "runner_timeout"),
        ):
            with self.subTest(label=label):
                self.entry_mode(*flags)
                self.config["timeout_ms"] = 1500
                report = self.evaluate(
                    code=3 if label == "handler" else 1, name="entry-" + label
                )
                trial = report["trials"][0]
                self.assertIn(trial["error_code"], (expected, "runner_nonzero_exit"))
                self.assertNotEqual(trial["verdict"], "passed")

    # 回归用例：test_entry_configuration_rejected_before_execution。
    # 重点核验字段或诊断：evaluation_entry_initial_state_unbound；异常路径不能被当成成功。
    def test_entry_configuration_rejected_before_execution(self):
        for label, mutation in (
            ("variable", {"entry": {"input": {"$eval": "rubric"}}}),
            ("layout", {"run_layout": "single"}),
            ("transport", {"instrumentation_transport": "native"}),
            (
                "pointer",
                {"entry": {"input": {"$eval": "input"}, "output_pointer": "bad"}},
            ),
        ):
            with self.subTest(label=label):
                self.entry_mode()
                self.config["variants"][0].update(mutation)
                self.evaluate(code=2, name="entry-invalid-" + label)
        self.entry_mode()
        self.config["variants"][0]["entry"]["input"] = {"$eval": "input"}
        report = self.evaluate(code=3, name="entry-unbound-state")
        self.assertEqual(
            report["trials"][0]["error_code"], "evaluation_entry_initial_state_unbound"
        )

    # 回归用例：test_entry_capture_failure_does_not_wait_for_trial_timeout。
    # 重点核验字段或诊断：evaluation_entry_capture_sensitive_field_rejected、evaluation_entry_capture_task_not_found、evaluation_entry_trace_incomplete；异常路径不能被当成成功。
    def test_entry_capture_failure_does_not_wait_for_trial_timeout(self):
        for label, expected in (
            ("rejected-start", "evaluation_entry_capture_sensitive_field_rejected"),
            ("missing-start-gap", "evaluation_entry_capture_task_not_found"),
            ("open-gap", "evaluation_entry_trace_incomplete"),
            ("rejected-child", "evaluation_entry_capture_sensitive_field_rejected"),
        ):
            with self.subTest(label=label):
                self.entry_mode("--" + label)
                report = self.evaluate(code=3, name="capture-" + label)
                trial = report["trials"][0]
                self.assertEqual(trial["error_code"], expected, trial)
                self.assertEqual(trial["execution_status"], "error")
                self.assertLess(trial["duration_ms"], 3000)
                capture = trial["evidence"]["failure_capture"]
                self.assertFalse(capture["complete"])
                self.assertEqual(capture["expected_runs"], 2)
                self.assertTrue(any(run["complete"] for run in capture["runs"]))
                self.assertEqual(report["summary"]["model_calls"], 0)

    # 回归用例：test_entry_late_previous_failure_does_not_poison_next_turn。
    # 关键断言：self.assertEqual(trial["verdict"], "passed", trial)；此处只验证测试构造的场景。
    def test_entry_late_previous_failure_does_not_poison_next_turn(self):
        self.entry_mode("--late-previous-failure")
        trial = self.evaluate(name="late-previous-failure")["trials"][0]
        self.assertEqual(trial["verdict"], "passed", trial)
        self.assertEqual(len(trial["evidence"]["traces"]), 2)

    @unittest.skipUnless(shutil.which("node"), "Node is not installed")
    # 回归用例：test_javascript_entry_uses_same_protocol。
    # 关键断言：self.assertEqual(len(trial["evidence"]["traces"]), 2)；此处只验证测试构造的场景。
    def test_javascript_entry_uses_same_protocol(self):
        self.entry_mode()
        self.config["variants"][0]["command"] = [
            shutil.which("node"),
            str(SOURCE / "tests/clients/entry.mjs"),
        ]
        trial = self.evaluate(name="javascript-entry")["trials"][0]
        self.assertEqual(len(trial["evidence"]["traces"]), 2)

    # 回归用例：test_entry_is_inert_in_normal_application。
    # 关键断言：code = "from evolution_entry import evaluate, aevaluate; import asyncio; assert evaluate(None) is False; assert asyncio.run(aevaluate(None)) is False"；此处只验证测试构造的场景。
    def test_entry_is_inert_in_normal_application(self):
        env = {
            k: v
            for k, v in self.env.items()
            if not k.startswith("TRAJECTORY_EVOLUTION_")
        }
        env["PYTHONPATH"] = str(SOURCE / "clients/python")
        code = "from evolution_entry import evaluate, aevaluate; import asyncio; assert evaluate(None) is False; assert asyncio.run(aevaluate(None)) is False"
        result = subprocess.run(
            [sys.executable, "-c", code], env=env, capture_output=True, timeout=5
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    # 回归用例：test_entry_missing_output_and_extra_runs_are_errors。
    # 重点核验字段或诊断：evaluation_entry_output_missing、runner_nonzero_exit、evaluation_entry_handler_failed；异常路径不能被当成成功。
    def test_entry_missing_output_and_extra_runs_are_errors(self):
        self.entry_mode()
        self.config["variants"][0]["entry"]["output_pointer"] = "/missing"
        trial = self.evaluate(code=3, name="missing-answer")["trials"][0]
        self.assertIn(
            trial["error_code"],
            (
                "evaluation_entry_output_missing",
                "runner_nonzero_exit",
                "evaluation_entry_handler_failed",
            ),
        )
        self.entry_mode("--extra-run")
        trial = self.evaluate(code=3, name="unexpected-run")["trials"][0]
        self.assertIn(
            trial["error_code"],
            (
                "evaluation_entry_unexpected_run",
                "runner_nonzero_exit",
                "evaluation_entry_handler_failed",
            ),
        )

    # 回归用例：test_published_policy_file_is_bound_to_fresh_evaluation。
    # 重点核验字段或诊断：policy_version_file；异常路径不能被当成成功。
    def test_published_policy_file_is_bound_to_fresh_evaluation(self):
        self.entry_mode()
        variant = self.config["variants"][0]
        variant.pop("version")
        variant["policy_version_file"] = "published-version.json"
        owner = json.loads((self.root / "instrumentation.json").read_text())[
            "principals"
        ][0]["user_id"]
        artifact = dict(
            schema_version=1.0,
            agent=dict(name="test", revision="fresh-host-1"),
            user_id=owner,
            level="L0",
            policy=None,
        )

        # 测试辅助函数：publish。
        # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
        def publish():
            version = (
                "agent-version-v1:"
                + hashlib.sha256(
                    json.dumps(artifact, sort_keys=True, separators=(",", ":")).encode()
                ).hexdigest()
            )
            write_json(
                self.root / "published-version.json",
                dict(
                    schema_version=1,
                    version=version,
                    artifact=artifact,
                    evaluation=dict(gate=dict(passed=True)),
                ),
            )
            return version

        first_version = publish()
        first = self.evaluate(name="published-first")
        self.assertEqual(
            first["configuration"]["variants"][0]["version"], first_version
        )
        self.assertEqual(
            first["trials"][0]["evidence"]["loaded_version"], first_version
        )
        artifact["agent"]["revision"] = "fresh-host-2"
        second_version = publish()
        second = self.evaluate(name="published-second")
        self.assertNotEqual(first_version, second_version)
        self.assertNotEqual(first["config_fingerprint"], second["config_fingerprint"])
        self.assertNotEqual(
            first["trials"][0]["trial_id"], second["trials"][0]["trial_id"]
        )

    # 回归用例：test_published_policy_file_rejects_invalid_or_ambiguous_source。
    # 重点核验字段或诊断：policy_version_file；异常路径不能被当成成功。
    def test_published_policy_file_rejects_invalid_or_ambiguous_source(self):
        self.entry_mode()
        variant = self.config["variants"][0]
        variant.pop("version")
        variant["policy_version_file"] = "published-version.json"
        for label, bundle in (
            (
                "rejected",
                dict(schema_version=1, evaluation=dict(gate=dict(passed=False))),
            ),
            (
                "changed",
                dict(
                    schema_version=1,
                    version="changed",
                    artifact=dict(
                        schema_version=1,
                        agent=dict(revision="v1"),
                        user_id="user-a",
                        level="L0",
                        policy=None,
                    ),
                    evaluation=dict(gate=dict(passed=True)),
                ),
            ),
        ):
            write_json(self.root / "published-version.json", bundle)
            self.assertIsNone(self.evaluate(code=2, name=label))
            self.assertFalse((self.root / label).exists())
        variant["artifact"] = None
        self.assertIsNone(self.evaluate(code=2, name="ambiguous"))

    # 回归用例：test_component_owns_instrumented_evaluation。
    # 重点核验字段或诊断：experience_template_file、agent_version_id；异常路径不能被当成成功。
    def test_component_owns_instrumented_evaluation(self):
        config = json.loads((SOURCE / "config/boundary-service.json").read_text())
        config["runtime_config"] = str(SOURCE / "config/standard-runtime.json")
        config["experience_template_file"] = str(
            SOURCE / "config/prompts/runtime-experience.txt"
        )
        config["instrumentation"] = {
            "level": "L0",
            "capabilities": ["model", "actions", "loop"],
            "binding": {"pointer": "", "mode": "text"},
        }
        write_json(self.root / "instrumentation.json", config)
        self.config["variants"][0].update(
            command=[
                sys.executable,
                "-B",
                str(SOURCE / "tests/instrumented_agent.py"),
                str(BUILD / "libtrajectory_evolution_local.so"),
                "--file",
            ],
            instrumentation_config=str(self.root / "instrumentation.json"),
        )
        self.config["artifacts"] = {
            "one": [dict(path="report.json", media_type="application/json")]
        }
        self.config["checks"]["*"].append(
            dict(
                id="actual-file",
                path="/artifacts/report.json/json/total",
                op="equals",
                value=20,
            )
        )
        self.config.update(
            timeout_ms=5000, require_trace=True, max_output_bytes=1048576
        )
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
                self.assertTrue(
                    trial["evidence"]["trace"]["attributes"]["capture_complete"]
                )
                self.assertIn(
                    "agent_version_id",
                    trial["evidence"]["trace"]["events"][0]["attributes"],
                )
                self.assertEqual(
                    trial["evidence"]["artifacts"]["report.json"]["json"]["total"], 20
                )

    # 回归用例：test_capture_budget_is_independent_of_stdout。
    # 长对话在 HTTP 和原生采集端均采用评测预算；不覆盖原部署配置。
    # 宿主仍只打点，不需要额外配置适配器，也不丢弃长输出中的原始内容。
    def test_isolated_capture_uses_evaluation_budget_without_mutating_deployment(self):
        task = json.loads((self.dataset / "train.jsonl").read_text().splitlines()[0])
        task["followup_inputs"] = []
        self.make_dataset([task])
        for transport in ("http", "native"):
            with self.subTest(transport=transport):
                flags = ["--huge-answer"] if transport == "http" else []
                if transport == "native":
                    flags += ["--native", str(BUILD / "libtrajectory_evolution_local.so")]
                self.entry_mode(*flags)
                source = self.root / "instrumentation.json"
                config = json.loads(source.read_text())
                source_limit = 131072 if transport == "http" else 1024
                config.update(max_request_bytes=source_limit, max_run_bytes=source_limit)
                config["learning"] = {"max_batch_bytes": source_limit}
                config["http_persistence"]["max_buffer_bytes"] = 1048576
                write_json(source, config)
                original = source.read_bytes()
                self.config["max_trace_bytes"] = 4194304
                variant = self.config["variants"][0]
                # 在真实子进程中核对生成的隔离配置，不能只检查父进程内对象。
                wrapper = (
                    "import json, os, runpy, sys\n"
                    "c=json.load(open(os.environ['TRAJECTORY_EVOLUTION_TRIAL_CONFIG']))\n"
                    "assert c['max_run_bytes'] == 4194304\n"
                    "assert c['learning']['max_batch_bytes'] == 4194304\n"
                    "assert c['http_persistence']['max_buffer_bytes'] == 4194304\n"
                    "runpy.run_path(sys.argv[1], run_name='__main__')\n"
                )
                variant["command"] = [
                    sys.executable, "-B", "-c", wrapper, *variant["command"][2:]
                ]
                trial = self.evaluate(name="budget-" + transport)["trials"][0]
                self.assertEqual(trial["verdict"], "passed", trial.get("error_code"))
                traces = trial["evidence"].get("traces",
                                               [trial["evidence"].get("trace")])
                self.assertEqual(len(traces), 1)
                if transport == "http":
                    self.assertIn(
                        "中段验收证据完整", json.dumps(traces, ensure_ascii=False)
                    )
                self.assertGreater(len(json.dumps(traces).encode()),
                                   90000 if transport == "http" else 1024)
                self.assertEqual(source.read_bytes(), original)

    # 重点核验字段或诊断：experience_template_file、max_trace_bytes、evaluation_trace_bytes_limit；异常路径不能被当成成功。
    def test_capture_budget_is_independent_of_stdout(self):
        config = json.loads((SOURCE / "config/boundary-service.json").read_text())
        config["runtime_config"] = str(SOURCE / "config/standard-runtime.json")
        config["experience_template_file"] = str(
            SOURCE / "config/prompts/runtime-experience.txt"
        )
        config["instrumentation"] = {
            "level": "L0",
            "capabilities": ["model", "actions", "loop"],
            "binding": {"pointer": "", "mode": "text"},
        }
        write_json(self.root / "instrumentation.json", config)
        self.config["variants"][0].update(
            command=[
                sys.executable,
                "-B",
                str(SOURCE / "tests/instrumented_agent.py"),
                str(BUILD / "libtrajectory_evolution_local.so"),
                "--file",
            ],
            instrumentation_config=str(self.root / "instrumentation.json"),
        )
        self.config.update(timeout_ms=5000, require_trace=True, max_output_bytes=2048)
        trial = self.evaluate(name="small-stdout")["trials"][0]
        self.assertEqual(trial["verdict"], "passed")
        self.assertGreater(len(json.dumps(trial["evidence"]["trace"]).encode()), 2048)
        self.config["max_trace_bytes"] = 128
        limited = self.evaluate(code=3, name="small-trace")["trials"][0]
        self.assertEqual(limited["error_code"], "evaluation_trace_bytes_limit")
        self.assertEqual(limited["verdict"], "error")

    # 回归用例：test_configured_artifacts_are_validated_before_execution。
    # 重点核验字段或诊断：evaluation_file_not_regular；异常路径不能被当成成功。
    def test_configured_artifacts_are_validated_before_execution(self):
        for name, files in (
            ("unknown-task", {"missing-task": []}),
            ("unsafe", {"one": [{"path": "../outside.txt"}]}),
            ("duplicate", {"one": [{"path": "a.txt"}, {"path": "a.txt"}]}),
            (
                "unsupported",
                {"one": [{"path": "a.bin", "media_type": "application/octet-stream"}]},
            ),
        ):
            with self.subTest(name=name):
                self.config["artifacts"] = files
                self.evaluate(code=2, name=name)
                self.assertFalse((self.root / name).exists())
        self.config["artifacts"] = {"one": [{"path": "missing.txt"}]}
        report = self.evaluate(code=1, name="missing-file")
        self.assertEqual(
            report["trials"][0]["error_code"], "evaluation_file_not_regular"
        )

    # 回归用例：test_business_failure_is_not_batch_crash。
    # 关键断言：self.assertEqual(report["trials"][0]["execution_status"], "completed")；此处只验证测试构造的场景。
    def test_business_failure_is_not_batch_crash(self):
        self.mode("wrong")
        report = self.evaluate(code=1)
        self.assertEqual(report["trials"][0]["execution_status"], "completed")
        self.assertEqual(report["trials"][0]["verdict"], "failed")

    # 回归用例：test_multiple_real_runs_in_one_trial。
    # 重点核验字段或诊断：experience_template_file、evaluation_loaded_version_mismatch、evaluation_file_not_regular；异常路径不能被当成成功。
    def test_multiple_real_runs_in_one_trial(self):
        config = json.loads((SOURCE / "config/boundary-service.json").read_text())
        config["runtime_config"] = str(SOURCE / "config/standard-runtime.json")
        config["experience_template_file"] = str(
            SOURCE / "config/prompts/runtime-experience.txt"
        )
        config["instrumentation"] = {
            "level": "L0",
            "capabilities": ["model", "actions", "loop"],
            "binding": {"pointer": "", "mode": "text"},
        }
        write_json(self.root / "instrumentation.json", config)
        command = [
            sys.executable,
            "-B",
            str(SOURCE / "tests/instrumented_agent.py"),
            str(BUILD / "libtrajectory_evolution_local.so"),
            "--separate-runs",
        ]
        self.config["variants"][0].update(
            instrumentation_config=str(self.root / "instrumentation.json"),
            run_layout="per_input",
        )
        self.config.update(
            timeout_ms=5000, require_trace=True, max_output_bytes=2097152
        )
        for mode in (
            "native",
            "http",
            "async",
            "missing-metrics",
            "exit-after-run",
            "exit-open-second",
            "exit-between-runs",
            "wrong-owner",
        ):
            with self.subTest(mode=mode):
                flags = (
                    []
                    if mode == "native"
                    else (
                        ["--http", "--dispatch", "--between-run-delay"]
                        if mode == "async"
                        else ["--" + mode]
                    )
                )
                self.config["variants"][0].update(
                    command=command + flags,
                    instrumentation_transport="http"
                    if mode in ("http", "async")
                    else "native",
                )
                error = mode in (
                    "exit-after-run",
                    "exit-open-second",
                    "exit-between-runs",
                    "wrong-owner",
                )
                trial = self.evaluate(name="multi-" + mode, code=3 if error else 0)[
                    "trials"
                ][0]
                if mode == "wrong-owner":
                    self.assertEqual(
                        trial["error_code"], "evaluation_loaded_version_mismatch"
                    )
                    self.assertNotIn("evidence", trial)
                elif mode in ("exit-open-second", "exit-between-runs"):
                    capture = trial["evidence"]["failure_capture"]
                    self.assertFalse(capture["complete"])
                    self.assertEqual(
                        [r["complete"] for r in capture["runs"]],
                        [True, False] if mode == "exit-open-second" else [True],
                    )
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
                    self.assertEqual(
                        [t["events"][-1]["output"]["state"]["turn"] for t in traces],
                        [0, 1],
                    )
                    self.assertEqual(
                        trial["evidence"]["trajectory"]["summary"]["run_count"], 2
                    )
        self.config["variants"][0].update(
            command=command, instrumentation_transport="native"
        )
        self.config["artifacts"] = {"one": [{"path": "missing.txt"}]}
        trial = self.evaluate(name="multi-missing-artifact", code=1)["trials"][0]
        self.assertEqual(trial["error_code"], "evaluation_file_not_regular")
        self.assertEqual(len(trial["evidence"]["traces"]), 2)
        self.assertEqual(trial["evidence"]["metrics"]["llm_calls"], 2)

    # 回归用例：test_failed_instrumented_runner_preserves_safe_capture。
    # 重点核验字段或诊断：experience_template_file、runner_nonzero_exit、failure_capture_incomplete；异常路径不能被当成成功。
    def test_failed_instrumented_runner_preserves_safe_capture(self):
        config = json.loads((SOURCE / "config/boundary-service.json").read_text())
        config["runtime_config"] = str(SOURCE / "config/standard-runtime.json")
        config["experience_template_file"] = str(
            SOURCE / "config/prompts/runtime-experience.txt"
        )
        config["instrumentation"] = {
            "level": "L0",
            "capabilities": ["model", "actions", "loop"],
            "binding": {"pointer": "", "mode": "text"},
        }
        write_json(self.root / "instrumentation.json", config)
        command = [
            sys.executable,
            "-B",
            str(SOURCE / "tests/instrumented_agent.py"),
            str(BUILD / "libtrajectory_evolution_local.so"),
        ]
        self.config["variants"][0].update(
            instrumentation_config=str(self.root / "instrumentation.json")
        )
        self.config.update(
            timeout_ms=5000, require_trace=True, max_output_bytes=1048576
        )
        for transport in ("native", "http"):
            for mode in (
                "--exit-after-run",
                "--exit-open-run",
                "--exit-before-run",
                "--corrupt-capture",
            ):
                if transport == "http" and mode == "--corrupt-capture":
                    continue  # 采集文件由 HTTP 服务持有，而不是子进程。
                with self.subTest(transport=transport, mode=mode):
                    flags = (
                        ["--exit-after-run", mode]
                        if mode == "--corrupt-capture"
                        else [mode]
                    )
                    self.config["variants"][0]["command"] = (
                        command + flags + (["--http"] if transport == "http" else [])
                    )
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
                        self.assertFalse(
                            trial["evidence"]["failure_capture"]["complete"]
                        )
                        self.assertNotIn("metrics", trial)
                        self.assertIn("failure_capture_incomplete", trial["warnings"])
                    else:
                        self.assertNotIn("evidence", trial)
                        self.assertTrue(
                            any(
                                w.startswith("failure_evidence_unavailable")
                                for w in trial["warnings"]
                            )
                        )

    # 回归用例：test_variant_pairing_and_failure_isolation。
    # 重点核验字段或诊断：pass_rate_delta；异常路径不能被当成成功。
    def test_variant_pairing_and_failure_isolation(self):
        bad = dict(
            self.config["variants"][0],
            id="bad",
            command=self.config["variants"][0]["command"][:-1] + ["wrong"],
        )
        self.config["variants"].append(bad)
        self.config["repetitions"] = 3
        report = self.evaluate(code=1)
        self.assertEqual(report["summary"]["trials"], 6)
        self.assertEqual(report["comparisons"]["bad"]["losses"], 3)
        self.assertEqual(report["comparisons"]["bad"]["pass_rate_delta"], -1)

    # 回归用例：test_timeout。
    # 关键断言：self.assertEqual(report["trials"][0]["error_code"], "runner_timeout")；此处只验证测试构造的场景。
    def test_timeout(self):
        self.mode("sleep")
        self.config["timeout_ms"] = 100
        report = self.evaluate(code=1)
        self.assertEqual(report["trials"][0]["error_code"], "runner_timeout")

    # 回归用例：test_environment_error_unscored。
    # 关键断言：self.assertEqual(report["summary"]["coverage"], 0)；此处只验证测试构造的场景。
    def test_environment_error_unscored(self):
        self.mode("environment")
        report = self.evaluate(code=3)
        self.assertIsNone(report["summary"]["pass_rate"])
        self.assertEqual(report["summary"]["coverage"], 0)

    # 回归用例：test_protocol_errors。
    # 关键断言：self.assertEqual(self.evaluate(code=3, name=mode)["trials"][0]["verdict"], "error")；此处只验证测试构造的场景。
    def test_protocol_errors(self):
        for mode in ("wrong_id", "bad_json", "nonzero", "oversize"):
            with self.subTest(mode=mode):
                self.mode(mode)
                self.assertEqual(
                    self.evaluate(code=3, name=mode)["trials"][0]["verdict"], "error"
                )

    # 回归用例：test_credential_rejected_before_persistence。
    # 重点核验字段或诊断：evaluation_sensitive_output_rejected；异常路径不能被当成成功。
    def test_credential_rejected_before_persistence(self):
        self.mode("leak")
        report = self.evaluate(code=1)
        self.assertEqual(
            report["trials"][0]["error_code"], "evaluation_sensitive_output_rejected"
        )
        self.assertNotIn("evidence", report["trials"][0])

    # 回归用例：test_environment_allowlist。
    # 关键断言：self.assertFalse(report["trials"][0]["evidence"]["state"]["unexpected_env"])；此处只验证测试构造的场景。
    def test_environment_allowlist(self):
        self.mode("env")
        report = self.evaluate()
        self.assertFalse(report["trials"][0]["evidence"]["state"]["unexpected_env"])

    # 回归用例：test_budget_and_unknown_metrics。
    # 重点核验字段或诊断：step_or_tool_budget_unobserved、host_reported_metrics；异常路径不能被当成成功。
    def test_budget_and_unknown_metrics(self):
        self.mode("budget")
        self.evaluate(code=1, name="budget")
        self.mode("no_metrics")
        report = self.evaluate(name="unknown")
        self.assertIn("step_or_tool_budget_unobserved", report["trials"][0]["warnings"])
        self.assertEqual(report["summary"]["host_reported_metrics"], {})

    # 回归用例：test_artifact_is_read_from_disk。
    # 关键断言：self.assertEqual(；此处只验证测试构造的场景。
    def test_artifact_is_read_from_disk(self):
        self.mode("artifact")
        self.config["checks"]["*"].append(
            dict(
                id="file",
                path="/artifacts/report.json/json/total",
                op="equals",
                value=20,
            )
        )
        report = self.evaluate()
        self.assertEqual(
            report["trials"][0]["evidence"]["artifacts"]["report.json"]["json"][
                "total"
            ],
            20,
        )

    # 回归用例：test_judge_receives_artifact_facts_and_their_post_run_provenance。
    # 关键断言：self.assertIn("评测器在 Agent 执行后的产物检查", prompt)；此处只验证测试构造的场景。
    def test_judge_receives_artifact_facts_and_their_post_run_provenance(self):
        self.mode("png")
        self.model()
        self.config["artifacts"] = {
            "*": [{"pattern": "output/*/assets/*.png", "media_type": "image/png"}]
        }
        self.evaluate()
        request = REQUESTS[-1]
        prompt = request["messages"][0]["content"]
        self.assertIn("评测器在 Agent 执行后的产物检查", prompt)
        self.assertIn("产物正确不能补足缺失动作", prompt)
        packet = json.loads(request["messages"][1]["content"])
        artifact = packet["evidence"]["artifacts"]["output/run/assets/image0.png"]
        self.assertEqual(artifact["width"], 2)
        self.assertEqual(artifact["height"], 1)

    # 回归用例：test_missing_and_sensitive_artifacts。
    # 关键断言：self.assertFalse(list((self.root / mode / "artifacts").rglob("*.json")))；此处只验证测试构造的场景。
    def test_missing_and_sensitive_artifacts(self):
        for mode in ("missing", "artifact_leak"):
            self.mode(mode)
            self.evaluate(code=1, name=mode)
            self.assertFalse(list((self.root / mode / "artifacts").rglob("*.json")))

    # 回归用例：test_artifact_patterns_collect_once_and_allow_zero_matches。
    # 关键断言：self.assertEqual(list(files), ["output/run/assets/image0.png"])；此处只验证测试构造的场景。
    def test_artifact_patterns_collect_once_and_allow_zero_matches(self):
        self.mode("png_declared")
        self.config["artifacts"] = {
            "*": [{"pattern": "output/*/assets/*.png", "media_type": "image/png"}],
            "one": [
                {"path": "output/run/assets/image0.png", "media_type": "image/png"}
            ],
        }
        trial = self.evaluate(name="matched")["trials"][0]
        files = trial["evidence"]["artifacts"]
        self.assertEqual(list(files), ["output/run/assets/image0.png"])
        self.assertEqual(files["output/run/assets/image0.png"]["width"], 2)
        self.mode("good")
        del self.config["artifacts"]["one"]
        self.assertEqual(
            self.evaluate(name="optional")["trials"][0]["evidence"]["artifacts"], {}
        )

    # 回归用例：test_artifact_patterns_reject_unsafe_paths_symlinks_and_limit。
    # 重点核验字段或诊断：evaluation_artifact_symlink、png_parent_symlink、evaluation_artifact_count_limit；异常路径不能被当成成功。
    def test_artifact_patterns_reject_unsafe_paths_symlinks_and_limit(self):
        for index, declaration in enumerate(
            (
                {"pattern": "../*.png"},
                {"pattern": "output/**/*.png"},
                {"pattern": "/output/*.png"},
                {"pattern": "output/[ab].png"},
                {"path": "a.png", "pattern": "*.png"},
            )
        ):
            self.config["artifacts"] = {"*": [declaration]}
            self.evaluate(code=2, name=f"invalid-pattern-{index}")
        self.config["artifacts"] = {
            "*": [{"pattern": "output/*/assets/*.png", "media_type": "image/png"}]
        }
        private = self.root / "private.txt"
        private.write_text(KEY)
        for mode, error in (
            ("png_symlink", "evaluation_artifact_symlink"),
            ("png_parent_symlink", "evaluation_artifact_symlink"),
            ("png_many", "evaluation_artifact_count_limit"),
        ):
            self.config["variants"][0]["command"] = [
                sys.executable,
                "-B",
                str(SOURCE / "tests/evaluation_host.py"),
                mode,
                str(private),
            ]
            trial = self.evaluate(code=1, name=mode)["trials"][0]
            self.assertEqual(trial["error_code"], error)
            self.assertFalse(list((self.root / mode / "artifacts").rglob("*.png")))

    # 回归用例：test_png_decode_rejects_corrupt_truncated_and_expanding_images。
    # 重点核验字段或诊断：evaluation_image_decode_failed、png_no_end、evaluation_image_decoded_bytes_limit；异常路径不能被当成成功。
    def test_png_decode_rejects_corrupt_truncated_and_expanding_images(self):
        self.mode("png")
        self.config["artifacts"] = {
            "*": [{"pattern": "output/*/assets/*.png", "media_type": "image/png"}]
        }
        trial = self.evaluate(name="valid-png")["trials"][0]
        if (
            trial["evidence"]["artifacts"]["output/run/assets/image0.png"]["validation"]
            == "header_only"
        ):
            self.skipTest("libpng decoding is not enabled in this build")
        for mode, error in (
            ("png_corrupt", "evaluation_image_decode_failed"),
            ("png_truncated", "evaluation_image_decode_failed"),
            ("png_no_end", "evaluation_image_decode_failed"),
            ("png_huge", "evaluation_image_decoded_bytes_limit"),
        ):
            self.mode(mode)
            trial = self.evaluate(code=1, name=mode)["trials"][0]
            self.assertEqual(trial["error_code"], error)
            self.assertFalse(list((self.root / mode / "artifacts").rglob("*.png")))

    # 回归用例：test_configured_artifact_media_conflict_fails。
    # 重点核验字段或诊断：evaluation_artifact_type_conflict；异常路径不能被当成成功。
    def test_configured_artifact_media_conflict_fails(self):
        self.mode("png_declared")
        self.config["artifacts"] = {
            "*": [{"pattern": "output/*/assets/*.png", "media_type": "text/plain"}]
        }
        trial = self.evaluate(code=1)["trials"][0]
        self.assertEqual(trial["error_code"], "evaluation_artifact_type_conflict")

    # 回归用例：test_artifact_failure_still_reviews_execution_efficiency。
    # 重点核验字段或诊断：loop_review_prompt、evaluation_file_not_regular；异常路径不能被当成成功。
    def test_artifact_failure_still_reviews_execution_efficiency(self):
        self.mode("loop_trace")
        self.model()
        Provider.behavior = "loops_detailed"
        self.config["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
        self.config["artifacts"] = {
            "*": [{"path": "missing.png", "media_type": "image/png"}]
        }
        report = self.evaluate(code=1)
        trial = report["trials"][0]
        self.assertEqual(trial["verdict"], "failed")
        self.assertEqual(trial["error_code"], "evaluation_file_not_regular")
        self.assertEqual(report["summary"]["model_calls"], 1)
        self.assertEqual(trial["evidence"]["loop_review"]["status"], "complete")
        self.assertEqual(
            trial["evidence"]["artifact_error"], "evaluation_file_not_regular"
        )
        self.assertFalse(list((self.root / "evaluation/artifacts").rglob("*.png")))

    # 回归用例：test_intentional_corrupt_input_uses_declared_media_not_suffix。
    # 重点核验字段或诊断：invalid_png_fixture、is_valid_png；异常路径不能被当成成功。
    def test_intentional_corrupt_input_uses_declared_media_not_suffix(self):
        self.mode("invalid_png_fixture")
        self.config["artifacts"] = {
            "*": [
                dict(pattern="output/*/assets/broken.png", media_type="text/plain"),
                dict(path="report.json", media_type="application/json"),
            ]
        }
        report = self.evaluate()
        files = report["trials"][0]["evidence"]["artifacts"]
        self.assertEqual(files["output/run/assets/broken.png"]["text"], "NOT_AN_IMAGE")
        self.assertFalse(files["report.json"]["json"]["is_valid_png"])

    # 回归用例：test_trace_validation_and_identity。
    # 重点核验字段或诊断：trace_wrong_id；异常路径不能被当成成功。
    def test_trace_validation_and_identity(self):
        self.config["require_trace"] = True
        self.mode("trace")
        report = self.evaluate(name="valid")
        self.assertIn("trajectory", report["trials"][0]["evidence"])
        for mode in ("good", "trace_wrong_id", "trace_open"):
            self.mode(mode)
            self.evaluate(code=3, name=mode)

    # 回归用例：test_execution_status_does_not_replace_business_acceptance。
    # 关键断言：self.assertEqual(report["trials"][0]["execution_status"], "failed")；此处只验证测试构造的场景。
    def test_execution_status_does_not_replace_business_acceptance(self):
        self.mode("failed")
        report = self.evaluate()
        self.assertEqual(report["trials"][0]["execution_status"], "failed")
        self.assertEqual(report["trials"][0]["verdict"], "passed")

    # 回归用例：test_actual_completion_order_does_not_change_identity。
    # 关键断言：self.assertEqual(len({x["trial_id"] for x in report["trials"]}), 6)；此处只验证测试构造的场景。
    def test_actual_completion_order_does_not_change_identity(self):
        cases = []
        for i, delay in enumerate((0.15, 0.01, 0.06)):
            cases.append(
                dict(
                    task_id=f"t{i}",
                    group_id=f"g{i}",
                    task_input=f"测试任务{i}",
                    expected_behavior="值为20",
                    difficulty="easy",
                    category="order",
                    initial_state={"value": 20, "delay": delay},
                    split="train",
                    review_status="reviewed",
                )
            )
        self.make_dataset(cases)
        self.mode("order")
        self.config["parallelism"] = 3
        self.config["repetitions"] = 2
        report = self.evaluate()
        self.assertEqual(len({x["trial_id"] for x in report["trials"]}), 6)
        for trial in report["trials"]:
            self.assertEqual(trial["task_id"], trial["evidence"]["state"]["task_id"])

    # 回归用例：test_symlink_artifact_cannot_read_outside_workspace。
    # 重点核验字段或诊断：evaluation_artifact_symlink；异常路径不能被当成成功。
    def test_symlink_artifact_cannot_read_outside_workspace(self):
        target = self.root / "private.json"
        write_json(target, {"total": KEY})
        self.mode("symlink")
        self.config["variants"][0]["command"].append(str(target))
        report = self.evaluate(code=1)
        self.assertEqual(
            report["trials"][0]["error_code"], "evaluation_artifact_symlink"
        )

    # 回归用例：test_descendants_are_stopped_at_timeout。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
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
            time.sleep(0.01)
        else:
            self.fail("descendant process survived evaluation timeout")

    # 回归用例：test_detached_descendants_are_reaped_before_workspace_cleanup。
    # 关键断言：self.assertEqual(report["trials"][0]["error_code"], "runner_timeout")；此处只验证测试构造的场景。
    def test_detached_descendants_are_reaped_before_workspace_cleanup(self):
        pid_file = self.root / "detached.pid"
        self.mode("detached_child")
        self.config["variants"][0]["command"].append(str(pid_file))
        self.config["timeout_ms"] = 500
        sentinel = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(20)"],
            start_new_session=True,
        )
        try:
            report = self.evaluate(code=1)
            self.assertEqual(report["trials"][0]["error_code"], "runner_timeout")
            self.assertTrue(pid_file.exists(), "detached worker never started")
            self.assertFalse(
                Path(f"/proc/{int(pid_file.read_text())}").exists(),
                "detached worker survived",
            )
            time.sleep(0.7)
            self.assertFalse(
                (self.root / "evaluation/.work").exists(),
                "late worker recreated the workspace",
            )
            self.assertIsNone(sentinel.poll(), "cleanup touched an unrelated process")
        finally:
            sentinel.terminate()
            sentinel.wait(timeout=5)

    # 回归用例：test_trial_limit_before_start。
    # 关键断言：self.assertFalse((self.root / "evaluation").exists())；此处只验证测试构造的场景。
    def test_trial_limit_before_start(self):
        self.config.update(repetitions=2, max_trials=1)
        self.evaluate(code=2)
        self.assertFalse((self.root / "evaluation").exists())

    # 回归用例：test_review_gate_and_explicit_override。
    # 关键断言：self.assertFalse((self.root / "blocked").exists())；此处只验证测试构造的场景。
    def test_review_gate_and_explicit_override(self):
        self.make_dataset(reviewed=False)
        self.evaluate(code=2, name="blocked")
        self.assertFalse((self.root / "blocked").exists())
        report = self.evaluate(extra=("--allow-unreviewed",))
        self.assertTrue(report["configuration"]["allow_unreviewed"])

    # 回归用例：test_invalid_grader_and_limits_before_run。
    # 关键断言：self.assertFalse((self.root / "no-grader").exists())；此处只验证测试构造的场景。
    def test_invalid_grader_and_limits_before_run(self):
        self.config["checks"] = {}
        self.evaluate(code=2, name="no-grader")
        self.assertFalse((self.root / "no-grader").exists())
        self.config["checks"] = {"*": [dict(id="x", path="/bad~3path", op="exists")]}
        self.evaluate(code=2, name="bad-pointer")

    # 回归用例：test_model_pass_and_prompt_separation。
    # 重点核验字段或诊断：evidence_text_refs；异常路径不能被当成成功。
    def test_model_pass_and_prompt_separation(self):
        constraints = ["只使用本次工作区。", "失败后停止，不得重试或伪造成功。"]
        self.make_dataset(
            cases=[
                dict(
                    task_id="one",
                    group_id="one",
                    task_input="计算已知数据的总量。",
                    expected_behavior="总量为20，不编造缺失的数据。",
                    constraints=constraints,
                    difficulty="easy",
                    category="data",
                    initial_state={"value": 20},
                    followup_inputs=["请再次确认。"],
                    split="train",
                    review_status="reviewed",
                )
            ]
        )
        self.model()
        report = self.evaluate()
        self.assertEqual(report["summary"]["model_calls"], 1)
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        self.assertIn("expected_behavior", packet)
        self.assertEqual(packet["constraints"], constraints)
        self.assertIn("禁止项", packet["contract"]["reason"])
        self.assertIn(
            "task_input/followup_inputs 规定用户要完成的工作",
            REQUESTS[-1]["messages"][0]["content"],
        )
        self.assertIn("required_checks", REQUESTS[-1]["messages"][0]["content"])
        self.assertIn(
            "内部经验、策略、工具说明和输出不能新增验收禁令",
            REQUESTS[-1]["messages"][0]["content"],
        )
        self.assertIn(
            "先检查明确缺失的必需项或违规", REQUESTS[-1]["messages"][0]["content"]
        )
        self.assertIn("A && B || echo", REQUESTS[-1]["messages"][0]["content"])
        self.assertIn(
            "后续正确产物也不能补足未执行的必需步骤",
            REQUESTS[-1]["messages"][0]["content"],
        )
        self.assertIn(
            "目标工具未再次执行，不算目标操作重试",
            REQUESTS[-1]["messages"][0]["content"],
        )
        self.assertIn(
            "可计循环浪费，不能仅据此判业务失败", REQUESTS[-1]["messages"][0]["content"]
        )
        self.assertIn("除非标准禁止该调度尝试", REQUESTS[-1]["messages"][0]["content"])
        self.assertIn(
            "failed 的 reason 须引用用户请求或 expected_behavior/constraints/required_checks",
            REQUESTS[-1]["messages"][0]["content"],
        )
        loop_prompt = (SOURCE / "config/prompts/task-loop-review.txt").read_text(
            encoding="utf-8"
        )
        self.assertIn("浪费可与业务 passed 同时成立", loop_prompt)
        self.assertNotIn("违规仍使业务 failed", loop_prompt)
        self.assertEqual(packet["contract"]["evidence_paths"], ["/output"])
        self.assertLess(
            len(REQUESTS[-1]["messages"][0]["content"].encode("utf-8")), 6000
        )
        self.assertNotIn("evidence_text_refs", packet)
        self.assertEqual(REQUESTS[-1]["thinking"]["type"], "disabled")
        self.assertNotIn(KEY, json.dumps(packet))

        self.mode("loop_trace")
        self.evaluate(name="with-trace")
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        self.assertEqual(packet["contract"]["evidence_paths"], ["/trace"])

    # 回归用例：test_json_object_output_is_opt_in。
    # 重点核验字段或诊断：json_object_output；异常路径不能被当成成功。
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
                self.assertEqual(
                    REQUESTS[-1].get("response_format"),
                    {"type": "json_object"} if enabled else None,
                )
                self.assertEqual(REQUESTS[-1]["thinking"]["type"], "disabled")

    # 回归用例：test_json_object_output_rejects_non_boolean_before_execution。
    # 重点核验字段或诊断：json_object_output；异常路径不能被当成成功。
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

    # 回归用例：test_judge_shares_repeated_text_without_losing_evidence。
    # 重点核验字段或诊断：share_repeated_text、max_output_bytes、max_input_bytes；异常路径不能被当成成功。
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
        details = {
            "nested/~": [repeated, None, {"again/~": repeated}],
            "copies": [repeated] * 10,
            "unique": "不重复的证据。" * 80,
            "spoof": {"$evidence_ref": "/not-real"},
        }
        self.make_dataset(
            [
                dict(
                    task_id="shared",
                    group_id="shared",
                    task_input="核验总量。",
                    expected_behavior="总量为20，保留完整证据。",
                    difficulty="easy",
                    category="data",
                    initial_state=dict(value=20, details=details),
                    split="train",
                    review_status="reviewed",
                )
            ]
        )
        self.config["max_output_bytes"] = 1048576
        self.config["judge"]["max_input_bytes"] = 131072
        report = self.evaluate()
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        original = report["trials"][0]["evidence"]

        # 测试辅助函数：model_view。
        # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
        def model_view(raw):
            view = json.loads(json.dumps(raw))
            for field in (
                "trajectory",
                "execution_run_id",
                "loaded_version",
                "loop_review",
            ):
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
                    for field in (
                        "action_id",
                        "execution_id",
                        "step_id",
                        "loop_id",
                        "operation",
                        "status",
                        "decision",
                        "reason",
                    ):
                        if event.get(field) == "":
                            event.pop(field)
                    for field in (
                        "protocol",
                        "agent_version_id",
                        "evaluation_trial_id",
                    ):
                        event.get("attributes", {}).pop(field, None)
            return view

        self.assertEqual(original["state"]["details"], details)
        self.assertEqual(report["summary"]["model_calls"], 1)
        self.assertTrue(packet["evidence_text_refs"])
        self.assertLessEqual(
            len(REQUESTS[-1]["messages"][1]["content"].encode()), 131072
        )
        self.assertGreater(
            len(json.dumps(original, ensure_ascii=False).encode()), 131072
        )
        restored = packet["evidence"]
        self.assertEqual(restored["output"], original["output"])
        self.assertEqual(restored["state"], original["state"])
        for event, raw in zip(restored["trace"]["events"], original["trace"]["events"]):
            self.assertEqual(event.get("output"), raw.get("output"))

        # 沿已校验路径定位请求字段，缺失位置不擅自创建不受支持的协议结构。
        def locate(path):
            parts = [
                p.replace("~1", "/").replace("~0", "~") for p in path[1:].split("/")
            ]
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

        # 测试辅助函数：value_at。
        # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
        def value_at(path):
            value = packed
            for part in path[1:].split("/"):
                part = part.replace("~1", "/").replace("~0", "~")
                value = value[int(part)] if isinstance(value, list) else value[part]
            return value

        # 测试辅助函数：restore。
        # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
        def restore(value, path="", active=frozenset()):
            if path in aliases:
                self.assertNotIn(path, active)
                source = aliases[path]
                self.assertEqual(value, {"$evidence_ref": source})
                return restore(value_at(source), source, active | {path})
            if isinstance(value, list):
                return [
                    restore(v, path + "/" + str(i), active) for i, v in enumerate(value)
                ]
            if isinstance(value, dict):
                return {
                    k: restore(
                        v, path + "/" + k.replace("~", "~0").replace("/", "~1"), active
                    )
                    for k, v in value.items()
                }
            return value

        self.assertTrue(
            any(
                isinstance(value_at(source), (dict, list))
                for source in aliases.values()
            )
        )
        original = report["trials"][0]["evidence"]
        self.assertEqual(restore(packed), model_view(original))
        self.assertEqual(packed["output"], original["output"])
        self.assertEqual(packed["state"], original["state"])
        self.assertTrue(any("/output" in path for path in aliases))
        self.assertTrue(
            any(
                path.endswith("/attributes") or "/attributes/" in path
                for path in aliases
            )
        )
        self.assertLess(
            len(REQUESTS[-1]["messages"][1]["content"].encode()), previous_bytes
        )

    # 回归用例：test_minimum_judge_input_is_the_default。
    # 重点核验字段或诊断：max_output_bytes、max_input_bytes、evidence_text_refs；异常路径不能被当成成功。
    def test_minimum_judge_input_is_the_default(self):
        self.model()
        self.mode("repeated_evidence")
        self.make_dataset(
            [
                dict(
                    task_id="minimal",
                    group_id="minimal",
                    task_input="核验总量。",
                    expected_behavior="总量为20。",
                    difficulty="easy",
                    category="data",
                    initial_state=dict(
                        value=20, details={"copies": ["必要中文证据。" * 120] * 10}
                    ),
                    split="train",
                    review_status="reviewed",
                )
            ]
        )
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

    # 回归用例：test_http_grading_uses_configured_parallel_workers。
    # 关键断言：self.assertEqual(report["summary"]["model_calls"], 2)；此处只验证测试构造的场景。
    def test_http_grading_uses_configured_parallel_workers(self):
        condition = threading.Condition()
        state = dict(active=0, entered=0, peak=0)

        class ConcurrentProvider(Provider):
            # 测试辅助函数：do_POST。
            # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
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
            runtime["model"]["endpoint"] = (
                f"http://127.0.0.1:{server.server_port}/chat/completions"
            )
            write_json(runtime_path, runtime)
            self.config.update(repetitions=2, parallelism=2)
            report = self.evaluate()
            self.assertEqual(report["summary"]["model_calls"], 2)
            self.assertEqual(
                state["peak"], 2, "HTTP grading was serialized across parallel workers"
            )
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    # 回归用例：test_http_retries_share_one_timeout_budget。
    # 重点核验字段或诊断：evaluation_judge_transport_28、evaluation_judge_http_401；异常路径不能被当成成功。
    def test_http_retries_share_one_timeout_budget(self):
        self.model()
        runtime_path = self.root / "runtime.json"
        runtime = json.loads(runtime_path.read_text())
        for scenario, calls, reason in (
            ("timeout", 3, "evaluation_judge_transport_28"),
            ("remaining", 3, "evaluation_judge_transport_28"),
            ("stalled_recovered", 2, None),
            ("no_retry", 1, "evaluation_judge_transport_28"),
            ("recovered", 2, None),
            ("unauthorized", 1, "evaluation_judge_http_401"),
        ):
            with self.subTest(scenario=scenario):
                entered = []
                release = threading.Event()

                class BudgetProvider(Provider):
                    # 测试辅助函数：do_POST。
                    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
                    def do_POST(self):
                        entered.append(time.monotonic())
                        if (
                            scenario in ("recovered", "stalled_recovered")
                            and len(entered) > 1
                        ):
                            return super().do_POST()
                        self.rfile.read(int(self.headers["Content-Length"]))
                        if scenario == "unauthorized" or (
                            scenario in ("remaining", "recovered") and len(entered) == 1
                        ):
                            if scenario == "remaining":
                                time.sleep(0.2)
                            self.send_response(
                                401 if scenario == "unauthorized" else 503
                            )
                            self.send_header("Content-Length", "0")
                            self.end_headers()
                        else:
                            release.wait(10)

                server = http.server.ThreadingHTTPServer(
                    ("127.0.0.1", 0), BudgetProvider
                )
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                try:
                    runtime["model"].update(
                        endpoint=f"http://127.0.0.1:{server.server_port}/chat/completions",
                        timeout_ms=1200,
                        max_retries=0 if scenario == "no_retry" else 2,
                    )
                    write_json(runtime_path, runtime)
                    report = self.evaluate(
                        code=0 if reason is None else 3, name="http-" + scenario
                    )
                    self.assertEqual(
                        len(entered), calls, "retries renewed the network timeout"
                    )
                    self.assertLess(
                        time.monotonic() - entered[0],
                        2.0,
                        "transport retries exceeded the shared budget and cleanup allowance",
                    )
                    if len(entered) > 1:
                        self.assertLess(
                            entered[-1] - entered[0],
                            1.2,
                            "transport retry started after the total budget",
                        )
                    self.assertEqual(
                        report["summary"]["model_calls"],
                        1,
                        "transport failure incorrectly entered semantic recovery",
                    )
                    check = report["trials"][0]["checks"][-1]
                    self.assertEqual(
                        check["status"], "passed" if reason is None else "error"
                    )
                    if reason is not None:
                        self.assertEqual(check["reason"], reason)
                finally:
                    release.set()
                    server.shutdown()
                    server.server_close()
                    thread.join()

    # 回归用例：test_failed_completion_envelope_is_not_a_valid_judgment。
    # 重点核验字段或诊断：evaluation_judge_invalid_api_response；异常路径不能被当成成功。
    def test_failed_completion_envelope_is_not_a_valid_judgment(self):
        self.model()
        mode = "content_filter"

        class EnvelopeProvider(Provider):
            # 测试辅助函数：do_POST。
            # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
            def do_POST(self):
                self.rfile.read(int(self.headers["Content-Length"]))
                answer = json.dumps(
                    dict(
                        verdict="passed",
                        reason="实际输出满足要求。",
                        evidence_paths=["/state"],
                    )
                )
                choice = dict(finish_reason="stop", message=dict(content=answer))
                value = dict(choices=[choice])
                if mode == "content_filter":
                    choice["finish_reason"] = "content_filter"
                elif mode == "error":
                    value["error"] = dict(
                        message="provider error body must remain private"
                    )
                elif mode == "multiple":
                    value["choices"].append(dict(choice))
                elif mode == "refusal":
                    choice["message"]["refusal"] = "not a completed judgment"
                encoded = json.dumps(value).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), EnvelopeProvider)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            runtime = json.loads((self.root / "runtime.json").read_text())
            runtime["model"]["endpoint"] = (
                f"http://127.0.0.1:{server.server_port}/chat/completions"
            )
            write_json(self.root / "runtime.json", runtime)
            for mode in ("content_filter", "error", "multiple", "refusal"):
                with self.subTest(mode=mode):
                    report = self.evaluate(code=3, name="envelope-" + mode)
                    self.assertEqual(
                        report["trials"][0]["checks"][-1]["reason"],
                        "evaluation_judge_invalid_api_response",
                    )
                    self.assertEqual(report["summary"]["model_calls"], 1)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    # 回归用例：test_model_prompt_is_not_silently_cut_at_nul。
    # 关键断言：self.assertTrue(REQUESTS[-1]["messages"][0]["content"].endswith("\0正文仍须保留。"))；此处只验证测试构造的场景。
    def test_model_prompt_is_not_silently_cut_at_nul(self):
        self.model()
        path = self.root / self.config["judge"]["prompt"]
        path.write_text(path.read_text() + "\n\0正文仍须保留。", encoding="utf-8")
        self.evaluate()
        self.assertTrue(
            REQUESTS[-1]["messages"][0]["content"].endswith("\0正文仍须保留。")
        )

    # 回归用例：test_ambiguous_model_envelope_is_rejected。
    # 重点核验字段或诊断：evaluation_judge_invalid_api_response；异常路径不能被当成成功。
    def test_ambiguous_model_envelope_is_rejected(self):
        self.model()
        mode = "root"

        class AmbiguousProvider(Provider):
            # 测试辅助函数：do_POST。
            # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
            def do_POST(self):
                self.rfile.read(int(self.headers["Content-Length"]))
                answer = json.dumps(
                    dict(
                        verdict="passed",
                        reason="实际输出满足要求。",
                        evidence_paths=["/state"],
                    )
                )
                content = json.dumps(answer)
                if mode == "stream":
                    encoded = (
                        'data: {"choices":[{"index":0,"delta":{"content":'
                        + content
                        + ',"content":""},"finish_reason":"stop"}]}\n\ndata: [DONE]\n\n'
                    ).encode()
                else:
                    message = (
                        '{"content":'
                        + content
                        + (',"content":""' if mode == "message" else "")
                        + "}"
                    )
                    choice = '{"finish_reason":"stop","message":' + message
                    if mode == "choice":
                        choice += ',"finish_reason":"content_filter"'
                    if mode == "index":
                        choice += ',"index":1'
                    encoded = (
                        '{"choices":['
                        + choice
                        + "}]"
                        + (',"choices":[]' if mode == "root" else "")
                        + "}"
                    ).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), AmbiguousProvider)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            runtime_path = self.root / "runtime.json"
            runtime = json.loads(runtime_path.read_text())
            for mode in ("root", "choice", "message", "index", "stream"):
                with self.subTest(mode=mode):
                    runtime["model"].update(
                        endpoint=f"http://127.0.0.1:{server.server_port}/chat/completions",
                        stream=mode == "stream",
                    )
                    write_json(runtime_path, runtime)
                    report = self.evaluate(code=3, name="ambiguous-" + mode)
                    self.assertEqual(
                        report["trials"][0]["checks"][-1]["reason"],
                        "evaluation_judge_invalid_api_response",
                    )
                    self.assertEqual(report["summary"]["model_calls"], 1)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    # 回归用例：test_streamed_model_responses_are_complete_and_bounded。
    # 重点核验字段或诊断：evaluation_judge_output_token_limit_reached；异常路径不能被当成成功。
    def test_streamed_model_responses_are_complete_and_bounded(self):
        self.model()
        runtime_path = self.root / "runtime.json"
        runtime = json.loads(runtime_path.read_text())
        mode = "normal"
        observed = []

        class StreamProvider(Provider):
            # 测试辅助函数：do_POST。
            # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
            def do_POST(self):
                if mode == "json_fallback":
                    return super().do_POST()
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                observed.append(body)
                answer = json.dumps(
                    dict(
                        verdict="passed",
                        reason="实际结果满足全部要求。",
                        evidence_paths=["/state"],
                    ),
                    ensure_ascii=False,
                )
                if mode == "oversized":
                    answer += " " * 2000
                frames = []
                for start in range(0, len(answer), 5):
                    frames.append(
                        {
                            "choices": [
                                {
                                    "index": 0,
                                    "delta": {"content": answer[start : start + 5]},
                                    "finish_reason": None,
                                }
                            ]
                        }
                    )
                frames.append(
                    {
                        "choices": [
                            {
                                "index": 0,
                                "delta": {},
                                "finish_reason": "length"
                                if mode == "length"
                                else "stop",
                            }
                        ]
                    }
                )
                frames.append({"choices": [], "usage": {"completion_tokens": 20}})
                if mode == "malformed":
                    frames.append(
                        {"error": {"message": "do not persist provider diagnostics"}}
                    )
                if mode == "null_error":
                    frames[0]["error"] = None
                if mode == "refusal":
                    frames[0]["choices"][0]["delta"]["refusal"] = (
                        "not a completed judgment"
                    )
                if mode == "multiple_choices":
                    frames[0]["choices"].append(
                        {"index": 1, "delta": {"content": "other"}}
                    )
                data = "\ufeff:keepalive\r\n\r\n"
                for frame in frames:
                    # 覆盖多行 SSE 数据及被网络分片切开的 UTF-8 字符。
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
                        self.wfile.write(encoded[start : start + 29])
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), StreamProvider)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            for mode in (
                "normal",
                "json_fallback",
                "null_error",
                "refusal",
                "length",
                "missing_done",
                "malformed",
                "multiple_choices",
                "oversized",
            ):
                with self.subTest(mode=mode):
                    runtime["model"].update(
                        endpoint=f"http://127.0.0.1:{server.server_port}/chat/completions",
                        stream=True,
                        idle_timeout_ms=1500,
                        max_response_bytes=1024,
                    )
                    write_json(runtime_path, runtime)
                    ok = mode in ("normal", "json_fallback", "null_error")
                    report = self.evaluate(code=0 if ok else 3, name="stream-" + mode)
                    check = report["trials"][0]["checks"][-1]
                    self.assertEqual(check["status"], "passed" if ok else "error")
                    self.assertEqual(report["summary"]["model_calls"], 1)
                    if mode == "length":
                        self.assertEqual(
                            check["reason"],
                            "evaluation_judge_output_token_limit_reached",
                        )
            self.assertTrue(all(r["stream"] for r in observed))
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    # 回归用例：test_stream_idle_retry_does_not_wait_for_total_budget。
    # 关键断言：self.assertEqual(len(entered), 2)；此处只验证测试构造的场景。
    def test_stream_idle_retry_does_not_wait_for_total_budget(self):
        self.model()
        runtime_path = self.root / "runtime.json"
        runtime = json.loads(runtime_path.read_text())
        entered = []
        release = threading.Event()

        class IdleProvider(Provider):
            # 测试辅助函数：do_POST。
            # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
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
            runtime["model"].update(
                endpoint=f"http://127.0.0.1:{server.server_port}/chat/completions",
                stream=True,
                idle_timeout_ms=300,
                timeout_ms=6000,
                max_retries=1,
            )
            write_json(runtime_path, runtime)
            report = self.evaluate()
            self.assertEqual(len(entered), 2)
            self.assertLess(entered[1] - entered[0], 2.0)
            self.assertEqual(report["trials"][0]["checks"][-1]["status"], "passed")
            self.assertEqual(report["summary"]["model_calls"], 1)
        finally:
            release.set()
            server.shutdown()
            server.server_close()
            thread.join()

    # 回归用例：test_sequential_model_calls_reuse_connection。
    # 关键断言：self.assertEqual(report["summary"]["model_calls"], 2)；此处只验证测试构造的场景。
    def test_sequential_model_calls_reuse_connection(self):
        self.model()
        self.config.update(parallelism=1, repetitions=2)
        addresses = []

        class KeepAliveProvider(Provider):
            protocol_version = "HTTP/1.1"

            # 测试辅助函数：do_POST。
            # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
            def do_POST(self):
                addresses.append(self.client_address)
                return super().do_POST()

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), KeepAliveProvider)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            path = self.root / "runtime.json"
            runtime = json.loads(path.read_text())
            runtime["model"]["endpoint"] = (
                f"http://127.0.0.1:{server.server_port}/chat/completions"
            )
            write_json(path, runtime)
            report = self.evaluate()
            self.assertEqual(report["summary"]["model_calls"], 2)
            self.assertEqual(len(addresses), 2)
            self.assertEqual(
                addresses[0],
                addresses[1],
                "sequential requests recreated the TCP connection",
            )
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    # 回归用例：test_stream_options_are_validated_before_agent_execution。
    # 重点核验字段或诊断：idle_timeout_ms；异常路径不能被当成成功。
    def test_stream_options_are_validated_before_agent_execution(self):
        self.model()
        path = self.root / "runtime.json"
        runtime = json.loads(path.read_text())
        for settings in (
            {"stream": "yes"},
            {"stream": False, "idle_timeout_ms": 100},
            {"stream": True, "idle_timeout_ms": -1},
        ):
            with self.subTest(settings=settings):
                altered = json.loads(json.dumps(runtime))
                altered["model"].update(settings)
                write_json(path, altered)
                self.assertIsNone(self.evaluate(code=2))
                self.assertEqual(len(REQUESTS), self.start_requests)

    # 回归用例：test_repeated_credentials_are_rejected_before_model_packing。
    # 重点核验字段或诊断：share_repeated_text、evaluation_sensitive_output_rejected；异常路径不能被当成成功。
    def test_repeated_credentials_are_rejected_before_model_packing(self):
        self.model()
        self.config["judge"]["share_repeated_text"] = True
        self.mode("repeated_leak")
        report = self.evaluate(code=1)
        self.assertEqual(
            report["trials"][0]["error_code"], "evaluation_sensitive_output_rejected"
        )
        self.assertNotIn("evidence", report["trials"][0])
        self.assertEqual(len(REQUESTS), self.start_requests)

    # 回归用例：test_text_sharing_option_is_validated_before_execution。
    # 重点核验字段或诊断：share_repeated_text、share_repeated_inputs；异常路径不能被当成成功。
    def test_text_sharing_option_is_validated_before_execution(self):
        self.model()
        for option in ("share_repeated_text", "share_repeated_inputs"):
            with self.subTest(option=option):
                self.config["judge"][option] = "true"
                self.evaluate(code=2, name="invalid-" + option)
                self.assertFalse((self.root / ("invalid-" + option)).exists())
                self.assertEqual(len(REQUESTS), self.start_requests)
                self.config["judge"].pop(option)

    # 回归用例：test_container_sharing_keeps_privacy_checks_before_model。
    # 重点核验字段或诊断：share_repeated_text、share_repeated_inputs、evaluation_sensitive_output_rejected；异常路径不能被当成成功。
    def test_container_sharing_keeps_privacy_checks_before_model(self):
        self.model()
        self.config["judge"]["share_repeated_text"] = False
        self.config["judge"]["share_repeated_inputs"] = True
        self.mode("repeated_leak")
        report = self.evaluate(code=1)
        self.assertEqual(
            report["trials"][0]["error_code"], "evaluation_sensitive_output_rejected"
        )
        self.assertNotIn("evidence", report["trials"][0])
        self.assertEqual(len(REQUESTS), self.start_requests)

    # 回归用例：test_loop_review_reuses_judge_and_counts_distinct_iterations。
    # 重点核验字段或诊断：loop_review_prompt、anchor_event_id、allowed_evidence_paths；异常路径不能被当成成功。
    def test_loop_review_reuses_judge_and_counts_distinct_iterations(self):
        self.model()
        self.mode("loop_trace")
        Provider.behavior = "loops"
        self.config["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
        report = self.evaluate()
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        inventory = packet["loop_inventory"]
        self.assertEqual(
            len(inventory), 4
        )  # 两条分支各一轮，共两轮，不是按 16 个步骤或动作事件计数。
        self.assertEqual(
            {v["execution_id"] for v in inventory.values()}, {"left", "right"}
        )
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
        self.assertEqual(
            evidence["trajectory"]["summary"]["invalid_loop_lower_bound"], 0
        )
        self.assertEqual(
            report["summary"]["invalid_loop_assessment"],
            dict(method="model_review", mean=1, observations=1),
        )

    # 回归用例：test_loop_review_uses_short_refs_and_persists_canonical_ids。
    # 重点核验字段或诊断：loop_review_prompt、evidence_event_ids；异常路径不能被当成成功。
    def test_loop_review_uses_short_refs_and_persists_canonical_ids(self):
        self.model()
        self.mode("loop_trace")
        self.config["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
        Provider.behavior = "loops_detailed"
        report = self.evaluate()
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        inventory = packet["loop_inventory"]
        self.assertEqual(set(inventory), {"i1", "i2", "i3", "i4"})
        canonical = {
            hashlib.sha256(
                json.dumps(
                    [
                        item["run_id"],
                        item["execution_id"],
                        item["loop_id"],
                        str(item["iteration"]),
                    ],
                    separators=(",", ":"),
                ).encode()
            ).hexdigest(): item
            for item in inventory.values()
        }
        review = report["trials"][0]["evidence"]["loop_review"]
        self.assertEqual(
            {item["iteration_id"] for item in review["iteration_reviews"]},
            set(canonical),
        )
        for item in review["iteration_reviews"] + review["wasted_iterations"]:
            self.assertIn(
                item["evidence_event_ids"][0],
                canonical[item["iteration_id"]]["event_ids"],
            )
        self.assertEqual(report["summary"]["model_calls"], 1)

    # 回归用例：test_loop_review_short_refs_follow_observations_not_hashes。
    # 重点核验字段或诊断：loop_review_prompt、anchor_event_id；异常路径不能被当成成功。
    def test_loop_review_short_refs_follow_observations_not_hashes(self):
        self.model()
        self.mode("loop_trace")
        command = self.config["variants"][0]["command"]
        # 十二次真实迭代跨越 i9/i10 的字典序边界，
        # 不能因此合并六条独立观测的执行分支。
        wrapper = (
            "import json,subprocess,sys\n"
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
            "events.append(end); trace['events']=events\nprint(json.dumps(value))\n"
        )
        self.config["variants"][0]["command"] = [
            sys.executable,
            "-c",
            wrapper,
            *command,
        ]
        self.config["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
        Provider.behavior = "loops_detailed"
        report = self.evaluate()
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        inventory = packet["loop_inventory"]
        self.assertEqual(list(inventory), [f"i{i:02}" for i in range(1, 13)])
        trace = packet["evidence"]["trace"]
        order = {
            (trace["identity"]["run_id"], event["event_id"]): index
            for index, event in enumerate(trace["events"])
        }
        ranks = [
            order[(item["run_id"], item["anchor_event_id"])]
            for item in inventory.values()
        ]
        self.assertEqual(ranks, sorted(ranks))
        reviews = report["trials"][0]["evidence"]["loop_review"]["iteration_reviews"]
        self.assertEqual(len({item["iteration_id"] for item in reviews}), 12)
        self.assertTrue(all(len(item["iteration_id"]) == 64 for item in reviews))
        prompt = REQUESTS[-1]["messages"][0]["content"]
        self.assertIn("最后汇总业务 verdict", prompt)
        self.assertIn("不代表并发依赖，前因以 caused_by 为准", prompt)

    # 回归用例：test_loop_review_rejects_duplicates_unknown_ids_and_other_loop_evidence。
    # 重点核验字段或诊断：loops_other_event、evaluation_judge_failed、invalid_loop_assessment；异常路径不能被当成成功。
    def test_loop_review_rejects_duplicates_unknown_ids_and_other_loop_evidence(self):
        self.model()
        self.mode("loop_trace")
        self.config["judge"].update(
            loop_review_prompt="prompts/task-loop-review.txt", max_repair_attempts=0
        )
        for behavior in ("loops_duplicate", "loops_unknown", "loops_other_event"):
            with self.subTest(behavior=behavior):
                Provider.behavior = behavior
                report = self.evaluate(code=3, name=behavior)
                self.assertEqual(
                    report["trials"][0]["error_code"], "evaluation_judge_failed"
                )
                self.assertIsNone(report["summary"]["invalid_loop_assessment"]["mean"])
                self.assertEqual(
                    report["summary"]["invalid_loop_assessment"]["observations"], 0
                )

    # 回归用例：test_detailed_loop_review_counts_decisions_and_keeps_all_reasons。
    # 重点核验字段或诊断：loop_review_prompt；异常路径不能被当成成功。
    def test_detailed_loop_review_counts_decisions_and_keeps_all_reasons(self):
        self.model()
        self.mode("loop_trace")
        self.config["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
        Provider.behavior = "loops_detailed"
        report = self.evaluate()
        review = report["trials"][0]["evidence"]["loop_review"]
        self.assertEqual(len(review["iteration_reviews"]), 4)
        self.assertEqual(review["count"], 1)
        self.assertEqual(
            [r["iteration_id"] for r in review["wasted_iterations"]],
            [
                r["iteration_id"]
                for r in review["iteration_reviews"]
                if r["decision"] == "waste"
            ],
        )
        self.assertEqual(report["summary"]["model_calls"], 1)
        packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
        self.assertIn("iteration_reviews", packet["contract"]["loop_review"])
        self.assertNotIn("wasted_iterations", packet["contract"]["loop_review"])

    # 回归用例：test_detailed_loop_review_requires_coverage_and_valid_decisions。
    # 重点核验字段或诊断：missing_iteration_reviews、invalid_iteration_decision、uncertain_iteration_in_complete_review；异常路径不能被当成成功。
    def test_detailed_loop_review_requires_coverage_and_valid_decisions(self):
        self.model()
        self.mode("loop_trace")
        self.config["judge"].update(
            loop_review_prompt="prompts/task-loop-review.txt", max_repair_attempts=0
        )
        for suffix, constraint in (
            ("missing", "missing_iteration_reviews"),
            ("duplicate", "duplicate_iteration"),
            ("unknown", "unknown_iteration"),
            ("invalid_decision", "invalid_iteration_decision"),
            ("uncertain_complete", "uncertain_iteration_in_complete_review"),
            ("both", "review_forms_exclusive"),
        ):
            with self.subTest(suffix=suffix):
                Provider.behavior = "loops_detailed_" + suffix
                report = self.evaluate(code=3, name=suffix)
                self.assertEqual(
                    report["trials"][0]["checks"][-1]["reason"],
                    "evaluation_loop_review_invalid:" + constraint,
                )
                self.assertIsNone(report["summary"]["invalid_loop_assessment"]["mean"])

    # 回归用例：test_detailed_uncertain_review_never_becomes_a_partial_or_zero_count。
    # 重点核验字段或诊断：loop_review_prompt、loops_detailed_uncertain、invalid_loop_assessment；异常路径不能被当成成功。
    def test_detailed_uncertain_review_never_becomes_a_partial_or_zero_count(self):
        self.model()
        self.mode("loop_trace")
        self.config["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
        Provider.behavior = "loops_detailed_uncertain"
        report = self.evaluate()
        review = report["trials"][0]["evidence"]["loop_review"]
        self.assertEqual(review["status"], "uncertain")
        self.assertIsNone(review["count"])
        self.assertEqual(review["wasted_iterations"], [])
        self.assertEqual(len(review["iteration_reviews"]), 4)
        self.assertEqual(
            report["summary"]["invalid_loop_assessment"]["observations"], 0
        )

    # 回归用例：test_loop_review_preserves_deterministic_failure。
    # 关键断言：self.assertEqual(trial["verdict"], "failed")；此处只验证测试构造的场景。
    def test_loop_review_preserves_deterministic_failure(self):
        self.model()
        self.mode("loop_trace")
        self.config["checks"]["*"][0]["value"] = 99
        self.config["judge"].update(
            loop_review_prompt="prompts/task-loop-review.txt", max_repair_attempts=0
        )
        for behavior, expected in (
            ("loops", 1),
            ("loops_uncertain", None),
            ("loops_unknown", None),
        ):
            with self.subTest(behavior=behavior):
                Provider.behavior = behavior
                report = self.evaluate(code=1, name=behavior)
                trial = report["trials"][0]
                self.assertEqual(trial["verdict"], "failed")
                self.assertEqual(report["summary"]["pass_rate"], 0)
                self.assertEqual(report["summary"]["model_calls"], 1)
                self.assertEqual(trial["evidence"]["loop_review"]["count"], expected)

    # 回归用例：test_loop_review_allows_prior_causes_with_anchor_but_not_future_evidence。
    # 重点核验字段或诊断：loops_prior_event、loops_future_event；异常路径不能被当成成功。
    def test_loop_review_allows_prior_causes_with_anchor_but_not_future_evidence(self):
        self.model()
        self.mode("loop_trace")
        self.config["judge"].update(
            loop_review_prompt="prompts/task-loop-review.txt", max_repair_attempts=0
        )
        for behavior, code in (("loops_prior_event", 0), ("loops_future_event", 3)):
            with self.subTest(behavior=behavior):
                Provider.behavior = behavior
                report = self.evaluate(code=code, name=behavior)
                trial = report["trials"][0]
                self.assertEqual(
                    trial["checks"][-1]["status"], "passed" if code == 0 else "error"
                )
                self.assertEqual(report["summary"]["model_calls"], 1)
                self.assertEqual(
                    trial["evidence"]["loop_review"]["count"], 1 if code == 0 else None
                )

    # 回归用例：test_loop_review_uses_causal_order_for_unordered_trace_input。
    # 重点核验字段或诊断：loop_trace_unordered、loops_prior_event、loops_future_event；异常路径不能被当成成功。
    def test_loop_review_uses_causal_order_for_unordered_trace_input(self):
        self.model()
        self.mode("loop_trace_unordered")
        self.config["judge"].update(
            loop_review_prompt="prompts/task-loop-review.txt", max_repair_attempts=0
        )
        for behavior, code in (("loops_prior_event", 0), ("loops_future_event", 3)):
            Provider.behavior = behavior
            report = self.evaluate(code=code, name=behavior)
            self.assertEqual(
                report["trials"][0]["evidence"]["loop_review"]["count"],
                1 if code == 0 else None,
            )

    # 回归用例：test_loop_review_accepts_joined_child_but_not_unrelated_or_future_events。
    # 重点核验字段或诊断：loop_trace_join、loop_trace_join_unordered、loops_join_child；异常路径不能被当成成功。
    def test_loop_review_accepts_joined_child_but_not_unrelated_or_future_events(self):
        self.model()
        self.config["judge"].update(
            loop_review_prompt="prompts/task-loop-review.txt", max_repair_attempts=0
        )
        for mode in ("loop_trace_join", "loop_trace_join_unordered"):
            self.mode(mode)
            for behavior, constraint in (
                ("loops_join_child", None),
                ("loops_join_without_anchor", "missing_iteration_anchor"),
                ("loops_join_unrelated", "event_not_in_iteration_or_prior_context"),
                ("loops_join_future", "event_not_in_iteration_or_prior_context"),
            ):
                with self.subTest(mode=mode, behavior=behavior):
                    Provider.behavior = behavior
                    report = self.evaluate(
                        code=3 if constraint else 0, name=mode + behavior
                    )
                    trial = report["trials"][0]
                    review = trial["evidence"]["loop_review"]
                    self.assertEqual(review["count"], None if constraint else 1)
                    inventory = json.loads(REQUESTS[-1]["messages"][1]["content"])[
                        "loop_inventory"
                    ]
                    self.assertEqual(len(inventory), 4)
                    if constraint:
                        self.assertEqual(
                            review["validation_error"]["constraint"], constraint
                        )
                    else:
                        current = next(
                            item
                            for item in inventory.values()
                            if hashlib.sha256(
                                json.dumps(
                                    [
                                        item["run_id"],
                                        item["execution_id"],
                                        item["loop_id"],
                                        str(item["iteration"]),
                                    ],
                                    separators=(",", ":"),
                                ).encode()
                            ).hexdigest()
                            == review["wasted_iterations"][0]["iteration_id"]
                        )
                        self.assertEqual(
                            review["wasted_iterations"][0]["evidence_event_ids"],
                            [current["event_ids"][0], "right-2-action.end"],
                        )
                    self.assertEqual(report["summary"]["model_calls"], 1)

    # 回归用例：test_uncertain_or_unobserved_loop_review_is_not_zero。
    # 重点核验字段或诊断：loop_review_prompt、invalid_loop_assessment；异常路径不能被当成成功。
    def test_uncertain_or_unobserved_loop_review_is_not_zero(self):
        self.model()
        Provider.behavior = "loops_uncertain"
        self.config["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
        for mode in ("loop_trace", "trace"):
            self.mode(mode)
            report = self.evaluate(name=mode)
            self.assertEqual(report["trials"][0]["verdict"], "passed")
            self.assertIsNone(report["trials"][0]["evidence"]["loop_review"]["count"])
            self.assertEqual(
                report["summary"]["invalid_loop_assessment"]["observations"], 0
            )

    # 回归用例：test_model_cannot_override_hard_failure。
    # 关键断言：self.assertEqual(len(REQUESTS), self.start_requests)；此处只验证测试构造的场景。
    def test_model_cannot_override_hard_failure(self):
        self.model()
        self.mode("wrong")
        self.evaluate(code=1)
        self.assertEqual(len(REQUESTS), self.start_requests)

    # 回归用例：test_enclosing_json_fence_and_bounded_semantic_repair。
    # 重点核验字段或诊断：judge_validation_error；异常路径不能被当成成功。
    def test_enclosing_json_fence_and_bounded_semantic_repair(self):
        self.model()
        Provider.behavior = "fence"
        self.evaluate(name="fence")
        Provider.behavior = "repair"
        report = self.evaluate(name="repaired")
        self.assertEqual(report["summary"]["model_calls"], 2)
        self.assertEqual(report["trials"][0]["checks"][-1]["attempts"], 2)
        self.assertEqual(len(report["trials"]), 1)
        feedback = json.loads(REQUESTS[-1]["messages"][1]["content"])[
            "validation_feedback"
        ]
        self.assertEqual(feedback["code"], "invalid_json")
        self.assertEqual(
            feedback["json_error"],
            dict(
                parser_code=3,
                description="Invalid value.",
                offset=0,
                response_bytes=len("invalid response"),
            ),
        )
        self.assertNotIn("previous_response", feedback)
        self.assertNotIn("judge_validation_error", report["trials"][0]["evidence"])

    # 回归用例：test_semantic_repair_can_be_disabled。
    # 重点核验字段或诊断：max_repair_attempts、judge_validation_error；异常路径不能被当成成功。
    def test_semantic_repair_can_be_disabled(self):
        self.model()
        Provider.behavior = "repair"
        self.config["judge"]["max_repair_attempts"] = 0
        report = self.evaluate(code=3)
        self.assertEqual(report["summary"]["model_calls"], 1)
        diagnostic = report["trials"][0]["evidence"]["judge_validation_error"]
        self.assertEqual(diagnostic["code"], "invalid_json")
        self.assertEqual(
            diagnostic["json_error"],
            dict(
                parser_code=3,
                description="Invalid value.",
                offset=0,
                response_bytes=len("invalid response"),
            ),
        )
        self.assertNotIn("invalid response", json.dumps(diagnostic))

    # 回归用例：test_model_uncertain_error_and_forged_reference。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_model_uncertain_error_and_forged_reference(self):
        self.model()
        for mode in ("uncertain", "reference", "leak", "length", "http_error"):
            Provider.behavior = mode
            report = self.evaluate(code=3, name=mode)
            self.assertNotEqual(report["trials"][0]["verdict"], "passed")

    # 回归用例：test_model_can_cite_existing_descendants_but_not_missing_or_foreign_paths。
    # 重点核验字段或诊断：max_repair_attempts、missing_child_reference、encoded_root_reference；异常路径不能被当成成功。
    def test_model_can_cite_existing_descendants_but_not_missing_or_foreign_paths(self):
        self.model()
        self.mode("trace")
        self.config["judge"]["max_repair_attempts"] = 0
        for mode, valid in (
            ("child_reference", True),
            ("array_reference", True),
            ("missing_child_reference", False),
            ("prefix_reference", False),
            ("encoded_root_reference", False),
            ("packet_reference", False),
            ("missing_array_reference", False),
        ):
            with self.subTest(mode=mode):
                Provider.behavior = mode
                report = self.evaluate(code=0 if valid else 3, name=mode)
                check = report["trials"][0]["checks"][-1]
                self.assertEqual(check["status"], "passed" if valid else "error")
                if not valid:
                    self.assertEqual(
                        check["reason"], "evaluation_judge_evidence_invalid"
                    )
                    diagnostic = report["trials"][0]["evidence"][
                        "judge_validation_error"
                    ]
                    self.assertEqual(diagnostic["code"], check["reason"])
                    self.assertEqual(len(diagnostic["invalid_evidence_paths"]), 1)
                    self.assertNotIn("previous_response", diagnostic)
                else:
                    self.assertNotIn(
                        "judge_validation_error", report["trials"][0]["evidence"]
                    )
                self.assertEqual(report["summary"]["model_calls"], 1)

    # 回归用例：test_path_recovery_reports_specific_rejected_paths。
    # 重点核验字段或诊断：evaluation_judge_evidence_invalid、invalid_evidence_paths、judge_validation_error；异常路径不能被当成成功。
    def test_path_recovery_reports_specific_rejected_paths(self):
        self.model()
        for behavior, path in (
            ("path_repair", "/state/missing"),
            ("syntax_repair", "/state/~2bad"),
        ):
            with self.subTest(behavior=behavior):
                Provider.behavior = behavior
                report = self.evaluate(name=behavior)
                feedback = json.loads(REQUESTS[-1]["messages"][1]["content"])[
                    "validation_feedback"
                ]
                self.assertEqual(feedback["code"], "evaluation_judge_evidence_invalid")
                self.assertEqual(feedback["invalid_evidence_paths"], [path])
                self.assertEqual(
                    feedback["previous_response"]["evidence_paths"], ["/output", path]
                )
                self.assertEqual(report["summary"]["model_calls"], 2)
                self.assertEqual(
                    report["trials"][0]["checks"][-1]["evidence_paths"], ["/state"]
                )
                self.assertNotIn(
                    "judge_validation_error", report["trials"][0]["evidence"]
                )

    # 回归用例：test_loop_recovery_receives_previous_safe_judgment。
    # 重点核验字段或诊断：loop_review_prompt、evaluation_loop_review_invalid；异常路径不能被当成成功。
    def test_loop_recovery_receives_previous_safe_judgment(self):
        self.model()
        self.mode("loop_trace")
        self.config["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
        Provider.behavior = "loops_repair"
        report = self.evaluate()
        feedback = json.loads(REQUESTS[-1]["messages"][1]["content"])[
            "validation_feedback"
        ]
        self.assertEqual(feedback["code"], "evaluation_loop_review_invalid")
        self.assertEqual(
            feedback["previous_response"]["loop_review"]["wasted_iterations"][0][
                "iteration_id"
            ],
            "not-an-iteration",
        )
        self.assertEqual(report["summary"]["model_calls"], 2)
        self.assertEqual(report["trials"][0]["evidence"]["loop_review"]["count"], 1)

    # 回归用例：test_loop_recovery_identifies_constraint_and_item。
    # 重点核验字段或诊断：loop_review_prompt、loops_future_event、event_not_in_iteration_or_prior_context；异常路径不能被当成成功。
    def test_loop_recovery_identifies_constraint_and_item(self):
        self.model()
        self.mode("loop_trace")
        self.config["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
        for behavior, constraints, index in (
            ("loops_unknown", {"unknown_iteration"}, 0),
            ("loops_duplicate", {"duplicate_iteration"}, 1),
            ("loops_future_event", {"event_not_in_iteration_or_prior_context"}, 0),
            (
                "loops_other_event",
                {"event_not_in_iteration_or_prior_context", "missing_iteration_anchor"},
                0,
            ),
        ):
            with self.subTest(behavior=behavior):
                Provider.behavior = behavior
                report = self.evaluate(code=3, name=behavior)
                feedback = json.loads(REQUESTS[-1]["messages"][1]["content"])[
                    "validation_feedback"
                ]
                self.assertEqual(feedback["code"], "evaluation_loop_review_invalid")
                detail = feedback["loop_review_error"]
                self.assertIn(detail["constraint"], constraints)
                self.assertEqual(detail["wasted_iteration_index"], index)
                self.assertEqual(report["summary"]["model_calls"], 2)
                self.assertEqual(
                    report["trials"][0]["checks"][-1]["reason"],
                    "evaluation_loop_review_invalid:" + detail["constraint"],
                )
                self.assertEqual(
                    report["trials"][0]["evidence"]["loop_review"]["validation_error"],
                    detail,
                )
                if behavior == "loops_future_event":
                    packet = json.loads(REQUESTS[-1]["messages"][1]["content"])
                    previous = feedback["previous_response"]["loop_review"][
                        "wasted_iterations"
                    ][index]
                    reference = detail["reference"]
                    self.assertEqual(
                        reference["iteration_id"], previous["iteration_id"]
                    )
                    self.assertEqual(reference["decision"], "waste")
                    self.assertEqual(reference["evidence_event_index"], 1)
                    self.assertEqual(
                        reference["event_id"], previous["evidence_event_ids"][1]
                    )
                    self.assertEqual(
                        reference["allowed_iteration_event_ids"],
                        packet["loop_inventory"][previous["iteration_id"]]["event_ids"],
                    )
                    self.assertNotIn(
                        reference["event_id"], reference["allowed_iteration_event_ids"]
                    )

    # 回归用例：test_missing_loop_anchor_recovery_identifies_current_events。
    # 重点核验字段或诊断：loops_anchor_repair、missing_iteration_anchor、invalid_loop_assessment；异常路径不能被当成成功。
    def test_missing_loop_anchor_recovery_identifies_current_events(self):
        self.model()
        self.mode("loop_trace")
        self.config["judge"].update(
            loop_review_prompt="prompts/task-loop-review.txt", max_repair_attempts=0
        )
        Provider.behavior = "loops_anchor_repair"
        report = self.evaluate(code=3, name="missing-anchor")
        review = report["trials"][0]["evidence"]["loop_review"]
        self.assertEqual(
            review["validation_error"]["constraint"], "missing_iteration_anchor"
        )
        self.assertIsNone(review["count"])
        self.assertEqual(
            report["summary"]["invalid_loop_assessment"]["observations"], 0
        )
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
        self.assertEqual(
            reference["allowed_iteration_event_ids"],
            packet["loop_inventory"][previous["iteration_id"]]["event_ids"],
        )
        self.assertFalse(
            set(reference["allowed_iteration_event_ids"])
            & set(previous["evidence_event_ids"])
        )
        self.assertNotIn("reason", reference)
        self.assertEqual(report["summary"]["model_calls"], 2)
        review = report["trials"][0]["evidence"]["loop_review"]
        self.assertEqual(review["count"], 1)
        self.assertNotIn("validation_error", review)

    # 回归用例：test_loop_diagnostics_persist_without_retry_and_do_not_survive_recovery。
    # 重点核验字段或诊断：loops_future_event、evidence_event_index、invalid_loop_assessment；异常路径不能被当成成功。
    def test_loop_diagnostics_persist_without_retry_and_do_not_survive_recovery(self):
        self.model()
        self.mode("loop_trace")
        self.config["judge"].update(
            loop_review_prompt="prompts/task-loop-review.txt", max_repair_attempts=0
        )
        Provider.behavior = "loops_future_event"
        report = self.evaluate(code=3, name="no-retry")
        review = report["trials"][0]["evidence"]["loop_review"]
        self.assertEqual(review["status"], "not_reviewed")
        self.assertIsNone(review["count"])
        self.assertEqual(
            review["validation_error"]["reference"]["evidence_event_index"], 1
        )
        self.assertEqual(report["summary"]["model_calls"], 1)
        self.assertEqual(
            report["summary"]["invalid_loop_assessment"]["observations"], 0
        )
        self.config["judge"]["max_repair_attempts"] = 1
        Provider.behavior = "loops_repair"
        report = self.evaluate(name="recovered")
        self.assertNotIn(
            "validation_error", report["trials"][0]["evidence"]["loop_review"]
        )
        self.assertEqual(report["summary"]["model_calls"], 2)

    # 回归用例：test_model_budget_does_not_fake_success。
    # 重点核验字段或诊断：max_model_calls；异常路径不能被当成成功。
    def test_model_budget_does_not_fake_success(self):
        self.model()
        self.config["repetitions"] = 2
        self.config["judge"]["max_model_calls"] = 1
        report = self.evaluate(code=3)
        self.assertEqual(report["summary"]["passed"], 1)
        self.assertEqual(report["summary"]["errors"], 1)

    # 回归用例：test_no_overwrite_before_execution_or_model。
    # 关键断言：self.assertEqual(len(REQUESTS), self.start_requests)；此处只验证测试构造的场景。
    def test_no_overwrite_before_execution_or_model(self):
        self.evaluate()
        self.model()
        self.evaluate(code=2)
        self.assertEqual(len(REQUESTS), self.start_requests)

    # 回归用例：test_tampered_dataset_refused。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_tampered_dataset_refused(self):
        with (self.dataset / "train.jsonl").open("a") as stream:
            stream.write("\n")
        self.evaluate(code=2)

    # 回归用例：test_cancellation_records_trials。
    # 重点核验字段或诊断：not_started_trials；异常路径不能被当成成功。
    def test_cancellation_records_trials(self):
        slow = json.loads(json.dumps(self.config["variants"][0]))
        slow["id"] = "slow"
        slow["command"][-1] = "sleep"
        self.config["variants"].append(slow)
        self.config["parallelism"] = 1
        self.config["repetitions"] = 3
        self.config["timeout_ms"] = 5000
        write_json(self.root / "evaluation.json", self.config)
        process = subprocess.Popen(
            [
                str(CLI),
                "dataset",
                "run",
                str(self.dataset),
                "--config",
                str(self.root / "evaluation.json"),
                "--output-root",
                str(self.root / "cancel"),
            ],
            env=self.env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            for _ in range(200):
                finished = list((self.root / "cancel/trials").glob("*.json"))
                if finished:
                    break
                time.sleep(0.01)
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
        self.assertEqual(
            report["summary"]["not_started_trials"], 6 - len(report["trials"])
        )
        self.assertEqual(finished[0].read_bytes(), original)
        self.assertEqual(
            len(list((self.root / "cancel/trials").glob("*.json"))),
            len(report["trials"]),
        )
        self.assertFalse(
            json.loads((self.root / "cancel/manifest.json").read_text())["complete"]
        )


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(TrialTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    write_json(
        WORK / "test-summary.json",
        dict(
            tests=result.testsRun,
            failures=len(result.failures),
            errors=len(result.errors),
            fake_model_requests=len(REQUESTS),
            paid_model_requests=0,
        ),
    )
    SERVER.shutdown()
    print(f"Test output: {WORK}")
    sys.exit(not result.wasSuccessful())
