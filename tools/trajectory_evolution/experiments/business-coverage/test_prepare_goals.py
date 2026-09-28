"""子目标准备回归：全程使用本地桩，不加载真实凭据、不发送 API 请求。"""

from copy import deepcopy
import fcntl
import io
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

import prepare_goals as goals


def task(name="case-a", turns=1):
    """构造带无关敏感载荷的标准输入，验证提取器仅选择验收必要字段。"""
    return {"task_id": name, "task_version": "task-v1:" + name,
            "task_input": "查询天气并保存结果", "followup_inputs": ["补充日期说明"] * (turns - 1),
            "constraints": ["不得捏造查询结果"],
            "attributes": {"turn_expectations": [
                {"turn": n, "expected_behavior": "查询天气并保存结果", "expected_targets": ["private-skill"]}
                for n in range(1, turns + 1)]},
            "initial_state": {"private": "PRIVATE_INITIAL"}, "memory": "PRIVATE_MEMORY",
            "output": "PRIVATE_AGENT_OUTPUT", "policy": "PRIVATE_POLICY"}


def proposal(source):
    """保留显式原目标；其他轮生成两个独立的普通目标。"""
    rows = []
    for turn in source["turns"]:
        criteria = deepcopy(turn.get("criteria"))
        if criteria is None:
            criteria = [{"id": f"turn-{turn['turn']}-{i}", "expected_behavior": text,
                         "due_turn": turn["turn"], "weight": 1, "critical": False,
                         "conditional": False}
                        for i, text in enumerate(("查询天气", "保存查询结果"), 1)]
        rows.append({"turn": turn["turn"], "criteria": criteria})
    return {"turn_expectations": rows}


def accept(stage, payload, **kwargs):
    """模型桩：分开响应提取与复核，测试不会进入真实传输函数。"""
    return proposal(payload["source"]) if stage == "extract" else {"approved": True, "issues": []}


class PreparationTests(unittest.TestCase):
    """覆盖契约保护、恢复幂等、预算、互斥和错误脱敏。"""

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.output = Path(self.folder.name) / "gepa-goals.json"

    def prepare(self, rows=None, **options):
        """统一使用测试桩；调用方可替换桩验证故障和修复分支。"""
        options.setdefault("request", accept)
        return goals.prepare(rows or [task()], source_version="dataset-v1:test",
                             output=self.output, **options)

    def test_prepare_minimal_input_and_exact_output_contract(self):
        calls = []
        original = task(turns=2)
        before = deepcopy(original)

        def capture(stage, payload, **kwargs):
            calls.append((stage, deepcopy(payload), kwargs))
            return accept(stage, payload, **kwargs)

        result = self.prepare([original], request=capture, environment={"OPENAI_API_KEY": "FAKE_TEST_KEY"})
        self.assertEqual([c[0] for c in calls], ["extract", "review"])
        self.assertEqual(set(result), {"schema_version", "source_version", "tasks"})
        self.assertEqual(result["source_version"], "dataset-v1:test")
        self.assertEqual(result["tasks"][0]["task_version"], original["task_version"])
        self.assertEqual(original, before)
        payload = json.dumps([c[1] for c in calls])
        for excluded in ("PRIVATE_INITIAL", "PRIVATE_MEMORY", "PRIVATE_AGENT_OUTPUT", "PRIVATE_POLICY", "private-skill"):
            self.assertNotIn(excluded, payload)
        self.assertEqual(calls[0][2]["model"]["thinking"], "disabled")
        self.assertEqual(calls[0][2]["timeout"], 120)
        for path in (self.output, self.output.with_name("gepa-goals.state.json")):
            self.assertNotIn("FAKE_TEST_KEY", path.read_text())

    def test_existing_criteria_preserved_without_model(self):
        row = task(turns=2)
        for n, turn in enumerate(row["attributes"]["turn_expectations"], 1):
            turn["criteria"] = [{"id": f"original-{n}", "expected_behavior": "保留原文", "weight": 2}]
        result = self.prepare([row], enabled=False, request=lambda *a, **k: self.fail("不应请求模型"))
        self.assertEqual(result["tasks"][0]["turn_expectations"],
                         [{"turn": t["turn"], "criteria": t["criteria"]} for t in row["attributes"]["turn_expectations"]])
        with self.assertRaisesRegex(goals.PreparationError, "criteria_missing_model_disabled"):
            self.prepare([task("missing")], enabled=False)

    def test_rejected_review_repairs_only_within_budget(self):
        calls = []

        def retry(stage, payload, **kwargs):
            calls.append(stage)
            if stage == "review" and calls.count("review") == 1:
                return {"approved": False, "issues": ["missing_requirement"]}
            if stage == "extract" and calls.count("extract") == 2:
                self.assertEqual(payload["correction"]["issues"], ["missing_requirement"])
            return accept(stage, payload, **kwargs)

        self.prepare(request=retry)
        self.assertEqual(calls, ["extract", "review", "extract", "review"])

    def test_partial_success_resumes_without_resending_and_key_rotation_is_not_a_revision(self):
        calls = []

        def first(stage, payload, **kwargs):
            name = payload["source"]["task_id"]
            calls.append((stage, name))
            if name == "case-b":
                raise RuntimeError("SECRET_PROVIDER_BODY")
            return accept(stage, payload, **kwargs)

        with self.assertRaisesRegex(goals.PreparationError, "^goal_preparation_incomplete$"):
            self.prepare([task(), task("case-b")], request=first,
                         environment={"OPENAI_API_KEY": "FAKE_FIRST_KEY"})
        self.assertEqual([t["task_id"] for t in json.loads(self.output.read_text())["tasks"]], ["case-a"])
        calls.clear()

        def resumed(stage, payload, **kwargs):
            calls.append((stage, payload["source"]["task_id"]))
            return accept(stage, payload, **kwargs)

        self.prepare([task(), task("case-b")], request=resumed,
                     environment={"OPENAI_API_KEY": "FAKE_SECOND_KEY"})
        self.assertEqual(calls, [("extract", "case-b"), ("review", "case-b")])
        self.assertNotIn("SECRET_PROVIDER_BODY", self.output.read_text())

    def test_model_and_task_changes_invalidate_cache(self):
        self.prepare(model={"model_id": "model-a"})
        calls = []

        def capture(stage, payload, **kwargs):
            calls.append(stage)
            return accept(stage, payload, **kwargs)

        self.prepare(request=capture, model={"model_id": "model-b"})
        self.assertEqual(len(calls), 2)
        calls.clear()
        row = task()
        row["task_input"] = "查询天气并保存结果，注明日期"
        self.prepare([row], request=capture, model={"model_id": "model-b"})
        self.assertEqual(len(calls), 2)

    def test_invalid_contracts_fail_closed(self):
        source = goals._source_task(task(turns=2))
        for mutation in (
                lambda p: p.update(unknown=True),
                lambda p: p["turn_expectations"][0]["criteria"][0].update(weight=True),
                lambda p: p["turn_expectations"][0]["criteria"][0].update(weight=float("nan")),
                lambda p: p["turn_expectations"][0]["criteria"][0].update(due_turn=3),
                lambda p: p["turn_expectations"][0]["criteria"][0].update(due_turn=True),
                lambda p: p["turn_expectations"][1]["criteria"][0].update(id="turn-1-1"),
                lambda p: p["turn_expectations"][0]["criteria"][0].update(id="constraint-1"),
                lambda p: p["turn_expectations"][0]["criteria"][0].update(critical=True)):
            candidate = proposal(source)
            mutation(candidate)
            with self.assertRaises(goals.PreparationError):
                goals._validate(candidate, source)

    def test_existing_ids_global_uniqueness_and_partial_preservation(self):
        row = task(turns=2)
        row["attributes"]["turn_expectations"][0]["criteria"] = [
            {"id": "existing", "expected_behavior": "原始限定要求", "due_turn": 2}]
        source = goals._source_task(row)
        candidate = proposal(source)
        candidate["turn_expectations"][0]["criteria"][0]["expected_behavior"] = "被改写"
        with self.assertRaisesRegex(goals.PreparationError, "existing_criteria_changed"):
            goals._validate(candidate, source)
        self.prepare([row])
        row["attributes"]["turn_expectations"][1]["criteria"] = [
            {"id": "existing", "expected_behavior": "重复编号"}]
        with self.assertRaisesRegex(goals.PreparationError, "criterion_identity_invalid"):
            self.prepare([row], enabled=False)

    def test_false_review_never_becomes_success_and_secret_reflection_not_persisted(self):
        calls = []

        def reject(stage, payload, **kwargs):
            calls.append(stage)
            return proposal(payload["source"]) if stage == "extract" else {
                "approved": False, "issues": ["added_requirement"]}

        with self.assertRaisesRegex(goals.PreparationError, "goal_preparation_incomplete"):
            self.prepare(request=reject, max_repairs=1)
        self.assertEqual(len(calls), 4)
        self.assertFalse(json.loads(self.output.read_text())["tasks"])

        def reflect(stage, payload, **kwargs):
            value = proposal(payload["source"])
            value["turn_expectations"][0]["criteria"][0]["expected_behavior"] = "FAKE_REFLECTED_KEY"
            return value

        with self.assertRaises(goals.PreparationError):
            self.prepare(request=reflect, environment={"OPENAI_API_KEY": "FAKE_REFLECTED_KEY"})
        self.assertNotIn("FAKE_REFLECTED_KEY", self.output.read_text())

    def test_concurrency_limit_and_process_lock(self):
        mutex = threading.Lock()
        active, maximum = 0, 0

        def concurrent(stage, payload, **kwargs):
            nonlocal active, maximum
            with mutex:
                active += 1
                maximum = max(maximum, active)
            time.sleep(0.005)
            with mutex:
                active -= 1
            return accept(stage, payload, **kwargs)

        self.prepare([task(f"case-{n}") for n in range(8)], request=concurrent, concurrency=3)
        self.assertGreater(maximum, 1)
        self.assertLessEqual(maximum, 3)
        with self.output.with_name(self.output.name + ".lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaisesRegex(goals.PreparationError, "goal_preparation_busy"):
                self.prepare()
        for options in ({"concurrency": 17}, {"timeout": 121}, {"max_repairs": 3}):
            with self.assertRaises(goals.PreparationError):
                self.prepare(**options)

    def test_http_adapter_is_stubbed_and_disables_thinking(self):
        class Reply:
            status = 200
            fp = None

            def __init__(self):
                self.body = io.BytesIO(json.dumps({"choices": [{"finish_reason": "stop",
                    "message": {"content": '{"approved":true,"issues":[]}'}}]}).encode())

            def read1(self, size):
                return self.body.read(size)

        class Connection:
            sock = None
            body = None

            def __init__(self, *args, **kwargs):
                pass

            def request(self, method, path, body, headers):
                Connection.body = json.loads(body)

            def getresponse(self):
                return Reply()

            def close(self):
                pass

        with patch.object(goals.http.client, "HTTPSConnection", Connection):
            result = goals.request_model("review", {}, environment={"KEY": "FAKE"},
                model={"endpoint": "https://example.invalid/chat/completions", "model_id": "test",
                       "max_output_tokens": 8192}, timeout=120, prompt="测试", api_key_env="KEY")
        self.assertTrue(result["approved"])
        self.assertEqual(Connection.body["thinking"], {"type": "disabled"})


if __name__ == "__main__":
    unittest.main()
