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
                        if getattr(owner, "search_judge_invalid_json", False):
                            response = json.dumps(dict(choices=[dict(message=dict(content="not-json"))])).encode()
                            self.send_response(200); self.send_header("Content-Type", "application/json")
                            self.send_header("Content-Length", str(len(response))); self.end_headers(); self.wfile.write(response)
                            return
                        self.send_response(getattr(owner, "search_judge_status", 503))
                        self.send_header("Content-Length", "0"); self.end_headers()
                        return
                    inventory = packet["loop_inventory"]
                    key = next(iter(inventory), None)
                    judgment = dict(verdict="passed", reason="测试评分器确认任务输出。", evidence_paths=["/output"],
                        loop_review=dict(status="complete" if key else "uncertain", wasted_iterations=[dict(iteration_id=key,
                            reason="测试中将本轮标为可省去，用于核验评分口径。", evidence_event_ids=[inventory[key]["event_ids"][0]])] if key else []))
                    if getattr(owner, "rank_feedback", False) and packet["task_input"].startswith("TRAIN_VISIBLE 甲类"):
                        judgment["loop_review"]["wasted_iterations"] = []
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
                if packet.get("phase") == "repair_component_selection":
                    selected = packet["selected_components"]
                    if selected == ["schedule_rules"]:
                        plans = [plan for row in packet.get("feedback", [])
                                 for run in row["trial"].get("evidence", {}).get("runs", [])
                                 for plan in run.get("schedule_plans", [])]
                        draft = dict(reason="修正请求缺少实际编排证据。", experiences=[])
                        if plans:
                            draft = dict(reason="根据保留的训练批次修正获选编排组件。", controls=dict(schedule_rules=[dict(
                                memory_id="test-experience", before_operation="prepare", after_operation="image",
                                require_success=True, task_terms=["生成视频时缺少图片素材"],
                                exclude_terms=["互不依赖"], reason="准备成功后再生成图像。")]))
                    elif selected == ["action_rules"]:
                        draft = dict(reason="按获选动作组件修正候选。", controls=dict(action_rules=[dict(
                            memory_id="test-experience", operation="calculate", required_arguments=["verified"],
                            reason="缺少核验参数时返回可恢复的拒绝。")]))
                    elif selected == ["loop_rules"]:
                        draft = dict(reason="按获选循环组件修正候选。", controls=dict(loop_rules=[dict(
                            memory_id="test-experience", max_iterations=12, max_no_progress=1,
                            reason="根据重复状态证据收紧循环。")]))
                    else:
                        draft = dict(reason="没有获选经验的相关证据，保留父版本。", experiences=[])
                    content = json.dumps(draft, ensure_ascii=False)
                    response = json.dumps(dict(choices=[dict(finish_reason="stop", message=dict(content=content))])).encode()
                    self.send_response(200); self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(response))); self.end_headers(); self.wfile.write(response)
                    return
                if getattr(owner, "fail_reflection_rate_limit", False):
                    self.send_response(429)
                    self.send_header("Content-Length", "0"); self.end_headers()
                    return
                level = (packet.get("controls") or {}).get("level", "L1")
                ordinal = sum((p.get("controls") or {}).get("level", "L1") == level for p in owner.requests)
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
                elif owner.behavior.startswith("schema_actions"):
                    source = "test-experience"
                    if "bad_source" in owner.behavior and (packet.get("phase") != "repair_control_source"
                            or owner.behavior.endswith("always")):
                        source = "bounded-agent-loop"
                    controls = dict(action_rules=[dict(memory_id=source,
                        primitive_id="schema_required_nonempty",
                        reason="运行态 schema 的必填参数为空时拒绝执行。")], loop_rules=[])
                    if getattr(owner, "vary_control_reason", False):
                        controls["action_rules"][0]["reason"] += str(ordinal)
                    content = json.dumps(dict(reason="收紧必填参数的非空检查。", controls=controls), ensure_ascii=False)
                elif owner.behavior == "repeat_target":
                    content = json.dumps(dict(reason="同一输出目标跨执行实例重复失败。",
                        controls=dict(action_rules=[dict(memory_id="test-experience", primitive_id="repeat_failed_target",
                            operation="calculate", failure_key_arguments=["output"], max_failed_attempts=2,
                            reason="相同目标的重复失败预算已耗尽。")], loop_rules=[])), ensure_ascii=False)
                elif owner.behavior == "repeat_failures":
                    content = json.dumps(dict(reason="同一操作连续失败后拦截第三次尝试，独立操作继续。",
                        controls=dict(action_rules=[dict(memory_id="test-experience",
                            primitive_id="repeat_failed_operation", reason="停止已落定的重复失败。")], loop_rules=[])), ensure_ascii=False)
                elif owner.behavior == "invalid_schema_action":
                    controls = dict(action_rules=[dict(memory_id="test-experience", operation="calculate",
                        required_arguments=["value"], reason="未授权具体工具规则。")], loop_rules=[])
                    content = json.dumps(dict(reason="测试未授权规则审计。", controls=controls), ensure_ascii=False)
                elif owner.behavior == "full_controls":
                    content = json.dumps(dict(reason="合并编排与分支失败恢复的独立执行证据。", controls=dict(
                        schedule_rules=[dict(memory_id="test-experience", before_operation="prepare", after_operation="image",
                            require_success=True, task_terms=["生成视频"], reason="准备完成后才生成图像。")],
                        loop_rules=[dict(memory_id="test-experience", replan_on_failure=True,
                            reason="失败时有界重规划，保持其他分支可用。")])), ensure_ascii=False)
                elif owner.behavior in ("schedule", "schedule_overstrict", "schedule_wrong_success"):
                    terms = ["先准备素材", "再生成图像", "生成短视频"] if owner.behavior == "schedule_overstrict" else ["先准备素材"]
                    content = json.dumps(dict(reason="训练中的并行图像分支先于准备完成，需要调整技能依赖。",
                        controls=dict(schedule_rules=[dict(memory_id=getattr(owner, "control_source_id", "test-experience"), before_operation="prepare",
                            after_operation="image", require_success=owner.behavior != "schedule_wrong_success", task_terms=terms,
                            reason="准备成功后再生成图像。")])) , ensure_ascii=False)
                elif owner.behavior == "schedule_bad_component":
                    content = json.dumps(dict(reason="测试模型错误地改写未获选的经验。",
                        experience=dict(memory_id="test-experience", content="生成图像前先完成准备。")), ensure_ascii=False)
                elif owner.behavior == "recovery_budget":
                    content = json.dumps(dict(reason="重复失败时额外重规划没有改进。", controls=dict(loop_rules=[dict(
                        memory_id="test-experience", max_replans=0, replan_on_failure=False,
                        reason="达到原无进展阈值后停止该分支，不再额外恢复。")])) , ensure_ascii=False)
                elif owner.behavior == "failure_replan":
                    content = json.dumps(dict(reason="工具已失败且状态仍在变化，需要真实的失败恢复机会。",
                        controls=dict(loop_rules=[dict(memory_id="test-experience", replan_on_failure=True,
                            reason="失败后在本分支重新规划，独立工作继续。")])) , ensure_ascii=False)
                elif owner.behavior in ("actions", "loops", "untriggerable_loop"):
                    controls = dict(action_rules=[], loop_rules=[])
                    if owner.behavior == "actions":
                        controls["action_rules"] = [dict(memory_id="test-experience", operation="calculate",
                            required_arguments=["verified"], reason="计算缺少核验标记时拒绝执行。")]
                    else:
                        aggressive = packet.get("controls", {}).get("aggressive_controls", False)
                        controls["loop_rules"] = [dict(memory_id="test-experience",
                            max_iterations=11 if owner.behavior == "untriggerable_loop" else (2 if aggressive else 12),
                            max_no_progress=3 if aggressive else 1, reason="根据重复任务的执行反馈收紧循环次数。")]
                    content = json.dumps(dict(reason="根据训练执行的结果修改受限规则。", controls=controls), ensure_ascii=False)
                elif owner.behavior == "unused_experience":
                    content = json.dumps(dict(reason="测试正文可编译，但完整注入上下文超过请求预算。",
                        experience=dict(memory_id="test-experience", content="缺少素材时先核验输入，独立任务继续。" * 16)),
                        ensure_ascii=False)
                elif owner.behavior == "disable_enable":
                    content = json.dumps(dict(reason="比较停用经验，并恢复种子批准的同一经验。",
                        experience=dict(memory_id="test-experience", content=None if ordinal == 1 else
                            "缺少素材时核验甲类任务，并核验乙类任务，独立计算继续。")), ensure_ascii=False)
                elif owner.behavior == "operation_guard":
                    content = json.dumps(dict(reason="结果类经验需要实际操作，而不是文本提及。", experience=dict(
                        memory_id="test-experience", content="缺少素材时核验甲类任务，并核验乙类任务，独立计算继续。",
                        observed_operations=["calculate"])), ensure_ascii=False)
                elif owner.behavior == "trigger_override":
                    change = dict(memory_id="test-experience",
                        content="缺少素材时核验甲类任务，并核验乙类任务，独立计算继续。" + ("避免重复检查。" if ordinal > 1 else ""))
                    if ordinal == 1:
                        change["trigger_pattern"] = "生成视频时缺少图片素材，核验输入"
                    content = json.dumps(dict(reason="根据实际任务收窄经验适用范围。", experience=change), ensure_ascii=False)
                elif owner.behavior == "empty_change":
                    content = json.dumps(dict(reason="独立任务保持并行，没有需要修改的组件。", experiences=[]))
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
                if getattr(owner, "prefixed", False):
                    content = "Provider note: structured result follows.\n" + content
                elif owner.fenced:
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

    def test_baseline_scores_are_reused_when_only_search_configuration_changes(self):
        self.behavior = "empty_change"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        first_id, _, _ = self.wait_search()
        first = self.call("agent_evaluation.tree", job_id=first_id)
        self.assertEqual(first["baseline_cache"]["status"], "miss", first)
        seed = json.loads((self.root / "production/agent-evaluation" / first_id / "seed.json").read_text())

        self.process.terminate()
        self.process.communicate(timeout=10)
        self.process = None
        config.update(max_rounds=2, max_model_calls=2)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        second_id = self.call("agent_evaluation.submit", artifact=seed)["job_id"]
        deadline = time.monotonic() + 70
        while time.monotonic() < deadline:
            job = self.call("agent_evaluation.status")["jobs"][second_id]
            if job["status"] not in ("pending", "running"):
                break
            time.sleep(.05)
        else:
            self.fail("second search did not finish")
        second = self.call("agent_evaluation.tree", job_id=second_id)
        self.assertEqual(second["baseline_cache"]["status"], "hit", second)
        self.assertEqual(second["baseline_validation"], first["baseline_validation"])
        self.assertFalse(any(row.get("version") == second["baseline_version"] and row.get("split") == "val"
            for row in second["evaluations"].values()), second["evaluations"])

        self.process.terminate()
        self.process.communicate(timeout=10)
        self.process = None
        self.evaluation["timeout_ms"] += 1
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        third_id = self.call("agent_evaluation.submit", artifact=seed)["job_id"]
        deadline = time.monotonic() + 70
        while time.monotonic() < deadline:
            job = self.call("agent_evaluation.status")["jobs"][third_id]
            if job["status"] not in ("pending", "running"):
                break
            time.sleep(.05)
        else:
            self.fail("changed-evaluator search did not finish")
        third = self.call("agent_evaluation.tree", job_id=third_id)
        self.assertEqual(third["baseline_cache"]["status"], "miss", third)
        self.assertNotEqual(third["baseline_cache"]["key"], second["baseline_cache"]["key"])

    def check_action_feedback_contract(self, mode, behavior):
        self.behavior = behavior
        self.config.update(max_level="L2", evaluation_levels=["L2"], control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2))
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = mode
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(component_selector="round_robin", max_rounds=1, max_model_calls=1,
                      max_input_bytes=16384)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        return self.call("agent_evaluation.tree", job_id=job_id), job

    def test_dispatch_guard_feedback_drives_real_generated_rule(self):
        tree, job = self.check_action_feedback_contract("gepa_repeat_guard", "repeat_failures")
        self.assertEqual(job["status"], "passed", job)
        actions = [a for p in self.requests for row in p["feedback"]
                   for run in row["trial"]["evidence"]["runs"] for a in run["actions"]]
        guards = [a for a in actions if a.get("guard_only")]
        self.assertTrue(guards, actions)
        self.assertTrue(any(a["action_control"].get("prior_failures") == 2 for a in guards))
        self.assertTrue(all(a["actor_id"] == "fixture-worker" and not a["executed"] for a in guards))
        node = tree["nodes"][1]
        self.assertEqual(node["components"], ["action_rules"])
        self.assertTrue(node["control_preflight"]["passed"])
        self.assertGreater(sum(r["applied"] for r in node["control_coverage"]["rules"].values()), 0)

    def test_repeated_tool_failure_is_an_action_opportunity_without_guard_wrapper(self):
        tree, job = self.check_action_feedback_contract("gepa_repeat_tool", "repeat_failures")
        self.assertEqual(job["status"], "passed", job)
        self.assertEqual(tree["nodes"][1]["components"], ["action_rules"])
        self.assertTrue(tree["nodes"][1]["control_preflight"]["passed"])

    def test_repeated_target_survives_redispatch_and_is_attributed(self):
        tree, job = self.check_action_feedback_contract("gepa_repeat_target", "repeat_target")
        self.assertEqual(job["status"], "passed", job)
        node = tree["nodes"][1]
        self.assertEqual(node["status"], "accepted", node)
        self.assertTrue(node["control_preflight"]["passed"])
        self.assertTrue(node["screening"]["applied_changes"], node["screening"])
        self.assertTrue(node["validation_applied_changes"]["0"])
        actions = [a for p in self.requests for row in p["feedback"]
                   for run in row["trial"]["evidence"]["runs"] for a in run["actions"]]
        candidates = [item for a in actions for item in (a.get("action_control") or {}).get("failure_key_candidates", [])]
        self.assertTrue(any(item["argument"] == "output" and item["prior_failures"] == 2 for item in candidates), candidates)

    def test_schedule_success_contract_repairs_the_draft_without_another_model_call(self):
        self.schedule_dataset({"train-a", "train-b", "val-a", "val-b", "holdout-a", "holdout-b"})
        self.behavior = "schedule_wrong_success"
        self.config.update(max_level="L2", evaluation_levels=["L2"], control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2),
            instrumentation=dict(level="L2", capabilities=["model", "actions", "schedule"]))
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_schedule"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(component_selector="round_robin", max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertEqual(job["status"], "passed", job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertTrue(tree["nodes"][1]["schedule_contract_repair"])
        self.assertEqual(tree["model_calls"], 1)
        candidate = json.loads((self.root / "production/agent-evaluation" / job_id / "candidate.json").read_text())
        self.assertTrue(all(rule["require_success"] for rule in candidate["policy"]["options"]["schedule_rules"]))

    def test_custom_metadata_is_not_mistaken_for_a_schedule_contract(self):
        tasks = [json.loads(line) for line in (self.root / "gepa-tasks.jsonl").read_text().splitlines()]
        for index, task in enumerate(tasks):
            task["attributes"] = dict(dependency=["prepare", "image"], stop_on_failure=True) if index % 2 \
                else dict(dependency="prepare_before_image", stop_on_failure="true")
        (self.root / "gepa-tasks.jsonl").write_text(
            "\n".join(json.dumps(task, ensure_ascii=False) for task in tasks), encoding="utf-8")
        self.schedule_dataset({task["task_id"] for task in tasks}, reviewed=False)
        self.behavior = "schedule_wrong_success"
        self.config.update(max_level="L2", evaluation_levels=["L2"], control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2),
            instrumentation=dict(level="L2", capabilities=["model", "actions", "schedule"]))
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_schedule"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(component_selector="round_robin", max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertEqual(job["status"], "passed", job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertNotIn("schedule_contract_repair", tree["nodes"][1])
        self.assertEqual(tree["model_calls"], 1)

    def test_reason_only_control_mutation_does_not_repeat_agent_evaluation(self):
        self.behavior = "schema_actions"
        self.vary_control_reason = True
        self.config.update(max_level="L2", evaluation_levels=["L2"], control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2))
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_schema_actions"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(component_selector="model_choice", max_rounds=2, max_model_calls=2)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, _, _ = self.wait_search("L2")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        node = tree["nodes"][2]
        self.assertEqual(node["status"], "duplicate", node)
        self.assertEqual(node["duplicate_of"], 1)
        self.assertEqual(node["skip_reason"], "no_control_predicate_change")
        self.assertFalse(any(e["version"] == node["version"] for e in tree["evaluations"].values()))

    def test_l3_can_stop_at_no_progress_instead_of_another_replan(self):
        self.behavior = "recovery_budget"
        self.config.update(max_level="L3", evaluation_levels=["L3"], control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2))
        self.config["runtime_options"]["aggressive_controls"] = True
        self.config["runtime_options"]["loop_rules"][0].update(max_no_progress=1, max_replans=1, no_progress_decision="replan")
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_recovery_budget"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(component_selector="round_robin", max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, _, _ = self.wait_search("L3")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        node = tree["nodes"][1]
        self.assertEqual(node["status"], "accepted", node)
        self.assertTrue(node["control_preflight"]["passed"])
        self.assertTrue(any(row.get("loop", 0) for row in node["screening"]["applied_changes"].values()))
        self.assertTrue(any(r["boundary"] == "loop" and r["applied"] > 0 for r in node["control_coverage"]["rules"].values()))

    def test_truncated_present_argument_does_not_create_missing_argument_evidence(self):
        tree, _ = self.check_action_feedback_contract("gepa_schema_wide", "empty_change")
        self.assertFalse(tree.get("training_opportunities", {}).get("action_rules"))
        self.assertNotIn("action_rules", tree["nodes"][1]["components"])

    def test_late_missing_argument_survives_control_feedback_budget(self):
        tree, job = self.check_action_feedback_contract("gepa_schema_late", "schema_actions")
        self.assertEqual(job["status"], "passed", job)
        actions = [a for p in self.requests for row in p["feedback"]
                   for run in row["trial"]["evidence"]["runs"] for a in run["actions"]]
        self.assertTrue(any(a.get("action_control", {}).get("missing_required_arguments") == ["zz_value"]
                            for a in actions))
        self.assertTrue(tree["nodes"][1]["control_preflight"]["passed"])

    def test_operation_guard_is_persisted_and_does_not_invent_execution(self):
        self.behavior = "operation_guard"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, _, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        root = self.root / "production/agent-evaluation" / job_id / "search"
        child = json.loads((root / "versions/1.json").read_text())
        self.assertEqual(child["policy"]["options"]["experience_overrides"][0]["observed_operations"], ["calculate"])
        self.assertEqual(tree["nodes"][1]["status"], "rejected_minibatch", tree)
        # This fixture's only model call precedes calculate; its text cannot
        # manufacture the completed-action evidence required by the candidate.
        self.assertEqual(tree["nodes"][1]["screening"]["control_coverage"]["capabilities"]["llm"]["applied"], 0)

    def test_trigger_override_survives_further_content_mutation_and_export(self):
        self.behavior = "trigger_override"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=2, max_model_calls=2, candidate_selection_strategy="current_best")
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertNotIn("error", [node["status"] for node in tree["nodes"]], tree)
        root = self.root / "production/agent-evaluation" / job_id / "search"
        first = json.loads((root / "versions/1.json").read_text())
        second = json.loads((root / "versions/2.json").read_text())
        for version in [first, second]:
            self.assertEqual(version["policy"]["options"]["experience_overrides"][0]["trigger_pattern"],
                             "生成视频时缺少图片素材，核验输入")
        self.assertEqual(tree["nodes"][2]["parents"], [1])
        self.assertEqual(first["policy"]["snapshot"], second["policy"]["snapshot"])
        self.assertIsInstance(self.requests[1]["components"]["experience:test-experience"], dict)
        self.assertTrue(any("experience_query" in action
            for packet in self.requests for feedback in packet["feedback"]
            for run in feedback["trial"]["evidence"]["runs"] for action in run["actions"]))

    def test_empty_proposal_preserves_seed_and_skips_candidate_execution(self):
        self.behavior = "empty_change"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=2, max_model_calls=2)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, _, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(len(tree["nodes"]), 3)
        for node in tree["nodes"][1:]:
            self.assertEqual(node["status"], "duplicate", node)
            self.assertEqual(node["skip_reason"], "no_behavior_change")
            self.assertEqual(node["version"], tree["nodes"][0]["version"])
            self.assertNotIn("screening", node)
        root = self.root / "production/agent-evaluation" / job_id / "search"
        self.assertEqual(json.loads((root / "versions/0.json").read_text()),
                         json.loads((root / "versions/1.json").read_text()))

    def test_explicit_protobuf_defaults_do_not_create_a_new_candidate(self):
        self.behavior = "empty_change"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.config["evaluation_levels"] = []
        fixture.write(self.root / "service.json", self.config)
        request = dict(memory_path=str(self.root / "production/by-user/user-a/memory/evolution-memory.pb"),
            identity=dict(agent_id="eval-test", user_id="user-a", conversation_id="test", run_id="compile"),
            shadow_options=dict(self.config["shadow_options"], target_scopes=["agent:eval-test/actor:assistant"]),
            options=dict(requested_level="L1", host_capabilities=["context.request"], allowed_users=["user-a"],
                         fallback="strict", experience_template="{{experience}}", max_context_bytes=4096))
        compiled = subprocess.run([str(fixture.CLI), "runtime", "compile"], input=json.dumps(request),
                                  capture_output=True, text=True, timeout=20)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        policy = json.loads(compiled.stdout)
        options = dict(policy["options"], approved_experiences=[
            dict(memory_id=item["memory_id"], content_revision=item["content_revision"])
            for item in policy["snapshot"]["experiences"]])
        compiled = subprocess.run([str(fixture.CLI), "runtime", "recompile"], input=json.dumps(dict(policy=policy, options=options)),
                                  capture_output=True, text=True, timeout=20)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        policy = json.loads(compiled.stdout)
        self.assertIn("request_context_mode", policy["options"])
        artifact = dict(schema_version=1, agent=self.assessment["agent"], user_id="user-a", level="L1", policy=policy)
        self.start()
        self.call("agent_evaluation.submit", artifact=artifact)
        job_id, _, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        child = tree["nodes"][1]
        self.assertEqual(child["status"], "duplicate", child)
        self.assertEqual(child["version"], tree["nodes"][0]["version"])
        self.assertNotIn("screening", child)

    def test_l3_experience_only_is_not_a_full_level_publication(self):
        self.config.update(max_level="L3", evaluation_levels=["L3"])
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=2, max_model_calls=2)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L3")
        self.assertEqual(job["status"], "rejected", job)
        self.assertIn("level_capability_not_observed", job["gate"]["reasons"])
        coverage = job["gate"]["control_coverage"]
        self.assertGreater(coverage["capabilities"]["llm"]["applied"], 0)
        self.assertIn("loop", coverage["unobserved_capabilities"])
        self.assertFalse((self.root / "production/agent-evaluation" / job_id / "search/published-version.json").exists())

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

    def test_single_prefixed_json_object_recovers_without_another_model_call(self):
        self.prefixed = True
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, _, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        node = tree["nodes"][1]
        self.assertNotEqual(node["status"], "error", node)
        recovery = node["response_recovery"]
        self.assertEqual(recovery["method"], "single_json_object")
        self.assertEqual(int(recovery["provider_envelope"]) + int(recovery["message_content"]), 1)
        self.assertEqual(tree["model_calls"], 1)

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

    def test_reflection_limit_keeps_successful_but_wasteful_training_task(self):
        self.judge_requests = []
        self.rank_feedback = True
        self.behavior = "no_change"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1, max_feedback_trials=1)
        fixture.write(self.root / "gepa.json", config)
        self.evaluation["checks"] = {}
        self.evaluation["variants"][0]["command"][-2] = "gepa_review"
        self.evaluation["judge"] = dict(enabled=True, runtime_config="model.json",
            prompt="prompts/task-judge-system.txt", loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100, max_input_bytes=1048576)
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        self.wait_search()
        self.assertEqual(len(self.requests), 1)
        feedback = self.requests[0]["feedback"]
        self.assertEqual([row["task_id"] for row in feedback], ["train-b"])
        self.assertEqual(feedback[0]["trial"]["verdict"], "passed")
        self.assertEqual(feedback[0]["trial"]["evidence"]["loop_review"]["count"], 1)

    def test_reflection_separates_reviewed_waste_from_runtime_loop_counter(self):
        self.judge_requests = []
        self.behavior = "no_change"
        self.config.update(max_level="L3", evaluation_levels=["L3"], control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2))
        fixture.write(self.root / "service.json", self.config)
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
        self.wait_search("L3")
        packet = next(p for p in self.requests if p.get("controls", {}).get("level") == "L3")
        for row in packet["feedback"]:
            evidence = row["trial"]["evidence"]
            self.assertEqual(evidence["loop_review"]["count"], 1)
            loops = [a for run in evidence["runs"] for a in run["actions"] if a["kind"] == "loop.decision"]
            self.assertTrue(loops)
            for action in loops:
                control = action["loop_control"]
                self.assertTrue(control["query_event_id"])
                self.assertEqual(control["observed_no_progress"], 0)
                self.assertEqual(control["proposed_decision"], "continue")
                self.assertFalse(control["pending_actions"])
                self.assertNotIn("fingerprint", control)

    def test_reflection_keeps_actual_loop_repetition_counters(self):
        self.exercise_controls("loops", "L3")
        packet = next(p for p in self.requests if p.get("controls", {}).get("level") == "L3")
        for row in packet["feedback"]:
            loops = [a for run in row["trial"]["evidence"]["runs"] for a in run["actions"]
                     if a["kind"] == "loop.decision"]
            self.assertEqual([a["loop_control"]["observed_no_progress"] for a in loops], [0, 1, 2, 3])
            self.assertEqual(len({a["loop_control"]["query_event_id"] for a in loops}), 4)

    def test_reflection_disabled_loop_counter_stays_unknown(self):
        self.behavior = "no_change"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_review"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        self.wait_search()  # L1 records loop points without evaluating L3 counters.
        loops = [a for row in self.requests[0]["feedback"] for run in row["trial"]["evidence"]["runs"]
                 for a in run["actions"] if a["kind"] == "loop.decision"]
        self.assertTrue(loops)
        for action in loops:
            self.assertIsNone(action["loop_control"]["observed_no_progress"])

    def test_l1_rewrite_without_actual_injection_is_not_accepted(self):
        self.behavior = "unused_experience"
        self.evaluation["variants"][0]["command"][-2] = "good"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.config["runtime_options"] = dict(max_context_bytes=1536)
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1, minibatch_size=1, acceptance="improvement_or_equal",
            objective=dict(invalid_loop_weight=0, token_weight=0, latency_weight=0))
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, _, _ = self.wait_search()
        node = self.call("agent_evaluation.tree", job_id=job_id)["nodes"][1]
        self.assertEqual(node["status"], "unobserved_controls", node)
        for stage in (node["screening"], node["validation_probe"]):
            rule = next(iter(stage["control_coverage"]["rules"].values()))
            self.assertEqual(rule["boundary"], "llm")
            self.assertGreater(rule["queries"], 0)
            self.assertEqual(rule["triggered"], 0)
            self.assertEqual(rule["applied"], 0)

    def test_l1_rewrite_application_evidence_is_content_specific(self):
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=2, max_model_calls=2)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, _, _ = self.wait_search()
        nodes = self.call("agent_evaluation.tree", job_id=job_id)["nodes"][1:3]
        identities = []
        for node in nodes:
            self.assertEqual(node["status"], "accepted", node)
            coverage = node["control_coverage"]
            self.assertTrue(coverage["complete"])
            identity, counts = next(iter(coverage["rules"].items()))
            self.assertEqual(counts["boundary"], "llm")
            self.assertGreater(counts["applied"], 0)
            identities.append(identity)
        self.assertNotEqual(*identities)

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
        self.assertEqual(job["status"], "cancelled")
        self.assertEqual(job["error"], "evaluation_cancelled")
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
        self.assertEqual(job["status"], "rejected" if kind == "loops" else "passed", job)
        if kind == "loops":
            self.assertEqual(job["gate"]["reasons"], ["level_capability_not_observed"])
            self.assertEqual(job["gate"]["control_coverage"]["unobserved_capabilities"], ["actions"])
            self.assertGreater(job["gate"]["control_coverage"]["capabilities"]["loop"]["applied"], 0)
        candidate = json.loads((self.root / "production/agent-evaluation" / job_id / "candidate.json").read_text())
        rules = candidate["policy"]["options"]["action_rules" if kind == "actions" else "loop_rules"]
        generated = [rule for rule in rules if "source_experience" in rule]
        self.assertEqual(len(generated), 1)
        self.assertEqual(generated[0]["source_experience"]["memory_id"], "test-experience")
        return generated[0]

    def test_l2_generated_action_rules_reach_actual_host(self):
        rule = self.exercise_controls("actions", "L2")
        self.assertEqual(rule["required_arguments"], ["verified"])

    def test_l2_generic_schema_rule_needs_no_tool_whitelist_and_reaches_host(self):
        self.behavior = "schema_actions"
        self.evaluation["variants"][0]["command"][-2] = "gepa_schema_actions"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.config.update(max_level="L2", control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2))
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertEqual(job["status"], "passed", job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertIn("action_rules", tree["available_components"])
        candidate = json.loads((self.root / "production/agent-evaluation" / job_id / "candidate.json").read_text())
        generated = [rule for rule in candidate["policy"]["options"]["action_rules"]
                     if "source_experience" in rule]
        self.assertEqual(len(generated), 1)
        self.assertEqual(generated[0]["operation"], "*")
        self.assertEqual(generated[0]["required_arguments"], ["$schema_required_nonempty"])
        packet = next(p for p in self.requests if p.get("controls", {}).get("level") == "L2")
        self.assertEqual(packet["controls"]["action_primitives"][0]["primitive_id"], "schema_required_nonempty")
        self.assertEqual(packet["controls"]["action_primitives"][0]["operation"], "*")
        self.assertNotIn("actions", packet["controls"]["limits"])

    def run_schema_search(self, mode, behavior="schema_actions", model_calls=1, max_attempts=2):
        self.behavior = behavior
        self.evaluation["variants"][0]["command"][-2] = mode
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.config.update(max_level="L2", control_learning=dict(enabled=True,
            prompt_file="prompts/gepa-reflection.txt", max_rules=2, max_attempts=max_attempts))
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=model_calls, minibatch_size=1,
            acceptance="improvement_or_equal")
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, _, _ = self.wait_search("L2")
        return self.call("agent_evaluation.tree", job_id=job_id)

    def test_unobserved_control_stops_after_train_and_validation_probe(self):
        tree = self.run_schema_search("gepa_schema_noop")
        node = tree["nodes"][1]
        self.assertEqual(node["status"], "unobserved_controls", node)
        self.assertEqual(node["reason"], "control_unobserved_in_train_and_validation")
        self.assertNotIn("validation", node)
        self.assertEqual(len(node["validation_probe"]["scores"]), 1)
        for stage in (node["screening"], node["validation_probe"]):
            rule = next(iter(stage["control_coverage"]["rules"].values()))
            self.assertGreater(rule["queries"], 0)
            self.assertEqual(rule["applied"], 0)

    def test_control_missed_by_training_can_pass_validation_without_reexecution(self):
        tree = self.run_schema_search("gepa_schema_validation")
        node = tree["nodes"][1]
        self.assertEqual(node["status"], "accepted", node)
        self.assertFalse(node["screening"]["control_coverage"]["complete"])
        self.assertTrue(node["validation_probe"]["control_coverage"]["complete"])
        self.assertEqual(len(node["validation"]), 2)
        self.assertEqual(sum(r["applied"] for r in node["control_coverage"]["rules"].values()), 2)
        # The unchanged L0 reference reuses its matching seed validation score.
        # The validation probe is not rerun.
        self.assertEqual(tree["trials_used"], 10, tree["evaluations"])

    def test_control_observed_only_in_training_keeps_its_application_evidence(self):
        tree = self.run_schema_search("gepa_schema_training")
        node = tree["nodes"][1]
        self.assertEqual(node["status"], "accepted", node)
        self.assertNotIn("validation_probe", node)
        self.assertTrue(node["screening"]["control_coverage"]["complete"])
        self.assertEqual(node["control_coverage"]["unobserved_rules"], [])
        self.assertEqual(sum(r["applied"] for r in node["control_coverage"]["rules"].values()), 1)

    def test_bad_control_source_is_repaired_once_with_compact_input(self):
        tree = self.run_schema_search("gepa_schema_actions", "schema_actions_bad_source", model_calls=2)
        node = tree["nodes"][1]
        self.assertEqual(node["status"], "accepted", node)
        self.assertEqual(node["repair_error"], "control_source_not_approved")
        packets = [p for p in self.requests if p.get("controls", {}).get("level") == "L2"]
        self.assertEqual(len(packets), 2)
        self.assertEqual(packets[1]["phase"], "repair_control_source")
        self.assertNotIn("feedback", packets[1])
        self.assertEqual(packets[1]["experiences"], packets[0]["experiences"])
        self.assertEqual(tree["model_calls"], 2)

    def test_bad_control_source_repair_cannot_loop(self):
        tree = self.run_schema_search("gepa_schema_actions", "schema_actions_bad_source_always", model_calls=3)
        self.assertEqual(tree["nodes"][1]["error"], "control_source_not_approved")
        self.assertEqual(tree["model_calls"], 2)

    def test_bad_control_source_respects_model_budget(self):
        tree = self.run_schema_search("gepa_schema_actions", "schema_actions_bad_source", model_calls=1)
        self.assertEqual(tree["nodes"][1]["error"], "control_source_not_approved")
        self.assertEqual(tree["model_calls"], 1)

    def test_bad_control_source_respects_disabled_repair(self):
        tree = self.run_schema_search("gepa_schema_actions", "schema_actions_bad_source", model_calls=2, max_attempts=1)
        self.assertEqual(tree["nodes"][1]["error"], "control_source_not_approved")
        self.assertEqual(tree["model_calls"], 1)

    def test_unauthorized_control_draft_is_rejected_explicitly(self):
        self.behavior = "invalid_schema_action"
        self.config.update(max_level="L2", control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2))
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, _, _ = self.wait_search("L2")
        node = self.call("agent_evaluation.tree", job_id=job_id)["nodes"][1]
        self.assertEqual(node["status"], "error")
        self.assertEqual(node["error"], "control_operation_not_authorized")
        draft = self.root / "production/agent-evaluation" / job_id / "search/reflections/1.draft.json"
        self.assertEqual(json.loads(draft.read_text())["controls"]["action_rules"][0]["operation"], "calculate")

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
        actions = [action for row in packet["feedback"] for run in row["trial"]["evidence"]["runs"]
                   for action in run["actions"] if action["kind"] == "action.end"]
        # Control reflection keeps actual action evidence, not redundant model envelopes.
        self.assertFalse(any(a.get("semantic") == "model" for a in actions))
        controlled = [a for a in actions if a.get("action_control")]
        self.assertTrue(controlled)
        self.assertTrue(all(a["operation"] == "calculate" for a in controlled))
        self.assertTrue(all(a["action_control"]["query_event_id"].startswith("event-") for a in controlled))
        self.assertTrue(all(a["action_control"]["target_scope"] == "agent:eval-test/actor:assistant" for a in controlled))

    def test_l2_without_control_learning_reports_experience_only_search(self):
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
        self.assertIn("action_rules", tree["available_components"])
        self.assertEqual(tree["search_warnings"], [])
        self.assertEqual(job["search"]["fixed_controls"], ["observed_tool_required_arguments"])
        packet = next(p for p in self.requests if p.get("controls", {}).get("level") == "L2")
        self.assertEqual(packet["controls"]["action_primitives"][0]["required_arguments"],
                         ["$schema_required_nonempty"])

    def test_l3_generated_loop_rules_reach_actual_host(self):
        rule = self.exercise_controls("loops", "L3")
        self.assertEqual(int(rule["max_iterations"]), 12)
        self.assertEqual(int(rule["max_no_progress"]), 1)

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
        self.assertEqual(job["status"], "rejected", job)
        self.assertEqual(job["gate"]["reasons"], ["level_capability_not_observed"])
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertIn("loop_rules", tree["available_components"])
        self.assertIn("action_rules", tree["available_components"])
        candidate = json.loads((self.root / "production/agent-evaluation" / job_id / "candidate.json").read_text())
        generated = [rule for rule in candidate["policy"]["options"]["loop_rules"] if "source_experience" in rule]
        self.assertEqual(len(generated), 1)
        self.assertEqual(int(generated[0]["max_iterations"]), 12)
        self.assertEqual(int(generated[0]["max_no_progress"]), 1)

    def test_loop_evidence_survives_earlier_unrelated_failures(self):
        self.behavior = "loops"
        self.config["runtime_options"]["aggressive_controls"] = False
        self.config["runtime_options"]["loop_rules"][0]["max_no_progress"] = 3
        self.config.update(max_level="L3", evaluation_levels=["L3"], control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2))
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_loops_late"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(component_selector="round_robin", max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, _, _ = self.wait_search("L3")
        node = self.call("agent_evaluation.tree", job_id=job_id)["nodes"][1]
        self.assertEqual(node["components"], ["loop_rules"])
        self.assertTrue(node["control_preflight"]["passed"])
        self.assertGreater(sum(rule["applied"] for rule in node["control_coverage"]["rules"].values()), 0)
        controls = [action["loop_control"] for packet in self.requests for row in packet["feedback"]
                    for run in row["trial"]["evidence"]["runs"] for action in run.get("actions", [])
                    if action.get("loop_control")]
        self.assertTrue(any(item.get("observed_no_progress", 0) for item in controls), controls)

    def test_aggressive_loop_budget_is_generated_evaluated_and_preserved(self):
        self.config["runtime_options"]["aggressive_controls"] = True
        rule = self.exercise_controls("loops", "L3")
        self.assertEqual(int(rule["max_iterations"]), 2)
        self.assertEqual(int(rule["max_no_progress"]), 3)
        job_id, _, _ = self.wait_search("L3")
        candidate = json.loads((self.root / "production/agent-evaluation" / job_id / "candidate.json").read_text())
        self.assertTrue(candidate["policy"]["options"]["aggressive_controls"])
        self.assertTrue(any(p.get("controls", {}).get("aggressive_controls") for p in self.requests))

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
        self.assertEqual(tree["algorithm"], "gepa-reflective-dag-v4-constrained-portfolio")
        self.assertTrue(tree["baseline_validation"])
        self.assertIn("optimization", tree["nodes"][0])
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

    def test_transient_runner_failure_recovers_before_joint_merge(self):
        self.configure_merge(mode="gepa_merge_retry")
        marker = self.root / "runner-failed-once"
        self.env["GEPA_RUNNER_FAIL_ONCE"] = str(marker)
        self.evaluation["variants"][0]["pass_env"].append("GEPA_RUNNER_FAIL_ONCE")
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertEqual(job["status"], "passed", job)
        self.assertTrue(marker.is_file())
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["nodes"][3]["kind"], "merge")
        self.assertEqual(tree["nodes"][3]["parents"], [1, 2])
        self.assertEqual(tree["nodes"][3]["status"], "accepted")
        recovered = [value for value in tree["evaluations"].values() if value.get("source_report_refs")]
        self.assertTrue(recovered)

    def test_runner_timeout_retries_only_the_failed_trial(self):
        self.configure_merge(mode="gepa_merge_timeout_retry")
        marker = self.root / "runner-timed-out-once"
        self.env["GEPA_RUNNER_FAIL_ONCE"] = str(marker)
        self.evaluation["variants"][0]["pass_env"].append("GEPA_RUNNER_FAIL_ONCE")
        self.evaluation["timeout_ms"] = 500
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertEqual(job["status"], "passed", job)
        self.assertTrue(marker.is_file())
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        recovered = [value for value in tree["evaluations"].values() if value.get("source_report_refs")]
        self.assertTrue(recovered)
        self.assertTrue(any(value.get("trials_reserved") == 1 for value in tree["evaluations"].values()))

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
        automatic_regrade = fault == "invalid" and enabled
        if automatic_regrade:
            self.assertNotIn("error", tree["nodes"][1])
            self.assertEqual(len(self.cache_train_calls), 4)
            self.assertEqual(self.cache_train_calls.count(self.cache_train_calls[0]), 2)
            # This case injects one training judge fault. Other splits may have
            # their own bounded runner recovery without invalidating that fact.
            self.assertEqual(sum(value.get("attempt", 0) == 2 for value in tree["evaluations"].values()
                                 if value.get("split") == "train"), 1)
            self.assertTrue(any(value.get("source_reports") for value in tree["evaluations"].values()))
        else:
            self.assertEqual(tree["nodes"][1]["error"], "gepa_parent_evaluation_inconclusive")
            self.assertEqual(len(self.cache_train_calls), 4 if enabled else 6)
        if enabled and not automatic_regrade:
            self.assertEqual(self.cache_train_calls.count(self.cache_train_calls[0]), 2)
            self.assertEqual(self.cache_train_calls.count(self.cache_failed_task), 1)
            partial = {key for key, value in tree["evaluations"].items() if value["status"] == "inconclusive"}
            if fault == "invalid":
                self.assertTrue(any(partial & set(value.get("source_reports", [])) for value in tree["evaluations"].values()))
        elif not enabled:
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
        expected = repetitions * 4 + failures * (1 if enabled else repetitions * 2)
        self.assertEqual(len(self.seed_judge_tasks), expected)
        self.assertEqual(sum("乙类" in task for task in self.seed_judge_tasks), repetitions * (2 if enabled else 2 + failures))
        self.assertEqual((root / original["report"]).read_bytes(), original_bytes)
        if enabled:
            if fault == "uncertain":
                self.assertEqual(cached["trials_reserved"], 1)
                restored = json.loads((root / cached["report"]).read_text())
                self.assertEqual(restored["summary"]["reused_trials"], repetitions * 2 - 1)
                self.assertIn("resume_source", restored["summary"])
            else:
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
        key, cached = next((k, v) for k, v in tree["evaluations"].items()
                           if v["split"] == "val" and v["version"] == tree["seed_version"])
        self.assertEqual(cached["attempt"], 3)
        before = json.loads((root / "evaluations" / key / "attempt-1/report.json").read_text())
        after = json.loads((root / cached["report"]).read_text())
        self.assertEqual(after["summary"].get("regraded_trials"), 1)
        self.assertEqual(sum(p["task_input"].startswith("VALIDATION_PRIVATE") for p in self.judge_requests), 6)
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
        key, cached = next((k, v) for k, v in tree["evaluations"].items()
                           if v["split"] == "val" and v["version"] == tree["seed_version"])
        self.assertEqual(cached["attempt"], 2)
        before = json.loads((root / "evaluations" / key / "attempt-1/report.json").read_text())
        after = json.loads((root / cached["report"]).read_text())
        self.assertEqual(after["summary"].get("regraded_trials"), 1)
        self.assertEqual(sum(p["task_input"].startswith("VALIDATION_PRIVATE") for p in self.judge_requests), 5)
        old = {t["task_id"]: t for t in before["trials"]}
        for trial in after["trials"]:
            previous = old[trial["task_id"]]
            for field in ("trial_id", "duration_ms", "metrics", "execution_status"):
                self.assertEqual(trial[field], previous[field], field)
            self.assertEqual(trial["evidence"]["trace"], previous["evidence"]["trace"])

    def test_invalid_json_judge_recovers_automatically_without_agent_reexecution(self):
        self.judge_requests = []
        self.fail_search_judge, self.search_judge_failures = True, 1
        self.search_judge_invalid_json = True
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
        key, cached = next((k, v) for k, v in tree["evaluations"].items()
                           if v["split"] == "val" and v["version"] == tree["seed_version"])
        self.assertEqual(cached["attempt"], 2)
        before = json.loads((root / "evaluations" / key / "attempt-1/report.json").read_text())
        after = json.loads((root / cached["report"]).read_text())
        self.assertEqual(after["summary"].get("regraded_trials"), 1)
        self.assertEqual(sum(p["task_input"].startswith("VALIDATION_PRIVATE") for p in self.judge_requests), 5)
        old = {t["task_id"]: t for t in before["trials"]}
        for trial in after["trials"]:
            previous = old[trial["task_id"]]
            for field in ("trial_id", "duration_ms", "metrics", "execution_status"):
                self.assertEqual(trial[field], previous[field], field)
            self.assertEqual(trial["evidence"]["trace"], previous["evidence"]["trace"])

    def test_manual_resume_keeps_automatic_judge_recovery(self):
        self.judge_requests = []
        self.fail_search_judge, self.search_judge_status = True, 429
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
        self.assertEqual(job.get("error"), "gepa_model_rate_limited")
        root = self.root / "production/agent-evaluation" / job_id / "search"
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        key, cached = next(iter(tree["evaluations"].items()))
        before = json.loads((root / cached["report"]).read_text())
        self.assertEqual(cached["attempt"], 1)
        self.assertFalse(cached.get("automatic_recovery_used", False))
        self.search_judge_status, self.search_judge_failures = 503, 1
        self.call("agent_evaluation.retry", job_id=job_id)
        _, job, _ = self.wait_search()
        self.assertIn(job["status"], ("passed", "rejected"), job)
        cached = self.call("agent_evaluation.tree", job_id=job_id)["evaluations"][key]
        self.assertEqual(cached["attempt"], 3)
        self.assertTrue(cached["automatic_recovery_used"])
        after = json.loads((root / cached["report"]).read_text())
        self.assertEqual(after["summary"].get("regraded_trials"), 1)
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
        source = next(v for v in tree["evaluations"].values()
                      if v.get("split") == "val" and v["version"] == tree["seed_version"])
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

    def test_round_robin_repairs_mutation_of_an_unselected_component_once(self):
        self.configure_merge(component_selector="round_robin", max_rounds=1, max_model_calls=2, proposals_per_round=1)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["nodes"][1]["components"], ["action_rules"])
        self.assertNotEqual(tree["nodes"][1]["status"], "error", tree["nodes"][1])
        self.assertEqual(tree["nodes"][1]["repair_error"], "gepa_component_not_authorized")
        self.assertEqual(self.requests[-1]["phase"], "repair_component_selection")
        self.assertEqual(self.requests[-1]["selected_components"], ["action_rules"])
        self.assertEqual(len(self.requests[-1]["feedback"]), 1)
        request = next(p for p in self.requests
                       if p.get("selected_components") == ["action_rules"] and "phase" not in p)
        self.assertEqual(set(request["components"]), {"action_rules"})
        self.assertIn("optimization_contract", request)

    def test_component_repair_preserves_bounded_schedule_evidence(self):
        self.behavior = "schedule_bad_component"
        self.config.update(max_level="L2", evaluation_levels=["L2"], control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2),
            instrumentation=dict(level="L2", capabilities=["model", "actions", "schedule"]))
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_schedule"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(component_selector="round_robin", max_rounds=1, max_model_calls=2,
                      max_input_bytes=8192)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        node = tree["nodes"][1]
        self.assertEqual(node["repair_error"], "gepa_component_not_authorized")
        repair = self.requests[-1]
        self.assertEqual(repair["phase"], "repair_component_selection")
        self.assertEqual(len(repair.get("feedback", [])), 1, repair)
        self.assertIn(repair["feedback"][0], self.requests[0]["feedback"])
        self.assertNotIn("draft", repair)
        self.assertNotIn("HOLDOUT_PRIVATE", json.dumps(self.requests))
        self.assertNotIn("VALIDATION_PRIVATE", json.dumps(self.requests))
        self.assertLessEqual(len(json.dumps(repair, ensure_ascii=False, separators=(",", ":")).encode()), 8192)
        self.assertEqual(job["status"], "passed", job)
        rules = [r for r in node["control_coverage"]["rules"].values() if r["boundary"] == "schedule"]
        self.assertGreater(sum(r["applied"] for r in rules), 0)

    def test_round_robin_replaces_control_without_executable_evidence(self):
        self.config.update(max_level="L3", control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2,
            min_iterations=2, min_no_progress=1))
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(component_selector="round_robin", max_rounds=1,
            max_model_calls=1, proposals_per_round=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, _, _ = self.wait_search("L3")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        node = tree["nodes"][1]
        self.assertEqual(node["component_fallback"], {
            "from": "action_rules", "to": "experience:test-experience",
            "reason": "no_executable_training_evidence"})
        self.assertEqual(node["components"], ["experience:test-experience"])
        request = self.requests[-1]
        self.assertEqual(request["selected_components"], ["experience:test-experience"])
        self.assertEqual(set(request["components"]), {"experience:test-experience"})
        self.assertNotIn("controls", request)

    def test_failure_replan_gets_generation_opportunity_and_real_application(self):
        self.behavior = "failure_replan"
        self.config["runtime_options"]["aggressive_controls"] = False
        self.config["runtime_options"]["loop_rules"][0]["max_no_progress"] = 3
        self.config.update(max_level="L3", evaluation_levels=["L3"], control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2))
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_failure_replan"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(component_selector="round_robin", max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L3")
        self.assertEqual(job["status"], "rejected", job)
        self.assertEqual(job["gate"]["reasons"], ["level_capability_not_observed"])
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        node = tree["nodes"][1]
        self.assertEqual(node["components"], ["loop_rules"])
        self.assertTrue(tree["training_opportunities"]["loop_rules"])
        self.assertEqual(node["component_fallback"]["reason"], "unexplored_control_opportunity")
        self.assertTrue(node["control_preflight"]["passed"])
        self.assertGreater(sum(node["control_preflight"]["rules"].values()), 0)
        self.assertGreater(sum(r["applied"] for r in node["control_coverage"]["rules"].values()), 0)
        loops = [a["loop_control"] for p in self.requests for row in p["feedback"]
                 for run in row["trial"]["evidence"]["runs"] for a in run["actions"] if a["kind"] == "loop.decision"]
        self.assertTrue(any(q["failed_actions"] > 0 and q["repeated_failure_steps"] > 0 for q in loops))

    def test_control_feedback_budget_preserves_structured_loop_evidence(self):
        self.behavior = "failure_replan"
        self.config["runtime_options"]["aggressive_controls"] = True
        self.config["runtime_options"]["loop_rules"][0]["max_no_progress"] = 3
        self.config.update(max_level="L3", evaluation_levels=["L3"], control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2))
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_failure_replan"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(component_selector="round_robin", max_rounds=1, max_model_calls=1,
                      max_input_bytes=16384)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, _, _ = self.wait_search("L3")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        node = tree["nodes"][1]
        packet = next(packet for packet in self.requests if packet["selected_components"] == ["loop_rules"])
        encoded = json.dumps(packet, ensure_ascii=False, separators=(",", ":")).encode()
        self.assertLessEqual(len(encoded), config["max_input_bytes"])
        self.assertTrue(any(row["trial"].get("control_evidence_preserved") for row in packet["feedback"]))
        loops = [action["loop_control"] for row in packet["feedback"]
                 for run in row["trial"]["evidence"]["runs"] for action in run["actions"]
                 if action["kind"] == "loop.decision"]
        self.assertTrue(any(item["repeated_failure_steps"] > 0 for item in loops), loops)
        self.assertTrue(node["control_preflight"]["passed"], node)
        self.assertGreater(sum(node["control_preflight"]["rules"].values()), 0)

    def schedule_dataset(self, dependent_ids, reviewed=True):
        tasks = [json.loads(line) for line in (self.root / "gepa-tasks.jsonl").read_text().splitlines()]
        for task in tasks:
            prefix = task["task_input"].split("：", 1)[0]
            if task["task_id"] in dependent_ids:
                task["task_input"] = f'{prefix}：生成视频时缺少图片素材，先准备素材，再生成图像；独立计算继续。'
                if reviewed:
                    task["attributes"] = dict(dependency="prepare_before_image", stop_on_failure=True)
            else:
                task["task_input"] = f'{prefix}：生成视频时缺少图片素材；准备和图像生成互不依赖。'
        (self.root / "gepa-tasks.jsonl").write_text(
            "\n".join(json.dumps(task, ensure_ascii=False) for task in tasks), encoding="utf-8")
        result = subprocess.run([str(fixture.CLI), "dataset", "build", "--config", str(self.root / "gepa-dataset.json"),
            "--output-root", str(self.root / "dependency-dataset")], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assessment["dataset"] = "dependency-dataset"
        fixture.write(self.root / "agent-evaluation.json", self.assessment)

    def test_schedule_component_generates_and_really_orders_skills(self):
        self.schedule_dataset({"train-a", "train-b", "val-a", "val-b", "holdout-a", "holdout-b"})
        self.behavior = "schedule"
        self.config.update(max_level="L2", evaluation_levels=["L2"], control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2),
            instrumentation=dict(level="L2", capabilities=["model", "actions", "schedule"]))
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = getattr(self, "schedule_host_mode", "gepa_schedule")
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(component_selector="round_robin", max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertEqual(job["status"], "passed", job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        node = tree["nodes"][1]
        self.assertEqual(node["components"], ["schedule_rules"])
        self.assertTrue(node["control_preflight"]["passed"])
        rules = [rule for rule in node["control_coverage"]["rules"].values() if rule["boundary"] == "schedule"]
        self.assertTrue(rules)
        self.assertGreater(sum(rule["applied"] for rule in rules), 0)
        plans = [plan for packet in self.requests for row in packet["feedback"]
                 for run in row["trial"]["evidence"]["runs"] for plan in run.get("schedule_plans", [])]
        self.assertTrue(plans)
        self.assertNotIn("HOLDOUT_PRIVATE", json.dumps(self.requests))
        self.assertNotIn("VALIDATION_PRIVATE", json.dumps(self.requests))

    def test_schedule_evidence_survives_earlier_single_action_batches(self):
        self.schedule_host_mode = "gepa_schedule_late"
        self.test_schedule_component_generates_and_really_orders_skills()
        plans = [plan for packet in self.requests for row in packet["feedback"]
                 for run in row["trial"]["evidence"]["runs"] for plan in run.get("schedule_plans", [])]
        self.assertTrue(any(len(plan.get("actions", [])) > 1 for plan in plans), plans)

    def test_schedule_training_prefers_reviewed_dependency_over_parallel_batch(self):
        self.schedule_dataset({"train-b", "val-b", "holdout-b"})
        self.behavior = "schedule"
        self.config.update(max_level="L2", evaluation_levels=["L2"], control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2),
            instrumentation=dict(level="L2", capabilities=["model", "actions", "schedule"]))
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_schedule"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(component_selector="round_robin", minibatch_size=1, max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertEqual(job["status"], "passed", job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["nodes"][1]["tasks"], ["train-b"])
        self.assertEqual(self.requests[0]["feedback"][0]["task_id"], "train-b")
        self.assertTrue(tree["nodes"][1]["control_preflight"]["passed"])
        reports = [entry for entry in tree["evaluations"].values()
                   if entry["split"] == "val" and entry["version"] == job["version"]]
        self.assertEqual(len(reports), 1)
        report = json.loads((self.root / "production/agent-evaluation" / job_id / "search" /
                             reports[0]["report"]).read_text())
        statuses = {trial["task_id"]: [event["attributes"]["decision"]["schedule"]["status"]
            for event in trial["evidence"]["trace"]["events"]
            if event.get("attributes", {}).get("type") == "policy.query"
            and event["attributes"]["decision"]["query"].get("boundary") == "schedule"]
            for trial in report["trials"]}
        self.assertEqual(statuses, {"val-a": ["unchanged"], "val-b": ["scheduled"]})

    def test_schedule_guard_repair_uses_only_observed_training_text(self):
        self.schedule_dataset({"train-b", "val-b", "holdout-b"})
        self.behavior = "schedule_overstrict"
        self.config.update(max_level="L2", evaluation_levels=["L2"], control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2),
            instrumentation=dict(level="L2", capabilities=["model", "actions", "schedule"]))
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_schedule"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(component_selector="round_robin", minibatch_size=1, max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertEqual(job["status"], "passed", job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        node = tree["nodes"][1]
        self.assertTrue(node["control_preflight"]["passed"], node)
        self.assertEqual(node["schedule_guard_repair"][0]["removed_terms"], 1)
        self.assertEqual(tree["model_calls"], 1)
        reports = [entry for entry in tree["evaluations"].values()
                   if entry["split"] == "val" and entry["version"] == job["version"]]
        report = json.loads((self.root / "production/agent-evaluation" / job_id / "search" /
                             reports[0]["report"]).read_text())
        statuses = {trial["task_id"]: [event["attributes"]["decision"]["schedule"]["status"]
            for event in trial["evidence"]["trace"]["events"]
            if event.get("attributes", {}).get("type") == "policy.query"
            and event["attributes"]["decision"]["query"].get("boundary") == "schedule"]
            for trial in report["trials"]}
        self.assertEqual(statuses, {"val-a": ["unchanged"], "val-b": ["scheduled"]})

    def test_schedule_training_prefers_failed_case_without_dependency_annotation(self):
        self.schedule_dataset({"train-a", "val-a", "holdout-a"}, reviewed=False)
        self.behavior = "schedule"
        self.config.update(max_level="L2", evaluation_levels=["L2"], control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2),
            instrumentation=dict(level="L2", capabilities=["model", "actions", "schedule"]))
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_schedule"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(component_selector="round_robin", minibatch_size=1, max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertEqual(job["status"], "passed", job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["nodes"][1]["tasks"], ["train-a"])
        self.assertEqual(self.requests[0]["feedback"][0]["task_id"], "train-a")

    def test_schedule_reflection_skips_independent_case_without_annotation(self):
        self.schedule_dataset({"train-b", "val-b", "holdout-b"}, reviewed=False)
        self.behavior = "schedule"
        self.config.update(max_level="L2", evaluation_levels=["L2"], control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2),
            instrumentation=dict(level="L2", capabilities=["model", "actions", "schedule"]))
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_schedule"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(component_selector="round_robin", minibatch_size=2, max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertEqual(job["status"], "passed", job)
        self.assertEqual([row["task_id"] for row in self.requests[0]["feedback"]], ["train-b"])

    def test_schedule_reflection_excludes_independent_batch_when_dependency_exists(self):
        self.schedule_dataset({"train-b", "val-b", "holdout-b"})
        self.behavior = "schedule"
        self.config.update(max_level="L2", evaluation_levels=["L2"], control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2),
            instrumentation=dict(level="L2", capabilities=["model", "actions", "schedule"]))
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_schedule"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(component_selector="round_robin", minibatch_size=2, max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertEqual(job["status"], "passed", job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["nodes"][1]["tasks"], ["train-a", "train-b"])
        self.assertEqual([row["task_id"] for row in self.requests[0]["feedback"]], ["train-b"])

    def test_control_source_approval_survives_candidate_composition(self):
        path = self.root / "production/by-user/user-a/memory/evolution-memory.pb"
        protoc_args = [fixture.PROTOC, "-I" + str(fixture.SOURCE / "proto"),
                       "-I" + str(Path(fixture.PROTOC).resolve().parent.parent / "include"),
                       "hybrid_evaluation.proto"]
        decoded = subprocess.run(protoc_args + ["--decode=agentflow.trajectory.evaluation.EvolutionMemoryStore"],
            input=path.read_bytes(), capture_output=True, timeout=10)
        self.assertEqual(decoded.returncode, 0, decoded.stderr)
        control = '''
entries { memory_id: "test-control" status: EVOLUTION_MEMORY_STATUS_CANDIDATE
 candidate { schema_version: 1 candidate_id: "control-candidate" generator_version: "scoped-experience-generator-v1"
 target_type: CANDIDATE_TARGET_TYPE_SKILL_ORCHESTRATION target_scope: "agent:eval-test/actor:assistant"
 status: EVOLUTION_CANDIDATE_STATUS_PROPOSED trigger_pattern: "图像技能依赖准备结果"
 observed_problem: "准备尚未完成就启动图像分支" proposed_change: "准备成功后再启动图像分支。"
 support_user_count: 1 support_evidence { evidence_id: "control-evidence"
 source { user_id: "user-a" dialogue_id: "control-run" } } }}
'''
        encoded = subprocess.run(protoc_args + ["--encode=agentflow.trajectory.evaluation.EvolutionMemoryStore"],
            input=decoded.stdout + control.encode(), capture_output=True, timeout=10)
        self.assertEqual(encoded.returncode, 0, encoded.stderr)
        path.write_bytes(encoded.stdout)
        self.control_source_id = "test-control"
        self.test_schedule_component_generates_and_really_orders_skills()
        job_id, job, _ = self.wait_search("L2")
        folder = self.root / "production/agent-evaluation" / job_id
        candidate = json.loads((folder / "candidate.json").read_text())
        options = candidate["policy"]["options"]
        self.assertIn("test-control", [item["memory_id"] for item in options["approved_experiences"]])
        self.assertTrue(any(rule["source_experience"]["memory_id"] == "test-control"
                            for rule in options["schedule_rules"]))
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertNotIn("experience:test-control", tree["available_components"])
        self.assertTrue(any(any(source["memory_id"] == "test-control"
                               for source in packet.get("controls", {}).get("control_sources", []))
                            for packet in self.requests))
        self.assertTrue(job["published"], job)

    def test_l3_publication_requires_actual_experience_schedule_and_loop_application(self):
        self.behavior = "full_controls"
        self.config["runtime_options"]["aggressive_controls"] = True
        self.config.update(max_level="L3", evaluation_levels=["L3"], control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2),
            instrumentation=dict(level="L3", capabilities=["model", "actions", "schedule", "loop"]))
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_full"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(component_selector="model_choice", max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, status = self.wait_search("L3")
        self.assertEqual(job["status"], "passed", job)
        coverage = job["gate"]["control_coverage"]
        self.assertTrue(coverage["level_complete"], coverage)
        self.assertEqual(coverage["unobserved_capabilities"], [])
        for boundary in ("llm", "schedule", "loop"):
            self.assertTrue(coverage["capabilities"][boundary]["required"])
            self.assertGreater(coverage["capabilities"][boundary]["applied"], 0)
        candidate = json.loads((self.root / "production/agent-evaluation" / job_id / "candidate.json").read_text())
        self.assertTrue(candidate["policy"]["options"]["aggressive_controls"])
        for component in ("schedule_rules", "loop_rules"):
            self.assertTrue(any("source_experience" in rule for rule in candidate["policy"]["options"][component]))
        self.assertTrue(status["publications"])
        self.assertNotIn("HOLDOUT_PRIVATE", json.dumps(self.requests))
        self.assertNotIn("VALIDATION_PRIVATE", json.dumps(self.requests))

        def deployed_run(policy_gate, conversation):
            run = self.call("run.begin", conversation_id=conversation,
                input="生成视频时缺少图片素材，核验准备步骤。", level="L3",
                capabilities=["model", "actions", "schedule", "loop"])
            self.assertEqual(run.get("agent_version_id"), job["version"], run)
            self.assertEqual(run.get("policy_gate"), policy_gate, run)
            rid = run["run_id"]
            control = self.call("loop.check", run_id=rid, loop_id="production-worker", state={"phase": 1})
            self.call("loop.commit", run_id=rid, control_id=control["control_id"], decision=control["decision"])
            original = {"messages": [{"role": "user", "content": "生成视频时缺少图片素材，核验准备步骤。"}]}
            model = self.call("activity.begin", run_id=rid, kind="model", name="production-model", input=original,
                binding={"pointer": "/messages", "mode": "system_message"}, control_id=control["control_id"])
            self.assertNotEqual(model["input"], original, "published L1 must reach the model input")
            self.call("activity.end", run_id=rid, activity_id=model["activity_id"], status="completed",
                output="check inputs", actual_input=model["input"], executed=True)
            action = self.call("activity.begin", run_id=rid, kind="action", name="calculate", input={"value": 1},
                control_id=control["control_id"])
            self.call("activity.end", run_id=rid, activity_id=action["activity_id"], status="completed",
                output={"success": False}, executed=True)
            first_failure = self.call("loop.check", run_id=rid, loop_id="production-worker", state={"phase": 2})
            self.assertEqual(first_failure["decision"], "continue", first_failure)
            self.call("loop.commit", run_id=rid, control_id=first_failure["control_id"], decision="continue")
            retry = self.call("activity.begin", run_id=rid, kind="action", name="calculate", input={"value": 2},
                control_id=first_failure["control_id"])
            self.call("activity.end", run_id=rid, activity_id=retry["activity_id"], status="completed",
                output={"success": False}, executed=True)
            recovery = self.call("loop.check", run_id=rid, loop_id="production-worker", state={"phase": 3})
            self.assertEqual(recovery["decision"], "replan", recovery)
            self.call("loop.commit", run_id=rid, control_id=recovery["control_id"], decision="replan")
            model = self.call("activity.begin", run_id=rid, kind="model", name="production-recovery", input=original,
                binding={"pointer": "/messages", "mode": "system_message"}, control_id=recovery["control_id"])
            self.assertTrue(any(recovery["context"] in m.get("content", "") for m in model["input"]["messages"]))
            self.call("activity.end", run_id=rid, activity_id=model["activity_id"], status="completed",
                output="recover", actual_input=model["input"], executed=True)
            schedule = self.call("activity.begin", run_id=rid, kind="schedule", name="production-skills", input={"actions": [
                {"action_id": "image", "operation": "image", "arguments": {}},
                {"action_id": "prepare", "operation": "prepare", "arguments": {}}]})
            view = schedule
            for expected in ("prepare", "image"):
                self.assertEqual([item["action_id"] for item in view["ready"]], [expected])
                self.call("schedule.observe", run_id=rid, activity_id=schedule["activity_id"],
                    input={"action_id": expected, "status": "running"})
                view = self.call("schedule.observe", run_id=rid, activity_id=schedule["activity_id"],
                    input={"action_id": expected, "status": "succeeded", "output": "done"})
            self.assertTrue(view["finished"])
            self.call("activity.end", run_id=rid, activity_id=schedule["activity_id"], status="completed", output="done")
            self.call("run.end", run_id=rid, status="completed", output="done")
            fixture.AgentEvaluationTests.tearDown(self)
            traces = list(Path(self.config["data_dir"]).rglob(rid + ".trace.json"))
            self.assertEqual(len(traces), 1)
            trace = json.loads(traces[0].read_text())
            applied = {event["attributes"]["receipt"]["boundary"] for event in trace["events"]
                       if event["kind"] == "advice.applied" and event["application"]["status"] == "applied"}
            self.assertTrue({"llm", "schedule", "loop"} <= applied, applied)
            self.assertNotIn("evaluation_trial_id", traces[0].read_text())

        # The published candidate must also work outside the evaluation host.
        deployed_run("published", "production")
        count = len(self.requests)
        deployment = self.root / "standalone"
        deployment.mkdir()
        bundle = self.root / "production/agent-evaluation" / job_id / "search/published-version.json"
        shutil.copyfile(bundle, deployment / "policy.json")
        self.config = dict(schema_version=1, agent_id="eval-test", actors=["assistant"],
            principals=self.config["principals"], port=0, max_level="L3", auto_evaluate=False,
            data_dir=str(deployment / "data"), policy_version_file=str(deployment / "policy.json"))
        fixture.write(self.root / "service.json", self.config)
        for conversation in ("offline-first", "offline-restarted"):
            self.start()
            deployed_run("offline_version", conversation)
        self.assertEqual(len(self.requests), count, "deployment must not call the reflection model")
        self.assertFalse(list(deployment.rglob("*memory*.pb")))
        self.assertFalse((deployment / "data/agent-evaluation").exists())

    def test_round_robin_handles_summarized_training_feedback(self):
        self.behavior = "no_change"
        self.config.update(max_level="L3", evaluation_levels=["L3"], control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2))
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(component_selector="round_robin", max_rounds=1, max_model_calls=1,
                      max_input_bytes=8192, unknown_token_policy="worst_case")
        fixture.write(self.root / "gepa.json", config)
        tasks = [json.loads(line) for line in (self.root / "gepa-tasks.jsonl").read_text().splitlines()]
        for task in tasks:
            task["followup_inputs"] = ["再次核验输入。"] * 20
        (self.root / "gepa-tasks.jsonl").write_text(
            "\n".join(json.dumps(task, ensure_ascii=False) for task in tasks), encoding="utf-8")
        result = subprocess.run([str(fixture.CLI), "dataset", "build", "--config", str(self.root / "gepa-dataset.json"),
            "--output-root", str(self.root / "gepa-long-dataset")], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assessment["dataset"] = "gepa-long-dataset"
        fixture.write(self.root / "agent-evaluation.json", self.assessment)
        self.start()
        job_id, job, _ = self.wait_search("L3")
        nodes = self.call("agent_evaluation.tree", job_id=job_id)["nodes"]
        self.assertGreater(len(nodes), 1, job)
        node = nodes[1]
        self.assertNotEqual(node["status"], "error", node)
        self.assertEqual(len(self.requests), 1)
        packet = self.requests[0]
        # The smaller experience packet is rebuilt after the control fallback;
        # it may retain a full row and omit its sibling instead of summarizing it.
        self.assertTrue(len(packet["feedback"]) < config["minibatch_size"]
                        or any(row["trial"].get("evidence_truncated") for row in packet["feedback"]))
        self.assertLessEqual(len(json.dumps(packet, ensure_ascii=False, separators=(",", ":")).encode()), 8192)
        self.assertEqual(node["component_fallback"]["reason"], "no_executable_training_evidence")

    def test_untriggerable_rule_is_stopped_before_running_candidate(self):
        self.behavior = "untriggerable_loop"
        self.config["runtime_options"]["aggressive_controls"] = True
        self.config.update(max_level="L3", evaluation_levels=["L3"], control_learning=dict(
            enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2))
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_failure_replan"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(component_selector="round_robin", max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, _, _ = self.wait_search("L3")
        node = self.call("agent_evaluation.tree", job_id=job_id)["nodes"][1]
        self.assertEqual(node["status"], "untriggerable_controls", node)
        self.assertEqual(node["reason"], "no_differential_training_trigger")
        self.assertFalse(node["control_preflight"]["passed"])
        self.assertNotIn("screening", node)
        self.assertNotIn("version", node)

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
