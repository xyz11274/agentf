"""Reflective search uses real collector/host trials and a local model fixture."""
import http.server
import json
import os
from pathlib import Path
import subprocess
import shutil
import threading
import time
import unittest

import test_agent_evaluation as fixture


class GepaSearchTests(unittest.TestCase):
    start = fixture.AgentEvaluationTests.start
    call = fixture.AgentEvaluationTests.call
    add_memory = fixture.AgentEvaluationTests.add_memory

    def setUp(self):
        fixture.AgentEvaluationTests.setUp(self)
        self.add_memory()
        self.requests = []
        self.release = threading.Event()
        self.release.set()
        self.behavior = "normal"
        self.fenced = False
        self.pause_on = None
        owner = self

        class Model(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                packet = json.loads(request["messages"][-1]["content"])
                if "loop_inventory" in packet:
                    owner.judge_requests.append(packet)
                    if getattr(owner, "fail_search_judge", False) and packet["task_input"].startswith(
                            getattr(owner, "fail_search_prefix", "")) and any(
                            (owner.root / "production/agent-evaluation").glob("*/search/tree.json")) \
                            and getattr(owner, "search_judge_failures", 1):
                        if hasattr(owner, "search_judge_failures"):
                            owner.search_judge_failures -= 1
                        self.send_response(getattr(owner, "search_judge_status", 503))
                        self.send_header("Content-Length", "0"); self.end_headers()
                        return
                    inventory = packet["loop_inventory"]
                    key = next(iter(inventory), None)
                    judgment = dict(verdict="passed", reason="测试评分器确认任务输出。", evidence_paths=["/output"],
                        loop_review=dict(status="complete" if key else "uncertain", wasted_iterations=[dict(iteration_id=key,
                            reason="测试中将本轮标为可省去，用于核验评分口径。", evidence_event_ids=[inventory[key]["event_ids"][0]])] if key else []))
                    if getattr(owner, "seed_fault", None) and packet["task_input"].startswith("VALIDATION_PRIVATE") \
                            and any((owner.root / "production/agent-evaluation").glob("*/search/tree.json")):
                        owner.seed_judge_tasks.append(packet["task_input"])
                        if "乙类" in packet["task_input"]:
                            judgment["verdict"] = "failed"
                        elif owner.seed_failures and owner.seed_fault != "tokens":
                            owner.seed_failures -= 1
                            judgment["verdict"] = "uncertain"
                    if getattr(owner, "cache_fault", None) and packet["task_input"].startswith("TRAIN_VISIBLE") \
                            and any((owner.root / "production/agent-evaluation").glob("*/search/tree.json")):
                        owner.cache_train_calls.append(packet["task_input"])
                        if len(owner.cache_train_calls) == 1:
                            if owner.cache_fault == "invalid":
                                judgment["loop_review"]["wasted_iterations"][0]["iteration_id"] = "unknown-iteration"
                            else:
                                judgment["loop_review"] = dict(status="uncertain", wasted_iterations=[])
                        else:
                            if not owner.cache_failed_task:
                                owner.cache_failed_task = packet["task_input"]
                            if packet["task_input"] == owner.cache_failed_task:
                                judgment["verdict"] = "failed"
                    response = json.dumps(dict(choices=[dict(message=dict(content=json.dumps(judgment)))])).encode()
                    self.send_response(200); self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(response))); self.end_headers(); self.wfile.write(response)
                    return
                owner.requests.append(packet)
                if getattr(owner, "fail_reflection_rate_limit", False):
                    self.send_response(429)
                    self.send_header("Content-Length", "0"); self.end_headers()
                    return
                level = packet.get("controls", {}).get("level", "L1")
                ordinal = sum(p.get("controls", {}).get("level", "L1") == level for p in owner.requests)
                if owner.pause_on is None or owner.pause_on == (level, ordinal):
                    owner.release.wait(30)
                if owner.behavior.startswith("combee"):
                    component = packet["selected_components"][0]
                    if component == "action_rules":
                        draft = dict(reason="汇总工具前置核验的训练证据。",
                            controls=dict(action_rules=[dict(memory_id="test-experience", operation="calculate",
                                required_arguments=["verified"], reason="缺少核验参数时返回可恢复的拒绝。")]))
                    else:
                        if packet.get("phase") == "combee_reduce":
                            text = "缺少素材时核验甲类任务，并核验乙类任务，独立计算继续。"
                        else:
                            text = "缺少素材时先核验甲类任务。" if ordinal % 2 else "缺少素材时先核验乙类任务。"
                        if owner.behavior == "combee_leak":
                            text += owner.env["GEPA_LEAK_SECRET"]
                        draft = dict(reason="根据分组训练证据修复核验步骤。",
                            experience=dict(memory_id="test-experience", content=text))
                    content = json.dumps(draft, ensure_ascii=False)
                elif owner.behavior == "merge":
                    level = packet.get("controls", {}).get("level", "L1")
                    n = sum(p.get("controls", {}).get("level", "L1") == level for p in owner.requests)
                    if n == 1 or level == "L1":
                        text = "缺少素材时先核验甲类任务。" if n == 1 else "缺少素材时先核验乙类任务。"
                        content = json.dumps(dict(reason="根据训练任务的反馈，保留一种独立改进。",
                            experience=dict(memory_id="test-experience", content=text)), ensure_ascii=False)
                    else:
                        content = json.dumps(dict(reason="对另一类任务补充工具调用前置核验。",
                            controls=dict(action_rules=[dict(memory_id="test-experience", operation="calculate",
                                required_arguments=["verified"], reason="缺少核验参数时返回可恢复的拒绝。")])), ensure_ascii=False)
                elif owner.behavior in ("actions", "loops"):
                    controls = dict(action_rules=[], loop_rules=[])
                    if owner.behavior == "actions":
                        controls["action_rules"] = [dict(memory_id="test-experience", operation="calculate",
                            required_arguments=["verified"], reason="计算缺少核验标记时拒绝执行。")]
                    else:
                        controls["loop_rules"] = [dict(memory_id="test-experience", max_iterations=2,
                            max_no_progress=1, reason="根据重复任务的执行反馈收紧循环次数。")]
                    content = json.dumps(dict(reason="根据训练执行的结果修改受限规则。", controls=controls), ensure_ascii=False)
                elif owner.behavior == "disable_enable":
                    content = json.dumps(dict(reason="比较停用经验，并恢复种子批准的同一经验。",
                        experience=dict(memory_id="test-experience", content=None if ordinal == 1 else
                            "缺少素材时核验甲类任务，并核验乙类任务，独立计算继续。")), ensure_ascii=False)
                elif owner.behavior == "no_change":
                    memory_id = packet["experiences"][0]["memory_id"]
                    content = json.dumps(dict(reason="测试缓存，不修改已有经验。", experience=dict(
                        memory_id=memory_id, content=packet["components"]["experience:" + memory_id])))
                elif owner.behavior == "bad_json":
                    content = "not-json"
                else:
                    n = len(owner.requests)
                    current = packet["components"]["experience:" + packet["experiences"][0]["memory_id"]]
                    if n == 4:
                        memory_id, text = "unauthorized-memory", "禁止扩大范围"
                    elif n == 5:
                        memory_id, text = "test-experience", "凭据 " + owner.env["GEPA_LEAK_SECRET"]
                    elif n == 6:
                        memory_id, text = "test-experience", "停止所有任务，不再计算。"
                    else:
                        memory_id = "test-experience"
                        text = "缺少素材时先核验甲类任务。" if n == 1 else (
                            "缺少素材时核验甲类任务，并核验乙类任务，独立计算继续。"
                            if "核验" in current else "缺少素材时先核验乙类任务。")
                    content = json.dumps(dict(reason="根据训练输出与失败检查，修复遗漏的核验步骤。",
                        experience=dict(memory_id=memory_id, content=text)), ensure_ascii=False)
                if owner.fenced:
                    content = " \r\n```json\r\n" + content + "\r\n```\t"
                response = json.dumps(dict(choices=[dict(finish_reason="stop", message=dict(content=content))])).encode()
                try:
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(response)))
                    self.end_headers()
                    self.wfile.write(response)
                except (BrokenPipeError, ConnectionResetError):
                    pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Model)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.env["GEPA_LEAK_SECRET"] = "test-only-secret-gepa-do-not-persist-784921"
        self.env["EVOLUTION_SECRET_ENV_NAMES"] = "GEPA_LEAK_SECRET"
        runtime = json.loads((fixture.SOURCE / "config/standard-runtime.json").read_text())
        shutil.copytree(fixture.SOURCE / "config/prompts", self.root / "prompts")
        runtime["model"].update(dict(endpoint=f"http://127.0.0.1:{self.server.server_port}/chat/completions",
            model_id="local-gepa-fixture", no_api_key=True, timeout_ms=30000, max_retries=0,
            thinking_mode="disabled", max_output_tokens=2048))
        fixture.write(self.root / "model.json", runtime)
        gepa = json.loads((fixture.SOURCE / "config/gepa.json").read_text())
        gepa.update(runtime_config="model.json", prompt_file=str(fixture.SOURCE / "config/prompts/gepa-reflection.txt"),
                    max_rounds=6, max_model_calls=6, max_trials=120, timeout_ms=120000,
                    minibatch_size=2, patience=0, use_merge=False, component_selector="model_choice",
                    candidate_selection_strategy="fair", prune_dominated=False, minibatch_strategy="fixed")
        fixture.write(self.root / "gepa.json", gepa)
        tasks = []
        for split, label in [("train", "TRAIN_VISIBLE"), ("val", "VALIDATION_PRIVATE"), ("holdout", "HOLDOUT_PRIVATE")]:
            for family in ("甲类", "乙类"):
                task_id = split + ("-a" if family == "甲类" else "-b")
                tasks.append(dict(task_id=task_id, group_id=task_id,
                    task_input=f"{label} {family}：生成视频时缺少图片素材，仍需完成独立计算。",
                    expected_behavior=f"{label} 对缺少素材的独立计算返回20。", initial_state={},
                    difficulty="easy", category=family, split=split, review_status="reviewed"))
        (self.root / "gepa-tasks.jsonl").write_text("\n".join(json.dumps(task, ensure_ascii=False) for task in tasks), encoding="utf-8")
        dataset = json.loads((fixture.SOURCE / "config/dataset.json").read_text())
        dataset["sources"] = [dict(kind="golden", path=str(self.root / "gepa-tasks.jsonl"))]
        fixture.write(self.root / "gepa-dataset.json", dataset)
        result = subprocess.run([str(fixture.CLI), "dataset", "build", "--config", str(self.root / "gepa-dataset.json"),
            "--output-root", str(self.root / "gepa-dataset")], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assessment = json.loads((self.root / "agent-evaluation.json").read_text())
        self.assessment.update(dataset="gepa-dataset", gepa_config="gepa.json")
        fixture.write(self.root / "agent-evaluation.json", self.assessment)
        self.evaluation["variants"][0]["command"][-2] = "gepa"
        self.evaluation["repetitions"] = 1
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.config.update(max_level="L1", auto_approve_experiences=True)
        fixture.write(self.root / "service.json", self.config)

    def tearDown(self):
        self.release.set()
        fixture.AgentEvaluationTests.tearDown(self)
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)

    def wait_search(self, level="L1"):
        deadline = time.monotonic() + 70
        status = {}
        while time.monotonic() < deadline:
            status = self.call("agent_evaluation.status")
            jobs = [(key, value) for key, value in status["jobs"].items() if value.get("gepa") and value["level"] == level]
            if jobs and all(job[1]["status"] not in ("pending", "running") for job in jobs):
                return jobs[0][0], jobs[0][1], status
            time.sleep(.05)
        self.fail("search did not finish: " + json.dumps(status))

    def test_reflection_tree_holdout_publication_and_restart(self):
        self.start()
        job_id, job, status = self.wait_search()
        self.assertEqual(job["status"], "passed", job)
        self.assertTrue(job["published"], job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(int(tree["model_calls"]), 6)
        self.assertEqual(len(tree["nodes"]), 7)
        self.assertGreaterEqual(max(node["depth"] for node in tree["nodes"]), 2)
        states = {node["status"] for node in tree["nodes"]}
        self.assertIn("error", states)
        self.assertIn("rejected_minibatch", states)
        self.assertLessEqual(tree["trials_used"], 120)
        self.assertNotEqual(tree["winner"], 0)
        for packet in self.requests:
            body = json.dumps(packet)
            self.assertNotIn("HOLDOUT_PRIVATE", body)
            self.assertNotIn("VALIDATION_PRIVATE", body)
            self.assertIn("TRAIN_VISIBLE", body)
            self.assertTrue(packet["feedback"])
        root = self.root / "production/agent-evaluation" / job_id
        self.assertTrue((root / "search/published-version.json").is_file())
        selected = json.loads((root / "candidate.json").read_text())
        seed = json.loads((root / "seed.json").read_text())
        self.assertEqual(selected["policy"]["snapshot"], seed["policy"]["snapshot"])
        self.assertTrue(selected["policy"]["options"]["experience_overrides"])
        for path in (self.root / "production").rglob("*"):
            if path.is_file():
                self.assertNotIn(self.env["GEPA_LEAK_SECRET"].encode(), path.read_bytes(), str(path))
        self.assertFalse(self.call("agent_evaluation.tree", token=fixture.TOKEN_B, job_id=job_id)["ok"])
        count = len(self.requests)
        fixture.AgentEvaluationTests.tearDown(self)
        self.start()
        _, restored, status = self.wait_search()
        self.assertEqual(restored["version"], job["version"])
        self.assertEqual(len(self.requests), count)
        self.assertEqual(len([value for value in status["jobs"].values() if value.get("gepa")]), 1)
        fixture.write(self.root / "test-summary.json", dict(job_id=job_id, search=job["search"],
            states=sorted(states), published=True, privacy="passed", restart="no duplicate search"))
        print("GEPA artifact:", self.root / "test-summary.json")

    def test_json_fence_preserves_search_publication_and_privacy_gates(self):
        self.fenced = True
        self.test_reflection_tree_holdout_publication_and_restart()

    def test_disabled_experience_remains_available_for_later_generations(self):
        self.behavior = "disable_enable"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=2, max_model_calls=2, acceptance="improvement_or_equal")
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search()
        root = self.root / "production/agent-evaluation" / job_id
        seed = json.loads((root / "seed.json").read_text())
        disabled = json.loads((root / "search/versions/1.json").read_text())
        self.assertFalse(disabled["policy"]["options"].get("approved_experiences"))
        self.assertFalse(disabled["policy"]["options"].get("experience_overrides"))
        self.assertEqual(disabled["policy"]["snapshot"], seed["policy"]["snapshot"])
        self.assertIsNone(self.requests[1]["components"]["experience:test-experience"])
        self.assertEqual(self.requests[1]["experiences"][0]["memory_id"], "test-experience")
        restored = json.loads((root / "search/versions/2.json").read_text())
        self.assertEqual(restored["policy"]["options"]["approved_experiences"],
                         seed["policy"]["options"]["approved_experiences"])
        self.assertEqual(restored["policy"]["snapshot"], seed["policy"]["snapshot"])
        self.assertEqual(job["status"], "passed", job)

    def test_search_uses_reviewed_loops_instead_of_the_zero_rule_bound(self):
        self.judge_requests = []
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.evaluation["checks"] = {}
        self.evaluation["variants"][0]["command"][-2] = "gepa_review"
        self.evaluation["judge"] = dict(enabled=True, runtime_config="model.json",
            prompt="prompts/task-judge-system.txt", loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100, max_input_bytes=1048576)
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        job_id, job, _ = self.wait_search()
        self.assertEqual(job["status"], "passed", job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        for score in tree["nodes"][0]["validation"].values():
            self.assertEqual(score["loop_efficiency"], .5)  # Model count=1, rule lower bound=0.
        self.assertTrue(self.judge_requests)
        self.assertEqual(job["gate"]["candidate"]["invalid_loop_assessment"]["method"], "model_review")

    def test_holdout_rejection_does_not_publish_or_reflect_again(self):
        self.evaluation["variants"][0]["command"][-2] = "gepa_holdout_failure"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        job_id, job, status = self.wait_search()
        self.assertEqual(job["status"], "rejected", job)
        self.assertFalse(job.get("published", False))
        self.assertNotIn("L1", status["publications"])
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertTrue(tree["holdout_started"])
        self.assertEqual(len(self.requests), 6)
        self.assertFalse((self.root / "production/agent-evaluation" / job_id / "search/published-version.json").exists())

    def test_cancel_model_call_keeps_the_main_service_responsive(self):
        self.release.clear()
        self.start()
        deadline = time.monotonic() + 20
        while not self.requests and time.monotonic() < deadline:
            time.sleep(.05)
        self.assertTrue(self.requests)
        before = time.monotonic()
        status = self.call("agent_evaluation.status")
        self.assertLess(time.monotonic() - before, 1)
        job_id = next(key for key, value in status["jobs"].items() if value.get("gepa"))
        self.call("agent_evaluation.cancel", job_id=job_id)
        _, job, _ = self.wait_search()
        self.assertEqual(job["status"], "error")
        self.assertFalse(job.get("published", False))
        self.assertLess(time.monotonic() - before, 5)

    def test_interrupted_search_resumes_without_replaying_finished_trials(self):
        self.release.clear()
        self.start()
        deadline = time.monotonic() + 20
        while not self.requests and time.monotonic() < deadline:
            time.sleep(.05)
        self.assertTrue(self.requests)
        status = self.call("agent_evaluation.status")
        job_id = next(key for key, value in status["jobs"].items() if value.get("gepa"))
        before = self.call("agent_evaluation.tree", job_id=job_id)
        cached = {key for key, value in before["evaluations"].items() if value["status"] == "complete"}
        self.process.kill()
        self.process.communicate(timeout=5)
        self.release.set()
        self.start()
        _, job, _ = self.wait_search()
        self.assertIn(job["status"], ("passed", "rejected"), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(int(job["attempt"]), 2)
        self.assertEqual(tree["nodes"][1]["status"], "interrupted")
        self.assertEqual(int(tree["model_calls"]), 6)
        for key in cached:
            self.assertEqual(tree["evaluations"][key]["attempt"], before["evaluations"][key]["attempt"])

    def test_published_bundle_runs_offline_without_original_memory(self):
        self.start()
        job_id, job, _ = self.wait_search()
        self.assertEqual(job["status"], "passed", job)
        fixture.AgentEvaluationTests.tearDown(self)
        count = len(self.requests)
        self.config.pop("policy_evaluation_file")
        self.config["policy_version_file"] = str(self.root / "production/agent-evaluation" / job_id / "search/published-version.json")
        self.config["data_dir"] = str(self.root / "offline")
        fixture.write(self.root / "service.json", self.config)
        self.start()
        run = self.call("run.begin", input="生成视频时缺少图片素材，核对甲类和乙类任务。", level="L1", capabilities=["model"])
        self.assertEqual(run.get("agent_version_id"), job["version"], run)
        activity = self.call("activity.begin", run_id=run["run_id"], kind="model", name="offline-model",
            input={"messages": [{"role": "user", "content": "生成视频时缺少图片素材，核对甲类和乙类任务。"}]},
            binding={"pointer": "/messages", "mode": "system_message"})
        self.assertIn("核验甲类", json.dumps(activity["input"], ensure_ascii=False))
        self.assertIn("核验乙类", json.dumps(activity["input"], ensure_ascii=False))
        self.call("activity.end", run_id=run["run_id"], activity_id=activity["activity_id"],
            status="completed", output="完成", actual_input=activity["input"], executed=True, metrics={"tokens": 100})
        self.call("run.end", run_id=run["run_id"], status="completed", output="完成")
        other = self.call("run.begin", token=fixture.TOKEN_B, input="生成视频时缺少图片素材。", level="L1", capabilities=["model"])
        self.assertNotIn("agent_version_id", other)
        self.call("run.end", token=fixture.TOKEN_B, run_id=other["run_id"], status="completed", output="未授权其他用户的版本")
        self.assertEqual(len(self.requests), count)
        fixture.AgentEvaluationTests.tearDown(self)
        traces = list((self.root / "offline/by-user/user-a").rglob("*.trace.json"))
        self.assertEqual(len(traces), 1)
        self.assertNotIn("evaluation_trial_id", traces[0].read_text())

    def test_trial_budget_reserves_final_holdout(self):
        config = json.loads((self.root / "gepa.json").read_text())
        config["max_trials"] = 12
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search()
        self.assertEqual(job["status"], "rejected", job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertLessEqual(tree["trials_used"], 12)
        self.assertTrue(tree["holdout_started"])
        self.assertEqual(tree["stop_reason"], "gepa_trial_budget_exhausted")
        self.assertTrue(any(value["split"] == "holdout" for value in tree["evaluations"].values()))

    def exercise_controls(self, kind, level):
        self.behavior = kind
        self.evaluation["variants"][0]["command"][-2] = "gepa_" + kind
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.config["max_level"] = level
        self.config["control_learning"] = dict(enabled=True, prompt_file="prompts/gepa-reflection.txt",
            actions={"calculate": ["verified"]}, max_rules=2, min_iterations=2, min_no_progress=1)
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search(level)
        self.assertEqual(job["status"], "passed", job)
        candidate = json.loads((self.root / "production/agent-evaluation" / job_id / "candidate.json").read_text())
        rules = candidate["policy"]["options"]["action_rules" if kind == "actions" else "loop_rules"]
        generated = [rule for rule in rules if "source_experience" in rule]
        self.assertEqual(len(generated), 1)
        self.assertEqual(generated[0]["source_experience"]["memory_id"], "test-experience")
        return generated[0]

    def test_l2_generated_action_rules_reach_actual_host(self):
        rule = self.exercise_controls("actions", "L2")
        self.assertEqual(rule["required_arguments"], ["verified"])

    def test_l2_search_reports_real_control_space_without_duplicate_memories(self):
        self.exercise_controls("actions", "L2")
        job_id, job, _ = self.wait_search("L2")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertIn("action_rules", tree["available_components"])
        self.assertEqual(tree["search_warnings"], [])
        self.assertEqual(job["search"]["available_components"], tree["available_components"])
        packet = next(p for p in self.requests if p.get("controls", {}).get("level") == "L2")
        self.assertNotIn("memories", packet["controls"])
        self.assertEqual(packet["controls"]["limits"]["actions"], {"calculate": ["verified"]})

    def test_l2_without_control_authorization_reports_experience_only_search(self):
        self.config["max_level"] = "L2"
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertNotIn("action_rules", tree["available_components"])
        self.assertEqual(tree["search_warnings"], [])
        self.assertEqual(tree["fixed_controls"], ["observed_tool_required_arguments"])
        self.assertEqual(job["search"]["search_warnings"], tree["search_warnings"])

    def test_rewritten_experiences_keep_original_constraints_in_reflection(self):
        self.start()
        self.wait_search()
        originals = [item["original_content"] for packet in self.requests
                     for item in packet["experiences"] if "original_content" in item]
        self.assertTrue(originals)
        self.assertEqual(len(set(originals)), 1)
        for packet in self.requests:
            for item in packet["experiences"]:
                self.assertIn("condition", item)
                self.assertIn("scope", item)
                self.assertNotIn("content", item)

    def test_l2_enabled_without_operations_warns_without_crashing(self):
        self.config.update(max_level="L2", control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt"))
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertNotEqual(job["status"], "error", job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertNotIn("action_rules", tree["available_components"])
        self.assertEqual(tree["search_warnings"], [])
        self.assertEqual(job["search"]["fixed_controls"], ["observed_tool_required_arguments"])

    def test_l3_generated_loop_rules_reach_actual_host(self):
        rule = self.exercise_controls("loops", "L3")
        self.assertEqual(int(rule["max_iterations"]), 2)

    def test_l3_loop_search_without_manual_tool_whitelist(self):
        self.behavior = "loops"
        self.evaluation["variants"][0]["command"][-2] = "gepa_loops"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.config.update(max_level="L3", control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2, min_iterations=2, min_no_progress=1))
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L3")
        self.assertEqual(job["status"], "passed", job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertIn("loop_rules", tree["available_components"])
        self.assertNotIn("action_rules", tree["available_components"])
        candidate = json.loads((self.root / "production/agent-evaluation" / job_id / "candidate.json").read_text())
        generated = [rule for rule in candidate["policy"]["options"]["loop_rules"] if "source_experience" in rule]
        self.assertEqual(len(generated), 1)
        self.assertEqual(int(generated[0]["max_iterations"]), 2)

    def configure_merge(self, mode="gepa_merge", **overrides):
        self.behavior = "merge"
        self.evaluation["variants"][0]["command"][-2] = mode
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.config["max_level"] = "L2"
        self.config["control_learning"] = dict(enabled=True, prompt_file="prompts/gepa-reflection.txt",
            actions={"calculate": ["verified"]}, max_rules=2, min_iterations=2, min_no_progress=1)
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(use_merge=True, max_rounds=3, max_model_calls=2, max_merge_invocations=2,
            proposals_per_round=2, component_selector="all", candidate_selection_strategy="pareto",
            prune_dominated=True, minibatch_strategy="epoch_shuffled")
        config.update(overrides)
        fixture.write(self.root / "gepa.json", config)

    def test_two_branches_jointly_generate_one_published_candidate(self):
        self.configure_merge()
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertEqual(job["status"], "passed", job)
        self.assertTrue(job["published"])
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["algorithm"], "gepa-reflective-dag-v2")
        self.assertEqual(tree["model_calls"], 2)
        self.assertEqual(len(tree["nodes"]), 4)
        child = tree["nodes"][3]
        self.assertEqual(child["kind"], "merge")
        self.assertEqual(child["parents"], [1, 2])
        self.assertEqual(child["ancestor"], 0)
        self.assertEqual(child["status"], "accepted")
        self.assertEqual(child["component_sources"]["experience:test-experience"], 1)
        self.assertEqual(child["component_sources"]["action_rules"], 2)
        self.assertEqual(tree["winner"], 3)
        root = self.root / "production/agent-evaluation" / job_id
        version = json.loads((root / "candidate.json").read_text())
        options = version["policy"]["options"]
        self.assertTrue(options["experience_overrides"])
        self.assertTrue(any("source_experience" in rule for rule in options["action_rules"]))
        for packet in self.requests:
            self.assertNotIn("VALIDATION_PRIVATE", json.dumps(packet))
            self.assertNotIn("HOLDOUT_PRIVATE", json.dumps(packet))
        count = len(self.requests)
        fixture.AgentEvaluationTests.tearDown(self)
        self.start()
        _, restored, _ = self.wait_search("L2")
        self.assertEqual(restored["version"], job["version"])
        self.assertEqual(len(self.requests), count)
        self.assertEqual(self.call("agent_evaluation.tree", job_id=job_id)["trials_used"], tree["trials_used"])
        fixture.write(self.root / "merge-summary.json", dict(job_id=job_id, search=job["search"],
            parents=child["parents"], sources=child["component_sources"], published=True, restart="no repeated trials"))
        print("GEPA merge artifact:", self.root / "merge-summary.json")

    def test_merge_regression_is_not_accepted_or_published(self):
        self.configure_merge("gepa_merge_regression")
        self.start()
        job_id, job, _ = self.wait_search("L2")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["nodes"][3]["status"], "rejected_merge")
        self.assertFalse(job.get("published", False))
        self.assertNotEqual(tree["winner"], 3)
        self.assertEqual(tree["model_calls"], 2)

    def test_pareto_screening_keeps_joint_search_and_final_gate(self):
        self.configure_merge(acceptance="pareto", frontier_type="hybrid")
        self.assessment["gate"]["min_invalid_loop_reduction"] = .15
        fixture.write(self.root / "agent-evaluation.json", self.assessment)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["nodes"][3]["kind"], "merge")
        self.assertEqual(tree["nodes"][3]["status"], "accepted")
        # Retaining a useful node is not permission to bypass the publish gate.
        self.assertFalse(job.get("published", False))
        self.assertNotEqual(job["status"], "passed")

    def test_partial_merge_validation_only_executes_each_task_once(self):
        self.configure_merge(merge_minibatch_size=1)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertTrue(job.get("published", False), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["trials_used"], 18)
        assembled = [item for item in tree["evaluations"].values() if item.get("source_reports")]
        self.assertTrue(assembled)
        self.assertTrue(all(item["trials_reserved"] == 0 for item in assembled))
        self.assertTrue(all(key in tree["evaluations"] for item in assembled for key in item["source_reports"]))

    def test_perfect_training_batch_skips_unnecessary_reflection(self):
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=4, max_model_calls=4, skip_perfect_score=True,
            candidate_selection_strategy="current_best", objective=dict(invalid_loop_weight=0, token_weight=0, latency_weight=0))
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertTrue(job.get("published", False), job)
        self.assertEqual(tree["model_calls"], 2)
        self.assertTrue(any(node["status"] == "skipped_perfect" for node in tree["nodes"]))

    def test_interrupted_multi_proposal_batch_keeps_finished_siblings(self):
        self.configure_merge(max_rounds=4, max_model_calls=3)
        self.pause_on = ("L2", 2)
        self.release.clear()
        self.start()
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if sum(p.get("controls", {}).get("level") == "L2" for p in self.requests) >= 2:
                break
            time.sleep(.05)
        self.assertEqual(sum(p.get("controls", {}).get("level") == "L2" for p in self.requests), 2)
        status = self.call("agent_evaluation.status")
        job_id = next(key for key, value in status["jobs"].items() if value.get("gepa") and value["level"] == "L2")
        before = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(before["nodes"][1]["status"], "screened")
        cached = {key: value["attempt"] for key, value in before["evaluations"].items() if value["status"] == "complete"}
        self.process.kill()
        self.process.communicate(timeout=5)
        self.release.set()
        self.start()
        _, job, _ = self.wait_search("L2")
        self.assertTrue(job.get("published", False), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["nodes"][2]["status"], "interrupted")
        self.assertEqual(tree["nodes"][1]["status"], "accepted")
        self.assertEqual(tree["model_calls"], 3)
        for key, attempt in cached.items():
            self.assertEqual(tree["evaluations"][key]["attempt"], attempt)

    def test_interrupted_merge_resumes_with_both_parents(self):
        self.configure_merge("gepa_merge_pause")
        self.env["GEPA_MERGE_READY"] = str(self.root / "merge-ready")
        self.env["GEPA_MERGE_RELEASE"] = str(self.root / "merge-release")
        self.evaluation["variants"][0]["pass_env"] = ["GEPA_MERGE_READY", "GEPA_MERGE_RELEASE"]
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        deadline = time.monotonic() + 35
        while not (self.root / "merge-ready").exists() and time.monotonic() < deadline:
            time.sleep(.05)
        self.assertTrue((self.root / "merge-ready").exists())
        status = self.call("agent_evaluation.status")
        job_id = next(key for key, value in status["jobs"].items() if value.get("gepa") and value["level"] == "L2")
        before = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(before["nodes"][3]["parents"], [1, 2])
        cached = {key: value["attempt"] for key, value in before["evaluations"].items() if value["status"] == "complete"}
        count = len(self.requests)
        self.process.kill()
        self.process.communicate(timeout=5)
        (self.root / "merge-release").write_text("resume", encoding="utf-8")
        self.start()
        _, job, _ = self.wait_search("L2")
        self.assertTrue(job.get("published", False), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["nodes"][3]["parents"], [1, 2])
        self.assertEqual(tree["nodes"][3]["status"], "accepted")
        self.assertEqual(len(self.requests), count)
        self.assertEqual(len(tree["merge_attempts"]), 1)
        for key, attempt in cached.items():
            self.assertEqual(tree["evaluations"][key]["attempt"], attempt)
        self.assertGreater(tree["trials_used"], before["trials_used"])

    def test_best_of_batch_only_validates_the_selected_proposal(self):
        self.configure_merge(proposal_selection="best")
        self.start()
        job_id, _, _ = self.wait_search("L2")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(len(tree["nodes"]), 3)
        self.assertEqual(sum(n["status"] == "not_selected" for n in tree["nodes"]), 1)
        omitted = next(n for n in tree["nodes"] if n["status"] == "not_selected")
        self.assertNotIn("validation", omitted)
        self.assertFalse(tree["merge_attempts"])

    def test_pxn_sampling_and_cartesian_frontier(self):
        self.configure_merge(sampling_strategy="pxn", parents_per_round=2, proposals_per_round=1,
                             frontier_type="cartesian")
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertTrue(job.get("published", False), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["nodes"][3]["parents"], [1, 2])
        self.assertTrue(all(key.startswith("pair:") for key in tree["frontier_mapping"]))
        self.assertEqual(tree["reflection_batches"], 1)
        self.assertEqual(tree["training_batches"], 2)

    def test_independent_sampling_obeys_the_same_total_budget(self):
        self.configure_merge(sampling_strategy="independent", proposals_per_round=2)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertTrue(job.get("published", False), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["model_calls"], 2)
        self.assertEqual(tree["nodes"][3]["parents"], [1, 2])

    def test_quality_target_stops_search_before_call_budget(self):
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(target_fitness=.9, candidate_selection_strategy="current_best",
            objective=dict(invalid_loop_weight=0, token_weight=0, latency_weight=0))
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search()
        self.assertTrue(job.get("published", False), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["stop_reason"], "target_fitness")
        self.assertEqual(tree["model_calls"], 2)
        self.assertTrue(tree["holdout_started"])

    def configure_combee(self, **overrides):
        self.behavior = "combee"
        self.evaluation["repetitions"] = 2
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(reflection_strategy="combee", max_rounds=1, max_model_calls=3,
                      max_feedback_trials=4, component_selector="all")
        config.update(overrides)
        fixture.write(self.root / "gepa.json", config)

    def test_combee_map_reduce_uses_training_evidence_and_counts_every_call(self):
        self.configure_combee()
        self.start()
        job_id, job, _ = self.wait_search()
        self.assertTrue(job.get("published", False), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["model_calls"], 3)
        self.assertEqual(len(tree["nodes"]), 2)
        stages = tree["nodes"][1]["reflection_stages"]
        self.assertEqual([stage["phase"] for stage in stages], ["map", "map", "reduce"])
        self.assertEqual([packet["phase"] for packet in self.requests], ["combee_map", "combee_map", "combee_reduce"])
        self.assertEqual(sum(len(packet["feedback"]) for packet in self.requests[:2]), 8)
        self.assertNotIn("feedback", self.requests[-1])
        self.assertEqual(len(self.requests[-1]["intermediate_proposals"]), 2)
        for packet in self.requests:
            body = json.dumps(packet)
            self.assertNotIn("HOLDOUT_PRIVATE", body)
            self.assertNotIn("VALIDATION_PRIVATE", body)
        folder = self.root / "production/agent-evaluation" / job_id / "search/reflections"
        for stage in stages:
            self.assertTrue((folder / (stage["file"] + ".request.json")).is_file())
            self.assertTrue((folder / (stage["file"] + ".draft.json")).is_file())

    def test_combee_budget_shortage_does_not_start_partial_model_work(self):
        self.configure_combee(max_model_calls=2)
        self.start()
        job_id, job, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["model_calls"], 0)
        self.assertEqual(tree["nodes"][1]["error"], "gepa_model_budget_exhausted")
        self.assertEqual(len(self.requests), 0)
        self.assertTrue(tree["holdout_started"])
        self.assertFalse(job.get("published", False))

    def test_combee_small_batch_falls_back_to_single_reflection(self):
        self.configure_combee()
        self.evaluation["repetitions"] = 1
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        job_id, _, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["model_calls"], 1)
        self.assertEqual(len(self.requests), 1)
        self.assertNotIn("phase", self.requests[0])
        self.assertNotIn("reflection_stages", tree["nodes"][1])

    def test_combee_rejects_sensitive_map_output_before_reduction_or_persistence(self):
        self.configure_combee()
        self.behavior = "combee_leak"
        self.start()
        job_id, job, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["model_calls"], 1)
        self.assertEqual(tree["nodes"][1]["error"], "credential_value_rejected")
        self.assertFalse(job.get("published", False))
        for path in (self.root / "production").rglob("*"):
            if path.is_file():
                self.assertNotIn(self.env["GEPA_LEAK_SECRET"].encode(), path.read_bytes(), str(path))

    def test_combee_combines_only_the_selected_components(self):
        self.configure_merge(max_rounds=1, proposals_per_round=1)
        self.configure_combee(max_model_calls=6)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertTrue(job.get("published", False), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["model_calls"], 6)
        self.assertEqual(len(tree["nodes"][1]["reflection_stages"]), 6)
        root = self.root / "production/agent-evaluation" / job_id
        options = json.loads((root / "candidate.json").read_text())["policy"]["options"]
        self.assertTrue(options["experience_overrides"])
        self.assertTrue(any("source_experience" in rule for rule in options["action_rules"]))
        for packet in self.requests:
            self.assertEqual(len(packet["selected_components"]), 1)

    def test_combee_interruption_preserves_stages_and_spent_budget(self):
        self.configure_combee(max_rounds=2, max_model_calls=5)
        self.pause_on = ("L1", 2)
        self.release.clear()
        self.start()
        deadline = time.monotonic() + 25
        while len(self.requests) < 2 and time.monotonic() < deadline:
            time.sleep(.05)
        self.assertEqual(len(self.requests), 2)
        status = self.call("agent_evaluation.status")
        job_id = next(key for key, value in status["jobs"].items() if value.get("gepa"))
        before = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(len(before["nodes"][1]["reflection_stages"]), 1)
        self.assertEqual(before["model_calls"], 2)
        self.process.kill()
        self.process.communicate(timeout=5)
        self.release.set()
        self.start()
        _, job, _ = self.wait_search()
        self.assertTrue(job.get("published", False), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["nodes"][1]["status"], "interrupted")
        self.assertEqual(len(tree["nodes"][1]["reflection_stages"]), 1)
        self.assertEqual(len(tree["nodes"][2]["reflection_stages"]), 3)
        self.assertEqual(tree["model_calls"], 5)

    def test_disabled_cache_remeasures_parent_and_merge_subset(self):
        self.configure_merge(merge_minibatch_size=1, cache_evaluations=False)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertTrue(job.get("published", False), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["trials_used"], 21)
        self.assertFalse(any(item.get("source_reports") for item in tree["evaluations"].values()))

    def check_partial_cache(self, fault, enabled=True):
        self.judge_requests, self.cache_train_calls = [], []
        self.cache_fault, self.cache_failed_task = fault, None
        self.behavior = "no_change"
        tasks = [json.loads(line) for line in (self.root / "gepa-tasks.jsonl").read_text().splitlines()]
        tasks.append(dict(tasks[0], task_id="train-c", group_id="train-c", task_input="TRAIN_VISIBLE 丙类：核验后完成独立计算。"))
        (self.root / "gepa-tasks.jsonl").write_text("\n".join(json.dumps(task, ensure_ascii=False) for task in tasks))
        result = subprocess.run([str(fixture.CLI), "dataset", "build", "--config", str(self.root / "gepa-dataset.json"),
            "--output-root", str(self.root / "partial-dataset")], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assessment["dataset"] = "partial-dataset"
        fixture.write(self.root / "agent-evaluation.json", self.assessment)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=3, max_model_calls=3, minibatch_size=2, cache_evaluations=enabled)
        fixture.write(self.root / "gepa.json", config)
        self.evaluation["checks"] = {}
        self.evaluation["variants"][0]["command"][-2] = "gepa_review"
        self.evaluation["judge"] = dict(enabled=True, runtime_config="model.json", max_repair_attempts=0,
            prompt="prompts/task-judge-system.txt", loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100, max_input_bytes=1048576)
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        job_id, _, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["nodes"][1]["error"], "gepa_parent_evaluation_inconclusive")
        self.assertEqual(len(self.cache_train_calls), 4 if enabled else 6)
        if enabled:
            self.assertEqual(self.cache_train_calls.count(self.cache_train_calls[0]), 2)
            self.assertEqual(self.cache_train_calls.count(self.cache_failed_task), 1)
            partial = {key for key, value in tree["evaluations"].items() if value["status"] == "inconclusive"}
            if fault == "invalid":
                self.assertTrue(any(partial & set(value.get("source_reports", [])) for value in tree["evaluations"].values()))
        else:
            self.assertFalse(any(value.get("source_reports") for value in tree["evaluations"].values()))
        self.assertGreaterEqual(len([node for node in tree["nodes"][2:] if node["status"] == "duplicate"]), 1)

    def test_partial_cache_reuses_scored_tasks_but_retries_judge_error(self):
        self.check_partial_cache("invalid")

    def test_partial_cache_does_not_reuse_unknown_loop_metrics(self):
        self.check_partial_cache("uncertain")

    def test_partial_cache_can_be_disabled(self):
        self.check_partial_cache("invalid", enabled=False)

    def check_seed_cache_recovery(self, fault, enabled=True, failures=1, repetitions=1):
        self.judge_requests, self.seed_judge_tasks = [], []
        self.seed_fault, self.seed_failures = fault, failures
        self.behavior = "no_change"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1, cache_evaluations=enabled)
        fixture.write(self.root / "gepa.json", config)
        self.evaluation.update(checks={}, repetitions=repetitions)
        self.evaluation["variants"][0]["command"][-2] = "gepa_review"
        if fault == "tokens":
            self.env["GEPA_MISSING_METRIC_ONCE"] = str(self.root / "metric-missing-once")
            self.evaluation["variants"][0]["pass_env"].append("GEPA_MISSING_METRIC_ONCE")
        self.evaluation["judge"] = dict(enabled=True, runtime_config="model.json", max_repair_attempts=0,
            prompt="prompts/task-judge-system.txt", loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100, max_input_bytes=1048576)
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        job_id, job, _ = self.wait_search()
        self.assertEqual(job.get("error"), "gepa_seed_evaluation_inconclusive")
        root = self.root / "production/agent-evaluation" / job_id / "search"
        before = self.call("agent_evaluation.tree", job_id=job_id)
        key = next(iter(before["evaluations"]))
        original = before["evaluations"][key]
        original_bytes = (root / original["report"]).read_bytes()
        self.assertEqual(original["status"], "inconclusive")
        if fault == "tokens":
            report = json.loads(original_bytes)
            self.assertTrue(all(t["verdict"] in ("passed", "failed") for t in report["trials"]))
            self.assertTrue(any("tokens" not in t["metrics"] for t in report["trials"]))
            # Simulate an older cache index without changing its report or hash.
            fixture.AgentEvaluationTests.tearDown(self)
            before["evaluations"][key]["status"] = "complete"
            fixture.write(root / "tree.json", before)
            self.start()
        for attempt in range(failures):
            self.call("agent_evaluation.retry", job_id=job_id)
            _, job, _ = self.wait_search()
            tree = self.call("agent_evaluation.tree", job_id=job_id)
            cached = tree["evaluations"][key]
            if attempt + 1 < failures:
                self.assertEqual(job.get("error"), "gepa_seed_evaluation_inconclusive")
                report = json.loads((root / cached["report"]).read_text())
                self.assertTrue(report["complete"], "a completed trial batch became structurally incomplete")
                self.assertEqual(cached["status"], "inconclusive")
        self.assertIn(job["status"], ("passed", "rejected"), job)
        self.assertEqual(tree["nodes"][0]["status"], "accepted")
        self.assertEqual(len(self.seed_judge_tasks), repetitions * (2 + failures * (1 if enabled else 2)))
        self.assertEqual(sum("乙类" in task for task in self.seed_judge_tasks), repetitions * (1 if enabled else 1 + failures))
        self.assertEqual((root / original["report"]).read_bytes(), original_bytes)
        if enabled:
            self.assertEqual(cached["trials_reserved"], 0)
            refs = cached["source_report_refs"]
            self.assertTrue(any(ref["key"] == key and ref["report"] != cached["report"] for ref in refs))
            for ref in refs:
                self.assertTrue((root / ref["report"]).is_file())
        else:
            self.assertNotIn("source_report_refs", cached)

    def test_exact_seed_retry_reuses_valid_tasks(self):
        self.check_seed_cache_recovery("uncertain", repetitions=2)

    def test_judge_failure_retry_preserves_completed_execution(self):
        self.judge_requests = []
        self.fail_search_judge = True
        self.behavior = "no_change"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.evaluation["checks"] = {}
        self.evaluation["variants"][0]["command"][-2] = "gepa_review"
        self.evaluation["judge"] = dict(enabled=True, runtime_config="model.json", max_repair_attempts=0,
            prompt="prompts/task-judge-system.txt", loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100, max_input_bytes=1048576)
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        job_id, job, _ = self.wait_search()
        self.assertEqual(job.get("error"), "gepa_seed_evaluation_inconclusive")
        root = self.root / "production/agent-evaluation" / job_id / "search"
        before = self.call("agent_evaluation.tree", job_id=job_id)
        key, original = next(iter(before["evaluations"].items()))
        source = root / original["report"]
        source_bytes = source.read_bytes()
        old = json.loads(source_bytes)
        self.assertEqual(len(old["trials"]), 2)
        self.assertTrue(all(t["execution_status"] == "completed"
            and t["error_code"] == "evaluation_judge_failed" for t in old["trials"]))
        self.fail_search_judge = False
        self.call("agent_evaluation.retry", job_id=job_id)
        _, job, _ = self.wait_search()
        self.assertIn(job["status"], ("passed", "rejected"), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        restored = json.loads((root / tree["evaluations"][key]["report"]).read_text())
        self.assertEqual(restored["summary"].get("regraded_trials"), 2)
        for previous, current in zip(old["trials"], restored["trials"]):
            for field in ("trial_id", "task_id", "task_version", "duration_ms", "metrics", "execution_status"):
                self.assertEqual(current[field], previous[field], field)
            self.assertEqual(current["evidence"]["trace"], previous["evidence"]["trace"])
        self.assertEqual(source.read_bytes(), source_bytes)

    def test_transient_judge_retry_regrades_only_the_failed_task(self):
        self.judge_requests = []
        self.fail_search_judge = True
        self.fail_search_prefix = "VALIDATION_PRIVATE 甲类"
        self.behavior = "no_change"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.config["evaluation_levels"] = ["L1"]
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["checks"] = {}
        self.evaluation["variants"][0]["command"][-2] = "gepa_review"
        self.evaluation["judge"] = dict(enabled=True, runtime_config="model.json", max_repair_attempts=0,
            prompt="prompts/task-judge-system.txt", loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100, max_input_bytes=1048576)
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        job_id, job, _ = self.wait_search()
        self.assertEqual(job.get("error"), "gepa_seed_evaluation_inconclusive")
        self.fail_search_judge = False
        self.call("agent_evaluation.retry", job_id=job_id)
        _, job, _ = self.wait_search()
        self.assertIn(job["status"], ("passed", "rejected"), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        root = self.root / "production/agent-evaluation" / job_id / "search"
        key, cached = next((k, v) for k, v in tree["evaluations"].items() if v["split"] == "val")
        self.assertEqual(cached["attempt"], 3)
        before = json.loads((root / "evaluations" / key / "attempt-1/report.json").read_text())
        after = json.loads((root / cached["report"]).read_text())
        self.assertEqual(after["summary"].get("regraded_trials"), 1)
        self.assertEqual(sum(p["task_input"].startswith("VALIDATION_PRIVATE") for p in self.judge_requests), 4)
        old = {t["task_id"]: t for t in before["trials"]}
        for trial in after["trials"]:
            previous = old[trial["task_id"]]
            for field in ("trial_id", "duration_ms", "metrics", "execution_status"):
                self.assertEqual(trial[field], previous[field], field)
            self.assertEqual(trial["evidence"]["trace"], previous["evidence"]["trace"])

    def test_transient_judge_recovers_automatically_without_agent_reexecution(self):
        self.judge_requests = []
        self.fail_search_judge, self.search_judge_failures = True, 1
        self.fail_search_prefix = "VALIDATION_PRIVATE 甲类"
        self.behavior = "no_change"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.config["evaluation_levels"] = ["L1"]
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["checks"] = {}
        self.evaluation["variants"][0]["command"][-2] = "gepa_review"
        self.evaluation["judge"] = dict(enabled=True, runtime_config="model.json", max_repair_attempts=0,
            prompt="prompts/task-judge-system.txt", loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100, max_input_bytes=1048576)
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        job_id, job, _ = self.wait_search()
        self.assertIn(job["status"], ("passed", "rejected"), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        root = self.root / "production/agent-evaluation" / job_id / "search"
        key, cached = next((k, v) for k, v in tree["evaluations"].items() if v["split"] == "val")
        self.assertEqual(cached["attempt"], 2)
        before = json.loads((root / "evaluations" / key / "attempt-1/report.json").read_text())
        after = json.loads((root / cached["report"]).read_text())
        self.assertEqual(after["summary"].get("regraded_trials"), 1)
        self.assertEqual(sum(p["task_input"].startswith("VALIDATION_PRIVATE") for p in self.judge_requests), 3)
        old = {t["task_id"]: t for t in before["trials"]}
        for trial in after["trials"]:
            previous = old[trial["task_id"]]
            for field in ("trial_id", "duration_ms", "metrics", "execution_status"):
                self.assertEqual(trial[field], previous[field], field)
            self.assertEqual(trial["evidence"]["trace"], previous["evidence"]["trace"])

    def test_exact_seed_retry_keeps_good_tasks_after_another_failure(self):
        self.check_seed_cache_recovery("uncertain", failures=2)

    def test_rate_limited_parent_judge_preserves_the_pending_generation(self):
        self.judge_requests = []
        self.fail_search_judge, self.search_judge_status = True, 429
        self.fail_search_prefix = "TRAIN_VISIBLE"
        self.behavior = "no_change"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=2, max_model_calls=2)
        fixture.write(self.root / "gepa.json", config)
        self.evaluation["checks"] = {}
        self.evaluation["variants"][0]["command"][-2] = "gepa_review"
        self.evaluation["judge"] = dict(enabled=True, runtime_config="model.json", max_repair_attempts=0,
            prompt="prompts/task-judge-system.txt", loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100, max_input_bytes=1048576)
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        job_id, job, _ = self.wait_search()
        self.assertEqual(job.get("error"), "gepa_model_rate_limited")
        root = self.root / "production/agent-evaluation" / job_id / "search"
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual((tree["rounds"], tree["model_calls"], tree["active"]), (1, 0, 1))
        self.assertEqual(len(tree["nodes"]), 2)
        self.assertEqual(tree["nodes"][1]["status"], "pending")
        self.assertFalse(tree.get("holdout_started", False))
        key, cached = next((k, v) for k, v in tree["evaluations"].items() if v["split"] == "train")
        previous = json.loads((root / cached["report"]).read_text())
        self.fail_search_judge = False
        self.call("agent_evaluation.retry", job_id=job_id)
        _, job, _ = self.wait_search()
        self.assertIn(job["status"], ("passed", "rejected"), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        restored = json.loads((root / tree["evaluations"][key]["report"]).read_text())
        self.assertEqual(restored["summary"].get("regraded_trials"), 2)
        self.assertEqual([t["trial_id"] for t in previous["trials"]], [t["trial_id"] for t in restored["trials"]])
        self.assertEqual(tree["rounds"], 2)
        self.assertEqual(tree["model_calls"], 2)

    def test_rate_limited_reflection_retries_the_same_generation(self):
        self.fail_reflection_rate_limit = True
        self.behavior = "no_change"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=3)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search()
        self.assertEqual(job.get("error"), "gepa_model_rate_limited")
        before = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual((before["rounds"], before["model_calls"], before["active"]), (1, 1, 1))
        self.assertEqual(before["nodes"][1]["status"], "pending")
        self.assertFalse(before.get("holdout_started", False))
        self.fail_reflection_rate_limit = False
        self.call("agent_evaluation.retry", job_id=job_id)
        _, job, _ = self.wait_search()
        self.assertIn(job["status"], ("passed", "rejected"), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual((tree["rounds"], tree["model_calls"]), (1, 2))
        self.assertEqual(len(self.requests), 2)
        self.assertEqual(tree["nodes"][1]["status"], "duplicate")
        for key, cached in before["evaluations"].items():
            self.assertEqual(tree["evaluations"][key]["attempt"], cached["attempt"])

    def test_legacy_complete_seed_cache_with_missing_tokens_is_recoverable(self):
        self.check_seed_cache_recovery("tokens")

    def check_conservative_cost(self, fault):
        self.judge_requests, self.seed_judge_tasks = [], []
        self.seed_fault, self.seed_failures = fault, 1
        self.behavior = "no_change"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1, unknown_token_policy="worst_case")
        fixture.write(self.root / "gepa.json", config)
        self.evaluation.update(checks={}, repetitions=2)
        self.evaluation["variants"][0]["command"][-2] = "gepa_review"
        if fault == "tokens":
            self.env["GEPA_MISSING_METRIC_ONCE"] = str(self.root / "metric-missing-once")
            self.evaluation["variants"][0]["pass_env"].append("GEPA_MISSING_METRIC_ONCE")
        self.evaluation["judge"] = dict(enabled=True, runtime_config="model.json", max_repair_attempts=0,
            prompt="prompts/task-judge-system.txt", loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100, max_input_bytes=1048576)
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        job_id, job, _ = self.wait_search()
        if fault != "tokens":
            self.assertEqual(job.get("error"), "gepa_seed_evaluation_inconclusive")
            return
        self.assertIn(job["status"], ("passed", "rejected"), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        root = self.root / "production/agent-evaluation" / job_id / "search"
        source = next(v for v in tree["evaluations"].values() if v.get("split") == "val")
        report = json.loads((root / source["report"]).read_text())
        self.assertTrue(any("tokens" not in t["metrics"] for t in report["trials"]))
        objective = config["objective"]
        for task, row in tree["nodes"][0]["validation"].items():
            trials = [t for t in report["trials"] if t["task_id"] == task]
            missing = sum("tokens" not in t["metrics"] for t in trials)
            self.assertEqual(row.get("unknown_token_trials", 0), missing)
            costs = [t["metrics"]["tokens"] / (objective["token_scale"] + t["metrics"]["tokens"])
                     if "tokens" in t["metrics"] else 1.0 for t in trials]
            expected = sum((t["verdict"] == "passed")
                - objective["invalid_loop_weight"] * t["evidence"]["loop_review"]["count"]
                    / (objective["invalid_loop_scale"] + t["evidence"]["loop_review"]["count"])
                - objective["token_weight"] * cost for t, cost in zip(trials, costs)) / len(trials)
            self.assertAlmostEqual(row["fitness"], expected)
            self.assertAlmostEqual(row["token_efficiency"], sum(1 - c for c in costs) / len(costs))

    def test_unknown_tokens_can_use_explicit_conservative_cost(self):
        self.check_conservative_cost("tokens")

    def test_conservative_cost_does_not_accept_uncertain_grading(self):
        self.check_conservative_cost("uncertain")

    def test_exact_seed_retry_can_disable_cache(self):
        self.check_seed_cache_recovery("uncertain", enabled=False)

    def test_round_robin_rejects_mutation_of_an_unselected_component(self):
        self.configure_merge(component_selector="round_robin", max_rounds=1, max_model_calls=1, proposals_per_round=1)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["nodes"][1]["components"], ["action_rules"])
        self.assertEqual(tree["nodes"][1]["error"], "gepa_component_not_authorized")
        self.assertFalse(job.get("published", False))

    def test_new_config_rejects_unknown_strategies_and_invalid_limits(self):
        config = json.loads((self.root / "gepa.json").read_text())
        for field, value in (("candidate_selection_strategy", "unknown"), ("proposals_per_round", 0),
                             ("max_merge_invocations", 257), ("use_merge", "true"), ("epsilon", -1),
                             ("frontier_type", "everything"), ("component_selector", "host_code"),
                             ("reflection_strategy", "unknown"), ("duplication_factor", 0)):
            with self.subTest(field=field):
                fixture.write(self.root / "invalid-gepa.json", dict(config, **{field: value}))
                result = subprocess.run([str(fixture.BUILD / "test_gepa_config"), str(self.root / "invalid-gepa.json")],
                    capture_output=True, text=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
