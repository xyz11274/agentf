# test_gepa_search 回归验证：检查包内协议、边界条件与失败处理。
# 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
# 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

"""反思搜索回归使用真实采集器和宿主执行，模型响应来自本地测试桩。"""

import hashlib
import http.server
import json
import os
from pathlib import Path
import subprocess
import shutil
import signal
import threading
import time
import unittest

import test_agent_evaluation as fixture


class GepaSearchTests(unittest.TestCase):
    start = fixture.AgentEvaluationTests.start
    call = fixture.AgentEvaluationTests.call
    add_memory = fixture.AgentEvaluationTests.add_memory

    # 测试辅助函数：model_draft_content。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def model_draft_content(self, packet, content):
        if not self.incremental_controls:
            return content
        draft = json.loads(content)
        malformed = getattr(self, "unwrapped_controls", "")
        if (
            malformed
            and "controls" in draft
            and (packet.get("phase") != "repair_draft_format" or malformed == "always")
        ):
            # 复现实际模型把控制数组直接放在顶层、漏掉 controls 包装的返回。
            draft.update(draft.pop("controls"))
        violation = getattr(self, "incremental_component_violation", "")
        if (
            violation
            and packet["selected_components"] == ["schedule_rules"]
            and (
                packet.get("phase") != "repair_component_selection"
                or violation == "always"
            )
        ):
            # 复现真实草稿：获选编排组件，却提出动作派发预算，来源本身合法。
            draft = dict(
                reason="已完成图像生成后不应再次派发同一操作。",
                control_updates=[
                    dict(
                        component="action_rules",
                        op="add",
                        value=dict(
                            memory_id="test-experience",
                            primitive_id="operation_budget",
                            operation="image",
                            max_attempts=1,
                            attempt_key_arguments=[],
                            reason="避免已经完成工作的重复派发。",
                        ),
                    )
                ],
            )
        if "controls" in draft:
            updates = []
            for component, values in draft.pop("controls").items():
                parent = packet.get("components", {}).get(component, [])
                for index, value in enumerate(values):
                    update = dict(
                        component=component,
                        op="set" if index < len(parent) else "add",
                        value=value,
                    )
                    if update["op"] == "set":
                        update["index"] = index
                    updates.append(update)
                updates.extend(
                    dict(component=component, op="remove", index=index)
                    for index in range(len(values), len(parent))
                )
            draft["control_updates"] = updates
        self.model_drafts.append(
            dict(
                phase=packet.get("phase"),
                level=packet.get("controls", {}).get("level", "L1"),
                selected_components=packet["selected_components"],
                draft=draft,
            )
        )
        return json.dumps(draft, ensure_ascii=False)

    # 测试辅助函数：setUp。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def setUp(self):
        fixture.AgentEvaluationTests.setUp(self)
        self.add_memory()
        self.requests = []
        self.incremental_controls = False
        self.model_drafts = []
        self.release = threading.Event()
        self.release.set()
        self.behavior = "normal"
        self.fenced = False
        self.pause_on = None
        owner = self

        class Model(http.server.BaseHTTPRequestHandler):
            # 测试辅助函数：log_message。
            # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
            def log_message(self, *args):
                pass

            # 测试辅助函数：do_POST。
            # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
            def do_POST(self):
                request = json.loads(
                    self.rfile.read(int(self.headers["Content-Length"]))
                )
                packet = json.loads(request["messages"][-1]["content"])
                if packet.get("kind") == "task-judge-consistency.v1":
                    # 一致性核验复用相同 HTTP 模型，但不混入主裁判调用计数。
                    conflicts = []
                    conflict_prefixes = getattr(
                        owner,
                        "consistency_judge_prefixes",
                        ("VALIDATION_PRIVATE 甲类",),
                    )
                    affected = any(
                        any(text.startswith(prefix) for prefix in conflict_prefixes)
                        for text in packet["inputs"].values()
                    )
                    matched_prefix = next(
                        (
                            prefix
                            for prefix in conflict_prefixes
                            if any(
                                text.startswith(prefix)
                                for text in packet["inputs"].values()
                            )
                        ),
                        "",
                    )
                    failed_prefixes = getattr(owner, "consistency_failed_prefixes", set())
                    fail_once = getattr(owner, "consistency_fail_each_prefix_once", False)
                    if (getattr(owner, "consistency_judge_failures", 0) and affected
                            and (not fail_once or matched_prefix not in failed_prefixes)
                            and any((owner.root / "production/agent-evaluation").glob(
                                "*/search/tree.json"))):
                        owner.consistency_judge_failures -= 1
                        if fail_once:
                            failed_prefixes.add(matched_prefix)
                            owner.consistency_failed_prefixes = failed_prefixes
                        conflicts = (
                            "invalid-array"
                            if getattr(owner, "consistency_bad_reply", False)
                            else [dict(id=packet["goals"][0]["id"],
                                       issue=getattr(
                                           owner,
                                           "consistency_issue",
                                           "missing_requirement_waived",
                                       ))]
                        )
                    response = json.dumps(dict(choices=[dict(message=dict(
                        content=json.dumps(dict(conflicts=conflicts))))])).encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(response)))
                    self.end_headers()
                    self.wfile.write(response)
                    return
                if "loop_inventory" in packet:
                    owner.judge_requests.append(packet)
                    if (
                        getattr(owner, "fail_search_judge", False)
                        and packet["task_input"].startswith(
                            getattr(owner, "fail_search_prefix", "")
                        )
                        and any(
                            (owner.root / "production/agent-evaluation").glob(
                                "*/search/tree.json"
                            )
                        )
                        and getattr(owner, "search_judge_failures", 1)
                    ):
                        if hasattr(owner, "search_judge_failures"):
                            owner.search_judge_failures -= 1
                        if getattr(owner, "search_judge_invalid_json", False):
                            response = json.dumps(
                                dict(choices=[dict(message=dict(content="not-json"))])
                            ).encode()
                            self.send_response(200)
                            self.send_header("Content-Type", "application/json")
                            self.send_header("Content-Length", str(len(response)))
                            self.end_headers()
                            self.wfile.write(response)
                            return
                        self.send_response(getattr(owner, "search_judge_status", 503))
                        self.send_header("Content-Length", "0")
                        self.end_headers()
                        return
                    inventory = packet["loop_inventory"]
                    key = next(iter(inventory), None)
                    judgment = dict(
                        verdict="passed",
                        reason="测试评分器确认任务输出。",
                        evidence_paths=["/output"],
                        loop_review=dict(
                            status="complete" if key else "uncertain",
                            wasted_iterations=[
                                dict(
                                    iteration_id=key,
                                    reason="测试中将本轮标为可省去，用于核验评分口径。",
                                    evidence_event_ids=[inventory[key]["event_ids"][0]],
                                )
                            ]
                            if key
                            else [],
                        ),
                    )
                    if "assessment" in packet:
                        judgment["goal_verdicts"] = [
                            dict(id=g["id"], status="passed", reason="目标按测试事实完成。",
                                 evidence_paths=["/traces"])
                            for g in packet["assessment"]["goals"]
                        ]
                        if "turn_expectations" in packet:
                            judgment["turn_verdicts"] = [
                                dict(turn=t["turn"], verdict="passed", reason="当轮完成。")
                                for t in packet["turn_expectations"]
                            ]
                        # 一次完整 JSON 的总结矛盾，只能重评旧执行，不重跑 Agent。
                        if (getattr(owner, "summary_judge_failures", 0)
                                and packet["task_input"].startswith("VALIDATION_PRIVATE 甲类")
                                and any((owner.root / "production/agent-evaluation").glob(
                                    "*/search/tree.json"))):
                            owner.summary_judge_failures -= 1
                            judgment["verdict"] = "failed"
                    if getattr(owner, "rank_feedback", False) and packet[
                        "task_input"
                    ].startswith("TRAIN_VISIBLE 甲类"):
                        judgment["loop_review"]["wasted_iterations"] = []
                    if (
                        getattr(owner, "seed_fault", None)
                        and packet["task_input"].startswith("VALIDATION_PRIVATE")
                        and any(
                            (owner.root / "production/agent-evaluation").glob(
                                "*/search/tree.json"
                            )
                        )
                    ):
                        owner.seed_judge_tasks.append(packet["task_input"])
                        if "乙类" in packet["task_input"]:
                            judgment["verdict"] = "failed"
                        elif owner.seed_failures and owner.seed_fault != "tokens":
                            owner.seed_failures -= 1
                            judgment["verdict"] = "uncertain"
                    if (
                        getattr(owner, "cache_fault", None)
                        and packet["task_input"].startswith("TRAIN_VISIBLE")
                        and any(
                            (owner.root / "production/agent-evaluation").glob(
                                "*/search/tree.json"
                            )
                        )
                    ):
                        owner.cache_train_calls.append(packet["task_input"])
                        if len(owner.cache_train_calls) == 1:
                            if owner.cache_fault == "invalid":
                                judgment["loop_review"]["wasted_iterations"][0][
                                    "iteration_id"
                                ] = "unknown-iteration"
                            else:
                                judgment["loop_review"] = dict(
                                    status="uncertain", wasted_iterations=[]
                                )
                        else:
                            if not owner.cache_failed_task:
                                owner.cache_failed_task = packet["task_input"]
                            if packet["task_input"] == owner.cache_failed_task:
                                judgment["verdict"] = "failed"
                    response = json.dumps(
                        dict(choices=[dict(message=dict(content=json.dumps(judgment)))])
                    ).encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(response)))
                    self.end_headers()
                    self.wfile.write(response)
                    return
                owner.requests.append(packet)
                # 保存实际发给模型的模板，用于验证组件隔离，而非只检查本地配置文件。
                if not hasattr(owner, "reflection_systems"):
                    owner.reflection_systems = []
                owner.reflection_systems.append(request["messages"][0]["content"])
                if packet.get("phase") == "repair_component_selection":
                    selected = packet["selected_components"]
                    if selected == ["schedule_rules"]:
                        plans = [
                            plan
                            for row in packet.get("feedback", [])
                            for run in row["trial"].get("evidence", {}).get("runs", [])
                            for plan in run.get("schedule_plans", [])
                        ]
                        draft = dict(
                            reason="修正请求缺少实际编排证据。", experiences=[]
                        )
                        if plans:
                            draft = dict(
                                reason="根据保留的训练批次修正获选编排组件。",
                                controls=dict(
                                    schedule_rules=[
                                        dict(
                                            memory_id="test-experience",
                                            before_operation="prepare",
                                            after_operation="image",
                                            require_success=True,
                                            task_terms=["生成视频时缺少图片素材"],
                                            exclude_terms=["互不依赖"],
                                            reason="准备成功后再生成图像。",
                                        )
                                    ]
                                ),
                            )
                    elif selected == ["action_rules"]:
                        draft = dict(
                            reason="按获选动作组件修正候选。",
                            controls=dict(
                                action_rules=[
                                    dict(
                                        memory_id="test-experience",
                                        operation="calculate",
                                        required_arguments=["verified"],
                                        reason="缺少核验参数时返回可恢复的拒绝。",
                                    )
                                ]
                            ),
                        )
                    elif selected == ["loop_rules"]:
                        draft = dict(
                            reason="按获选循环组件修正候选。",
                            controls=dict(
                                loop_rules=[
                                    dict(
                                        memory_id="test-experience",
                                        max_iterations=12,
                                        max_no_progress=1,
                                        reason="根据重复状态证据收紧循环。",
                                    )
                                ]
                            ),
                        )
                    elif getattr(owner, "leak_during_repair", False):
                        # 专用隐私回归：越权草稿触发纠错后，模型仍可能返回真实敏感值。
                        # 纠错发生在同一节点内；它也必须经过隐私拒绝，不能只检查首次响应。
                        draft = dict(
                            reason="核验纠错响应仍受隐私约束。",
                            experience=dict(
                                memory_id="test-experience",
                                content="凭据 " + owner.env["GEPA_LEAK_SECRET"],
                            ),
                        )
                    else:
                        draft = dict(
                            reason="没有获选经验的相关证据，保留父版本。",
                            experiences=[],
                        )
                    content = owner.model_draft_content(
                        packet, json.dumps(draft, ensure_ascii=False)
                    )
                    response = json.dumps(
                        dict(
                            choices=[
                                dict(
                                    finish_reason="stop", message=dict(content=content)
                                )
                            ]
                        )
                    ).encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(response)))
                    self.end_headers()
                    self.wfile.write(response)
                    return
                if getattr(owner, "fail_reflection_rate_limit", False):
                    self.send_response(429)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                level = (packet.get("controls") or {}).get("level", "L1")
                ordinal = sum(
                    (p.get("controls") or {}).get("level", "L1") == level
                    for p in owner.requests
                )
                if owner.pause_on is None or owner.pause_on == (level, ordinal):
                    owner.release.wait(30)
                if owner.behavior.startswith("combee"):
                    component = packet["selected_components"][0]
                    if component == "action_rules":
                        draft = dict(
                            reason="汇总工具前置核验的训练证据。",
                            controls=dict(
                                action_rules=[
                                    dict(
                                        memory_id="test-experience",
                                        operation="calculate",
                                        required_arguments=["verified"],
                                        reason="缺少核验参数时返回可恢复的拒绝。",
                                    )
                                ]
                            ),
                        )
                    else:
                        if packet.get("phase") == "combee_reduce":
                            text = (
                                "缺少素材时核验甲类任务，并核验乙类任务，独立计算继续。"
                            )
                        else:
                            text = (
                                "缺少素材时先核验甲类任务。"
                                if ordinal % 2
                                else "缺少素材时先核验乙类任务。"
                            )
                        if owner.behavior == "combee_leak":
                            text += owner.env["GEPA_LEAK_SECRET"]
                        draft = dict(
                            reason="根据分组训练证据修复核验步骤。",
                            experience=dict(memory_id="test-experience", content=text),
                        )
                    content = json.dumps(draft, ensure_ascii=False)
                elif owner.behavior == "merge":
                    level = packet.get("controls", {}).get("level", "L1")
                    n = sum(
                        p.get("controls", {}).get("level", "L1") == level
                        for p in owner.requests
                    )
                    if n == 1 or level == "L1":
                        text = (
                            "缺少素材时先核验甲类任务。"
                            if n == 1
                            else "缺少素材时先核验乙类任务。"
                        )
                        content = json.dumps(
                            dict(
                                reason="根据训练任务的反馈，保留一种独立改进。",
                                experience=dict(
                                    memory_id="test-experience", content=text
                                ),
                            ),
                            ensure_ascii=False,
                        )
                    else:
                        content = json.dumps(
                            dict(
                                reason="对另一类任务补充工具调用前置核验。",
                                controls=dict(
                                    action_rules=[
                                        dict(
                                            memory_id="test-experience",
                                            operation="calculate",
                                            required_arguments=["verified"],
                                            reason="缺少核验参数时返回可恢复的拒绝。",
                                        )
                                    ]
                                ),
                            ),
                            ensure_ascii=False,
                        )
                elif owner.behavior.startswith("schema_actions"):
                    source = "test-experience"
                    if "bad_source" in owner.behavior and (
                        packet.get("phase") != "repair_control_source"
                        or owner.behavior.endswith("always")
                    ):
                        source = "bounded-agent-loop"
                    controls = dict(
                        action_rules=[
                            dict(
                                memory_id=source,
                                primitive_id="schema_required_nonempty",
                                reason="运行态 schema 的必填参数为空时拒绝执行。",
                            )
                        ],
                        loop_rules=[],
                    )
                    if getattr(owner, "vary_control_reason", False):
                        controls["action_rules"][0]["reason"] += str(ordinal)
                    content = json.dumps(
                        dict(reason="收紧必填参数的非空检查。", controls=controls),
                        ensure_ascii=False,
                    )
                elif owner.behavior == "repeat_target":
                    content = json.dumps(
                        dict(
                            reason="同一输出目标跨执行实例重复失败。",
                            controls=dict(
                                action_rules=[
                                    dict(
                                        memory_id="test-experience",
                                        primitive_id="repeat_failed_target",
                                        operation="calculate",
                                        failure_key_arguments=["output"],
                                        max_failed_attempts=2,
                                        reason="相同目标的重复失败预算已耗尽。",
                                    )
                                ],
                                loop_rules=[],
                            ),
                        ),
                        ensure_ascii=False,
                    )
                elif owner.behavior == "repeat_failures":
                    content = json.dumps(
                        dict(
                            reason="同一操作连续失败后拦截第三次尝试，独立操作继续。",
                            controls=dict(
                                action_rules=[
                                    dict(
                                        memory_id="test-experience",
                                        primitive_id="repeat_failed_operation",
                                        operation="calculate",
                                        reason="停止已落定的重复失败。",
                                    )
                                ],
                                loop_rules=[],
                            ),
                        ),
                        ensure_ascii=False,
                    )
                elif owner.behavior == "invalid_schema_action":
                    controls = dict(
                        action_rules=[
                            dict(
                                memory_id="test-experience",
                                operation="calculate",
                                required_arguments=["value"],
                                reason="未授权具体工具规则。",
                            )
                        ],
                        loop_rules=[],
                    )
                    content = json.dumps(
                        dict(reason="测试未授权规则审计。", controls=controls),
                        ensure_ascii=False,
                    )
                elif owner.behavior == "full_controls":
                    content = json.dumps(
                        dict(
                            reason="合并编排与分支失败恢复的独立执行证据。",
                            controls=dict(
                                schedule_rules=[
                                    dict(
                                        memory_id="test-experience",
                                        before_operation="prepare",
                                        after_operation="image",
                                        require_success=True,
                                        task_terms=["生成视频"],
                                        reason="准备完成后才生成图像。",
                                    )
                                ],
                                loop_rules=[
                                    dict(
                                        memory_id="test-experience",
                                        replan_on_failure=True,
                                        reason="失败时有界重规划，保持其他分支可用。",
                                    )
                                ],
                            ),
                        ),
                        ensure_ascii=False,
                    )
                elif owner.behavior in (
                    "schedule",
                    "schedule_overstrict",
                    "schedule_wrong_success",
                ):
                    terms = (
                        ["先准备素材", "再生成图像", "生成短视频"]
                        if owner.behavior == "schedule_overstrict"
                        else ["先准备素材"]
                    )
                    content = json.dumps(
                        dict(
                            reason="训练中的并行图像分支先于准备完成，需要调整技能依赖。",
                            controls=dict(
                                schedule_rules=[
                                    dict(
                                        memory_id=getattr(
                                            owner,
                                            "control_source_id",
                                            "test-experience",
                                        ),
                                        before_operation="prepare",
                                        after_operation="image",
                                        require_success=owner.behavior
                                        != "schedule_wrong_success",
                                        task_terms=terms,
                                        reason="准备成功后再生成图像。",
                                    )
                                ]
                            ),
                        ),
                        ensure_ascii=False,
                    )
                elif owner.behavior == "schedule_bad_component":
                    content = json.dumps(
                        dict(
                            reason="测试模型错误地改写未获选的经验。",
                            experience=dict(
                                memory_id=getattr(owner, "wrong_experience_id", "test-experience"),
                                content="生成图像前先完成准备。",
                            ),
                        ),
                        ensure_ascii=False,
                    )
                elif owner.behavior == "recovery_budget":
                    rule = dict(
                        memory_id="bounded-agent-loop"
                        if getattr(owner, "recovery_bad_source", False)
                        and not packet.get("phase")
                        else "test-experience",
                        max_replans=0,
                        replan_on_failure=False,
                        reason="达到原无进展阈值后停止该分支，不再额外恢复。",
                    )
                    repairing = packet.get("phase") == "repair_control_capability"
                    if (
                        getattr(owner, "conditional_recovery", False)
                        or repairing
                        and getattr(owner, "recovery_repair", "") == "success"
                    ):
                        rule["error_codes"] = ["FixtureUnavailable"]
                    draft = dict(
                        reason="重复失败时额外重规划没有改进。",
                        controls=dict(loop_rules=[rule]),
                    )
                    if repairing and getattr(owner, "recovery_repair", "") == "abstain":
                        draft = dict(
                            reason="没有足够条件支持收紧恢复预算，保留父策略。",
                            experiences=[],
                        )
                    content = json.dumps(draft, ensure_ascii=False)
                elif owner.behavior == "failure_replan":
                    content = json.dumps(
                        dict(
                            reason="工具已失败且状态仍在变化，需要真实的失败恢复机会。",
                            controls=dict(
                                loop_rules=[
                                    dict(
                                        memory_id="test-experience",
                                        replan_on_failure=True,
                                        reason="失败后在本分支重新规划，独立工作继续。",
                                    )
                                ]
                            ),
                        ),
                        ensure_ascii=False,
                    )
                elif owner.behavior in ("actions", "loops", "untriggerable_loop"):
                    controls = dict(action_rules=[], loop_rules=[])
                    if owner.behavior == "actions":
                        controls["action_rules"] = [
                            dict(
                                memory_id="test-experience",
                                operation="calculate",
                                required_arguments=["verified"],
                                reason="计算缺少核验标记时拒绝执行。",
                            )
                        ]
                    else:
                        aggressive = packet.get("controls", {}).get(
                            "aggressive_controls", False
                        )
                        controls["loop_rules"] = [
                            dict(
                                memory_id="test-experience",
                                max_iterations=11
                                if owner.behavior == "untriggerable_loop"
                                else (2 if aggressive else 12),
                                max_no_progress=3 if aggressive else 1,
                                reason="根据重复任务的执行反馈收紧循环次数。",
                            )
                        ]
                    content = json.dumps(
                        dict(
                            reason="根据训练执行的结果修改受限规则。", controls=controls
                        ),
                        ensure_ascii=False,
                    )
                elif owner.behavior == "unused_experience":
                    content = json.dumps(
                        dict(
                            reason="测试正文可编译，但完整注入上下文超过请求预算。",
                            experience=dict(
                                memory_id="test-experience",
                                content="缺少素材时先核验输入，独立任务继续。" * 16,
                            ),
                        ),
                        ensure_ascii=False,
                    )
                elif owner.behavior == "disable_enable":
                    content = json.dumps(
                        dict(
                            reason="比较停用经验，并恢复种子批准的同一经验。",
                            experience=dict(
                                memory_id="test-experience",
                                content=None
                                if ordinal == 1
                                else "缺少素材时核验甲类任务，并核验乙类任务，独立计算继续。",
                            ),
                        ),
                        ensure_ascii=False,
                    )
                elif owner.behavior == "operation_guard":
                    content = json.dumps(
                        dict(
                            reason="结果类经验需要实际操作，而不是文本提及。",
                            experience=dict(
                                memory_id="test-experience",
                                content="缺少素材时核验甲类任务，并核验乙类任务，独立计算继续。",
                                observed_operations=["calculate"],
                            ),
                        ),
                        ensure_ascii=False,
                    )
                elif owner.behavior == "trigger_override":
                    change = dict(
                        memory_id="test-experience",
                        content="缺少素材时核验甲类任务，并核验乙类任务，独立计算继续。"
                        + ("避免重复检查。" if ordinal > 1 else ""),
                    )
                    if ordinal == 1:
                        change["trigger_pattern"] = "生成视频时缺少图片素材，核验输入"
                    content = json.dumps(
                        dict(
                            reason="根据实际任务收窄经验适用范围。", experience=change
                        ),
                        ensure_ascii=False,
                    )
                elif owner.behavior == "empty_change":
                    content = json.dumps(
                        dict(
                            reason="独立任务保持并行，没有需要修改的组件。",
                            experiences=[],
                        )
                    )
                elif owner.behavior == "no_change":
                    memory_id = packet["experiences"][0]["memory_id"]
                    content = json.dumps(
                        dict(
                            reason="测试缓存，不修改已有经验。",
                            experience=dict(
                                memory_id=memory_id,
                                content=packet["components"]["experience:" + memory_id],
                            ),
                        )
                    )
                elif owner.behavior == "bad_json":
                    content = "not-json"
                else:
                    n = len(owner.requests)
                    current = packet["components"][
                        "experience:" + packet["experiences"][0]["memory_id"]
                    ]
                    if n == 4:
                        memory_id, text = "unauthorized-memory", "禁止扩大范围"
                    elif n == 5:
                        memory_id, text = (
                            "test-experience",
                            "凭据 " + owner.env["GEPA_LEAK_SECRET"],
                        )
                    elif n == 6:
                        memory_id, text = "test-experience", "停止所有任务，不再计算。"
                    else:
                        memory_id = "test-experience"
                        text = (
                            "缺少素材时先核验甲类任务。"
                            if n == 1
                            else (
                                "缺少素材时核验甲类任务，并核验乙类任务，独立计算继续。"
                                if "核验" in current
                                else "缺少素材时先核验乙类任务。"
                            )
                        )
                    content = json.dumps(
                        dict(
                            reason="根据训练输出与失败检查，修复遗漏的核验步骤。",
                            experience=dict(memory_id=memory_id, content=text),
                        ),
                        ensure_ascii=False,
                    )
                content = owner.model_draft_content(packet, content)
                if getattr(owner, "prefixed", False):
                    content = "Provider note: structured result follows.\n" + content
                elif owner.fenced:
                    content = " \r\n```json\r\n" + content + "\r\n```\t"
                response = json.dumps(
                    dict(
                        choices=[
                            dict(finish_reason="stop", message=dict(content=content))
                        ]
                    )
                ).encode()
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
        runtime = json.loads(
            (fixture.SOURCE / "config/standard-runtime.json").read_text()
        )
        shutil.copytree(fixture.SOURCE / "config/prompts", self.root / "prompts")
        runtime["model"].update(
            dict(
                endpoint=f"http://127.0.0.1:{self.server.server_port}/chat/completions",
                model_id="local-gepa-fixture",
                no_api_key=True,
                timeout_ms=30000,
                max_retries=0,
                thinking_mode="disabled",
                max_output_tokens=2048,
            )
        )
        fixture.write(self.root / "model.json", runtime)
        gepa = json.loads((fixture.SOURCE / "config/gepa.json").read_text())
        gepa.update(
            runtime_config="model.json",
            prompt_file=str(fixture.SOURCE / "config/prompts/gepa-reflection.txt"),
            max_rounds=6,
            max_model_calls=6,
            max_trials=120,
            timeout_ms=120000,
            minibatch_size=2,
            patience=0,
            use_merge=False,
            component_selector="model_choice",
            candidate_selection_strategy="fair",
            prune_dominated=False,
            minibatch_strategy="fixed",
        )
        fixture.write(self.root / "gepa.json", gepa)
        tasks = []
        for split, label in [
            ("train", "TRAIN_VISIBLE"),
            ("val", "VALIDATION_PRIVATE"),
            ("holdout", "HOLDOUT_PRIVATE"),
        ]:
            for family in ("甲类", "乙类"):
                task_id = split + ("-a" if family == "甲类" else "-b")
                tasks.append(
                    dict(
                        task_id=task_id,
                        group_id=task_id,
                        task_input=f"{label} {family}：生成视频时缺少图片素材，仍需完成独立计算。",
                        expected_behavior=f"{label} 对缺少素材的独立计算返回20。",
                        initial_state={},
                        difficulty="easy",
                        category=family,
                        split=split,
                        review_status="reviewed",
                    )
                )
        (self.root / "gepa-tasks.jsonl").write_text(
            "\n".join(json.dumps(task, ensure_ascii=False) for task in tasks),
            encoding="utf-8",
        )
        dataset = json.loads((fixture.SOURCE / "config/dataset.json").read_text())
        dataset["sources"] = [
            dict(kind="golden", path=str(self.root / "gepa-tasks.jsonl"))
        ]
        fixture.write(self.root / "gepa-dataset.json", dataset)
        result = subprocess.run(
            [
                str(fixture.CLI),
                "dataset",
                "build",
                "--config",
                str(self.root / "gepa-dataset.json"),
                "--output-root",
                str(self.root / "gepa-dataset"),
            ],
            capture_output=True,
            text=True,
            timeout=20,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assessment = json.loads((self.root / "agent-evaluation.json").read_text())
        # 本测试类验证 GEPA 搜索本身；基类只有一条记忆，若沿用生产冷启动门槛，
        # 启动后会先进入 training 而永远到不了本类等待的反思请求。
        self.assessment.update(
            dataset="gepa-dataset",
            gepa_config="gepa.json",
            training=dict(enabled=False),
        )
        fixture.write(self.root / "agent-evaluation.json", self.assessment)
        self.evaluation["variants"][0]["command"][-2] = "gepa"
        self.evaluation["repetitions"] = 1
        fixture.write(self.root / "evaluation.json", self.evaluation)
        # 基类显式关闭自动评测以测试手工管理接口；GEPA 夹具依赖启动时自动提交，
        # 因此在子类恢复该能力，避免“服务正常但 jobs={}”掩盖续树回归。
        self.config.update(
            max_level="L1", auto_evaluate=True, auto_approve_experiences=True
        )
        fixture.write(self.root / "service.json", self.config)

    # 测试辅助函数：tearDown。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def tearDown(self):
        self.release.set()
        fixture.AgentEvaluationTests.tearDown(self)
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)

    # 测试辅助函数：wait_search。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def wait_search(self, level="L1"):
        deadline = time.monotonic() + 70
        status = {}
        while time.monotonic() < deadline:
            status = self.call("agent_evaluation.status")
            jobs = [
                (key, value)
                for key, value in status["jobs"].items()
                if value.get("gepa") and value["level"] == level
            ]
            if jobs and all(
                job[1]["status"] not in ("pending", "running") for job in jobs
            ):
                return jobs[0][0], jobs[0][1], status
            time.sleep(0.05)
        self.fail("search did not finish: " + json.dumps(status))

    def test_completed_search_without_feedback_records_is_explicitly_empty(self):
        """正常完成且无反馈记录时必须落盘 empty，不能遗留 none。"""
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.behavior = "empty_change"

        self.start()
        job_id, job, _ = self.wait_search()

        self.assertIn(job["status"], ("passed", "rejected"), job)
        self.assertEqual(
            job["memory_feedback"],
            dict(
                status="empty",
                records=0,
                recovered_checkpoint=False,
                checkpoint_attempt=job["attempt"],
            ),
        )
        state = json.loads(
            (self.root / "production/agent-evaluation/state.json").read_text()
        )
        self.assertEqual(state["jobs"][job_id]["memory_feedback"], job["memory_feedback"])

    def test_feedback_uses_authoritative_memory_and_next_seed(self):
        """真实运行包内搜索与宿主桩，确认外置原库写回、去重及下一轮编译采用。"""
        source = self.root / "training"
        shutil.move(self.root / "production", source)
        workspace = self.root / "gepa-workspace"
        self.config.update(
            data_dir=str(workspace), memory_source_dir=str(source),
            principals=[dict(user_id="user-a", token_env="EVAL_USER_A_TOKEN")],
        )
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        text = "缺少素材时核验甲类任务，并核验乙类任务，独立计算继续。"
        self.model_draft_content = lambda packet, content: json.dumps(dict(
            reason="两个独立训练任务均需要核验。",
            experience=dict(memory_id="test-experience", content=text)), ensure_ascii=False)
        original = (source / "by-user/user-a/memory/evolution-memory.pb").read_bytes()
        self.start()
        job_id, job, _ = self.wait_search()
        self.assertEqual(job["status"], "passed", job)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            job = self.call("agent_evaluation.status")["jobs"][job_id]
            if job.get("memory_feedback", {}).get("status") == "committed":
                break
            time.sleep(0.05)
        else:
            self.fail("GEPA feedback was not committed: " + json.dumps(job))
        memory_file = source / "by-user/user-a/memory/evolution-memory.json"
        memory = json.loads(memory_file.read_text())
        records = memory["gepa_feedback"]["records"]
        self.assertTrue(records)
        self.assertTrue(any(row["status"] == "supported" for row in records.values()), records)
        for row in records.values():
            self.assertEqual(row["split"], "train")
            self.assertTrue(all(task["task_id"].startswith("train-") for task in row["tasks"].values()))
        self.assertNotEqual((source / "by-user/user-a/memory/evolution-memory.pb").read_bytes(), original)
        self.assertFalse(list(workspace.glob("by-user/*/memory/evolution-memory.*")))
        self.assertEqual(memory["entries"][0]["candidate"]["proposed_change"],
                         "缺少图片素材时先补充素材，独立的计算任务继续。")
        self.assertNotIn(memory["entries"][0]["status"],
                         ("EVOLUTION_MEMORY_STATUS_VALIDATED", "EVOLUTION_MEMORY_STATUS_ARCHIVED"))

        # 同一服务不因反馈入库立即循环启动 GEPA；冻结种子也没有被回写覆盖。
        frozen_seed = (workspace / "agent-evaluation" / job_id / "seed.json").read_bytes()
        count = len(self.requests)
        time.sleep(0.2)
        status = self.call("agent_evaluation.status")
        self.assertEqual(len(self.requests), count)
        self.assertEqual(len([value for value in status["jobs"].values() if value.get("gepa")]), 1)
        self.assertEqual((workspace / "agent-evaluation" / job_id / "seed.json").read_bytes(), frozen_seed)
        self.process.terminate()
        self.process.communicate(timeout=10)
        self.process = None

        # 新一轮显式调用用同一来源库编译新种子；已交付的反馈不再入库。
        config.update(max_rounds=2)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        deadline = time.monotonic() + 10
        next_job = None
        while time.monotonic() < deadline:
            status = self.call("agent_evaluation.status")
            candidates = [(key, value) for key, value in status["jobs"].items()
                          if value.get("gepa") and key != job_id]
            if candidates:
                next_job = candidates[0][0]
                break
            time.sleep(0.05)
        self.assertIsNotNone(next_job, status)
        seed = json.loads((workspace / "agent-evaluation" / next_job / "seed.json").read_text())
        overrides = seed["policy"]["options"]["experience_overrides"]
        self.assertTrue(any(row["content"] == text for row in overrides), overrides)
        self.assertEqual(json.loads(memory_file.read_text())["revision"], memory["revision"])
        self.assertFalse(list(workspace.glob("by-user/*/memory/evolution-memory.*")))

    def test_memory_counterevidence_reaches_next_search_without_config_edits(self, corrupt_seed=False):
        """原库失败修订驱动下一次显式搜索；配置不变、L0 复用、不自动无限续轮。"""
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        memory_file = self.root / "production/by-user/user-a/memory/evolution-memory.pb"
        arguments = ["-I" + str(fixture.SOURCE / "proto"),
                     "-I" + str(Path(fixture.PROTOC).resolve().parent.parent / "include"),
                     "hybrid_evaluation.proto"]
        message = "agentflow.trajectory.evaluation.EvolutionMemoryStore"
        decoded = subprocess.run([fixture.PROTOC, "--decode=" + message, *arguments],
                                 input=memory_file.read_bytes(), capture_output=True, check=True)
        original_text = "缺少素材时先核验甲类任务，独立计算继续。"
        source = "\n".join(
            "proposed_change: " + json.dumps(original_text, ensure_ascii=False)
            if line.strip().startswith("proposed_change:") else line
            for line in decoded.stdout.decode().splitlines())
        encoded = subprocess.run([fixture.PROTOC, "--encode=" + message, *arguments],
                                 input=source.encode(), capture_output=True, check=True)
        memory_file.write_bytes(encoded.stdout)
        fixed_config = (self.root / "gepa.json").read_bytes()

        def propose(packet, _content):
            # 同一个模型桩只在真的收到历史反证时改正，不按调用次数伪造学习成功。
            text = ("缺少素材时核验甲类任务，并核验乙类任务，独立计算继续。"
                    if packet.get("prior_search_rejections") else "停止所有任务，不再计算。")
            return json.dumps(dict(reason="根据已观测的训练反证恢复必要核验。",
                                   experience=dict(memory_id="test-experience", content=text)),
                              ensure_ascii=False)

        self.model_draft_content = propose
        command = [str(fixture.CLI), "learn", "--evaluation-only", "--config",
                   str(self.root / "service.json")]

        def execute():
            self.process = subprocess.Popen(command, env=self.env, stdout=subprocess.PIPE,
                                            stderr=subprocess.PIPE, text=True)
            stdout, stderr = self.process.communicate(timeout=35)
            self.assertEqual(self.process.returncode, 0, stdout + stderr)
            return json.loads((self.root / "production/agent-evaluation/state.json").read_text())

        first = execute()
        jobs = {key: value for key, value in first["jobs"].items() if value.get("gepa")}
        self.assertEqual(len(jobs), 1)
        first_id, job = next(iter(jobs.items()))
        self.assertEqual(job["status"], "rejected", job)
        self.assertEqual(job["memory_feedback"]["status"], "committed")
        self.assertNotIn("prior_search_rejections", self.requests[0])
        memory = json.loads(memory_file.with_suffix(".json").read_text())
        self.assertTrue(any(r["negative_task_count"] for r in memory["gepa_feedback"]["records"].values()))
        first_requests = len(self.requests)
        if corrupt_seed:
            # 状态文件声明版本正确也不够，缺损的种子正文不得被信任或阻断新的合法提交。
            (self.root / "production/agent-evaluation" / first_id / "seed.json").write_text("{}")
        second = execute()
        jobs = {key: value for key, value in second["jobs"].items() if value.get("gepa")}
        self.assertEqual(len(jobs), 2, jobs)
        next_id = next(key for key in jobs if key != first_id)
        self.assertEqual(jobs[next_id]["status"], "passed", jobs[next_id])
        self.assertTrue(jobs[next_id]["published"])
        self.assertEqual(jobs[next_id]["memory_feedback"]["status"], "committed")
        requests = self.requests[first_requests:]
        self.assertEqual(len(requests), 1)
        self.assertEqual(len(requests[0]["prior_search_rejections"]), 1)
        self.assertNotIn("HOLDOUT_PRIVATE", json.dumps(requests[0]))
        self.assertNotIn("VALIDATION_PRIVATE", json.dumps(requests[0]))
        self.assertEqual((self.root / "gepa.json").read_bytes(), fixed_config)
        tree = json.loads((self.root / "production/agent-evaluation" / next_id / "search/tree.json").read_text())
        self.assertEqual(tree["baseline_cache"]["status"], "hit")
        # 反馈使库修订增加，但内容未变的种子应保留原冻结身份，不重跑原执行。
        jobs_root = self.root / "production/agent-evaluation"
        if corrupt_seed:
            self.assertNotIn("seed_source_job", jobs[next_id])
        else:
            self.assertEqual(jobs[next_id]["seed_source_job"], first_id)
            self.assertEqual((jobs_root / next_id / "seed.json").read_bytes(),
                             (jobs_root / first_id / "seed.json").read_bytes())
        seed_validation = [
            value for value in tree["evaluations"].values()
            if value.get("version") == tree["seed_version"] and value.get("split") == "val"
        ]
        self.assertEqual(len(seed_validation), 1)
        if corrupt_seed:
            self.assertNotEqual(seed_validation[0].get("shared_cache"), "hit")
        else:
            self.assertEqual(seed_validation[0].get("shared_cache"), "hit")
            self.assertEqual(seed_validation[0]["trials_reserved"], 0)
        memory = json.loads(memory_file.with_suffix(".json").read_text())
        self.assertEqual(memory["entries"][0]["candidate"]["proposed_change"], original_text)
        # 已采用的成功修订再次编译得到发布版本，重复调用只读回执，不重复搜索或写库。
        request_count = len(self.requests)
        memory_bytes = memory_file.read_bytes()
        third = execute()
        self.assertEqual({key for key, value in third["jobs"].items() if value.get("gepa")},
                         set(jobs))
        self.assertEqual(len(self.requests), request_count)
        self.assertEqual(memory_file.read_bytes(), memory_bytes)

    def test_corrupt_frozen_seed_does_not_block_feedback_driven_search(self):
        """历史冻结种子损坏时使用本次合法种子，不伪造命中或丢掉新的反证。"""
        self.test_memory_counterevidence_reaches_next_search_without_config_edits(corrupt_seed=True)

    def test_feedback_waits_for_shared_owner_and_recovers_after_restart(self):
        self.exercise_feedback_owner_recovery(restart=True)

    def test_feedback_resumes_after_owner_releases_without_restart(self):
        self.exercise_feedback_owner_recovery(restart=False)

    def exercise_feedback_owner_recovery(self, restart):
        """原库被管理器占用时不覆盖；私库已提交的反馈在重启后只补公共视图和确认。"""
        import fcntl

        source = self.root / "training"
        shutil.move(self.root / "production", source)
        workspace = self.root / "gepa-workspace"
        self.config.update(data_dir=str(workspace), memory_source_dir=str(source),
                           sharing=dict(enabled=True),
                           principals=[dict(user_id="user-a", token_env="EVAL_USER_A_TOKEN")])
        self.config.setdefault("learning", {})["retry_backoff_ms"] = 1000
        fixture.write(self.root / "service.json", self.config)
        ledger = source / "learning/shared-memory.json"
        ledger.parent.mkdir(parents=True, exist_ok=True)
        fixture.write(ledger, dict(schema_version=1, application_id=self.config["agent_id"],
            revision=0, next_due_ms=0, entries={}, sources={}, relations={}, users={},
            pending_users=[], retired=[]))
        original_ledger = ledger.read_bytes()
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.model_draft_content = lambda packet, content: json.dumps(dict(
            reason="两个独立训练任务均需要核验。", experience=dict(memory_id="test-experience",
                content="缺少素材时核验甲类任务，并核验乙类任务，独立计算继续。")), ensure_ascii=False)
        memory_file = source / "by-user/user-a/memory/evolution-memory.json"
        with (source / "service.lock").open("a") as owner:
            fcntl.flock(owner, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.start()
            job_id, _, _ = self.wait_search()
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                memory = json.loads(memory_file.read_text()) if memory_file.exists() else {}
                if memory.get("gepa_feedback", {}).get("records"):
                    break
                time.sleep(.05)
            else:
                self.fail("private feedback did not commit before the blocked shared projection")
            revision = memory["revision"]
            self.assertEqual(ledger.read_bytes(), original_ledger)
            job = self.call("agent_evaluation.status")["jobs"][job_id]
            self.assertEqual(job["memory_feedback"]["status"], "pending")
            if restart:
                self.process.terminate()
                self.process.communicate(timeout=10)
                self.process = None
        if restart:
            self.start()
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            job = self.call("agent_evaluation.status")["jobs"][job_id]
            if job.get("memory_feedback", {}).get("status") == "committed":
                break
            time.sleep(.05)
        else:
            self.fail("pending feedback was not recovered: " + json.dumps(job))
        self.assertEqual(json.loads(memory_file.read_text())["revision"], revision)
        self.assertTrue((source / "memory/evolution-memory.pb").is_file())
        self.assertTrue((source / "memory/evolution-memory.json").is_file())
        self.assertFalse((workspace / "memory/evolution-memory.pb").exists())
        self.assertFalse((workspace / "learning/shared-memory.json").exists())

    def test_consistency_prompt_change_invalidates_all_score_contexts(self):
        """只改核验提示词原文时，新作业、基线和候选缓存均不能沿用旧评分口径。"""
        self.behavior = "empty_change"
        self.judge_requests = []
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.evaluation["checks"] = {}
        self.evaluation["variants"][0]["command"][-2] = "gepa_review"
        self.evaluation["judge"] = dict(
            enabled=True, runtime_config="model.json", assessment_mode="objectives",
            prompt="prompts/task-judge-system.txt",
            loop_review_prompt="prompts/task-loop-review.txt",
            consistency_prompt="prompts/task-consistency.txt",
            max_model_calls=100, max_input_bytes=1048576)
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        first_id, first_job, _ = self.wait_search()
        self.assertIn(first_job["status"], ("passed", "rejected"), first_job)
        first = self.call("agent_evaluation.tree", job_id=first_id)
        root = self.root / "production/agent-evaluation"
        seed = json.loads((root / first_id / "seed.json").read_text())
        self.process.terminate()
        self.process.communicate(timeout=10)
        self.process = None

        # 保留路径、配置、模型和数据，只热替换这一份外置提示词。
        prompt = self.root / "prompts/task-consistency.txt"
        prompt.write_text(prompt.read_text() + "\n仅记录可由既有理由直接证明的矛盾。\n")
        self.start()
        second_id = self.call("agent_evaluation.submit", artifact=seed)["job_id"]
        self.assertNotEqual(second_id, first_id, "提示词内容变化未进入搜索身份")
        deadline = time.monotonic() + 70
        while time.monotonic() < deadline:
            job = self.call("agent_evaluation.status")["jobs"][second_id]
            if job["status"] not in ("pending", "running"):
                break
            time.sleep(0.05)
        else:
            self.fail("新评分口径的测试作业未完成")
        self.assertIn(job["status"], ("passed", "rejected"), job)
        second = self.call("agent_evaluation.tree", job_id=second_id)
        self.assertNotEqual(second["context"], first["context"])
        self.assertEqual(second["baseline_cache"]["status"], "miss")
        self.assertNotEqual(second["baseline_cache"]["key"], first["baseline_cache"]["key"])
        self.assertFalse(any(row.get("shared_cache") == "hit"
                             for row in second["evaluations"].values()))

    def _check_imported_baseline_judge(self, changed_prompt, kind="baseline", wrong_version=False):
        """种子与基线均经正式报告入口核对版本、裁判身份，不能借缓存跳过校验。"""
        self.behavior = "empty_change"
        self.judge_requests = []
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.evaluation["checks"] = {}
        self.evaluation["variants"][0]["command"][-2] = "gepa_review"
        self.evaluation["judge"] = dict(
            enabled=True, runtime_config="model.json",
            prompt="prompts/task-judge-system.txt",
            loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100, max_input_bytes=1048576)
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        first_id, _, _ = self.wait_search()
        first = self.call("agent_evaluation.tree", job_id=first_id)
        root = self.root / "production/agent-evaluation" / first_id
        source_kind = "baseline" if wrong_version else kind
        baseline = next(row for row in first["evaluations"].values()
                        if row["version"] == first[source_kind + "_version"] and row["split"] == "val")
        source = root / "search" / baseline["report"]
        seed = json.loads((root / "seed.json").read_text())
        self.assertTrue(json.loads(source.read_text())["configuration"]["judge_fingerprint"])
        self.process.terminate()
        self.process.communicate(timeout=10)
        self.process = None
        config.update(max_rounds=2, max_model_calls=2)
        config[kind + "_report_file"] = str(source)
        fixture.write(self.root / "gepa.json", config)
        if changed_prompt:
            prompt = self.root / "prompts/task-judge-system.txt"
            prompt.write_text(prompt.read_text() + "\n必须核对实际查询返回。\n")
        self.start()
        job_id = self.call("agent_evaluation.submit", artifact=seed)["job_id"]
        deadline = time.monotonic() + 70
        while time.monotonic() < deadline:
            job = self.call("agent_evaluation.status")["jobs"][job_id]
            if job["status"] not in ("pending", "running"):
                break
            time.sleep(0.05)
        else:
            self.fail("基线导入测试未及时完成")
        if changed_prompt:
            self.assertEqual(job.get("error"), "gepa_" + kind + "_judge_mismatch", job)
        elif wrong_version:
            self.assertEqual(job.get("error"), "gepa_" + kind + "_report_incompatible", job)
        elif kind == "seed":
            tree = self.call("agent_evaluation.tree", job_id=job_id)
            self.assertEqual(tree["nodes"][0]["validation_source"]["status"], "imported")
            self.assertEqual(tree["nodes"][0]["validation"], first["nodes"][0]["validation"])
            self.assertEqual(tree["nodes"][0]["control_coverage"], first["nodes"][0]["control_coverage"])
            self.assertFalse(any(row["version"] == tree["seed_version"] and row["split"] == "val"
                                 for row in tree["evaluations"].values()))
        else:
            tree = self.call("agent_evaluation.tree", job_id=job_id)
            self.assertEqual(tree["baseline_cache"]["status"], "imported", tree)
            self.assertEqual(tree["baseline_validation"], first["baseline_validation"])
            self.assertFalse(any(row["version"] == tree["baseline_version"]
                                 and row["split"] == "val"
                                 for row in tree["evaluations"].values()))

    def test_imported_seed_accepts_matching_judge(self):
        """复用种子真实执行与控制命中证据，不再次运行或评分选择题。"""
        self._check_imported_baseline_judge(False, kind="seed")

    def test_imported_seed_rejects_changed_judge_prompt(self):
        """旧裁判的种子分数不能直接进入新搜索，必须先复评保存轨迹。"""
        self._check_imported_baseline_judge(True, kind="seed")

    def test_imported_seed_rejects_other_policy(self):
        """数据相同但实际运行的是 L0，不能当作 L3 种子成绩导入。"""
        self._check_imported_baseline_judge(False, kind="seed", wrong_version=True)

    def test_imported_baseline_accepts_matching_judge(self):
        """相同评分口径导入已有 L0，不重新执行或评分基线。"""
        self._check_imported_baseline_judge(False)

    def test_imported_baseline_rejects_changed_judge_prompt(self):
        """模型名相同但裁判提示词变化时，拒绝把旧分数混入新搜索。"""
        self._check_imported_baseline_judge(True)

    # 回归用例：test_baseline_scores_are_reused_when_only_search_configuration_changes。
    # 关键断言：self.assertEqual(first["baseline_cache"]["status"], "miss", first)；此处只验证测试构造的场景。
    def test_baseline_scores_are_reused_when_only_search_configuration_changes(self):
        self.behavior = "empty_change"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        first_id, _, _ = self.wait_search()
        first = self.call("agent_evaluation.tree", job_id=first_id)
        self.assertEqual(first["baseline_cache"]["status"], "miss", first)
        seed = json.loads(
            (
                self.root / "production/agent-evaluation" / first_id / "seed.json"
            ).read_text()
        )

        self.process.terminate()
        self.process.communicate(timeout=10)
        self.process = None
        config.update(max_rounds=2, max_model_calls=2, holdout_max_report_bytes=536870912)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        second_id = self.call("agent_evaluation.submit", artifact=seed)["job_id"]
        deadline = time.monotonic() + 70
        while time.monotonic() < deadline:
            job = self.call("agent_evaluation.status")["jobs"][second_id]
            if job["status"] not in ("pending", "running"):
                break
            time.sleep(0.05)
        else:
            self.fail("second search did not finish")
        second = self.call("agent_evaluation.tree", job_id=second_id)
        self.assertEqual(second["baseline_cache"]["status"], "hit", second)
        self.assertEqual(second["baseline_validation"], first["baseline_validation"])
        self.assertFalse(
            any(
                row.get("version") == second["baseline_version"]
                and row.get("split") == "val"
                for row in second["evaluations"].values()
            ),
            second["evaluations"],
        )
        # 搜索配置变化只重建搜索树；相同 seed 的真实训练/验证执行复用完整检查点。
        second_seed = [
            row
            for row in second["evaluations"].values()
            if row.get("version") == second["seed_version"]
            and row.get("split") == "val"
        ]
        self.assertEqual(len(second_seed), 1, second["evaluations"])
        self.assertEqual(second_seed[0].get("shared_cache"), "hit", second_seed)
        self.assertEqual(second_seed[0]["trials_reserved"], 0, second_seed)
        first_seed = next(
            row
            for row in first["evaluations"].values()
            if row.get("version") == first["seed_version"] and row.get("split") == "val"
        )
        evaluations = self.root / "production/agent-evaluation"
        old_report = json.loads(
            (evaluations / first_id / "search" / first_seed["report"]).read_text()
        )
        new_report = json.loads(
            (evaluations / second_id / "search" / second_seed[0]["report"]).read_text()
        )
        self.assertEqual(
            [(t["trial_id"], t["duration_ms"]) for t in old_report["trials"]],
            [(t["trial_id"], t["duration_ms"]) for t in new_report["trials"]],
        )
        self.assertTrue(
            any(
                row.get("shared_cache") == "hit" and row.get("split") == "train"
                for row in second["evaluations"].values()
            ),
            second["evaluations"],
        )
        self.assertFalse(
            any(
                row.get("shared_cache") == "hit" and row.get("split") == "holdout"
                for row in second["evaluations"].values()
            ),
            second["evaluations"],
        )

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
            time.sleep(0.05)
        else:
            self.fail("changed-evaluator search did not finish")
        third = self.call("agent_evaluation.tree", job_id=third_id)
        self.assertEqual(third["baseline_cache"]["status"], "miss", third)
        self.assertNotEqual(
            third["baseline_cache"]["key"], second["baseline_cache"]["key"]
        )

    # 测试辅助函数：check_cross_job_cache_boundary。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def check_cross_job_cache_boundary(self, change_host):
        self.behavior = "empty_change"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        host = self.root / "entry.py"
        host.write_text(
            "from pathlib import Path\nimport runpy\n"
            "Path('proof.txt').write_text('verified execution', encoding='utf-8')\n"
            f"runpy.run_path({str(fixture.SOURCE / 'tests/agent_evaluation_host.py')!r}, run_name='__main__')\n",
            encoding="utf-8",
        )
        self.evaluation["variants"][0]["command"][2] = str(host)
        self.evaluation["artifacts"] = {
            "*": [dict(path="proof.txt", media_type="text/plain")]
        }
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        first_id, _, _ = self.wait_search()
        first = self.call("agent_evaluation.tree", job_id=first_id)
        evaluations = self.root / "production/agent-evaluation"
        seed = json.loads((evaluations / first_id / "seed.json").read_text())
        first_seed = next(
            row
            for row in first["evaluations"].values()
            if row.get("version") == first["seed_version"] and row.get("split") == "val"
        )
        report_path = evaluations / first_id / "search" / first_seed["report"]
        old_report = json.loads(report_path.read_text())
        self.process.terminate()
        self.process.communicate(timeout=10)
        self.process = None
        if change_host:
            # 路径、命令和 agent.revision 均不变，仅修改显式宿主入口内容。
            host.write_text(
                host.read_text() + "\n# new host revision\n", encoding="utf-8"
            )
        else:
            # report/manifest 仍正确，但源 artifact 已损坏：应完整校验失败后重跑，不能循环恢复。
            trial = old_report["trials"][0]
            artifact = next(iter(trial["evidence"]["artifacts"].values()))
            (
                report_path.parent
                / "artifacts"
                / trial["trial_id"]
                / artifact["stored_file"]
            ).write_text("changed")
            config.update(max_rounds=2, max_model_calls=2)
            fixture.write(self.root / "gepa.json", config)
        self.start()
        second_id = self.call("agent_evaluation.submit", artifact=seed)["job_id"]
        self.assertNotEqual(second_id, first_id)
        deadline = time.monotonic() + 70
        while time.monotonic() < deadline:
            job = self.call("agent_evaluation.status")["jobs"][second_id]
            if job["status"] not in ("pending", "running"):
                break
            time.sleep(0.05)
        else:
            self.fail("cache-boundary search did not finish")
        self.assertNotEqual(job["status"], "error", job)
        second = self.call("agent_evaluation.tree", job_id=second_id)
        second_seed = next(
            row
            for row in second["evaluations"].values()
            if row.get("version") == second["seed_version"]
            and row.get("split") == "val"
        )
        self.assertNotEqual(second_seed.get("shared_cache"), "hit", second_seed)
        self.assertGreater(second_seed["trials_reserved"], 0, second_seed)
        new_path = evaluations / second_id / "search" / second_seed["report"]
        new_report = json.loads(new_path.read_text())
        self.assertNotEqual(
            [t["trial_id"] for t in new_report["trials"]],
            [t["trial_id"] for t in old_report["trials"]],
        )
        if not change_host:
            self.assertEqual(second_seed.get("shared_cache"), "invalid", second_seed)
            self.assertEqual(second_seed["attempt"], 2, second_seed)
            failed = new_path.parent.parent / "attempt-1"
            self.assertTrue((failed / "started.json").exists())
            self.assertFalse((failed / "manifest.json").exists())

    # 回归用例：test_cross_job_cache_rejects_changed_host_entry_bytes。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_cross_job_cache_rejects_changed_host_entry_bytes(self):
        self.check_cross_job_cache_boundary(change_host=True)

    # 回归用例：test_cross_job_cache_reexecutes_when_source_artifact_is_corrupt。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_cross_job_cache_reexecutes_when_source_artifact_is_corrupt(self):
        self.check_cross_job_cache_boundary(change_host=False)

    # 测试辅助函数：check_action_feedback_contract。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def check_action_feedback_contract(self, mode, behavior):
        self.behavior = behavior
        self.config.update(
            max_level="L2",
            evaluation_levels=["L2"],
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = mode
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            component_selector="round_robin",
            max_rounds=1,
            max_model_calls=1,
            max_input_bytes=16384,
        )
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        return self.call("agent_evaluation.tree", job_id=job_id), job

    # 回归用例：test_dispatch_guard_feedback_drives_real_generated_rule。
    # 重点核验字段或诊断：gepa_repeat_guard；异常路径不能被当成成功。
    def test_dispatch_guard_feedback_drives_real_generated_rule(self):
        tree, job = self.check_action_feedback_contract(
            "gepa_repeat_guard", "repeat_failures"
        )
        self.assertEqual(job["status"], "passed", job)
        actions = [
            a
            for p in self.requests
            for row in p["feedback"]
            for run in row["trial"]["evidence"]["runs"]
            for a in run["actions"]
        ]
        guards = [a for a in actions if a.get("guard_only")]
        self.assertTrue(guards, actions)
        self.assertTrue(
            any(a["action_control"].get("prior_failures") == 2 for a in guards)
        )
        self.assertTrue(
            all(a["actor_id"] == "fixture-worker" and not a["executed"] for a in guards)
        )
        node = tree["nodes"][1]
        self.assertEqual(node["components"], ["action_rules"])
        self.assertTrue(node["control_preflight"]["passed"])
        self.assertGreater(
            sum(r["applied"] for r in node["control_coverage"]["rules"].values()), 0
        )

    # 回归用例：test_repeated_tool_failure_is_an_action_opportunity_without_guard_wrapper。
    # 重点核验字段或诊断：gepa_repeat_tool；异常路径不能被当成成功。
    def test_repeated_tool_failure_is_an_action_opportunity_without_guard_wrapper(self):
        tree, job = self.check_action_feedback_contract(
            "gepa_repeat_tool", "repeat_failures"
        )
        self.assertEqual(job["status"], "passed", job)
        self.assertEqual(tree["nodes"][1]["components"], ["action_rules"])
        self.assertTrue(tree["nodes"][1]["control_preflight"]["passed"])


    def check_repeat_operation_repair(self, model_calls=2, always_invalid=False):
        """省略目标时只修复一次；纠错仍保留真实训练证据与原有调用预算。"""
        self.behavior = "repeat_failures"
        self.config.update(
            max_level="L2", evaluation_levels=["L2"],
            control_learning=dict(enabled=True, prompt_file="prompts/gepa-reflection.txt",
                                  max_rules=2),
        )
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_repeat_tool"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(component_selector="round_robin", max_rounds=1,
                      max_model_calls=model_calls, max_input_bytes=16384)
        fixture.write(self.root / "gepa.json", config)

        def draft(packet, content):
            result = json.loads(content)
            rule = result["controls"]["action_rules"][0]
            if always_invalid or packet.get("error") != "control_repeat_operation_required":
                rule.pop("operation", None)
            return json.dumps(result, ensure_ascii=False)

        self.model_draft_content = draft
        self.start()
        job_id, job, _ = self.wait_search("L2")
        return self.call("agent_evaluation.tree", job_id=job_id), job, job_id

    def test_repeat_operation_missing_scope_is_repaired_once(self):
        """新提案补齐准确操作名后才能执行；保存的版本也不能退回通配符。"""
        tree, job, job_id = self.check_repeat_operation_repair()
        self.assertEqual(job["status"], "passed", job)
        node = tree["nodes"][1]
        self.assertEqual(node["repair_error"], "control_repeat_operation_required")
        self.assertEqual(tree["model_calls"], 2)
        repair = self.requests[1]
        self.assertEqual(repair["phase"], "repair_control_capability")
        self.assertTrue(repair["feedback"])
        self.assertEqual(repair["selected_components"], ["action_rules"])
        self.assertTrue(node["control_preflight"]["passed"])
        self.assertIn("screening", node)
        rules = json.loads((self.root / "production/agent-evaluation" / job_id
                            / "search/versions/1.json").read_text())["policy"]["options"]["action_rules"]
        self.assertTrue(rules)
        self.assertTrue(all(r["operation"] == "calculate" for r in rules
                            if r.get("source_experience")))

    def test_repeat_operation_invalid_repair_never_runs_child(self):
        """两次都省略操作名时拒绝候选，不继续花费评测次数。"""
        tree, _, _ = self.check_repeat_operation_repair(always_invalid=True)
        node = tree["nodes"][1]
        self.assertEqual(node["error"], "control_repeat_operation_required")
        self.assertEqual(tree["model_calls"], 2)
        self.assertNotIn("screening", node)
        self.assertNotIn("version", node)

    def test_repeat_operation_repair_respects_model_budget(self):
        """只剩一次调用额度时不隐式追加纠错请求。"""
        tree, _, _ = self.check_repeat_operation_repair(model_calls=1)
        node = tree["nodes"][1]
        self.assertEqual(node["error"], "control_repeat_operation_required")
        self.assertEqual(tree["model_calls"], 1)
        self.assertNotIn("screening", node)

    # 回归用例：test_operation_budget_bindings_match_http_and_persisted_request。
    # 重点核验字段或诊断：gepa_repeat_guard、max_input_bytes、prior_operation_rounds；异常路径不能被当成成功。
    def test_operation_budget_bindings_match_http_and_persisted_request(self):
        import io

        self.behavior = "empty_change"
        self.config.update(
            max_level="L3",
            evaluation_levels=["L3"],
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        # 复用真实打点 fixture：两次已结束的 calculate 后有一个尚未执行的 allow guard。
        self.evaluation["variants"][0]["command"][-2] = "gepa_repeat_guard"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            component_selector="round_robin",
            max_rounds=1,
            max_model_calls=1,
            proposals_per_round=1,
            max_input_bytes=32768,
        )
        fixture.write(self.root / "gepa.json", config)

        # 只在本用例捕获实际 HTTP 字节，仍交给原 mock handler 解析和返回，不重造 packet。
        user_inputs = []
        original_handler = self.server.RequestHandlerClass

        class CapturingModel(original_handler):
            # 测试辅助函数：do_POST。
            # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
            def do_POST(handler):
                body = handler.rfile.read(int(handler.headers["Content-Length"]))
                user_inputs.append(json.loads(body)["messages"][-1]["content"])
                original_stream = handler.rfile
                handler.rfile = io.BytesIO(body)
                try:
                    return original_handler.do_POST(handler)
                finally:
                    handler.rfile = original_stream

        self.server.RequestHandlerClass = CapturingModel
        self.start()
        job_id, _, _ = self.wait_search("L3")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["model_calls"], 1, tree)
        self.assertEqual(len(user_inputs), 1)
        self.assertEqual(len(self.requests), 1)
        user_input = user_inputs[0]
        packet = json.loads(user_input)
        persisted = json.loads(
            (
                self.root
                / "production/agent-evaluation"
                / job_id
                / "search/reflections/1.request.json"
            ).read_text()
        )["input"]
        self.assertEqual(packet, self.requests[0])
        self.assertEqual(packet, persisted)
        self.assertEqual(packet["selected_components"], ["action_rules"])
        self.assertLessEqual(len(user_input.encode("utf-8")), config["max_input_bytes"])
        primitive = next(
            item
            for item in packet["controls"]["action_primitives"]
            if item["primitive_id"] == "operation_budget"
        )
        bindings = primitive.get("observed_bindings", [])
        self.assertTrue(bindings, primitive)
        self.assertLessEqual(len(bindings), 4)
        for binding in bindings:
            self.assertTrue(binding["evidence_ref"].startswith("/feedback/"), binding)
            action = packet
            for token in binding["evidence_ref"].split("/")[1:]:
                token = token.replace("~1", "/").replace("~0", "~")
                action = (
                    action[int(token)] if isinstance(action, list) else action[token]
                )
            self.assertEqual(action["kind"], "action.end")
            self.assertNotEqual(action.get("semantic"), "model")
            self.assertIs(action["guard_only"], True)
            self.assertIs(action["executed"], False)
            self.assertEqual(action["action_control"]["decision"], "allow")
            self.assertEqual(binding["operation"], action["operation"])
            self.assertEqual(
                binding["target_scope"], action["action_control"]["target_scope"]
            )
            self.assertEqual(
                binding["prior_operation_rounds"],
                action["action_control"]["prior_operation_rounds"],
            )
            self.assertGreater(binding["prior_operation_rounds"], 0)

    # 测试辅助函数：exercise_editable_operation_binding。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def exercise_editable_operation_binding(self, repair=False, concrete=False):
        self.behavior = "empty_change"
        self.config.update(
            max_level="L3",
            evaluation_levels=["L3"],
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_repeat_guard"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            component_selector="round_robin",
            max_rounds=1,
            max_model_calls=2 if repair else 1,
            proposals_per_round=1,
            max_input_bytes=32768,
        )
        fixture.write(self.root / "gepa.json", config)
        bindings_seen, emitted = [], []

        # 测试辅助函数：draft_from_http。
        # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
        def draft_from_http(packet, fallback):
            # packet 是本地 HTTP handler 实际收到的正文，不自行构造可编辑动作表。
            binding = next(
                (
                    item
                    for item in packet.get("controls", {}).get("editable_bindings", [])
                    if item.get("component") == "action_rules"
                    and item.get("primitive_id") == "operation_budget"
                    and item.get("operation") == "calculate"
                ),
                None,
            )
            bindings_seen.append(binding)
            if binding is None:
                return fallback
            reason = "停止已完成两次的重复动作，不把包装调用当成独立操作。"
            if concrete:
                draft = dict(
                    reason=reason,
                    controls=dict(
                        action_rules=[
                            dict(
                                memory_id="test-experience",
                                primitive_id="operation_budget",
                                operation="calculate",
                                max_attempts=2,
                                attempt_key_arguments=[],
                                reason=reason,
                            )
                        ]
                    ),
                )
            else:
                binding_id = (
                    "control:not-from-this-request"
                    if repair is True and len(bindings_seen) == 1
                    else binding["binding_id"]
                )
                draft = dict(
                    reason=reason,
                    control_binding_updates=[
                        dict(
                            binding_id=binding_id,
                            memory_id="test-experience",
                            max_attempts=3
                            if repair == "budget" and len(bindings_seen) == 1
                            else 2,
                            reason=reason,
                        )
                    ],
                )
            emitted.append(draft)
            return json.dumps(draft, ensure_ascii=False)

        self.model_draft_content = draft_from_http
        self.start()
        job_id, _, _ = self.wait_search("L3")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        calls = 2 if repair else 1
        self.assertEqual(tree["model_calls"], calls, tree)
        self.assertEqual(len(self.requests), calls)
        self.assertEqual(len(bindings_seen), calls)
        self.assertTrue(all(bindings_seen), self.requests)
        self.assertEqual(len(emitted), calls)
        search = self.root / "production/agent-evaluation" / job_id / "search"
        for index, packet in enumerate(self.requests):
            name = "1-repair.request.json" if index else "1.request.json"
            persisted = json.loads((search / "reflections" / name).read_text())["input"]
            self.assertEqual(packet, persisted)
            self.assertEqual(packet["selected_components"], ["action_rules"])
        node = tree["nodes"][1]
        self.assertEqual(node["status"], "accepted", node)
        self.assertTrue(node["control_preflight"]["passed"], node)
        self.assertGreater(sum(node["control_preflight"]["rules"].values()), 0)
        self.assertGreater(
            sum(rule["applied"] for rule in node["control_coverage"]["rules"].values()),
            0,
        )
        version = json.loads((search / "versions/1.json").read_text())
        rules = [
            rule
            for rule in version["policy"]["options"]["action_rules"]
            if rule.get("source_experience")
        ]
        self.assertEqual(len(rules), 1)
        self.assertEqual(rules[0]["operation"], "calculate")
        self.assertEqual(rules[0]["operation"], bindings_seen[-1]["operation"])
        self.assertNotEqual(rules[0]["operation"], "run_skills")
        applied = node["control_coverage"]["rules"][rules[0]["rule_id"]]
        self.assertEqual(applied["boundary"], "actions")
        self.assertGreater(applied["applied"], 0, applied)
        self.assertTrue(node["screening"]["applied_changes"], node["screening"])
        return tree, bindings_seen, emitted

    # 回归用例：test_editable_binding_generates_the_observed_operation_and_really_applies。
    # 重点核验字段或诊断：prior_operation_rounds、control_binding_updates；异常路径不能被当成成功。
    def test_editable_binding_generates_the_observed_operation_and_really_applies(self):
        _, bindings, emitted = self.exercise_editable_operation_binding()
        binding = bindings[0]
        self.assertEqual(binding["component"], "action_rules")
        self.assertEqual(binding["prior_operation_rounds"], 2)
        self.assertTrue(binding["evidence_ref"].startswith("/feedback/"))
        action = self.requests[0]
        for token in binding["evidence_ref"].split("/")[1:]:
            token = token.replace("~1", "/").replace("~0", "~")
            action = action[int(token)] if isinstance(action, list) else action[token]
        self.assertEqual(action["operation"], binding["operation"])
        self.assertEqual(
            action["action_control"]["target_scope"], binding["target_scope"]
        )
        self.assertEqual(action["action_control"]["decision"], "allow")
        self.assertIs(action["guard_only"], True)
        self.assertIs(action["executed"], False)
        update = emitted[0]["control_binding_updates"][0]
        self.assertEqual(update["binding_id"], binding["binding_id"])
        self.assertNotIn("operation", update)
        self.assertNotIn("control_updates", emitted[0])

    # 回归用例：test_invalid_editable_binding_repairs_once_with_a_fresh_request_binding。
    # 重点核验字段或诊断：repair_control_binding、control_binding_invalid、control_binding_updates；异常路径不能被当成成功。
    def test_invalid_editable_binding_repairs_once_with_a_fresh_request_binding(self):
        tree, bindings, emitted = self.exercise_editable_operation_binding(repair=True)
        self.assertEqual(
            [packet.get("phase") for packet in self.requests],
            [None, "repair_control_binding"],
        )
        self.assertEqual(self.requests[1]["error"], "control_binding_invalid")
        self.assertEqual(tree["nodes"][1]["repair_error"], "control_binding_invalid")
        self.assertNotEqual(bindings[0]["binding_id"], bindings[1]["binding_id"])
        self.assertNotIn(
            bindings[0]["binding_id"],
            {
                binding["binding_id"]
                for binding in self.requests[1]["controls"]["editable_bindings"]
            },
        )
        self.assertEqual(
            emitted[0]["control_binding_updates"][0]["binding_id"],
            "control:not-from-this-request",
        )
        self.assertEqual(
            emitted[1]["control_binding_updates"][0]["binding_id"],
            bindings[1]["binding_id"],
        )
        diagnostic = self.requests[1]["binding_error"]
        self.assertEqual(diagnostic, dict(reason="unknown_binding_id", update_index=0))
        self.assertEqual(
            tree["reflection_calls"]["1"]["control_binding_error"], diagnostic
        )
        self.assertNotIn("control:not-from-this-request", json.dumps(diagnostic))

    # 回归用例：test_editable_binding_budget_repair_receives_the_exact_allowed_range。
    # 重点核验字段或诊断：budget_out_of_range、control_binding_error；异常路径不能被当成成功。
    def test_editable_binding_budget_repair_receives_the_exact_allowed_range(self):
        tree, bindings, _ = self.exercise_editable_operation_binding(repair="budget")
        diagnostic = self.requests[1]["binding_error"]
        self.assertEqual(
            diagnostic,
            dict(
                reason="budget_out_of_range",
                update_index=0,
                budget=dict(field="max_attempts", min=1, max=2),
            ),
        )
        self.assertEqual(
            tree["reflection_calls"]["1"]["control_binding_error"], diagnostic
        )
        self.assertEqual(bindings[0]["budget"], diagnostic["budget"])
        self.assertEqual(tree["model_calls"], 2)

    # 回归用例：test_existing_concrete_operation_budget_remains_compatible_with_editable_bindings。
    # 重点核验字段或诊断：control_binding_updates；异常路径不能被当成成功。
    def test_existing_concrete_operation_budget_remains_compatible_with_editable_bindings(
        self,
    ):
        tree, _, emitted = self.exercise_editable_operation_binding(concrete=True)
        self.assertIn("controls", emitted[0])
        self.assertNotIn("control_binding_updates", emitted[0])
        self.assertNotIn("repair_error", tree["nodes"][1])

    # 回归用例：test_repeated_target_survives_redispatch_and_is_attributed。
    # 重点核验字段或诊断：gepa_repeat_target、validation_applied_changes、failure_key_candidates；异常路径不能被当成成功。
    def test_repeated_target_survives_redispatch_and_is_attributed(self):
        tree, job = self.check_action_feedback_contract(
            "gepa_repeat_target", "repeat_target"
        )
        self.assertEqual(job["status"], "passed", job)
        node = tree["nodes"][1]
        self.assertEqual(node["status"], "accepted", node)
        self.assertTrue(node["control_preflight"]["passed"])
        self.assertTrue(node["screening"]["applied_changes"], node["screening"])
        self.assertTrue(node["validation_applied_changes"]["0"])
        actions = [
            a
            for p in self.requests
            for row in p["feedback"]
            for run in row["trial"]["evidence"]["runs"]
            for a in run["actions"]
        ]
        candidates = [
            item
            for a in actions
            for item in (a.get("action_control") or {}).get(
                "failure_key_candidates", []
            )
        ]
        self.assertTrue(
            any(
                item["argument"] == "output" and item["prior_failures"] == 2
                for item in candidates
            ),
            candidates,
        )

    # 回归用例：test_schedule_success_contract_repairs_the_draft_without_another_model_call。
    # 重点核验字段或诊断：schedule_wrong_success、schedule_contract_repair；异常路径不能被当成成功。
    def test_schedule_success_contract_repairs_the_draft_without_another_model_call(
        self,
    ):
        self.schedule_dataset(
            {"train-a", "train-b", "val-a", "val-b", "holdout-a", "holdout-b"}
        )
        self.behavior = "schedule_wrong_success"
        self.config.update(
            max_level="L2",
            evaluation_levels=["L2"],
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
            instrumentation=dict(
                level="L2", capabilities=["model", "actions", "schedule"]
            ),
        )
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
        candidate = json.loads(
            (
                self.root / "production/agent-evaluation" / job_id / "candidate.json"
            ).read_text()
        )
        self.assertTrue(
            all(
                rule["require_success"]
                for rule in candidate["policy"]["options"]["schedule_rules"]
            )
        )

    # 回归用例：test_custom_metadata_is_not_mistaken_for_a_schedule_contract。
    # 重点核验字段或诊断：prepare_before_image、schedule_wrong_success、schedule_contract_repair；异常路径不能被当成成功。
    def test_custom_metadata_is_not_mistaken_for_a_schedule_contract(self):
        tasks = [
            json.loads(line)
            for line in (self.root / "gepa-tasks.jsonl").read_text().splitlines()
        ]
        for index, task in enumerate(tasks):
            task["attributes"] = (
                dict(dependency=["prepare", "image"], stop_on_failure=True)
                if index % 2
                else dict(dependency="prepare_before_image", stop_on_failure="true")
            )
        (self.root / "gepa-tasks.jsonl").write_text(
            "\n".join(json.dumps(task, ensure_ascii=False) for task in tasks),
            encoding="utf-8",
        )
        self.schedule_dataset({task["task_id"] for task in tasks}, reviewed=False)
        self.behavior = "schedule_wrong_success"
        self.config.update(
            max_level="L2",
            evaluation_levels=["L2"],
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
            instrumentation=dict(
                level="L2", capabilities=["model", "actions", "schedule"]
            ),
        )
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

    # 回归用例：test_reason_only_control_mutation_does_not_repeat_agent_evaluation。
    # 重点核验字段或诊断：gepa_schema_actions、no_control_predicate_change；异常路径不能被当成成功。
    def test_reason_only_control_mutation_does_not_repeat_agent_evaluation(self):
        self.behavior = "schema_actions"
        self.vary_control_reason = True
        self.config.update(
            max_level="L2",
            evaluation_levels=["L2"],
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_schema_actions"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            component_selector="model_choice", max_rounds=2, max_model_calls=2
        )
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, _, _ = self.wait_search("L2")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        node = tree["nodes"][2]
        self.assertEqual(node["status"], "duplicate", node)
        self.assertEqual(node["duplicate_of"], 1)
        self.assertEqual(node["skip_reason"], "no_control_predicate_change")
        self.assertFalse(
            any(e["version"] == node["version"] for e in tree["evaluations"].values())
        )

    # 测试辅助函数：run_recovery_budget_search。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def run_recovery_budget_search(
        self,
        conditional=False,
        model_calls=1,
        max_attempts=2,
        max_rounds=1,
        patience=0,
    ):
        self.behavior = "recovery_budget"
        self.conditional_recovery = conditional
        self.config.update(
            max_level="L3",
            evaluation_levels=["L3"],
            control_learning=dict(
                enabled=True,
                prompt_file="prompts/gepa-reflection.txt",
                max_rules=2,
                max_attempts=max_attempts,
            ),
        )
        self.config["runtime_options"]["aggressive_controls"] = True
        self.config["runtime_options"]["loop_rules"][0].update(
            max_no_progress=1, max_replans=1, no_progress_decision="replan"
        )
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_recovery_budget"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            component_selector="round_robin",
            max_rounds=max_rounds,
            max_model_calls=model_calls,
            patience=patience,
        )
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L3")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        return tree, job

    # 回归用例：test_l3_unconditioned_recovery_budget_is_rejected_before_execution。
    # 关键断言：self.assertEqual(node["status"], "untriggerable_controls", node)；此处只验证测试构造的场景。
    def test_l3_unconditioned_recovery_budget_is_rejected_before_execution(self):
        tree, _ = self.run_recovery_budget_search()
        node = tree["nodes"][1]
        self.assertEqual(node["status"], "untriggerable_controls", node)
        self.assertFalse(node["control_preflight"]["passed"])
        self.assertTrue(node["control_preflight"]["unconditioned_recovery"])
        self.assertNotIn("screening", node)
        self.assertNotIn("validation", node)
        self.assertEqual(tree["model_calls"], 1)
        self.assertNotIn("repair_error", node)

    def test_untriggerable_candidate_does_not_consume_patience(self):
        """没有进入候选评测的预检拒绝不能提前耗尽搜索耐心。"""
        tree, _ = self.run_recovery_budget_search(
            model_calls=2, max_attempts=1, max_rounds=2, patience=1
        )
        self.assertEqual(tree["nodes"][1]["status"], "untriggerable_controls", tree)
        self.assertNotIn("screening", tree["nodes"][1])
        self.assertEqual(tree["rounds"], 2, tree)
        self.assertNotEqual(tree["stop_reason"], "patience", tree)

    # 回归用例：test_l3_preflight_repairs_missing_recovery_condition_once。
    # 重点核验字段或诊断：recovery_condition_required、preflight_before_repair、repair_control_capability；异常路径不能被当成成功。
    def test_l3_preflight_repairs_missing_recovery_condition_once(self):
        self.recovery_repair = "success"
        tree, job = self.run_recovery_budget_search(model_calls=2)
        node = tree["nodes"][1]
        self.assertEqual(tree["model_calls"], 2)
        self.assertEqual(node["repair_error"], "recovery_condition_required")
        self.assertEqual(node["status"], "accepted", node)
        self.assertEqual(job["status"], "passed", job)
        self.assertTrue(node["control_preflight"]["passed"])
        self.assertTrue(node["preflight_before_repair"]["unconditioned_recovery"])
        self.assertTrue(
            any(
                row.get("loop", 0)
                for row in node["screening"]["applied_changes"].values()
            )
        )
        repair = self.requests[-1]
        self.assertEqual(repair["phase"], "repair_control_capability")
        self.assertEqual(repair["error"], "recovery_condition_required")
        self.assertEqual(repair["preflight"], node["preflight_before_repair"])
        original = repair["draft"]["controls"]["loop_rules"][0]
        self.assertEqual(original["max_replans"], 0)
        self.assertNotIn("error_codes", original)
        self.assertEqual(len(repair["feedback"]), 1)
        self.assertTrue(
            all(row["task_id"].startswith("train-") for row in repair["feedback"])
        )
        self.assertTrue(
            any(
                "FixtureUnavailable"
                in action.get("loop_control", {}).get("error_codes", [])
                for row in repair["feedback"]
                for run in row["trial"]["evidence"]["runs"]
                for action in run["actions"]
            )
        )

    # 回归用例：test_l3_preflight_does_not_retry_an_invalid_repair。
    # 重点核验字段或诊断：recovery_condition_required；异常路径不能被当成成功。
    def test_l3_preflight_does_not_retry_an_invalid_repair(self):
        tree, _ = self.run_recovery_budget_search(model_calls=3)
        node = tree["nodes"][1]
        self.assertEqual(tree["model_calls"], 2)
        self.assertEqual(node["repair_error"], "recovery_condition_required")
        self.assertEqual(node["status"], "untriggerable_controls", node)
        self.assertTrue(node["control_preflight"]["unconditioned_recovery"])
        self.assertNotIn("screening", node)
        self.assertEqual(len(self.requests), 2)

    # 回归用例：test_l3_preflight_respects_control_attempt_budget。
    # 关键断言：self.assertEqual(tree["model_calls"], 1)；此处只验证测试构造的场景。
    def test_l3_preflight_respects_control_attempt_budget(self):
        tree, _ = self.run_recovery_budget_search(model_calls=2, max_attempts=1)
        node = tree["nodes"][1]
        self.assertEqual(tree["model_calls"], 1)
        self.assertNotIn("repair_error", node)
        self.assertEqual(node["status"], "untriggerable_controls", node)
        self.assertNotIn("screening", node)

    # 回归用例：test_l3_preflight_does_not_spend_a_second_repair_after_source_repair。
    # 重点核验字段或诊断：control_source_not_approved、repair_control_source；异常路径不能被当成成功。
    def test_l3_preflight_does_not_spend_a_second_repair_after_source_repair(self):
        self.recovery_bad_source = True
        tree, _ = self.run_recovery_budget_search(model_calls=3)
        node = tree["nodes"][1]
        self.assertEqual(tree["model_calls"], 2)
        self.assertEqual(node["repair_error"], "control_source_not_approved")
        self.assertEqual(node["status"], "untriggerable_controls", node)
        self.assertEqual(
            [r.get("phase") for r in self.requests], [None, "repair_control_source"]
        )
        self.assertNotIn("screening", node)

    # 回归用例：test_l3_preflight_repair_can_keep_the_parent_unchanged。
    # 重点核验字段或诊断：recovery_condition_required；异常路径不能被当成成功。
    def test_l3_preflight_repair_can_keep_the_parent_unchanged(self):
        self.recovery_repair = "abstain"
        tree, _ = self.run_recovery_budget_search(model_calls=2)
        node = tree["nodes"][1]
        self.assertEqual(tree["model_calls"], 2)
        self.assertEqual(node["repair_error"], "recovery_condition_required")
        self.assertEqual(node["status"], "duplicate", node)
        self.assertEqual(node["duplicate_of"], 0)
        self.assertTrue(node["control_preflight"]["passed"])
        self.assertNotIn("screening", node)

    # 回归用例：test_l3_error_conditioned_recovery_budget_really_applies。
    # 关键断言：self.assertEqual(node["status"], "accepted", node)；此处只验证测试构造的场景。
    def test_l3_error_conditioned_recovery_budget_really_applies(self):
        tree, job = self.run_recovery_budget_search(conditional=True)
        node = tree["nodes"][1]
        self.assertEqual(node["status"], "accepted", node)
        self.assertEqual(job["status"], "passed", job)
        self.assertTrue(node["control_preflight"]["passed"])
        self.assertNotIn("unconditioned_recovery", node["control_preflight"])
        self.assertTrue(
            any(
                row.get("loop", 0)
                for row in node["screening"]["applied_changes"].values()
            )
        )
        self.assertTrue(
            any(
                r["boundary"] == "loop" and r["applied"] > 0
                for r in node["control_coverage"]["rules"].values()
            )
        )
        observed = [
            action.get("loop_control") or {}
            for packet in self.requests
            for row in packet.get("feedback", [])
            for run in row["trial"]["evidence"].get("runs", [])
            for action in run.get("actions", [])
        ]
        self.assertTrue(
            any(
                "FixtureUnavailable" in control.get("error_codes", [])
                for control in observed
            )
        )
        # 可执行规则来自持久化版本，不来自反思正文。
        job_id = next(
            key
            for key, value in self.call("agent_evaluation.status")["jobs"].items()
            if value.get("gepa") and value["level"] == "L3"
        )
        version = json.loads(
            (
                self.root
                / "production/agent-evaluation"
                / job_id
                / "search/versions/1.json"
            ).read_text()
        )
        generated = [
            rule
            for rule in version["policy"]["options"]["loop_rules"]
            if rule.get("source_experience")
        ]
        self.assertEqual(len(generated), 1)
        self.assertEqual(generated[0]["error_codes"], ["FixtureUnavailable"])
        self.assertGreater(
            node["control_coverage"]["rules"][generated[0]["rule_id"]]["applied"], 0
        )

    # 回归用例：test_truncated_present_argument_does_not_create_missing_argument_evidence。
    # 重点核验字段或诊断：gepa_schema_wide；异常路径不能被当成成功。
    def test_truncated_present_argument_does_not_create_missing_argument_evidence(self):
        tree, _ = self.check_action_feedback_contract(
            "gepa_schema_wide", "empty_change"
        )
        self.assertFalse(tree.get("training_opportunities", {}).get("action_rules"))
        self.assertNotIn("action_rules", tree["nodes"][1]["components"])

    # 回归用例：test_late_missing_argument_survives_control_feedback_budget。
    # 重点核验字段或诊断：gepa_schema_late、missing_required_arguments；异常路径不能被当成成功。
    def test_late_missing_argument_survives_control_feedback_budget(self):
        tree, job = self.check_action_feedback_contract(
            "gepa_schema_late", "schema_actions"
        )
        self.assertEqual(job["status"], "passed", job)
        actions = [
            a
            for p in self.requests
            for row in p["feedback"]
            for run in row["trial"]["evidence"]["runs"]
            for a in run["actions"]
        ]
        self.assertTrue(
            any(
                a.get("action_control", {}).get("missing_required_arguments")
                == ["zz_value"]
                for a in actions
            )
        )
        self.assertTrue(tree["nodes"][1]["control_preflight"]["passed"])

    # 回归用例：test_operation_guard_is_persisted_and_does_not_invent_execution。
    # 关键断言：self.assertEqual(；此处只验证测试构造的场景。
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
        self.assertEqual(
            child["policy"]["options"]["experience_overrides"][0][
                "observed_operations"
            ],
            ["calculate"],
        )
        self.assertEqual(tree["nodes"][1]["status"], "rejected_minibatch", tree)
        # 该测试宿主唯一的模型调用发生在 calculate 之前，
        # 其文字不能伪造候选要求的已完成动作证据。
        self.assertEqual(
            tree["nodes"][1]["screening"]["control_coverage"]["capabilities"]["llm"][
                "applied"
            ],
            0,
        )

    # 回归用例：test_trigger_override_survives_further_content_mutation_and_export。
    # 关键断言：self.assertEqual(；此处只验证测试构造的场景。
    def test_trigger_override_survives_further_content_mutation_and_export(self):
        self.behavior = "trigger_override"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            max_rounds=2, max_model_calls=2, candidate_selection_strategy="current_best"
        )
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertNotIn("error", [node["status"] for node in tree["nodes"]], tree)
        root = self.root / "production/agent-evaluation" / job_id / "search"
        first = json.loads((root / "versions/1.json").read_text())
        second = json.loads((root / "versions/2.json").read_text())
        for version in [first, second]:
            self.assertEqual(
                version["policy"]["options"]["experience_overrides"][0][
                    "trigger_pattern"
                ],
                "生成视频时缺少图片素材，核验输入",
            )
        self.assertEqual(tree["nodes"][2]["parents"], [1])
        self.assertEqual(first["policy"]["snapshot"], second["policy"]["snapshot"])
        self.assertIsInstance(
            self.requests[1]["components"]["experience:test-experience"], dict
        )
        self.assertTrue(
            any(
                "experience_query" in action
                for packet in self.requests
                for feedback in packet["feedback"]
                for run in feedback["trial"]["evidence"]["runs"]
                for action in run["actions"]
            )
        )

    # 回归用例：test_empty_proposal_preserves_seed_and_skips_candidate_execution。
    # 重点核验字段或诊断：no_behavior_change；异常路径不能被当成成功。
    def test_empty_proposal_preserves_seed_and_skips_candidate_execution(self):
        self.behavior = "empty_change"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=2, max_model_calls=2, patience=1)
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
        self.assertNotEqual(tree["stop_reason"], "patience", tree)
        self.assertEqual(tree.get("stagnation", 0), 0, tree)
        root = self.root / "production/agent-evaluation" / job_id / "search"
        self.assertEqual(
            json.loads((root / "versions/0.json").read_text()),
            json.loads((root / "versions/1.json").read_text()),
        )

    # 回归用例：test_inherited_unobserved_control_does_not_block_experience_search。
    # 重点核验字段或诊断：improvement_or_equal、quality_below_threshold、generated_control_not_observed；异常路径不能被当成成功。
    def test_inherited_unobserved_control_does_not_block_experience_search(self):
        self.config.update(
            max_level="L2",
            evaluation_levels=[],
            control_learning=dict(
                enabled=True,
                prompt_file="prompts/gepa-reflection.txt",
                actions={"unused-tool": ["verified"]},
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            max_rounds=1,
            max_model_calls=1,
            minibatch_size=1,
            acceptance="improvement_or_equal",
        )
        fixture.write(self.root / "gepa.json", config)
        request = dict(
            memory_path=str(
                self.root / "production/by-user/user-a/memory/evolution-memory.pb"
            ),
            identity=dict(
                agent_id="eval-test",
                user_id="user-a",
                conversation_id="test",
                run_id="compile",
            ),
            shadow_options=dict(
                self.config["shadow_options"],
                target_scopes=["agent:eval-test/actor:assistant"],
            ),
            options=dict(
                requested_level="L2",
                host_capabilities=["context.request", "action.filter"],
                allowed_users=["user-a"],
                allowed_conversations=["test"],
                fallback="strict",
                experience_template="{{experience}}",
                max_context_bytes=4096,
            ),
        )
        compiled = subprocess.run(
            [str(fixture.CLI), "runtime", "compile"],
            input=json.dumps(request),
            capture_output=True,
            text=True,
            timeout=20,
        )
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        policy = json.loads(compiled.stdout)
        approved = [
            dict(memory_id=item["memory_id"], content_revision=item["content_revision"])
            for item in policy["snapshot"]["experiences"]
        ]
        old_rule = dict(
            target_scope="agent:eval-test/actor:assistant",
            operation="unused-tool",
            required_arguments=["verified"],
            source_experience=approved[0],
            match_mode="predicate",
            reason="Only applies to a different tool.",
        )
        old_id = (
            "learned-"
            + fixture.hashlib.sha256(
                json.dumps(
                    old_rule, ensure_ascii=False, sort_keys=True, separators=(",", ":")
                ).encode()
            ).hexdigest()[:32]
        )
        old_rule["rule_id"] = old_id
        options = dict(
            policy["options"], approved_experiences=approved, action_rules=[old_rule]
        )
        compiled = subprocess.run(
            [str(fixture.CLI), "runtime", "recompile"],
            input=json.dumps(dict(policy=policy, options=options)),
            capture_output=True,
            text=True,
            timeout=20,
        )
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        artifact = dict(
            schema_version=1,
            agent=self.assessment["agent"],
            user_id="user-a",
            level="L2",
            policy=json.loads(compiled.stdout),
        )
        self.start()
        self.call("agent_evaluation.submit", artifact=artifact)
        job_id, job, _ = self.wait_search("L2")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        node = tree["nodes"][1]
        self.assertEqual(node["status"], "accepted", node)
        self.assertNotIn("validation_probe", node)
        self.assertEqual(len(node["validation"]), 2)
        self.assertTrue(node["screening"]["applied_changes"])
        coverage = node["control_coverage"]
        self.assertFalse(coverage["complete"])
        self.assertEqual(coverage["rules"][old_id]["applied"], 0)
        self.assertTrue(
            any(
                rule["boundary"] == "llm" and rule["applied"] > 0
                for rule in coverage["rules"].values()
            )
        )
        self.assertEqual(
            tree["winner"], 1, "unused inherited rule must not suppress measured gains"
        )
        self.assertFalse(job.get("published", False), job)
        self.assertEqual(job["gate"]["reasons"], ["quality_below_threshold"])
        self.assertFalse(job["gate"]["control_coverage"]["complete"])
        self.assertNotIn("generated_control_not_observed", job["gate"]["reasons"])

    # 回归用例：test_explicit_protobuf_defaults_do_not_create_a_new_candidate。
    # 重点核验字段或诊断：request_context_mode；异常路径不能被当成成功。
    def test_explicit_protobuf_defaults_do_not_create_a_new_candidate(self):
        self.behavior = "empty_change"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.config["evaluation_levels"] = []
        fixture.write(self.root / "service.json", self.config)
        request = dict(
            memory_path=str(
                self.root / "production/by-user/user-a/memory/evolution-memory.pb"
            ),
            identity=dict(
                agent_id="eval-test",
                user_id="user-a",
                conversation_id="test",
                run_id="compile",
            ),
            shadow_options=dict(
                self.config["shadow_options"],
                target_scopes=["agent:eval-test/actor:assistant"],
            ),
            options=dict(
                requested_level="L1",
                host_capabilities=["context.request"],
                allowed_users=["user-a"],
                fallback="strict",
                experience_template="{{experience}}",
                max_context_bytes=4096,
            ),
        )
        compiled = subprocess.run(
            [str(fixture.CLI), "runtime", "compile"],
            input=json.dumps(request),
            capture_output=True,
            text=True,
            timeout=20,
        )
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        policy = json.loads(compiled.stdout)
        options = dict(
            policy["options"],
            approved_experiences=[
                dict(
                    memory_id=item["memory_id"],
                    content_revision=item["content_revision"],
                )
                for item in policy["snapshot"]["experiences"]
            ],
        )
        compiled = subprocess.run(
            [str(fixture.CLI), "runtime", "recompile"],
            input=json.dumps(dict(policy=policy, options=options)),
            capture_output=True,
            text=True,
            timeout=20,
        )
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        policy = json.loads(compiled.stdout)
        self.assertIn("request_context_mode", policy["options"])
        artifact = dict(
            schema_version=1,
            agent=self.assessment["agent"],
            user_id="user-a",
            level="L1",
            policy=policy,
        )
        self.start()
        self.call("agent_evaluation.submit", artifact=artifact)
        job_id, _, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        child = tree["nodes"][1]
        self.assertEqual(child["status"], "duplicate", child)
        self.assertEqual(child["version"], tree["nodes"][0]["version"])
        self.assertNotIn("screening", child)

    # 回归用例：test_l3_experience_only_is_not_a_full_level_publication。
    # 重点核验字段或诊断：require_level_coverage；异常路径不能被当成成功。
    def test_l3_experience_only_is_not_a_full_level_publication(self):
        self.assessment["gate"]["require_level_coverage"] = True
        fixture.write(self.root / "agent-evaluation.json", self.assessment)
        self.config.update(max_level="L3", evaluation_levels=["L3"])
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=2, max_model_calls=2)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L3")
        self.assertEqual(job["status"], "rejected", job)
        self.assertEqual(job["gate"]["stage"], "validation")
        coverage = job["gate"]["control_coverage"]
        self.assertGreater(coverage["capabilities"]["llm"]["applied"], 0)
        self.assertIn("loop", coverage["unobserved_capabilities"])
        self.assertFalse(
            (
                self.root
                / "production/agent-evaluation"
                / job_id
                / "search/published-version.json"
            ).exists()
        )

    # 回归用例：test_reflection_tree_holdout_publication_and_restart。
    # 关键断言：self.assertEqual(job["status"], "passed", job)；此处只验证测试构造的场景。
    def test_reflection_tree_holdout_publication_and_restart(self):
        self.leak_during_repair = True
        self.start()
        job_id, job, status = self.wait_search()
        self.assertEqual(job["status"], "passed", job)
        self.assertTrue(job["published"], job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(int(tree["model_calls"]), 6)
        # 第四个草稿的授权纠错占一次模型调用，但不会额外生成一个搜索节点。
        self.assertEqual(len(tree["nodes"]), 6)
        self.assertEqual(
            sum(packet.get("phase") == "repair_component_selection"
                for packet in self.requests), 1
        )
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
                self.assertNotIn(
                    self.env["GEPA_LEAK_SECRET"].encode(), path.read_bytes(), str(path)
                )
        self.assertFalse(
            self.call("agent_evaluation.tree", token=fixture.TOKEN_B, job_id=job_id)[
                "ok"
            ]
        )
        count = len(self.requests)
        fixture.AgentEvaluationTests.tearDown(self)
        self.start()
        _, restored, status = self.wait_search()
        self.assertEqual(restored["version"], job["version"])
        self.assertEqual(len(self.requests), count)
        self.assertEqual(
            len([value for value in status["jobs"].values() if value.get("gepa")]), 1
        )
        fixture.write(
            self.root / "test-summary.json",
            dict(
                job_id=job_id,
                search=job["search"],
                states=sorted(states),
                published=True,
                privacy="passed",
                restart="no duplicate search",
            ),
        )
        print("GEPA artifact:", self.root / "test-summary.json")

    # 回归用例：test_json_fence_preserves_search_publication_and_privacy_gates。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_json_fence_preserves_search_publication_and_privacy_gates(self):
        self.fenced = True
        self.test_reflection_tree_holdout_publication_and_restart()

    # 回归用例：test_final_holdout_reuses_only_fixed_baseline_across_search_jobs。
    # 重点核验字段或诊断：gepa_holdout_failure、holdout_baseline_cache、reused_baseline_trials；异常路径不能被当成成功。
    def test_final_holdout_reuses_only_fixed_baseline_across_search_jobs(self):
        # 默认配置不再施加绝对质量线；本用例显式开启该可选门槛，专门验证
        # 历史 L0 复用不会绕过调用方明确要求的发布约束。
        self.assessment["gate"]["min_pass_rate"] = 0.8
        fixture.write(self.root / "agent-evaluation.json", self.assessment)
        self.evaluation["variants"][0]["command"][-2] = "gepa_holdout_failure"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        first_id, first_job, _ = self.wait_search()
        self.assertEqual(first_job["status"], "rejected", first_job)
        first = self.call("agent_evaluation.tree", job_id=first_id)
        evaluations = self.root / "production/agent-evaluation"
        old_final = next(
            row
            for row in first["evaluations"].values()
            if row["split"] == "holdout" and row.get("baseline_version")
        )
        old_report = json.loads(
            (evaluations / first_id / "search" / old_final["report"]).read_text()
        )
        selected = json.loads((evaluations / first_id / "candidate.json").read_text())
        self.process.terminate()
        self.process.communicate(timeout=10)
        self.process = None
        self.behavior = "empty_change"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        # 本用例只检查显式提交的候选复用 L0 缓存。关闭启动时自动提交，
        # 否则两种提交入口竞逐同一发布槽，人工候选可能正常被更新的自动作业取代。
        self.config["evaluation_levels"] = []
        fixture.write(self.root / "service.json", self.config)
        self.start()
        second_id = self.call("agent_evaluation.submit", artifact=selected)["job_id"]
        deadline = time.monotonic() + 70
        while time.monotonic() < deadline:
            second_job = self.call("agent_evaluation.status")["jobs"][second_id]
            if second_job["status"] not in ("pending", "running"):
                break
            time.sleep(0.05)
        else:
            self.fail("second final comparison did not finish")
        self.assertEqual(second_job["status"], "rejected", second_job)
        second = self.call("agent_evaluation.tree", job_id=second_id)
        self.assertEqual(second["baseline_cache"]["status"], "hit", second)
        self.assertEqual(second["holdout_baseline_cache"]["status"], "hit", second)
        final = next(
            row
            for row in second["evaluations"].values()
            if row["split"] == "holdout" and row.get("baseline_version")
        )
        fresh = next(
            row
            for row in second["evaluations"].values()
            if row["split"] == "holdout" and not row.get("baseline_version")
        )
        self.assertEqual(final["trials_reserved"], 0)
        self.assertEqual(fresh["trials_reserved"], 2)
        result = json.loads(
            (evaluations / second_id / "search" / final["report"]).read_text()
        )
        self.assertEqual(result["report_kind"], "gepa_cached_pair.v1")
        self.assertEqual(result["summary"]["reused_baseline_trials"], 2)
        self.assertEqual(result["summary"]["comparison_timing"], "historical_baseline")
        old_base = [t for t in old_report["trials"] if t["variant_id"] == "baseline"]
        new_base = [t for t in result["trials"] if t["variant_id"] == "baseline"]
        self.assertEqual(old_base, new_base)
        self.assertEqual(
            result["summary"]["by_variant"]["baseline"],
            old_report["summary"]["by_variant"]["baseline"],
        )
        self.assertEqual(result["comparisons"]["candidate"]["paired_trials"], 2)
        self.assertFalse(
            {
                t["trial_id"]
                for t in old_report["trials"]
                if t["variant_id"] == "candidate"
            }
            & {
                t["trial_id"]
                for t in result["trials"]
                if t["variant_id"] == "candidate"
            }
        )
        self.assertEqual(second_job["status"], "rejected", second_job)
        # 该 fixture 对所有 holdout 版本都返回 0；验证绝对质量门槛未被历史配对绕过。
        self.assertEqual(result["summary"]["by_variant"]["baseline"]["pass_rate"], 0)
        self.assertEqual(result["summary"]["by_variant"]["candidate"]["pass_rate"], 0)
        self.assertIn("quality_below_threshold", second_job["gate"]["reasons"])

    # 回归用例：test_top_three_finalists_keep_frozen_validation_order。
    # 重点核验字段或诊断：frozen_validation_rank、final_holdout_selection；异常路径不能被当成成功。
    def test_top_three_finalists_keep_frozen_validation_order(self):
        """Top-3 留出只验证已冻结的候选，不能按留出分数重新排序。"""
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(holdout_top_k=3, max_rounds=8, max_model_calls=8, max_trials=180)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search()
        self.assertEqual(job["status"], "passed", job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        final = tree["final_holdout"]
        self.assertEqual(final["protocol"], "top-k-v1", final)
        self.assertEqual(final["selection"], "frozen_validation_rank", final)
        finalists = final["candidates"]
        # Top-K 只包含合格且版本不同的候选；不能为凑满三条引入拒绝／重复节点。
        self.assertGreaterEqual(len(finalists), 1, final)
        self.assertLessEqual(len(finalists), 3, final)
        self.assertEqual(
            [row["rank"] for row in finalists], list(range(1, len(finalists) + 1))
        )
        self.assertTrue(all(row["status"] == "complete" for row in finalists), final)
        # 三个候选必须共用同一份冻结 L0 基线，而候选自身均重新执行。
        identities = [row["baseline_source"]["identity"] for row in finalists]
        self.assertTrue(
            all(identity == identities[0] for identity in identities[1:]), final
        )
        gates = job["search"]["finalist_gates"]
        self.assertEqual(
            [row["node"] for row in gates], [row["node"] for row in finalists]
        )
        self.assertEqual(
            [row["rank"] for row in gates], list(range(1, len(finalists) + 1))
        )
        passing = next((row for row in gates if row["scored"] and row["passed"]), None)
        selection = job["search"]["final_holdout_selection"]
        # 无候选过门禁时，结果仍稳定归因给第 1 名；否则取第一个通过者。
        self.assertEqual(selection, passing if passing is not None else gates[0])
        self.assertEqual(job["version"], selection["version"])

    # 回归用例：test_single_prefixed_json_object_recovers_without_another_model_call。
    # 重点核验字段或诊断：single_json_object；异常路径不能被当成成功。
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
        self.assertEqual(
            int(recovery["provider_envelope"]) + int(recovery["message_content"]), 1
        )
        self.assertEqual(tree["model_calls"], 1)

    # 回归用例：test_disabled_experience_remains_available_for_later_generations。
    # 重点核验字段或诊断：improvement_or_equal；异常路径不能被当成成功。
    def test_disabled_experience_remains_available_for_later_generations(self):
        self.behavior = "disable_enable"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            max_rounds=2, max_model_calls=2, acceptance="improvement_or_equal"
        )
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search()
        root = self.root / "production/agent-evaluation" / job_id
        seed = json.loads((root / "seed.json").read_text())
        disabled = json.loads((root / "search/versions/1.json").read_text())
        self.assertEqual(
            disabled["policy"]["options"]["approved_experiences"],
            seed["policy"]["options"]["approved_experiences"],
        )
        self.assertEqual(
            disabled["policy"]["options"]["disabled_injections"],
            seed["policy"]["options"]["approved_experiences"],
        )
        self.assertFalse(disabled["policy"]["options"].get("experience_overrides"))
        self.assertEqual(disabled["policy"]["snapshot"], seed["policy"]["snapshot"])
        self.assertIsNone(self.requests[1]["components"]["experience:test-experience"])
        self.assertEqual(
            self.requests[1]["experiences"][0]["memory_id"], "test-experience"
        )
        restored = json.loads((root / "search/versions/2.json").read_text())
        self.assertEqual(
            restored["policy"]["options"]["approved_experiences"],
            seed["policy"]["options"]["approved_experiences"],
        )
        self.assertFalse(restored["policy"]["options"].get("disabled_injections"))
        self.assertEqual(restored["policy"]["snapshot"], seed["policy"]["snapshot"])
        self.assertEqual(job["status"], "passed", job)

    # 回归用例：test_second_same_parent_proposal_automatically_ablates_an_applied_experience。
    # 重点核验字段或诊断：improvement_or_equal；异常路径不能被当成成功。
    def test_second_same_parent_proposal_automatically_ablates_an_applied_experience(
        self,
    ):
        self.evaluation["variants"][0]["command"][-2] = "memory_bad"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            max_rounds=2,
            max_model_calls=2,
            proposals_per_round=2,
            sampling_strategy="same_parent",
            acceptance="improvement_or_equal",
        )
        config["objective"]["token_weight"] = 0  # 该测试宿主刻意不提供 token 回执。
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, _, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        model_node, ablation = tree["nodes"][1:3]
        self.assertEqual(model_node["parent"], ablation["parent"])
        self.assertEqual(model_node["components"], ablation["components"])
        self.assertEqual(ablation["generation_method"], "experience_ablation")
        self.assertNotIn("generation_method", model_node)
        self.assertEqual(
            tree["model_calls"], 1, "ablation must not spend an extra reflection call"
        )
        self.assertEqual(len(self.requests), 1)
        self.assertTrue(ablation["screening"]["applied_changes"], ablation)
        self.assertEqual(ablation["status"], "accepted", ablation)
        self.assertTrue(
            all(row["quality"] == 0 for row in ablation["screening"]["parent"].values())
        )
        self.assertTrue(
            all(
                row["quality"] == 1
                for row in ablation["screening"]["candidate"].values()
            )
        )
        folder = self.root / "production/agent-evaluation" / job_id / "search"
        disabled = json.loads((folder / "versions/2.json").read_text())["policy"][
            "options"
        ]
        self.assertEqual(
            disabled["disabled_injections"], disabled["approved_experiences"]
        )

    # 回归用例：test_independent_sampling_ablates_once_then_preserves_model_search。
    # 重点核验字段或诊断：improvement_or_equal；异常路径不能被当成成功。
    def test_independent_sampling_ablates_once_then_preserves_model_search(self):
        self.evaluation["variants"][0]["command"][-2] = "memory_bad"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            max_rounds=2,
            max_model_calls=2,
            proposals_per_round=2,
            sampling_strategy="independent",
            component_selector="round_robin",
            acceptance="improvement_or_equal",
        )
        config["objective"]["token_weight"] = 0
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, _, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        ablation, model_node = tree["nodes"][1:3]
        self.assertEqual(ablation["proposal_ordinal"], 0)
        self.assertEqual(model_node["proposal_ordinal"], 0)
        self.assertEqual(ablation["parent"], model_node["parent"])
        self.assertEqual(ablation["components"], model_node["components"])
        self.assertEqual(ablation["generation_method"], "experience_ablation")
        self.assertNotIn("generation_method", model_node)
        self.assertEqual(tree["model_calls"], 1)
        self.assertEqual(len(self.requests), 1)
        self.assertTrue(ablation["screening"]["applied_changes"], ablation)
        self.assertEqual(ablation["status"], "accepted", ablation)
        self.assertTrue(
            all(row["quality"] == 0 for row in ablation["screening"]["parent"].values())
        )
        self.assertTrue(
            all(
                row["quality"] == 1
                for row in ablation["screening"]["candidate"].values()
            )
        )
        folder = self.root / "production/agent-evaluation" / job_id / "search"
        disabled = json.loads((folder / "versions/1.json").read_text())["policy"][
            "options"
        ]
        self.assertEqual(
            disabled["disabled_injections"], disabled["approved_experiences"]
        )
        ordinary = json.loads((folder / "versions/2.json").read_text())["policy"][
            "options"
        ]
        self.assertFalse(ordinary.get("disabled_injections"))

    # 回归用例：test_independent_sampling_does_not_ablate_a_successful_unwasteful_injection。
    # 重点核验字段或诊断：improvement_or_equal；异常路径不能被当成成功。
    def test_independent_sampling_does_not_ablate_a_successful_unwasteful_injection(
        self,
    ):
        self.evaluation["variants"][0]["command"][-2] = "memory_good"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            max_rounds=2,
            max_model_calls=2,
            proposals_per_round=2,
            sampling_strategy="independent",
            component_selector="round_robin",
            acceptance="improvement_or_equal",
        )
        config["objective"]["token_weight"] = 0
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, _, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(len(tree["nodes"]), 3)
        self.assertEqual(tree["model_calls"], 2)
        self.assertEqual(len(self.requests), 2)
        self.assertTrue(
            all("generation_method" not in node for node in tree["nodes"][1:])
        )

    # 回归用例：test_search_uses_reviewed_loops_instead_of_the_zero_rule_bound。
    # 重点核验字段或诊断：invalid_loop_assessment；异常路径不能被当成成功。
    def test_search_uses_reviewed_loops_instead_of_the_zero_rule_bound(self):
        self.judge_requests = []
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.evaluation["checks"] = {}
        self.evaluation["variants"][0]["command"][-2] = "gepa_review"
        self.evaluation["judge"] = dict(
            enabled=True,
            runtime_config="model.json",
            prompt="prompts/task-judge-system.txt",
            loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100,
            max_input_bytes=1048576,
        )
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        job_id, job, _ = self.wait_search()
        self.assertEqual(job["status"], "passed", job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        for score in tree["nodes"][0]["validation"].values():
            self.assertEqual(
                score["loop_efficiency"], 0.5
            )  # 模型判定数量为 1，规则下界为 0。
        self.assertTrue(self.judge_requests)
        self.assertEqual(
            job["gate"]["candidate"]["invalid_loop_assessment"]["method"],
            "model_review",
        )

    # 回归用例：test_reflection_limit_keeps_successful_but_wasteful_training_task。
    # 关键断言：self.assertEqual(len(self.requests), 1)；此处只验证测试构造的场景。
    def test_reflection_limit_keeps_successful_but_wasteful_training_task(self):
        self.judge_requests = []
        self.rank_feedback = True
        self.behavior = "no_change"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1, max_feedback_trials=1)
        fixture.write(self.root / "gepa.json", config)
        self.evaluation["checks"] = {}
        self.evaluation["variants"][0]["command"][-2] = "gepa_review"
        self.evaluation["judge"] = dict(
            enabled=True,
            runtime_config="model.json",
            prompt="prompts/task-judge-system.txt",
            loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100,
            max_input_bytes=1048576,
        )
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        self.wait_search()
        self.assertEqual(len(self.requests), 1)
        feedback = self.requests[0]["feedback"]
        self.assertEqual([row["task_id"] for row in feedback], ["train-b"])
        self.assertEqual(feedback[0]["trial"]["verdict"], "passed")
        self.assertEqual(feedback[0]["trial"]["evidence"]["loop_review"]["count"], 1)

    # 回归用例：test_reflection_separates_reviewed_waste_from_runtime_loop_counter。
    # 重点核验字段或诊断：query_event_id、observed_no_progress；异常路径不能被当成成功。
    def test_reflection_separates_reviewed_waste_from_runtime_loop_counter(self):
        self.judge_requests = []
        self.behavior = "no_change"
        self.config.update(
            max_level="L3",
            evaluation_levels=["L3"],
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.evaluation["checks"] = {}
        self.evaluation["variants"][0]["command"][-2] = "gepa_review"
        self.evaluation["judge"] = dict(
            enabled=True,
            runtime_config="model.json",
            prompt="prompts/task-judge-system.txt",
            loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100,
            max_input_bytes=1048576,
        )
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        self.wait_search("L3")
        packet = next(
            p for p in self.requests if p.get("controls", {}).get("level") == "L3"
        )
        for row in packet["feedback"]:
            evidence = row["trial"]["evidence"]
            self.assertEqual(evidence["loop_review"]["count"], 1)
            loops = [
                a
                for run in evidence["runs"]
                for a in run["actions"]
                if a["kind"] == "loop.decision"
            ]
            self.assertTrue(loops)
            for action in loops:
                control = action["loop_control"]
                self.assertTrue(control["query_event_id"])
                self.assertEqual(control["observed_no_progress"], 0)
                self.assertEqual(control["proposed_decision"], "continue")
                self.assertFalse(control["pending_actions"])
                self.assertNotIn("fingerprint", control)

    # 回归用例：test_reflection_keeps_actual_loop_repetition_counters。
    # 重点核验字段或诊断：observed_no_progress、query_event_id；异常路径不能被当成成功。
    def test_reflection_keeps_actual_loop_repetition_counters(self):
        self.exercise_controls("loops", "L3")
        packet = next(
            p for p in self.requests if p.get("controls", {}).get("level") == "L3"
        )
        for row in packet["feedback"]:
            loops = [
                a
                for run in row["trial"]["evidence"]["runs"]
                for a in run["actions"]
                if a["kind"] == "loop.decision"
            ]
            self.assertEqual(
                [a["loop_control"]["observed_no_progress"] for a in loops], [0, 1, 2, 3]
            )
            self.assertEqual(
                len({a["loop_control"]["query_event_id"] for a in loops}), 4
            )

    # 回归用例：test_reflection_disabled_loop_counter_stays_unknown。
    # 重点核验字段或诊断：observed_no_progress；异常路径不能被当成成功。
    def test_reflection_disabled_loop_counter_stays_unknown(self):
        self.behavior = "no_change"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_review"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        self.wait_search()  # L1 记录循环打点，但不计算 L3 控制计数。
        loops = [
            a
            for row in self.requests[0]["feedback"]
            for run in row["trial"]["evidence"]["runs"]
            for a in run["actions"]
            if a["kind"] == "loop.decision"
        ]
        self.assertTrue(loops)
        for action in loops:
            self.assertIsNone(action["loop_control"]["observed_no_progress"])

    # 回归用例：test_l1_rewrite_without_actual_injection_is_not_accepted。
    # 重点核验字段或诊断：improvement_or_equal；异常路径不能被当成成功。
    def test_l1_rewrite_without_actual_injection_is_not_accepted(self):
        self.behavior = "unused_experience"
        self.evaluation["variants"][0]["command"][-2] = "good"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.config["runtime_options"] = dict(max_context_bytes=1536)
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            max_rounds=1,
            max_model_calls=1,
            minibatch_size=1,
            acceptance="improvement_or_equal",
            objective=dict(invalid_loop_weight=0, token_weight=0, latency_weight=0),
        )
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

    # 回归用例：test_l1_rewrite_application_evidence_is_content_specific。
    # 关键断言：self.assertEqual(node["status"], "accepted", node)；此处只验证测试构造的场景。
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

    # 回归用例：test_l1_feedback_prioritizes_its_own_applied_experience。
    # 关键断言：self.assertEqual(result.returncode, 0, result.stderr)；此处只验证测试构造的场景。
    def test_l1_feedback_prioritizes_its_own_applied_experience(self):
        tasks = [
            json.loads(line)
            for line in (self.root / "gepa-tasks.jsonl").read_text().splitlines()
        ]
        for task in tasks:
            if task["task_id"] == "train-a":
                task["task_input"] = (
                    "TRAIN_VISIBLE 甲类：只完成独立计算，不涉及其他任务。"
                )
        (self.root / "gepa-tasks.jsonl").write_text(
            "\n".join(json.dumps(task, ensure_ascii=False) for task in tasks),
            encoding="utf-8",
        )
        result = subprocess.run(
            [
                str(fixture.CLI),
                "dataset",
                "build",
                "--config",
                str(self.root / "gepa-dataset.json"),
                "--output-root",
                str(self.root / "relevance-dataset"),
            ],
            capture_output=True,
            text=True,
            timeout=20,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assessment["dataset"] = "relevance-dataset"
        fixture.write(self.root / "agent-evaluation.json", self.assessment)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1, max_feedback_trials=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        self.wait_search()
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(
            self.requests[0]["selected_components"], ["experience:test-experience"]
        )
        self.assertEqual(
            [row["task_id"] for row in self.requests[0]["feedback"]], ["train-b"]
        )
        self.assertNotIn("VALIDATION_PRIVATE", json.dumps(self.requests[0]))
        self.assertNotIn("HOLDOUT_PRIVATE", json.dumps(self.requests[0]))

    # 回归用例：test_validation_failure_does_not_start_holdout_or_export。
    # 关键断言：self.assertTrue(self.requests)；此处只验证测试构造的场景。
    def test_validation_failure_does_not_start_holdout_or_export(self):
        self.behavior = "empty_change"
        self.release.clear()
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        deadline = time.monotonic() + 20
        while not self.requests and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertTrue(self.requests)
        pending = self.call("agent_evaluation.status")
        current = next(
            key for key, value in pending["jobs"].items() if value.get("gepa")
        )
        search = self.root / "production/agent-evaluation" / current / "search"
        fixture.write(search / "selected-version.json", {"legacy_selection": True})
        self.release.set()
        job_id, job, status = self.wait_search()
        self.assertEqual(job["status"], "rejected", job)
        self.assertEqual(job["gate"]["stage"], "validation")
        self.assertNotIn("error", job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertFalse(tree.get("holdout_started", False))
        self.assertFalse(tree["nodes"][int(tree["winner"])]["optimization"]["passed"])
        self.assertFalse(
            any(row["split"] == "holdout" for row in tree["evaluations"].values())
        )
        self.assertNotIn("L1", status["publications"])
        search = self.root / "production/agent-evaluation" / job_id / "search"
        self.assertFalse((search / "selected-version.json").exists())
        self.assertFalse((search / "published-version.json").exists())

    # 回归用例：test_holdout_rejection_does_not_publish_or_reflect_again。
    # 重点核验字段或诊断：gepa_holdout_failure；异常路径不能被当成成功。
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
        self.assertFalse(
            (
                self.root
                / "production/agent-evaluation"
                / job_id
                / "search/published-version.json"
            ).exists()
        )

    def assert_learning_cli_signal_resume(self, stop_signal):
        """离线 CLI 收信号后保存取消点，重启继续原作业并复用已完成试题。"""
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=2, max_model_calls=2)
        fixture.write(self.root / "gepa.json", config)
        self.release.clear()
        command = [str(fixture.CLI), "learn", "--evaluation-only", "--config",
                   str(self.root / "service.json")]
        self.process = subprocess.Popen(command, env=self.env, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True)
        deadline = time.monotonic() + 20
        while not self.requests and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertTrue(self.requests, "GEPA did not reach the controlled reflection request")
        root = self.root / "production/agent-evaluation"
        state = json.loads((root / "state.json").read_text())
        job_id = next(key for key, value in state["jobs"].items() if value.get("gepa"))
        tree_path = root / job_id / "search/tree.json"
        before = json.loads(tree_path.read_text())
        cached = {key: value["attempt"] for key, value in before["evaluations"].items()
                  if value["status"] == "complete"}
        self.process.send_signal(stop_signal)
        stdout, stderr = self.process.communicate(timeout=10)
        self.assertEqual(self.process.returncode, 128 + stop_signal, stdout + stderr)
        cancelled = json.loads(tree_path.read_text())
        self.assertEqual(cancelled["status"], "cancelled", cancelled)
        self.assertEqual(cancelled["error"], "gepa_cancelled")
        self.assertGreater(cancelled["finished_ms"], cancelled["started_ms"])
        # 停机不修改时钟或产物；重启后由程序自己累计 paused_ms。
        self.release.set()
        time.sleep(0.05)
        self.process = subprocess.Popen(command, env=self.env, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True)
        stdout, stderr = self.process.communicate(timeout=35)
        self.assertEqual(self.process.returncode, 0, stdout + stderr)
        state = json.loads((root / "state.json").read_text())
        self.assertEqual(len([v for v in state["jobs"].values() if v.get("gepa")]), 1)
        self.assertIn(state["jobs"][job_id]["status"], ("passed", "rejected"))
        resumed = json.loads(tree_path.read_text())
        self.assertGreater(resumed["paused_ms"], 0)
        self.assertEqual(resumed["started_ms"], cancelled["started_ms"])
        for key, attempt in cached.items():
            self.assertEqual(resumed["evaluations"][key]["attempt"], attempt)

    def test_learning_cli_sigterm_preserves_cancel_checkpoint(self):
        """后台停止使用 SIGTERM，同样必须走持久化收尾。"""
        self.assert_learning_cli_signal_resume(signal.SIGTERM)

    def test_learning_cli_sigint_preserves_cancel_checkpoint(self):
        """前台 Ctrl+C 不强杀线程，也不让停机时间消耗下一次搜索预算。"""
        self.assert_learning_cli_signal_resume(signal.SIGINT)

    def test_cancelled_search_delivers_completed_pairs_without_publishing(self):
        """取消后仍交付已经落盘的训练配对；不等待剩余搜索，也不发布中间版本。"""
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=2, max_model_calls=2)
        fixture.write(self.root / "gepa.json", config)
        self.pause_on = ("L1", 2)
        self.release.clear()
        self.model_draft_content = lambda packet, content: json.dumps(dict(
            reason="保留已经完成的训练对照。",
            experience=dict(memory_id="test-experience", content="停止所有任务，不再计算。")),
            ensure_ascii=False)
        self.start()
        deadline = time.monotonic() + 20
        while len(self.requests) < 2 and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertEqual(len(self.requests), 2)
        status = self.call("agent_evaluation.status")
        job_id = next(key for key, row in status["jobs"].items() if row.get("gepa"))
        root = self.root / "production/agent-evaluation" / job_id
        tree = json.loads((root / "search/tree.json").read_text())
        self.assertTrue(tree["memory_feedback"]["records"])
        frozen_seed = (root / "seed.json").read_bytes()
        self.call("agent_evaluation.cancel", job_id=job_id)
        self.release.set()
        _, job, _ = self.wait_search()
        self.assertEqual(job["status"], "cancelled", job)

        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            job = self.call("agent_evaluation.status")["jobs"][job_id]
            if job.get("memory_feedback", {}).get("status") == "committed":
                break
            time.sleep(0.05)
        else:
            self.fail("completed pairs were stranded in a cancelled tree: " + json.dumps(job))
        self.assertTrue(job["memory_feedback"]["recovered_checkpoint"])
        self.assertEqual(job["status"], "cancelled")
        self.assertFalse(job.get("published", False))
        self.assertFalse((root / "search/published-version.json").exists())
        self.assertEqual((root / "seed.json").read_bytes(), frozen_seed)
        memory_file = self.root / "production/by-user/user-a/memory/evolution-memory.pb"
        memory_before = memory_file.read_bytes()
        memory = json.loads(memory_file.with_suffix(".json").read_text())
        records = memory["gepa_feedback"]["records"]
        self.assertEqual(set(records), set(tree["memory_feedback"]["records"]))
        for key, expected in tree["memory_feedback"]["records"].items():
            for field in ("tasks", "status", "positive_task_count", "negative_task_count"):
                self.assertEqual(records[key][field], expected[field])
        self.assertTrue(all(row["split"] == "train" for row in records.values()))
        self.assertTrue(all(task["task_id"].startswith("train-")
                            for row in records.values() for task in row["tasks"].values()))
        self.assertNotIn("VALIDATION_PRIVATE", json.dumps(records))
        self.assertNotIn("HOLDOUT_PRIVATE", json.dumps(records))

        # 显式取消不能在重启后擅自恢复；已确认的反馈也不再增加支持票或库修订。
        count = len(self.requests)
        self.process.terminate()
        self.process.communicate(timeout=10)
        self.process = None
        self.start()
        time.sleep(0.2)
        job = self.call("agent_evaluation.status")["jobs"][job_id]
        self.assertEqual(job["status"], "cancelled")
        self.assertEqual(job["memory_feedback"]["status"], "committed")
        self.assertEqual(len(self.requests), count)
        self.assertEqual(memory_file.read_bytes(), memory_before)

    def check_invalid_stopped_feedback(self, corruption):
        """构造旧版停止检查点；损坏的一份反馈不能阻塞服务或修改权威库。"""
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, _, _ = self.wait_search()
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            state = self.call("agent_evaluation.status")
            if state["jobs"][job_id].get("memory_feedback", {}).get("status") == "committed":
                break
            time.sleep(0.05)
        self.assertEqual(state["jobs"][job_id]["memory_feedback"]["status"], "committed")
        self.process.terminate()
        self.process.communicate(timeout=10)
        self.process = None
        root = self.root / "production/agent-evaluation"
        state = json.loads((root / "state.json").read_text())
        job = state["jobs"][job_id]
        job.update(status="interrupted", cancel_requested=True, published=False)
        damaged_pending = corruption in ("pending_missing", "pending_digest", "pending_json",
                                         "pending_split", "pending_shape")
        if corruption == "pending_evaluation" or damaged_pending:
            job["memory_feedback"]["status"] = "pending"
        else:
            job.pop("memory_feedback", None)
        state["publications"] = {key: value for key, value in state["publications"].items()
                                 if not value or value.get("job_id") != job_id}
        fixture.write(root / "state.json", state)
        tree_path = root / job_id / "search/tree.json"
        tree = json.loads(tree_path.read_text())
        if corruption == "context":
            tree["context"] = "another-search-context"
        elif corruption == "split":
            next(iter(tree["memory_feedback"]["records"].values()))["split"] = "holdout"
        elif corruption == "search" or damaged_pending:
            config["max_rounds"] += 1
            fixture.write(self.root / "gepa.json", config)
            if damaged_pending:
                delivery = root / job_id / "search/memory-feedback.json"
                if corruption == "pending_missing":
                    delivery.unlink()
                elif corruption == "pending_split":
                    payload = json.loads(delivery.read_text())
                    next(iter(payload["records"].values()))["split"] = "holdout"
                    fixture.write(delivery, payload)
                elif corruption == "pending_shape":
                    delivery.write_text("{}")
                else:
                    delivery.write_text("{invalid JSON")
                if corruption in ("pending_json", "pending_split", "pending_shape"):
                    job["memory_feedback"]["fingerprint"] = hashlib.sha256(delivery.read_bytes()).hexdigest()
                fixture.write(root / "state.json", state)
        else:
            # 只修改当前执行条件；旧树、配对内容与身份摘要全部保持真实。
            self.evaluation["timeout_ms"] += 1000
            fixture.write(self.root / "evaluation.json", self.evaluation)
        fixture.write(tree_path, tree)
        changed_evaluation = corruption in ("evaluation", "pending_evaluation")
        expected_status = ("committed" if corruption == "search" else
                           "obsolete" if changed_evaluation else "rejected")
        memory_file = self.root / "production/by-user/user-a/memory/evolution-memory.pb"
        previous = memory_file.read_bytes()
        self.release.clear()
        self.pause_on = None
        self.start()
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            job = self.call("agent_evaluation.status")["jobs"][job_id]
            if job.get("memory_feedback", {}).get("status") == expected_status:
                break
            time.sleep(0.05)
        self.assertEqual(job["memory_feedback"]["status"], expected_status, job)
        if corruption != "search":
            self.assertEqual(job["memory_feedback"]["error"],
                             "gepa_feedback_evaluation_context_changed" if changed_evaluation
                             else "gepa_memory_feedback_invalid" if damaged_pending
                             else "gepa_feedback_checkpoint_invalid")
        self.assertEqual(job["status"], "interrupted")
        self.assertEqual(memory_file.read_bytes(), previous)
        self.assertNotIn("worker_error", self.call("agent_evaluation.status"))
        if corruption == "search" or damaged_pending:
            # 一次明确的新启动应先消费旧反馈，再启动当前搜索；不能只入库就空退出。
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                status = self.call("agent_evaluation.status")
                newer = [row for key, row in status["jobs"].items()
                         if key != job_id and row.get("gepa")]
                if newer:
                    break
                time.sleep(0.05)
            self.assertTrue(newer, "feedback recovery swallowed the new explicit search")
            self.assertTrue(all(row["context"] != job["context"] for row in newer))

    def test_missing_pending_feedback_does_not_block_new_search(self):
        """单份投递文件丢失应隔离，不能使所有后续作业停止刷新。"""
        self.check_invalid_stopped_feedback("pending_missing")

    def test_changed_pending_feedback_does_not_block_new_search(self):
        """摘要不匹配必须拒绝入库，同时允许独立的新搜索继续。"""
        self.check_invalid_stopped_feedback("pending_digest")

    def test_invalid_pending_feedback_json_does_not_block_new_search(self):
        """摘要一致也不能跳过结构校验；非法 JSON 只影响该投递。"""
        self.check_invalid_stopped_feedback("pending_json")

    def test_incomplete_pending_feedback_does_not_block_new_search(self):
        """合法但缺少必需结构的 JSON 不能被确认成已入库。"""
        self.check_invalid_stopped_feedback("pending_shape")

    def test_pending_holdout_feedback_does_not_block_new_search(self):
        """合法 JSON 与摘要不代表训练来源合法；拒绝留出反馈，不阻断其他作业。"""
        self.check_invalid_stopped_feedback("pending_split")

    def test_stopped_feedback_survives_search_change_and_starts_new_job(self):
        """只改搜索配置时先交付旧反馈，再自动启动本次搜索，不要求再手动调用一次。"""
        self.check_invalid_stopped_feedback("search")

    def test_stopped_feedback_rejects_mismatched_search_identity(self):
        """旧树与作业身份不一致时拒绝交付，不把别人的反馈当作本次训练。"""
        self.check_invalid_stopped_feedback("context")

    def test_stopped_feedback_never_imports_holdout_records(self):
        """停止恢复同样执行 train 隔离验证，不能绕过正常写回约束。"""
        self.check_invalid_stopped_feedback("split")

    def test_stopped_feedback_cannot_update_memory_under_changed_evaluation(self):
        """历史训练反馈仍保留，但执行／评分口径变化后不能更新当前记忆状态。"""
        self.check_invalid_stopped_feedback("evaluation")

    def test_pending_feedback_cannot_update_memory_under_changed_evaluation(self):
        """已经登记但尚未消费的旧反馈也不能绕过当前评测口径隔离。"""
        self.check_invalid_stopped_feedback("pending_evaluation")

    def test_learning_cli_recovers_feedback_after_search_finished(self):
        """搜索已落盘、评测提交前停机：仅补结果与反馈，不重跑 Agent 或反思。"""
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        command = [str(fixture.CLI), "learn", "--evaluation-only", "--config",
                   str(self.root / "service.json")]
        self.process = subprocess.Popen(command, env=self.env, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True)
        stdout, stderr = self.process.communicate(timeout=35)
        self.assertEqual(self.process.returncode, 0, stdout + stderr)
        root = self.root / "production/agent-evaluation"
        state = json.loads((root / "state.json").read_text())
        job_id = next(key for key, value in state["jobs"].items() if value.get("gepa"))
        before = json.loads((root / job_id / "search/tree.json").read_text())
        self.assertTrue(before["memory_feedback"]["records"])
        memory_file = self.root / "production/by-user/user-a/memory/evolution-memory.pb"
        memory_before = memory_file.read_bytes()
        requests_before = len(self.requests)

        # 在隔离测试产物中构造可达的崩溃窗口：搜索完成，但 worker 尚未提交结果。
        # 不动 tree 与评测报告；取消仅来自服务关闭，并没有用户 cancel_requested。
        job = state["jobs"][job_id]
        job.update(status="error", error="evaluation_cancelled")
        for key in ("memory_feedback", "search", "gate", "published", "cancel_requested"):
            job.pop(key, None)
        seed = json.loads((root / job_id / "seed.json").read_text())
        job["version"] = before["seed_version"]
        fixture.write(root / job_id / "candidate.json", seed)
        state["publications"] = {
            key: value for key, value in state["publications"].items()
            if not value or value.get("job_id") != job_id
        }
        fixture.write(root / "state.json", state)
        self.process = subprocess.Popen(command, env=self.env, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True)
        stdout, stderr = self.process.communicate(timeout=35)
        self.assertEqual(self.process.returncode, 0, stdout + stderr)
        state = json.loads((root / "state.json").read_text())
        self.assertIn(state["jobs"][job_id]["status"], ("passed", "rejected"))
        self.assertEqual(state["jobs"][job_id]["memory_feedback"]["status"], "committed")
        after = json.loads((root / job_id / "search/tree.json").read_text())
        self.assertEqual(set(after["evaluations"]), set(before["evaluations"]))
        for key, value in before["evaluations"].items():
            self.assertEqual(after["evaluations"][key]["attempt"], value["attempt"])
        self.assertEqual(len(self.requests), requests_before)
        self.assertEqual(memory_file.read_bytes(), memory_before)

    # 回归用例：test_cancel_model_call_keeps_the_main_service_responsive。
    # 关键断言：self.assertTrue(self.requests)；此处只验证测试构造的场景。
    def test_cancel_model_call_keeps_the_main_service_responsive(self):
        self.release.clear()
        self.start()
        deadline = time.monotonic() + 20
        while not self.requests and time.monotonic() < deadline:
            time.sleep(0.05)
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

        # 用户主动取消与服务停机不同，重启后不能自动消费下一次搜索预算。
        self.process.terminate()
        self.process.communicate(timeout=10)
        self.process = None
        count = len(self.requests)
        self.start()
        self.assertEqual(self.call("agent_evaluation.status")["jobs"][job_id]["status"], "cancelled")
        self.assertEqual(len(self.requests), count)

    # 回归用例：test_cancelled_search_excludes_pause_without_replaying_completed_trials。
    # 重点核验字段或诊断：gepa_search_time_reserved；异常路径不能被当成成功。
    def test_cancelled_search_excludes_pause_without_replaying_completed_trials(self):
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=2, max_model_calls=2)
        fixture.write(self.root / "gepa.json", config)
        self.release.clear()
        self.start()
        deadline = time.monotonic() + 20
        while not self.requests and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertTrue(self.requests)
        status = self.call("agent_evaluation.status")
        job_id = next(key for key, value in status["jobs"].items() if value.get("gepa"))
        self.call("agent_evaluation.cancel", job_id=job_id)
        self.release.set()
        _, job, _ = self.wait_search()
        self.assertEqual(job["status"], "cancelled", job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["status"], "cancelled", tree)
        cached = {
            key: value["attempt"]
            for key, value in tree["evaluations"].items()
            if value["status"] == "complete"
        }
        # 模拟长时间停机，不实际等待，也不改任何试题、分数或已消耗的预算。
        tree["started_ms"] -= 600000
        tree["finished_ms"] -= 600000
        root = self.root / "production/agent-evaluation" / job_id / "search"
        fixture.write(root / "tree.json", tree)
        self.call("agent_evaluation.retry", job_id=job_id)
        _, job, _ = self.wait_search()
        self.assertIn(job["status"], ("passed", "rejected"), job)
        resumed = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(resumed["started_ms"], tree["started_ms"])
        self.assertGreaterEqual(resumed["paused_ms"], 600000)
        self.assertEqual(resumed["model_calls"], 2)
        self.assertNotEqual(resumed["stop_reason"], "gepa_search_time_reserved")
        for key, attempt in cached.items():
            self.assertEqual(resumed["evaluations"][key]["attempt"], attempt)

    # 回归用例：test_interrupted_search_resumes_without_replaying_finished_trials。
    # 关键断言：self.assertTrue(self.requests)；此处只验证测试构造的场景。
    def test_interrupted_search_resumes_without_replaying_finished_trials(self):
        self.release.clear()
        self.start()
        deadline = time.monotonic() + 20
        while not self.requests and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertTrue(self.requests)
        status = self.call("agent_evaluation.status")
        job_id = next(key for key, value in status["jobs"].items() if value.get("gepa"))
        before = self.call("agent_evaluation.tree", job_id=job_id)
        cached = {
            key
            for key, value in before["evaluations"].items()
            if value["status"] == "complete"
        }
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
            self.assertEqual(
                tree["evaluations"][key]["attempt"],
                before["evaluations"][key]["attempt"],
            )

    def test_interrupted_search_rejects_changed_runtime_content(self):
        """宿主命令或 daemon 内容变化后，旧树只能保留，不能跨 runtime 续跑。"""
        wrapper = self.root / "evaluation-host-wrapper.py"
        wrapper.write_text(
            "import runpy\nrunpy.run_path(" +
            repr(str(fixture.SOURCE / "tests/agent_evaluation_host.py")) +
            ", run_name='__main__')\n",
            encoding="utf-8",
        )
        daemon = self.root / "agentflowd-fixture"
        shutil.copy2(fixture.BUILD.parent / "agentflowd", daemon)
        daemon.chmod(0o755)
        self.env["AGENTFLOW_DAEMON"] = str(daemon)
        self.evaluation["variants"][0]["command"][2] = str(wrapper)
        fixture.write(self.root / "evaluation.json", self.evaluation)

        self.release.clear()
        self.start()
        deadline = time.monotonic() + 20
        while not self.requests and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertTrue(self.requests)
        status = self.call("agent_evaluation.status")
        job_id = next(key for key, value in status["jobs"].items() if value.get("gepa"))
        old_context = status["jobs"][job_id]["candidate_evaluation_context"]
        self.process.kill()
        self.process.communicate(timeout=5)
        self.process = None

        # 路径、参数和策略均保持不变，只替换实际执行内容。
        wrapper.write_text(wrapper.read_text(encoding="utf-8") + "# runtime-v2\n",
                           encoding="utf-8")
        with daemon.open("ab") as stream:
            stream.write(b"\nruntime-v2\n")
        self.release.set()
        self.start()
        restarted = self.call("agent_evaluation.status")
        self.assertEqual(restarted["jobs"][job_id]["status"], "interrupted")
        newer = [job for key, job in restarted["jobs"].items() if key != job_id and job.get("gepa")]
        if newer:
            self.assertTrue(any(job["candidate_evaluation_context"] != old_context
                                for job in newer))
        rejected = self.call("agent_evaluation.retry", job_id=job_id)
        self.assertFalse(rejected["ok"], rejected)
        self.assertEqual(rejected["error"], "evaluation_runtime_context_changed")

    # 回归用例：test_published_bundle_runs_offline_without_original_memory。
    # 重点核验字段或诊断：policy_evaluation_file、policy_version_file、agent_version_id；异常路径不能被当成成功。
    def test_published_bundle_runs_offline_without_original_memory(self):
        self.start()
        job_id, job, _ = self.wait_search()
        self.assertEqual(job["status"], "passed", job)
        fixture.AgentEvaluationTests.tearDown(self)
        count = len(self.requests)
        self.config.pop("policy_evaluation_file")
        self.config["policy_version_file"] = str(
            self.root
            / "production/agent-evaluation"
            / job_id
            / "search/published-version.json"
        )
        self.config["data_dir"] = str(self.root / "offline")
        fixture.write(self.root / "service.json", self.config)
        self.start()
        run = self.call(
            "run.begin",
            input="生成视频时缺少图片素材，核对甲类和乙类任务。",
            level="L1",
            capabilities=["model"],
        )
        self.assertEqual(run.get("agent_version_id"), job["version"], run)
        activity = self.call(
            "activity.begin",
            run_id=run["run_id"],
            kind="model",
            name="offline-model",
            input={
                "messages": [
                    {
                        "role": "user",
                        "content": "生成视频时缺少图片素材，核对甲类和乙类任务。",
                    }
                ]
            },
            binding={"pointer": "/messages", "mode": "system_message"},
        )
        self.assertIn("核验甲类", json.dumps(activity["input"], ensure_ascii=False))
        self.assertIn("核验乙类", json.dumps(activity["input"], ensure_ascii=False))
        self.call(
            "activity.end",
            run_id=run["run_id"],
            activity_id=activity["activity_id"],
            status="completed",
            output="完成",
            actual_input=activity["input"],
            executed=True,
            metrics={"tokens": 100},
        )
        self.call("run.end", run_id=run["run_id"], status="completed", output="完成")
        other = self.call(
            "run.begin",
            token=fixture.TOKEN_B,
            input="生成视频时缺少图片素材。",
            level="L1",
            capabilities=["model"],
        )
        self.assertNotIn("agent_version_id", other)
        self.call(
            "run.end",
            token=fixture.TOKEN_B,
            run_id=other["run_id"],
            status="completed",
            output="未授权其他用户的版本",
        )
        self.assertEqual(len(self.requests), count)
        fixture.AgentEvaluationTests.tearDown(self)
        traces = list((self.root / "offline/by-user/user-a").rglob("*.trace.json"))
        self.assertEqual(len(traces), 1)
        self.assertNotIn("evaluation_trial_id", traces[0].read_text())

    # 回归用例：test_trial_budget_reserves_final_holdout。
    # 重点核验字段或诊断：gepa_trial_budget_exhausted；异常路径不能被当成成功。
    def test_trial_budget_reserves_final_holdout(self):
        config = json.loads((self.root / "gepa.json").read_text())
        config["max_trials"] = 12
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search()
        self.assertEqual(job["status"], "rejected", job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertLessEqual(tree["trials_used"], 12)
        self.assertFalse(tree.get("holdout_started", False))
        self.assertEqual(tree["stop_reason"], "gepa_trial_budget_exhausted")
        self.assertEqual(job["gate"]["stage"], "validation")
        self.assertFalse(
            any(value["split"] == "holdout" for value in tree["evaluations"].values())
        )

    # 测试辅助函数：exercise_controls。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def exercise_controls(self, kind, level):
        self.behavior = kind
        self.evaluation["variants"][0]["command"][-2] = "gepa_" + kind
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.config["max_level"] = level
        self.config["control_learning"] = dict(
            enabled=True,
            prompt_file="prompts/gepa-reflection.txt",
            actions={"calculate": ["verified"]},
            max_rules=2,
            min_iterations=2,
            min_no_progress=1,
        )
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search(level)
        self.assertEqual(job["status"], "passed", job)
        if kind == "loops":
            self.assertEqual(job["gate"]["reasons"], [])
            self.assertFalse(job["gate"]["control_coverage"]["level_complete"])
            self.assertEqual(
                job["gate"]["control_coverage"]["unobserved_capabilities"], ["actions"]
            )
            self.assertGreater(
                job["gate"]["control_coverage"]["capabilities"]["loop"]["applied"], 0
            )
        candidate = json.loads(
            (
                self.root / "production/agent-evaluation" / job_id / "candidate.json"
            ).read_text()
        )
        rules = candidate["policy"]["options"][
            "action_rules" if kind == "actions" else "loop_rules"
        ]
        generated = [rule for rule in rules if "source_experience" in rule]
        self.assertEqual(len(generated), 1)
        self.assertEqual(
            generated[0]["source_experience"]["memory_id"], "test-experience"
        )
        return generated[0]

    # 回归用例：test_l2_generated_action_rules_reach_actual_host。
    # 关键断言：self.assertEqual(rule["required_arguments"], ["verified"])；此处只验证测试构造的场景。
    def test_l2_feedback_is_used_by_the_next_seed(self):
        self.exercise_control_memory_reuse("actions", "L2")

    def test_l3_feedback_is_used_by_the_next_seed(self):
        self.exercise_control_memory_reuse("loops", "L3")

    def exercise_control_memory_reuse(self, kind, level):
        """控制类经验也必须完成写回、重新编译，不只验证 L1 文本修订的闭环。"""
        expected = self.exercise_controls(kind, level)
        job_id, _, _ = self.wait_search(level)
        component = "action_rules" if kind == "actions" else "loop_rules"
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            job = self.call("agent_evaluation.status")["jobs"][job_id]
            if job.get("memory_feedback", {}).get("status") == "committed":
                break
            time.sleep(.05)
        else:
            self.fail("control feedback not committed: " + json.dumps(job))
        memory = json.loads((self.root / "production/by-user/user-a/memory/evolution-memory.json").read_text())
        self.assertTrue(any(component in row["after"] and row["status"] == "supported"
            for row in memory["gepa_feedback"]["records"].values()), memory["gepa_feedback"])
        previous_jobs = set(self.call("agent_evaluation.status")["jobs"])
        self.process.terminate()
        self.process.communicate(timeout=10)
        self.process = None
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=2)
        fixture.write(self.root / "gepa.json", config)
        self.config["evaluation_levels"] = [level]
        fixture.write(self.root / "service.json", self.config)
        self.start()
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            status = self.call("agent_evaluation.status")
            jobs = [(key, row) for key, row in status["jobs"].items()
                    if key not in previous_jobs and row["level"] == level and row.get("gepa")]
            if jobs:
                seed = json.loads((self.root / "production/agent-evaluation" / jobs[0][0] / "seed.json").read_text())
                rules = [row for row in seed["policy"]["options"][component]
                         if row.get("source_experience", {}).get("memory_id") == "test-experience"]
                self.assertTrue(rules, seed["policy"]["options"])
                field = "required_arguments" if kind == "actions" else "max_no_progress"
                self.assertEqual(rules[0][field], expected[field])
                return
            time.sleep(.05)
        self.fail("next control seed was not compiled: " + json.dumps(status))

    def test_l2_generated_action_rules_reach_actual_host(self):
        rule = self.exercise_controls("actions", "L2")
        self.assertEqual(rule["required_arguments"], ["verified"])

    # 回归用例：test_l2_generic_schema_rule_needs_no_tool_whitelist_and_reaches_host。
    # 重点核验字段或诊断：gepa_schema_actions、schema_required_nonempty；异常路径不能被当成成功。
    def test_l2_generic_schema_rule_needs_no_tool_whitelist_and_reaches_host(self):
        self.behavior = "schema_actions"
        self.evaluation["variants"][0]["command"][-2] = "gepa_schema_actions"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.config.update(
            max_level="L2",
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertEqual(job["status"], "passed", job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertIn("action_rules", tree["available_components"])
        candidate = json.loads(
            (
                self.root / "production/agent-evaluation" / job_id / "candidate.json"
            ).read_text()
        )
        generated = [
            rule
            for rule in candidate["policy"]["options"]["action_rules"]
            if "source_experience" in rule
        ]
        self.assertEqual(len(generated), 1)
        self.assertEqual(generated[0]["operation"], "*")
        self.assertEqual(
            generated[0]["required_arguments"], ["$schema_required_nonempty"]
        )
        packet = next(
            p for p in self.requests if p.get("controls", {}).get("level") == "L2"
        )
        self.assertEqual(
            packet["controls"]["action_primitives"][0]["primitive_id"],
            "schema_required_nonempty",
        )
        self.assertEqual(packet["controls"]["action_primitives"][0]["operation"], "*")
        self.assertNotIn("actions", packet["controls"]["limits"])

    # 测试辅助函数：run_schema_search。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def run_schema_search(
        self, mode, behavior="schema_actions", model_calls=1, max_attempts=2
    ):
        self.behavior = behavior
        self.evaluation["variants"][0]["command"][-2] = mode
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.config.update(
            max_level="L2",
            control_learning=dict(
                enabled=True,
                prompt_file="prompts/gepa-reflection.txt",
                max_rules=2,
                max_attempts=max_attempts,
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            max_rounds=1,
            max_model_calls=model_calls,
            minibatch_size=1,
            acceptance="improvement_or_equal",
        )
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, _, _ = self.wait_search("L2")
        return self.call("agent_evaluation.tree", job_id=job_id)

    # 回归用例：test_unobserved_control_stops_after_training_without_validation。
    # 重点核验字段或诊断：gepa_schema_noop、no_observed_decision_change；异常路径不能被当成成功。
    def test_unobserved_control_stops_after_training_without_validation(self):
        tree = self.run_schema_search("gepa_schema_noop")
        node = tree["nodes"][1]
        self.assertEqual(node["status"], "unobserved_changes", node)
        self.assertEqual(node["reason"], "no_observed_decision_change")
        self.assertNotIn("validation", node)
        self.assertNotIn("validation_probe", node)
        self.assertEqual(node["screening"]["applied_changes"], {})
        rule = next(iter(node["screening"]["control_coverage"]["rules"].values()))
        self.assertGreater(rule["queries"], 0)
        self.assertEqual(rule["applied"], 0)
        self.assertFalse(
            any(
                value["version"] == node["version"] and value["split"] == "val"
                for value in tree["evaluations"].values()
            )
        )

    # 回归用例：test_control_cannot_use_validation_to_find_its_first_real_application。
    # 重点核验字段或诊断：gepa_schema_validation；异常路径不能被当成成功。
    def test_control_cannot_use_validation_to_find_its_first_real_application(self):
        tree = self.run_schema_search("gepa_schema_validation")
        node = tree["nodes"][1]
        self.assertEqual(node["status"], "unobserved_changes", node)
        self.assertFalse(node["screening"]["control_coverage"]["complete"])
        self.assertNotIn("validation_probe", node)
        self.assertNotIn("validation", node)
        self.assertFalse(
            any(
                value["version"] == node["version"] and value["split"] == "val"
                for value in tree["evaluations"].values()
            )
        )

    # 回归用例：test_control_observed_only_in_training_keeps_its_application_evidence。
    # 重点核验字段或诊断：gepa_schema_training；异常路径不能被当成成功。
    def test_control_observed_only_in_training_keeps_its_application_evidence(self):
        tree = self.run_schema_search("gepa_schema_training")
        node = tree["nodes"][1]
        self.assertEqual(node["status"], "accepted", node)
        self.assertNotIn("validation_probe", node)
        self.assertTrue(node["screening"]["control_coverage"]["complete"])
        self.assertEqual(node["control_coverage"]["unobserved_rules"], [])
        self.assertEqual(
            sum(r["applied"] for r in node["control_coverage"]["rules"].values()), 1
        )

    # 回归用例：test_bad_control_source_is_repaired_once_with_compact_input。
    # 重点核验字段或诊断：gepa_schema_actions、schema_actions_bad_source、control_source_not_approved；异常路径不能被当成成功。
    def test_bad_control_source_is_repaired_once_with_compact_input(self):
        tree = self.run_schema_search(
            "gepa_schema_actions", "schema_actions_bad_source", model_calls=2
        )
        node = tree["nodes"][1]
        self.assertEqual(node["status"], "accepted", node)
        self.assertEqual(node["repair_error"], "control_source_not_approved")
        packets = [
            p for p in self.requests if p.get("controls", {}).get("level") == "L2"
        ]
        self.assertEqual(len(packets), 2)
        self.assertEqual(packets[1]["phase"], "repair_control_source")
        self.assertNotIn("feedback", packets[1])
        self.assertEqual(packets[1]["experiences"], packets[0]["experiences"])
        self.assertEqual(tree["model_calls"], 2)

    # 回归用例：test_bad_control_source_repair_cannot_loop。
    # 重点核验字段或诊断：gepa_schema_actions、schema_actions_bad_source_always、control_source_not_approved；异常路径不能被当成成功。
    def test_bad_control_source_repair_cannot_loop(self):
        tree = self.run_schema_search(
            "gepa_schema_actions", "schema_actions_bad_source_always", model_calls=3
        )
        self.assertEqual(tree["nodes"][1]["error"], "control_source_not_approved")
        self.assertEqual(tree["model_calls"], 2)

    # 回归用例：test_incremental_control_source_repair_keeps_parent_components。
    # 重点核验字段或诊断：gepa_schema_actions、schema_actions_bad_source、repair_control_source；异常路径不能被当成成功。
    def test_incremental_control_source_repair_keeps_parent_components(self):
        self.incremental_controls = True
        tree = self.run_schema_search(
            "gepa_schema_actions", "schema_actions_bad_source", model_calls=2
        )
        self.assertEqual(tree["nodes"][1]["status"], "accepted")
        original, repair = [
            p for p in self.requests if p.get("controls", {}).get("level") == "L2"
        ]
        self.assertEqual(repair["phase"], "repair_control_source")
        self.assertEqual(repair.get("components"), original["components"])
        self.assertEqual(repair["draft"], self.model_drafts[0]["draft"])
        self.assertNotIn("feedback", repair)
        self.assertEqual(tree["model_calls"], 2)

    # 回归用例：test_unwrapped_controls_format_is_repaired_once。
    # 重点核验字段或诊断：gepa_schema_actions、repair_draft_format；异常路径不能被当成成功。
    def test_unwrapped_controls_format_is_repaired_once(self):
        self.incremental_controls = True
        self.unwrapped_controls = "once"
        tree = self.run_schema_search("gepa_schema_actions", model_calls=2)
        node = tree["nodes"][1]
        self.assertEqual(node["status"], "accepted", node)
        self.assertEqual(node.get("repair_error"), "unknown_field")
        self.assertEqual(tree["model_calls"], 2)
        original, repair = [
            p for p in self.requests if p.get("controls", {}).get("level") == "L2"
        ]
        draft = next(row["draft"] for row in self.model_drafts if row["level"] == "L2")
        self.assertEqual(repair["phase"], "repair_draft_format")
        self.assertEqual(repair["draft"], draft)
        self.assertIn("action_rules", repair["draft"])
        self.assertIn("loop_rules", repair["draft"])
        self.assertNotIn("controls", repair["draft"])
        self.assertEqual(repair["components"], original["components"])
        self.assertNotIn("feedback", repair)

    # 回归用例：test_unwrapped_controls_format_repair_cannot_loop。
    # 重点核验字段或诊断：gepa_schema_actions、repair_draft_format；异常路径不能被当成成功。
    def test_unwrapped_controls_format_repair_cannot_loop(self):
        self.incremental_controls = True
        self.unwrapped_controls = "always"
        tree = self.run_schema_search("gepa_schema_actions", model_calls=3)
        node = tree["nodes"][1]
        self.assertEqual(node.get("repair_error"), "unknown_field", node)
        self.assertEqual((node["status"], node["error"]), ("error", "unknown_field"))
        self.assertEqual(tree["model_calls"], 2)
        packets = [
            p for p in self.requests if p.get("controls", {}).get("level") == "L2"
        ]
        self.assertEqual(
            [p.get("phase") for p in packets], [None, "repair_draft_format"]
        )

    # 回归用例：test_unwrapped_controls_format_repair_respects_model_budget。
    # 重点核验字段或诊断：gepa_schema_actions；异常路径不能被当成成功。
    def test_unwrapped_controls_format_repair_respects_model_budget(self):
        self.incremental_controls = True
        self.unwrapped_controls = "once"
        tree = self.run_schema_search("gepa_schema_actions", model_calls=1)
        node = tree["nodes"][1]
        self.assertEqual((node["status"], node["error"]), ("error", "unknown_field"))
        self.assertNotIn("repair_error", node)
        self.assertEqual(tree["model_calls"], 1)
        packets = [
            p for p in self.requests if p.get("controls", {}).get("level") == "L2"
        ]
        self.assertEqual(len(packets), 1)

    # 测试辅助函数：run_duplicate_parent_loop_repair。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def run_duplicate_parent_loop_repair(
        self, repeat=False, model_calls=2, max_attempts=2
    ):
        # 沿用父生成规则 fixture 的真实来源 revision，不手写反馈或绕过编译。
        scope = "agent:eval-test/actor:assistant"
        request = dict(
            memory_path=str(
                self.root / "production/by-user/user-a/memory/evolution-memory.pb"
            ),
            identity=dict(
                agent_id="eval-test",
                user_id="user-a",
                conversation_id="duplicate-loop",
                run_id="seed",
            ),
            options=dict(self.config["shadow_options"], target_scopes=[scope]),
        )
        result = subprocess.run(
            [str(fixture.CLI), "shadow", "compile"],
            input=json.dumps(request),
            capture_output=True,
            text=True,
            timeout=20,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        (source,) = json.loads(result.stdout)["experiences"]
        # 初始服务规则沿用 actor_id，由配置归一化补 target_scope，不能同时提供两者。
        parent_rule = dict(
            self.config["runtime_options"]["loop_rules"][0],
            rule_id="existing-learned-loop",
            max_no_progress=2,
            source_experience=dict(
                memory_id=source["memory_id"],
                content_revision=source["content_revision"],
            ),
        )
        self.config["runtime_options"]["loop_rules"].append(parent_rule)
        self.config.update(
            max_level="L3",
            evaluation_levels=["L3"],
            control_learning=dict(
                enabled=True,
                prompt_file="prompts/gepa-reflection.txt",
                max_rules=4,
                max_attempts=max_attempts,
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            component_selector="all",
            max_rounds=1,
            max_model_calls=model_calls,
            proposals_per_round=1,
        )
        fixture.write(self.root / "gepa.json", config)
        self.behavior = "empty_change"
        self.duplicate_loop_drafts = []

        # 测试辅助函数：draft_content。
        # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
        def draft_content(packet, content):
            value = dict(packet["components"]["loop_rules"][0])
            update = dict(component="loop_rules", op="add", value=value)
            if packet.get("phase") == "repair_draft_format" and not repeat:
                # 正确修复是引用父索引，而不是再追加同 actor/scope 的规则。
                update.update(op="set", index=0)
            draft = dict(
                reason="保留现有循环预算，不追加重复规则。", control_updates=[update]
            )
            self.duplicate_loop_drafts.append(draft)
            return json.dumps(draft, ensure_ascii=False)

        self.model_draft_content = draft_content
        self.start()
        job_id, _, _ = self.wait_search("L3")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(len(self.requests[0]["components"]["loop_rules"]), 1)
        self.assertEqual(
            self.duplicate_loop_drafts[0]["control_updates"][0]["op"], "add"
        )
        return tree

    # 回归用例：test_duplicate_parent_loop_scope_is_repaired_once。
    # 重点核验字段或诊断：control_duplicate_loop_scope、no_behavior_change、repair_draft_format；异常路径不能被当成成功。
    def test_duplicate_parent_loop_scope_is_repaired_once(self):
        tree = self.run_duplicate_parent_loop_repair()
        node = tree["nodes"][1]
        self.assertEqual(node.get("repair_error"), "control_duplicate_loop_scope", node)
        self.assertEqual(
            (node["status"], node["skip_reason"]), ("duplicate", "no_behavior_change")
        )
        self.assertEqual(node["version"], tree["nodes"][0]["version"])
        self.assertNotIn("screening", node)
        self.assertEqual(tree["model_calls"], 2)
        self.assertEqual(len(self.requests), 2)
        original, repair = self.requests
        self.assertEqual(repair["phase"], "repair_draft_format")
        self.assertEqual(repair["error"], "control_duplicate_loop_scope")
        self.assertEqual(repair["draft"], self.duplicate_loop_drafts[0])
        for field in ("selected_components", "components", "experiences", "controls"):
            self.assertEqual(repair[field], original[field])
        self.assertNotIn("feedback", repair)
        (update,) = self.duplicate_loop_drafts[1]["control_updates"]
        self.assertEqual((update["op"], update["index"]), ("set", 0))
        self.assertEqual(update["value"], original["components"]["loop_rules"][0])

    # 回归用例：test_duplicate_parent_loop_scope_repair_cannot_loop。
    # 重点核验字段或诊断：control_duplicate_loop_scope、repair_draft_format；异常路径不能被当成成功。
    def test_duplicate_parent_loop_scope_repair_cannot_loop(self):
        tree = self.run_duplicate_parent_loop_repair(repeat=True, model_calls=3)
        node = tree["nodes"][1]
        self.assertEqual(node.get("repair_error"), "control_duplicate_loop_scope", node)
        self.assertEqual(
            (node["status"], node["error"]), ("error", "control_duplicate_loop_scope")
        )
        self.assertEqual(tree["model_calls"], 2)
        self.assertEqual(
            [packet.get("phase") for packet in self.requests],
            [None, "repair_draft_format"],
        )

    # 回归用例：test_duplicate_parent_loop_scope_repair_respects_model_budget。
    # 重点核验字段或诊断：control_duplicate_loop_scope；异常路径不能被当成成功。
    def test_duplicate_parent_loop_scope_repair_respects_model_budget(self):
        tree = self.run_duplicate_parent_loop_repair(model_calls=1)
        node = tree["nodes"][1]
        self.assertEqual(
            (node["status"], node["error"]), ("error", "control_duplicate_loop_scope")
        )
        self.assertNotIn("repair_error", node)
        self.assertEqual(tree["model_calls"], 1)
        self.assertEqual(len(self.requests), 1)

    # 回归用例：test_duplicate_parent_loop_scope_respects_disabled_repair。
    # 重点核验字段或诊断：control_duplicate_loop_scope；异常路径不能被当成成功。
    def test_duplicate_parent_loop_scope_respects_disabled_repair(self):
        tree = self.run_duplicate_parent_loop_repair(max_attempts=1)
        node = tree["nodes"][1]
        self.assertEqual(
            (node["status"], node["error"]), ("error", "control_duplicate_loop_scope")
        )
        self.assertNotIn("repair_error", node)
        self.assertEqual(tree["model_calls"], 1)
        self.assertEqual(len(self.requests), 1)

    # 回归用例：test_bad_control_source_respects_model_budget。
    # 重点核验字段或诊断：gepa_schema_actions、schema_actions_bad_source、control_source_not_approved；异常路径不能被当成成功。
    def test_bad_control_source_respects_model_budget(self):
        tree = self.run_schema_search(
            "gepa_schema_actions", "schema_actions_bad_source", model_calls=1
        )
        self.assertEqual(tree["nodes"][1]["error"], "control_source_not_approved")
        self.assertEqual(tree["model_calls"], 1)

    # 回归用例：test_bad_control_source_respects_disabled_repair。
    # 重点核验字段或诊断：gepa_schema_actions、schema_actions_bad_source、control_source_not_approved；异常路径不能被当成成功。
    def test_bad_control_source_respects_disabled_repair(self):
        tree = self.run_schema_search(
            "gepa_schema_actions",
            "schema_actions_bad_source",
            model_calls=2,
            max_attempts=1,
        )
        self.assertEqual(tree["nodes"][1]["error"], "control_source_not_approved")
        self.assertEqual(tree["model_calls"], 1)

    # 回归用例：test_unauthorized_control_draft_is_rejected_explicitly。
    # 重点核验字段或诊断：invalid_schema_action、control_operation_not_authorized；异常路径不能被当成成功。
    def test_unauthorized_control_draft_is_rejected_explicitly(self):
        self.behavior = "invalid_schema_action"
        self.config.update(
            max_level="L2",
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, _, _ = self.wait_search("L2")
        node = self.call("agent_evaluation.tree", job_id=job_id)["nodes"][1]
        self.assertEqual(node["status"], "error")
        self.assertEqual(node["error"], "control_operation_not_authorized")
        draft = (
            self.root
            / "production/agent-evaluation"
            / job_id
            / "search/reflections/1.draft.json"
        )
        self.assertEqual(
            json.loads(draft.read_text())["controls"]["action_rules"][0]["operation"],
            "calculate",
        )

    # 回归用例：test_l2_search_reports_real_control_space_without_duplicate_memories。
    # 重点核验字段或诊断：query_event_id；异常路径不能被当成成功。
    def test_l2_search_reports_real_control_space_without_duplicate_memories(self):
        self.exercise_controls("actions", "L2")
        job_id, job, _ = self.wait_search("L2")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertIn("action_rules", tree["available_components"])
        self.assertEqual(tree["search_warnings"], [])
        self.assertEqual(
            job["search"]["available_components"], tree["available_components"]
        )
        packet = next(
            p for p in self.requests if p.get("controls", {}).get("level") == "L2"
        )
        self.assertNotIn("memories", packet["controls"])
        self.assertEqual(
            packet["controls"]["limits"]["actions"], {"calculate": ["verified"]}
        )
        actions = [
            action
            for row in packet["feedback"]
            for run in row["trial"]["evidence"]["runs"]
            for action in run["actions"]
            if action["kind"] == "action.end"
        ]
        # 控制反思保留真实动作证据，不保留冗余模型信封。
        self.assertFalse(any(a.get("semantic") == "model" for a in actions))
        controlled = [a for a in actions if a.get("action_control")]
        self.assertTrue(controlled)
        self.assertTrue(all(a["operation"] == "calculate" for a in controlled))
        self.assertTrue(
            all(
                a["action_control"]["query_event_id"].startswith("event-")
                for a in controlled
            )
        )
        self.assertTrue(
            all(
                a["action_control"]["target_scope"] == "agent:eval-test/actor:assistant"
                for a in controlled
            )
        )

    # 回归用例：test_l2_without_control_learning_reports_experience_only_search。
    # 重点核验字段或诊断：observed_tool_required_arguments；异常路径不能被当成成功。
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

    # 回归用例：test_rewritten_experiences_keep_original_constraints_in_reflection。
    # 关键断言：self.assertTrue(originals)；此处只验证测试构造的场景。
    def test_rewritten_experiences_keep_original_constraints_in_reflection(self):
        self.start()
        self.wait_search()
        originals = [
            item["original_content"]
            for packet in self.requests
            for item in packet["experiences"]
            if "original_content" in item
        ]
        self.assertTrue(originals)
        self.assertEqual(len(set(originals)), 1)
        for packet in self.requests:
            for item in packet["experiences"]:
                self.assertIn("condition", item)
                self.assertIn("scope", item)
                self.assertNotIn("content", item)

    # 回归用例：test_l2_enabled_without_operations_warns_without_crashing。
    # 重点核验字段或诊断：observed_tool_required_arguments；异常路径不能被当成成功。
    def test_l2_enabled_without_operations_warns_without_crashing(self):
        self.config.update(
            max_level="L2",
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt"
            ),
        )
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
        self.assertEqual(
            job["search"]["fixed_controls"], ["observed_tool_required_arguments"]
        )
        packet = next(
            p for p in self.requests if p.get("controls", {}).get("level") == "L2"
        )
        self.assertEqual(
            packet["controls"]["action_primitives"][0]["required_arguments"],
            ["$schema_required_nonempty"],
        )

    def run_scoped_iteration_repair(self, *, repeat=False, model_calls=2, max_attempts=2):
        """复现真实草稿漏写收尾条件，验证同一节点最多修复一次且不执行非法规则。"""
        self.behavior = "loops"
        self.config.update(
            max_level="L3", evaluation_levels=["L3"],
            control_learning=dict(enabled=True, prompt_file="prompts/gepa-reflection.txt",
                                  max_rules=2, max_attempts=max_attempts),
        )
        self.config["runtime_options"]["aggressive_controls"] = False
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_loops"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=model_calls, component_selector="all")
        fixture.write(self.root / "gepa.json", config)

        def draft_content(packet, content):
            # 首次草稿虽有 actor，却遗漏保守轮次上限必需的 require_finalization。
            # 修复选择原有、已观测的无进展原语，不由程序擅自填收尾能力或扩大权限。
            if packet.get("phase") == "repair_control_capability" and not repeat:
                return content
            return json.dumps(dict(reason="限制指定执行者的浪费尾部。", control_updates=[
                dict(component="loop_rules", op="add", value=dict(
                    memory_id="test-experience", actor_id="planner", max_iterations=2,
                    max_no_progress=3, reason="依据已观测循环提出有界候选。"))
            ]), ensure_ascii=False)

        self.model_draft_content = draft_content
        self.start()
        job_id, job, _ = self.wait_search("L3")
        return self.call("agent_evaluation.tree", job_id=job_id), job

    def test_l3_missing_finalization_condition_is_repaired_once(self):
        """缺条件不是全局作业故障；修复后的合法规则仍须预检、实际应用和评测。"""
        tree, job = self.run_scoped_iteration_repair()
        node = tree["nodes"][1]
        self.assertEqual(tree["model_calls"], 2)
        self.assertEqual(node["repair_error"], "control_loop_semantic_scope_required")
        self.assertEqual(node["status"], "accepted", node)
        self.assertEqual(job["status"], "passed", job)
        self.assertTrue(node["control_preflight"]["passed"])
        self.assertGreater(node["control_coverage"]["capabilities"]["loop"]["applied"], 0)
        original, repair = self.requests
        self.assertEqual(repair["phase"], "repair_control_capability")
        self.assertEqual(repair["error"], "control_loop_semantic_scope_required")
        self.assertEqual(repair["selected_components"], original["selected_components"])
        self.assertFalse(repair["controls"]["aggressive_controls"])
        self.assertEqual(len(repair["feedback"]), 1)
        self.assertTrue(all(row["task_id"].startswith("train-") for row in repair["feedback"]))

    def test_l3_missing_finalization_repair_cannot_loop(self):
        """模型再次漏写时记录候选错误，不无限重试、不执行候选，也不放宽校验。"""
        tree, _ = self.run_scoped_iteration_repair(repeat=True, model_calls=4)
        node = tree["nodes"][1]
        self.assertEqual(tree["model_calls"], 2)
        self.assertEqual(node["error"], "control_loop_semantic_scope_required")
        self.assertEqual(node["status"], "error")
        self.assertNotIn("screening", node)
        self.assertNotIn("validation", node)

    def test_l3_missing_finalization_respects_repair_budget(self):
        """没有额外模型预算时保持原拒绝行为，不偷偷多发模型请求。"""
        tree, _ = self.run_scoped_iteration_repair(model_calls=1)
        self.assertEqual(tree["model_calls"], 1)
        self.assertEqual(tree["nodes"][1]["error"], "control_loop_semantic_scope_required")
        self.assertNotIn("repair_error", tree["nodes"][1])

    def test_l3_missing_finalization_respects_disabled_repair(self):
        """显式关闭修复时不消耗第二次预算。"""
        tree, _ = self.run_scoped_iteration_repair(max_attempts=1)
        self.assertEqual(tree["model_calls"], 1)
        self.assertEqual(tree["nodes"][1]["error"], "control_loop_semantic_scope_required")
        self.assertNotIn("repair_error", tree["nodes"][1])

    # 回归用例：test_l3_generated_loop_rules_reach_actual_host。
    # 重点核验字段或诊断：max_no_progress；异常路径不能被当成成功。
    def test_l3_generated_loop_rules_reach_actual_host(self):
        rule = self.exercise_controls("loops", "L3")
        self.assertEqual(int(rule["max_iterations"]), 12)
        self.assertEqual(int(rule["max_no_progress"]), 1)

    # 回归用例：test_incremental_l3_rule_is_generated_preflighted_and_executed。
    # 重点核验字段或诊断：max_no_progress；异常路径不能被当成成功。
    def test_incremental_l3_rule_is_generated_preflighted_and_executed(self):
        self.incremental_controls = True
        rule = self.exercise_controls("loops", "L3")
        self.assertEqual(int(rule["max_iterations"]), 12)
        self.assertEqual(int(rule["max_no_progress"]), 1)
        drafts = [row["draft"] for row in self.model_drafts if row["level"] == "L3"]
        self.assertEqual(len(drafts), 1)
        self.assertNotIn("controls", drafts[0])
        self.assertEqual(len(drafts[0]["control_updates"]), 1)
        update = drafts[0]["control_updates"][0]
        self.assertEqual((update["component"], update["op"]), ("loop_rules", "add"))
        self.assertNotIn("index", update)
        job_id, _, _ = self.wait_search("L3")
        node = self.call("agent_evaluation.tree", job_id=job_id)["nodes"][1]
        self.assertTrue(node["control_preflight"]["passed"], node)
        self.assertGreater(sum(node["control_preflight"]["rules"].values()), 0)
        rules = [
            row
            for row in node["control_coverage"]["rules"].values()
            if row["boundary"] == "loop"
        ]
        self.assertTrue(rules)
        self.assertGreater(sum(row["applied"] for row in rules), 0)

    # 回归用例：test_l3_loop_search_without_manual_tool_whitelist。
    # 重点核验字段或诊断：max_no_progress；异常路径不能被当成成功。
    def test_l3_loop_search_without_manual_tool_whitelist(self):
        self.behavior = "loops"
        self.evaluation["variants"][0]["command"][-2] = "gepa_loops"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.config.update(
            max_level="L3",
            control_learning=dict(
                enabled=True,
                prompt_file="prompts/gepa-reflection.txt",
                max_rules=2,
                min_iterations=2,
                min_no_progress=1,
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L3")
        self.assertEqual(job["status"], "passed", job)
        self.assertEqual(job["gate"]["reasons"], [])
        self.assertFalse(job["gate"]["control_coverage"]["level_complete"])
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertIn("loop_rules", tree["available_components"])
        self.assertIn("action_rules", tree["available_components"])
        candidate = json.loads(
            (
                self.root / "production/agent-evaluation" / job_id / "candidate.json"
            ).read_text()
        )
        generated = [
            rule
            for rule in candidate["policy"]["options"]["loop_rules"]
            if "source_experience" in rule
        ]
        self.assertEqual(len(generated), 1)
        self.assertEqual(int(generated[0]["max_iterations"]), 12)
        self.assertEqual(int(generated[0]["max_no_progress"]), 1)

    # 回归用例：test_loop_evidence_survives_earlier_unrelated_failures。
    # 重点核验字段或诊断：max_no_progress、gepa_loops_late、observed_no_progress；异常路径不能被当成成功。
    def test_loop_evidence_survives_earlier_unrelated_failures(self):
        self.behavior = "loops"
        self.config["runtime_options"]["aggressive_controls"] = False
        self.config["runtime_options"]["loop_rules"][0]["max_no_progress"] = 3
        self.config.update(
            max_level="L3",
            evaluation_levels=["L3"],
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
        )
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
        self.assertGreater(
            sum(rule["applied"] for rule in node["control_coverage"]["rules"].values()),
            0,
        )
        controls = [
            action["loop_control"]
            for packet in self.requests
            for row in packet["feedback"]
            for run in row["trial"]["evidence"]["runs"]
            for action in run.get("actions", [])
            if action.get("loop_control")
        ]
        self.assertTrue(
            any(item.get("observed_no_progress", 0) for item in controls), controls
        )

    # 回归用例：test_aggressive_loop_budget_is_generated_evaluated_and_preserved。
    # 重点核验字段或诊断：max_no_progress；异常路径不能被当成成功。
    def test_aggressive_loop_budget_is_generated_evaluated_and_preserved(self):
        self.config["runtime_options"]["aggressive_controls"] = True
        rule = self.exercise_controls("loops", "L3")
        self.assertEqual(int(rule["max_iterations"]), 2)
        self.assertEqual(int(rule["max_no_progress"]), 3)
        job_id, _, _ = self.wait_search("L3")
        candidate = json.loads(
            (
                self.root / "production/agent-evaluation" / job_id / "candidate.json"
            ).read_text()
        )
        self.assertTrue(candidate["policy"]["options"]["aggressive_controls"])
        self.assertTrue(
            any(p.get("controls", {}).get("aggressive_controls") for p in self.requests)
        )

    # 测试辅助函数：configure_merge。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def configure_merge(self, mode="gepa_merge", **overrides):
        self.behavior = "merge"
        self.evaluation["variants"][0]["command"][-2] = mode
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.config["max_level"] = "L2"
        self.config["control_learning"] = dict(
            enabled=True,
            prompt_file="prompts/gepa-reflection.txt",
            actions={"calculate": ["verified"]},
            max_rules=2,
            min_iterations=2,
            min_no_progress=1,
        )
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            use_merge=True,
            max_rounds=3,
            max_model_calls=2,
            max_merge_invocations=2,
            proposals_per_round=2,
            component_selector="all",
            candidate_selection_strategy="pareto",
            prune_dominated=True,
            minibatch_strategy="epoch_shuffled",
        )
        config.update(overrides)
        fixture.write(self.root / "gepa.json", config)

    # 回归用例：test_two_branches_jointly_generate_one_published_candidate。
    # 关键断言：self.assertEqual(job["status"], "passed", job)；此处只验证测试构造的场景。
    def test_two_branches_jointly_generate_one_published_candidate(self):
        self.configure_merge()
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertEqual(job["status"], "passed", job)
        self.assertTrue(job["published"])
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(
            tree["algorithm"], "gepa-reflective-dag-v4-constrained-portfolio"
        )
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
        self.assertTrue(
            any("source_experience" in rule for rule in options["action_rules"])
        )
        for packet in self.requests:
            self.assertNotIn("VALIDATION_PRIVATE", json.dumps(packet))
            self.assertNotIn("HOLDOUT_PRIVATE", json.dumps(packet))
        count = len(self.requests)
        fixture.AgentEvaluationTests.tearDown(self)
        self.start()
        _, restored, _ = self.wait_search("L2")
        self.assertEqual(restored["version"], job["version"])
        self.assertEqual(len(self.requests), count)
        self.assertEqual(
            self.call("agent_evaluation.tree", job_id=job_id)["trials_used"],
            tree["trials_used"],
        )
        fixture.write(
            self.root / "merge-summary.json",
            dict(
                job_id=job_id,
                search=job["search"],
                parents=child["parents"],
                sources=child["component_sources"],
                published=True,
                restart="no repeated trials",
            ),
        )
        print("GEPA merge artifact:", self.root / "merge-summary.json")

    # 回归用例：test_transient_runner_failure_recovers_before_joint_merge。
    # 重点核验字段或诊断：gepa_merge_retry、source_report_refs；异常路径不能被当成成功。
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
        recovered = [
            value
            for value in tree["evaluations"].values()
            if value.get("source_report_refs")
        ]
        self.assertTrue(recovered)

    # 回归用例：test_runner_timeout_retries_only_the_failed_trial。
    # 重点核验字段或诊断：gepa_merge_timeout_retry、source_report_refs；异常路径不能被当成成功。
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
        recovered = [
            value
            for value in tree["evaluations"].values()
            if value.get("source_report_refs")
        ]
        self.assertTrue(recovered)
        self.assertTrue(
            any(
                value.get("trials_reserved") == 1
                for value in tree["evaluations"].values()
            )
        )

    # 回归用例：test_merge_regression_is_not_accepted_or_published。
    # 重点核验字段或诊断：gepa_merge_regression；异常路径不能被当成成功。
    def test_merge_regression_is_not_accepted_or_published(self):
        self.configure_merge("gepa_merge_regression")
        self.start()
        job_id, job, _ = self.wait_search("L2")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["nodes"][3]["status"], "rejected_merge")
        self.assertFalse(job.get("published", False))
        self.assertNotEqual(tree["winner"], 3)
        self.assertEqual(tree["model_calls"], 2)

    # 回归用例：test_pareto_screening_keeps_joint_search_and_final_gate。
    # 重点核验字段或诊断：min_invalid_loop_reduction；异常路径不能被当成成功。
    def test_pareto_screening_keeps_joint_search_and_final_gate(self):
        self.configure_merge(acceptance="pareto", frontier_type="hybrid")
        self.assessment["gate"]["min_invalid_loop_reduction"] = 0.15
        fixture.write(self.root / "agent-evaluation.json", self.assessment)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["nodes"][3]["kind"], "merge")
        self.assertEqual(tree["nodes"][3]["status"], "accepted")
        # 保留有价值节点不等于允许绕过发布门槛。
        self.assertFalse(job.get("published", False))
        self.assertNotEqual(job["status"], "passed")

    # 回归用例：test_partial_merge_validation_only_executes_each_task_once。
    # 关键断言：self.assertTrue(job.get("published", False), job)；此处只验证测试构造的场景。
    def test_partial_merge_validation_only_executes_each_task_once(self):
        self.configure_merge(merge_minibatch_size=1)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertTrue(job.get("published", False), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["trials_used"], 18)
        assembled = [
            item for item in tree["evaluations"].values() if item.get("source_reports")
        ]
        self.assertTrue(assembled)
        self.assertTrue(all(item["trials_reserved"] == 0 for item in assembled))
        self.assertTrue(
            all(
                key in tree["evaluations"]
                for item in assembled
                for key in item["source_reports"]
            )
        )

    # 回归用例：test_perfect_training_batch_skips_unnecessary_reflection。
    # 关键断言：self.assertTrue(job.get("published", False), job)；此处只验证测试构造的场景。
    def test_perfect_training_batch_skips_unnecessary_reflection(self):
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            max_rounds=4,
            max_model_calls=4,
            skip_perfect_score=True,
            candidate_selection_strategy="current_best",
            objective=dict(invalid_loop_weight=0, token_weight=0, latency_weight=0),
        )
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertTrue(job.get("published", False), job)
        self.assertEqual(tree["model_calls"], 2)
        self.assertTrue(
            any(node["status"] == "skipped_perfect" for node in tree["nodes"])
        )

    # 回归用例：test_interrupted_multi_proposal_batch_keeps_finished_siblings。
    # 关键断言：self.assertEqual(sum(p.get("controls", {}).get("level") == "L2" for p in self.requests), 2)；此处只验证测试构造的场景。
    def test_interrupted_multi_proposal_batch_keeps_finished_siblings(self):
        self.configure_merge(max_rounds=4, max_model_calls=3)
        self.pause_on = ("L2", 2)
        self.release.clear()
        self.start()
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if (
                sum(p.get("controls", {}).get("level") == "L2" for p in self.requests)
                >= 2
            ):
                break
            time.sleep(0.05)
        self.assertEqual(
            sum(p.get("controls", {}).get("level") == "L2" for p in self.requests), 2
        )
        status = self.call("agent_evaluation.status")
        job_id = next(
            key
            for key, value in status["jobs"].items()
            if value.get("gepa") and value["level"] == "L2"
        )
        before = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(before["nodes"][1]["status"], "screened")
        cached = {
            key: value["attempt"]
            for key, value in before["evaluations"].items()
            if value["status"] == "complete"
        }
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

    # 回归用例：test_interrupted_merge_resumes_with_both_parents。
    # 重点核验字段或诊断：gepa_merge_pause；异常路径不能被当成成功。
    def test_interrupted_merge_resumes_with_both_parents(self):
        self.configure_merge("gepa_merge_pause")
        self.env["GEPA_MERGE_READY"] = str(self.root / "merge-ready")
        self.env["GEPA_MERGE_RELEASE"] = str(self.root / "merge-release")
        self.evaluation["variants"][0]["pass_env"] = [
            "GEPA_MERGE_READY",
            "GEPA_MERGE_RELEASE",
        ]
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        deadline = time.monotonic() + 35
        while not (self.root / "merge-ready").exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertTrue((self.root / "merge-ready").exists())
        status = self.call("agent_evaluation.status")
        job_id = next(
            key
            for key, value in status["jobs"].items()
            if value.get("gepa") and value["level"] == "L2"
        )
        before = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(before["nodes"][3]["parents"], [1, 2])
        cached = {
            key: value["attempt"]
            for key, value in before["evaluations"].items()
            if value["status"] == "complete"
        }
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

    # 回归用例：test_best_of_batch_only_validates_the_selected_proposal。
    # 关键断言：self.assertEqual(len(tree["nodes"]), 3)；此处只验证测试构造的场景。
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

    # 回归用例：test_pxn_sampling_and_cartesian_frontier。
    # 关键断言：self.assertTrue(job.get("published", False), job)；此处只验证测试构造的场景。
    def test_pxn_sampling_and_cartesian_frontier(self):
        self.configure_merge(
            sampling_strategy="pxn",
            parents_per_round=2,
            proposals_per_round=1,
            frontier_type="cartesian",
        )
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertTrue(job.get("published", False), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["nodes"][3]["parents"], [1, 2])
        self.assertTrue(
            all(key.startswith("pair:") for key in tree["frontier_mapping"])
        )
        self.assertEqual(tree["reflection_batches"], 1)
        self.assertEqual(tree["training_batches"], 2)

    # 回归用例：test_independent_sampling_obeys_the_same_total_budget。
    # 关键断言：self.assertTrue(job.get("published", False), job)；此处只验证测试构造的场景。
    def test_independent_sampling_obeys_the_same_total_budget(self):
        self.configure_merge(sampling_strategy="independent", proposals_per_round=2)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertTrue(job.get("published", False), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["model_calls"], 2)
        self.assertEqual(tree["nodes"][3]["parents"], [1, 2])

    # 回归用例：test_quality_target_stops_search_before_call_budget。
    # 关键断言：self.assertTrue(job.get("published", False), job)；此处只验证测试构造的场景。
    def test_quality_target_stops_search_before_call_budget(self):
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            target_fitness=0.9,
            candidate_selection_strategy="current_best",
            objective=dict(invalid_loop_weight=0, token_weight=0, latency_weight=0),
        )
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search()
        self.assertTrue(job.get("published", False), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["stop_reason"], "target_fitness")
        self.assertEqual(tree["model_calls"], 2)
        self.assertTrue(tree["holdout_started"])

    # 测试辅助函数：configure_combee。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def configure_combee(self, **overrides):
        self.behavior = "combee"
        self.evaluation["repetitions"] = 2
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            reflection_strategy="combee",
            max_rounds=1,
            max_model_calls=3,
            max_feedback_trials=4,
            component_selector="all",
        )
        config.update(overrides)
        fixture.write(self.root / "gepa.json", config)

    def run_full_pool_without_loop_scores(self):
        """无正式 loop 评分的真实任务仍须保留明确缺测信息。"""
        import io

        self.judge_requests = []
        self.config.update(
            max_level="L2",
            evaluation_levels=["L2"],
            control_learning=dict(
                enabled=True,
                prompt_file="prompts/gepa-reflection.txt",
                actions={"calculate": ["verified"]},
                max_rules=2,
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            reflection_strategy="combee",
            max_rounds=1,
            max_model_calls=6,
            max_feedback_trials=1,
            max_input_bytes=65536,
            component_selector="round_robin",
            objective=dict(
                invalid_loop_weight=0,
                token_weight=0,
                latency_weight=0,
            ),
        )
        fixture.write(self.root / "gepa.json", config)
        # 宿主真实记录三轮；运行时次数不能当作未评分的无效循环次数。
        self.evaluation["variants"][0]["command"][-2] = "gepa_failure_replan"
        self.evaluation["checks"] = {
            "train-a": [dict(
                id="missing-required-state", path="/state/required_external_field",
                op="equals", value="required")]
        }
        self.evaluation["judge"] = dict(
            enabled=True,
            assessment_mode="objectives",
            runtime_config="model.json",
            prompt="prompts/task-judge-system.txt",
            max_model_calls=100,
            max_input_bytes=1048576,
        )
        self.assessment["gate"].pop("min_invalid_loop_reduction", None)
        fixture.write(self.root / "agent-evaluation.json", self.assessment)
        fixture.write(self.root / "evaluation.json", self.evaluation)
        original_handler = self.server.RequestHandlerClass
        owner = self

        class ScoredFactsModel(original_handler):
            def do_POST(handler):
                body = handler.rfile.read(int(handler.headers["Content-Length"]))
                packet = json.loads(json.loads(body)["messages"][-1]["content"])
                if packet.get("kind") == "task-judge-consistency.v1":
                    stream = handler.rfile
                    handler.rfile = io.BytesIO(body)
                    try:
                        return original_handler.do_POST(handler)
                    finally:
                        handler.rfile = stream
                if "assessment" in packet or "loop_inventory" in packet:
                    owner.judge_requests.append(packet)
                    hard_failed = packet["task_input"].startswith("TRAIN_VISIBLE 甲类")
                    reply = dict(
                        verdict="failed" if hard_failed else "passed",
                        reason="目标已完成；甲类训练任务的独立硬检查仍未通过。"
                        if hard_failed else "测试目标按可见证据完成。",
                        evidence_paths=["/traces"],
                        goal_verdicts=[dict(
                            id=goal["id"], status="passed", reason="本目标按测试事实完成。",
                            evidence_paths=["/traces"])
                            for goal in packet.get("assessment", {}).get("goals", [])],
                    )
                    if "turn_expectations" in packet:
                        reply["turn_verdicts"] = [dict(
                            turn=turn["turn"], verdict="passed", reason="本轮目标已完成。")
                            for turn in packet["turn_expectations"]]
                else:
                    owner.requests.append(packet)
                    phase = packet.get("phase", "")
                    if phase in ("pool_diagnostic_reduce", "pool_counterexample_review"):
                        proposal = next(row for row in packet["intermediate_proposals"]
                                        if "action_rules" in row["candidate_values"])
                        reply = dict(
                            reason="选择已经验证绑定的动作规则。",
                            selected_proposals={"action_rules": proposal["id"]},
                        )
                    else:
                        reply = dict(
                            reason="使用真实训练动作的核验参数。",
                            controls=dict(action_rules=[dict(
                                memory_id="test-experience", operation="calculate",
                                required_arguments=["verified"],
                                reason="缺少核验参数时返回可恢复的拒绝。")]),
                        )
                response = json.dumps(dict(choices=[dict(
                    finish_reason="stop", message=dict(content=json.dumps(
                        reply, ensure_ascii=False)))])).encode()
                handler.send_response(200)
                handler.send_header("Content-Type", "application/json")
                handler.send_header("Content-Length", str(len(response)))
                handler.end_headers()
                handler.wfile.write(response)

        self.server.RequestHandlerClass = ScoredFactsModel
        self.start()
        job_id, job, _ = self.wait_search("L2")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertNotEqual(job["status"], "error", job)
        maps = [packet for packet in self.requests
                if packet.get("phase") == "pool_diagnostic_map"]
        self.assertEqual(len(maps), 2, (tree, self.requests))
        self.assertTrue(all(packet["selected_components"] == ["action_rules"]
                            for packet in maps))
        review = next(packet for packet in self.requests
                      if packet.get("phase") == "pool_counterexample_review")
        self.assertEqual({row["task_id"] for row in review["case_outcomes"]},
                         {"train-a", "train-b"})
        return maps, review

    def test_full_pool_action_reflection_keeps_unscored_loops_missing(self):
        """无正式 loop 评分时保留缺值，运行时循环次数不能冒充正式零值。"""
        maps, review = self.run_full_pool_without_loop_scores()
        rows = [row for packet in maps for row in packet["feedback"]]
        facts = ([row["trial"]["metrics"] for row in rows]
                 + [row for packet in maps for row in packet["training_batch_summary"]]
                 + review["case_outcomes"])
        for fact in facts:
            self.assertNotIn("invalid_loops", fact, fact)
            self.assertNotIn("invalid_loop_count", fact, fact)
            self.assertEqual(fact["metric_coverage"], dict(
                effective_quality=True, strict_passed=True, invalid_loop_count=False), fact)
        for row in rows:
            self.assertEqual(row["trial"]["metrics"]["loop_count"], 3, row)

    # 回归用例：test_combee_map_reduce_uses_training_evidence_and_counts_every_call。
    # 关键断言：self.assertTrue(job.get("published", False), job)；此处只验证测试构造的场景。
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
        self.assertEqual(
            [packet["phase"] for packet in self.requests],
            ["combee_map", "combee_map", "combee_reduce"],
        )
        self.assertEqual(
            sum(len(packet["feedback"]) for packet in self.requests[:2]), 8
        )
        self.assertNotIn("feedback", self.requests[-1])
        self.assertEqual(len(self.requests[-1]["intermediate_proposals"]), 2)
        for packet in self.requests:
            body = json.dumps(packet)
            self.assertNotIn("HOLDOUT_PRIVATE", body)
            self.assertNotIn("VALIDATION_PRIVATE", body)
        folder = (
            self.root / "production/agent-evaluation" / job_id / "search/reflections"
        )
        for stage in stages:
            self.assertTrue((folder / (stage["file"] + ".request.json")).is_file())
            self.assertTrue((folder / (stage["file"] + ".draft.json")).is_file())

    # 回归用例：test_combee_budget_shortage_does_not_start_partial_model_work。
    # 重点核验字段或诊断：gepa_model_budget_exhausted；异常路径不能被当成成功。
    def test_combee_budget_shortage_does_not_start_partial_model_work(self):
        self.configure_combee(max_model_calls=2)
        self.start()
        job_id, job, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["model_calls"], 0)
        self.assertEqual(tree["nodes"][1]["error"], "gepa_model_budget_exhausted")
        self.assertEqual(len(self.requests), 0)
        self.assertFalse(tree.get("holdout_started", False))
        self.assertEqual(job["status"], "rejected")
        self.assertFalse(job.get("published", False))

    # 回归用例：test_combee_small_batch_falls_back_to_single_reflection。
    # 关键断言：self.assertEqual(tree["model_calls"], 1)；此处只验证测试构造的场景。
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

    # 回归用例：test_combee_rejects_sensitive_map_output_before_reduction_or_persistence。
    # 重点核验字段或诊断：credential_value_rejected；异常路径不能被当成成功。
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
                self.assertNotIn(
                    self.env["GEPA_LEAK_SECRET"].encode(), path.read_bytes(), str(path)
                )

    # 回归用例：test_combee_combines_only_the_selected_components。
    # 关键断言：self.assertTrue(job.get("published", False), job)；此处只验证测试构造的场景。
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
        self.assertTrue(
            any("source_experience" in rule for rule in options["action_rules"])
        )
        for packet in self.requests:
            self.assertEqual(len(packet["selected_components"]), 1)

    # 回归用例：test_incremental_combee_control_map_and_reduce_reach_actual_host。
    # 关键断言：self.assertEqual(；此处只验证测试构造的场景。
    def test_incremental_combee_control_map_and_reduce_reach_actual_host(self):
        self.incremental_controls = True
        self.test_combee_combines_only_the_selected_components()
        drafts = [
            row
            for row in self.model_drafts
            if row["selected_components"] == ["action_rules"]
        ]
        self.assertEqual(
            [row["phase"] for row in drafts],
            ["combee_map", "combee_map", "combee_reduce"],
        )
        for row in drafts:
            self.assertNotIn("controls", row["draft"])
            updates = row["draft"]["control_updates"]
            self.assertEqual(len(updates), 1)
            self.assertEqual(
                (updates[0]["component"], updates[0]["op"]), ("action_rules", "add")
            )
            self.assertNotIn("index", updates[0])
        job_id, _, _ = self.wait_search("L2")
        node = self.call("agent_evaluation.tree", job_id=job_id)["nodes"][1]
        self.assertTrue(node["control_preflight"]["passed"], node)
        rules = [
            row
            for row in node["control_coverage"]["rules"].values()
            if row["boundary"] == "actions"
        ]
        self.assertTrue(rules)
        self.assertGreater(sum(row["applied"] for row in rules), 0)

    # 回归用例：test_combee_interruption_preserves_stages_and_spent_budget。
    # 关键断言：self.assertEqual(len(self.requests), 2)；此处只验证测试构造的场景。
    def test_combee_interruption_preserves_stages_and_spent_budget(self):
        self.configure_combee(max_rounds=2, max_model_calls=5)
        self.pause_on = ("L1", 2)
        self.release.clear()
        self.start()
        deadline = time.monotonic() + 25
        while len(self.requests) < 2 and time.monotonic() < deadline:
            time.sleep(0.05)
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

    # 回归用例：test_disabled_cache_remeasures_parent_and_merge_subset。
    # 关键断言：self.assertTrue(job.get("published", False), job)；此处只验证测试构造的场景。
    def test_disabled_cache_remeasures_parent_and_merge_subset(self):
        self.configure_merge(merge_minibatch_size=1, cache_evaluations=False)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertTrue(job.get("published", False), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["trials_used"], 21)
        self.assertFalse(
            any(item.get("source_reports") for item in tree["evaluations"].values())
        )

    # 测试辅助函数：check_partial_cache。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def check_partial_cache(self, fault, enabled=True):
        self.judge_requests, self.cache_train_calls = [], []
        self.cache_fault, self.cache_failed_task = fault, None
        self.behavior = "no_change"
        tasks = [
            json.loads(line)
            for line in (self.root / "gepa-tasks.jsonl").read_text().splitlines()
        ]
        tasks.append(
            dict(
                tasks[0],
                task_id="train-c",
                group_id="train-c",
                task_input="TRAIN_VISIBLE 丙类：核验后完成独立计算。",
            )
        )
        (self.root / "gepa-tasks.jsonl").write_text(
            "\n".join(json.dumps(task, ensure_ascii=False) for task in tasks)
        )
        result = subprocess.run(
            [
                str(fixture.CLI),
                "dataset",
                "build",
                "--config",
                str(self.root / "gepa-dataset.json"),
                "--output-root",
                str(self.root / "partial-dataset"),
            ],
            capture_output=True,
            text=True,
            timeout=20,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assessment["dataset"] = "partial-dataset"
        fixture.write(self.root / "agent-evaluation.json", self.assessment)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            max_rounds=3, max_model_calls=3, minibatch_size=2, cache_evaluations=enabled
        )
        fixture.write(self.root / "gepa.json", config)
        self.evaluation["checks"] = {}
        self.evaluation["variants"][0]["command"][-2] = "gepa_review"
        self.evaluation["judge"] = dict(
            enabled=True,
            runtime_config="model.json",
            max_repair_attempts=0,
            prompt="prompts/task-judge-system.txt",
            loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100,
            max_input_bytes=1048576,
        )
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        job_id, _, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        automatic_regrade = fault in ("invalid", "uncertain") and enabled
        if automatic_regrade:
            self.assertNotIn("error", tree["nodes"][1])
            self.assertEqual(len(self.cache_train_calls), 4)
            self.assertEqual(self.cache_train_calls.count(self.cache_train_calls[0]), 2)
            # 本用例注入一次训练裁判故障；
            # 其他划分可有自己的有界执行器恢复，不影响该故障确实发生。
            self.assertEqual(
                sum(
                    value.get("attempt", 0) == 2
                    for value in tree["evaluations"].values()
                    if value.get("split") == "train"
                ),
                1,
            )
            self.assertTrue(
                any(
                    value.get("source_reports")
                    for value in tree["evaluations"].values()
                )
            )
        else:
            self.assertEqual(
                tree["nodes"][1]["error"], "gepa_parent_evaluation_inconclusive"
            )
            self.assertEqual(len(self.cache_train_calls), 4 if enabled else 6)
        if enabled and not automatic_regrade:
            self.assertEqual(self.cache_train_calls.count(self.cache_train_calls[0]), 2)
            self.assertEqual(self.cache_train_calls.count(self.cache_failed_task), 1)
            partial = {
                key
                for key, value in tree["evaluations"].items()
                if value["status"] == "inconclusive"
            }
            if fault == "invalid":
                self.assertTrue(
                    any(
                        partial & set(value.get("source_reports", []))
                        for value in tree["evaluations"].values()
                    )
                )
        elif not enabled:
            self.assertFalse(
                any(
                    value.get("source_reports")
                    for value in tree["evaluations"].values()
                )
            )
        self.assertGreaterEqual(
            len([node for node in tree["nodes"][2:] if node["status"] == "duplicate"]),
            1,
        )

    # 回归用例：test_partial_cache_reuses_scored_tasks_but_retries_judge_error。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_partial_cache_reuses_scored_tasks_but_retries_judge_error(self):
        self.check_partial_cache("invalid")

    # 回归用例：test_partial_cache_does_not_reuse_unknown_loop_metrics。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_partial_cache_does_not_reuse_unknown_loop_metrics(self):
        self.check_partial_cache("uncertain")

    # 回归用例：test_partial_cache_can_be_disabled。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_partial_cache_can_be_disabled(self):
        self.check_partial_cache("invalid", enabled=False)

    # 测试辅助函数：check_seed_cache_recovery。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def check_seed_cache_recovery(self, fault, enabled=True, failures=1, repetitions=1):
        self.judge_requests, self.seed_judge_tasks = [], []
        # 缓存开启时，首次未知会自动重评一次；额外制造一次未知以验证显式恢复。
        automatic = repetitions if enabled and fault == "uncertain" else 0
        self.seed_fault, self.seed_failures = fault, failures + automatic
        self.behavior = "no_change"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1, cache_evaluations=enabled)
        fixture.write(self.root / "gepa.json", config)
        self.evaluation.update(checks={}, repetitions=repetitions)
        self.evaluation["variants"][0]["command"][-2] = "gepa_review"
        if fault == "tokens":
            self.env["GEPA_MISSING_METRIC_ONCE"] = str(
                self.root / "metric-missing-once"
            )
            self.evaluation["variants"][0]["pass_env"].append(
                "GEPA_MISSING_METRIC_ONCE"
            )
        self.evaluation["judge"] = dict(
            enabled=True,
            runtime_config="model.json",
            max_repair_attempts=0,
            prompt="prompts/task-judge-system.txt",
            loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100,
            max_input_bytes=1048576,
        )
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        job_id, job, _ = self.wait_search()
        root = self.root / "production/agent-evaluation" / job_id / "search"
        if fault == "tokens":
            # 缺少目标成本字段时，搜索应在同一个作业内复用完整题，只补跑缺失题；
            # 不能再要求调用方先看到整批 inconclusive，再手工 retry。
            self.assertIn(job["status"], ("passed", "rejected"), job)
            tree = self.call("agent_evaluation.tree", job_id=job_id)
            recovered = [
                value
                for value in tree["evaluations"].values()
                if value.get("rerun_incomplete_metrics")
            ]
            self.assertEqual(len(recovered), 1, tree["evaluations"])
            cached = recovered[0]
            self.assertEqual(cached["status"], "complete")
            self.assertTrue(cached["automatic_recovery_used"])
            self.assertEqual(cached["trials_reserved"], 0)
            report = json.loads((root / cached["report"]).read_text())
            self.assertTrue(all("tokens" in t["metrics"] for t in report["trials"]))
            self.assertGreater(report["summary"].get("reused_trials", 0), 0)
            source_reports = [
                json.loads((root / ref["report"]).read_text())
                for ref in cached["source_report_refs"]
            ]
            self.assertTrue(
                any(
                    "tokens" not in trial["metrics"]
                    for source in source_reports
                    for trial in source["trials"]
                )
            )
            self.assertEqual(len(self.seed_judge_tasks), 5)
            return
        self.assertEqual(job.get("error"), "gepa_seed_evaluation_inconclusive")
        before = self.call("agent_evaluation.tree", job_id=job_id)
        key = next(iter(before["evaluations"]))
        original = before["evaluations"][key]
        original_bytes = (root / original["report"]).read_bytes()
        self.assertEqual(original["status"], "inconclusive")
        for attempt in range(failures):
            self.call("agent_evaluation.retry", job_id=job_id)
            _, job, _ = self.wait_search()
            tree = self.call("agent_evaluation.tree", job_id=job_id)
            cached = tree["evaluations"][key]
            if attempt + 1 < failures:
                self.assertEqual(job.get("error"), "gepa_seed_evaluation_inconclusive")
                report = json.loads((root / cached["report"]).read_text())
                self.assertTrue(
                    report["complete"],
                    "a completed trial batch became structurally incomplete",
                )
                self.assertEqual(cached["status"], "inconclusive")
        self.assertIn(job["status"], ("passed", "rejected"), job)
        self.assertEqual(tree["nodes"][0]["status"], "accepted")
        expected = repetitions * 4 + failures * (1 if enabled else repetitions * 2) + automatic
        self.assertEqual(len(self.seed_judge_tasks), expected)
        self.assertEqual(
            sum("乙类" in task for task in self.seed_judge_tasks),
            repetitions * (2 if enabled else 2 + failures),
        )
        self.assertEqual((root / original["report"]).read_bytes(), original_bytes)
        if enabled:
            if fault == "uncertain":
                self.assertEqual(cached["trials_reserved"], 0)
                restored = json.loads((root / cached["report"]).read_text())
                self.assertEqual(
                    restored["summary"]["reused_trials"], repetitions * 2 - 1
                )
                self.assertEqual(restored["summary"]["regraded_trials"], 1)
                self.assertTrue(cached["automatic_recovery_used"])
                old = {t["trial_id"]: t for t in json.loads(original_bytes)["trials"]}
                for trial in restored["trials"]:
                    self.assertIn(trial["trial_id"], old)
                    previous = old[trial["trial_id"]]
                    for field in ("duration_ms", "metrics", "execution_status"):
                        self.assertEqual(trial[field], previous[field])
                    self.assertEqual(trial["evidence"]["trace"], previous["evidence"]["trace"])
                self.assertIn("resume_source", restored["summary"])
            else:
                self.assertEqual(cached["trials_reserved"], 0)
                refs = cached["source_report_refs"]
                self.assertTrue(
                    any(
                        ref["key"] == key and ref["report"] != cached["report"]
                        for ref in refs
                    )
                )
                for ref in refs:
                    self.assertTrue((root / ref["report"]).is_file())
        else:
            self.assertNotIn("source_report_refs", cached)

    # 回归用例：test_exact_seed_retry_reuses_valid_tasks。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_exact_seed_retry_reuses_valid_tasks(self):
        self.check_seed_cache_recovery("uncertain", repetitions=2)

    # 回归用例：test_judge_failure_retry_preserves_completed_execution。
    # 重点核验字段或诊断：gepa_seed_evaluation_inconclusive、evaluation_judge_failed；异常路径不能被当成成功。
    def test_judge_failure_retry_preserves_completed_execution(self, cross_job=False):
        self.judge_requests = []
        self.fail_search_judge = True
        self.behavior = "no_change"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.evaluation["checks"] = {}
        self.evaluation["variants"][0]["command"][-2] = "gepa_review"
        self.evaluation["judge"] = dict(
            enabled=True,
            runtime_config="model.json",
            max_repair_attempts=0,
            prompt="prompts/task-judge-system.txt",
            loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100,
            max_input_bytes=1048576,
        )
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
        self.assertTrue(
            all(
                t["execution_status"] == "completed"
                and t["error_code"] == "evaluation_judge_failed"
                for t in old["trials"]
            )
        )
        self.fail_search_judge = False
        if cross_job:
            # 搜索配置变化会产生新作业；宿主、数据和评分身份仍相同。
            # 两条已完成执行应只重评，不能再次运行宿主。
            seed = json.loads((root.parent / "seed.json").read_text())
            self.process.terminate()
            self.process.communicate(timeout=10)
            self.process = None
            config.update(max_rounds=2, max_model_calls=2)
            fixture.write(self.root / "gepa.json", config)
            self.start()
            previous_id = job_id
            job_id = self.call("agent_evaluation.submit", artifact=seed)["job_id"]
            self.assertNotEqual(job_id, previous_id)
            deadline = time.monotonic() + 70
            while time.monotonic() < deadline:
                job = self.call("agent_evaluation.status")["jobs"][job_id]
                if job["status"] not in ("pending", "running"):
                    break
                time.sleep(0.05)
            else:
                self.fail("跨作业裁判重评未完成")
            root = self.root / "production/agent-evaluation" / job_id / "search"
        else:
            self.call("agent_evaluation.retry", job_id=job_id)
            _, job, _ = self.wait_search()
        self.assertIn(job["status"], ("passed", "rejected"), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        if cross_job:
            key, cached = next(
                (k, value) for k, value in tree["evaluations"].items()
                if value["split"] == "val" and value["version"] == tree["seed_version"]
            )
            self.assertEqual(cached.get("shared_cache"), "hit")
            self.assertEqual(cached["trials_reserved"], 0)
        restored = json.loads((root / tree["evaluations"][key]["report"]).read_text())
        self.assertEqual(restored["summary"].get("regraded_trials"), 2)
        for previous, current in zip(old["trials"], restored["trials"]):
            for field in (
                "trial_id",
                "task_id",
                "task_version",
                "duration_ms",
                "metrics",
                "execution_status",
            ):
                self.assertEqual(current[field], previous[field], field)
            self.assertEqual(
                current["evidence"]["trace"], previous["evidence"]["trace"]
            )
        self.assertEqual(source.read_bytes(), source_bytes)

    # 新作业复用评分失败批次的原始执行，只重评裁判，逐项核验执行 ID 与轨迹不变。
    def test_cross_job_inconclusive_cache_regrades_without_agent_execution(self):
        self.test_judge_failure_retry_preserves_completed_execution(cross_job=True)

    # 回归用例：test_transient_judge_retry_regrades_only_the_failed_task。
    # 重点核验字段或诊断：gepa_seed_evaluation_inconclusive；异常路径不能被当成成功。
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
        self.evaluation["judge"] = dict(
            enabled=True,
            runtime_config="model.json",
            max_repair_attempts=0,
            prompt="prompts/task-judge-system.txt",
            loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100,
            max_input_bytes=1048576,
        )
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
        key, cached = next(
            (k, v)
            for k, v in tree["evaluations"].items()
            if v["split"] == "val" and v["version"] == tree["seed_version"]
        )
        self.assertEqual(cached["attempt"], 3)
        before = json.loads(
            (root / "evaluations" / key / "attempt-1/report.json").read_text()
        )
        after = json.loads((root / cached["report"]).read_text())
        self.assertEqual(after["summary"].get("regraded_trials"), 1)
        self.assertEqual(
            sum(
                p["task_input"].startswith("VALIDATION_PRIVATE")
                for p in self.judge_requests
            ),
            6,
        )
        old = {t["task_id"]: t for t in before["trials"]}
        for trial in after["trials"]:
            previous = old[trial["task_id"]]
            for field in ("trial_id", "duration_ms", "metrics", "execution_status"):
                self.assertEqual(trial[field], previous[field], field)
            self.assertEqual(trial["evidence"]["trace"], previous["evidence"]["trace"])

    # 回归用例：test_transient_judge_recovers_automatically_without_agent_reexecution。
    # 关键断言：self.assertIn(job["status"], ("passed", "rejected"), job)；此处只验证测试构造的场景。
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
        self.evaluation["judge"] = dict(
            enabled=True,
            runtime_config="model.json",
            max_repair_attempts=0,
            prompt="prompts/task-judge-system.txt",
            loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100,
            max_input_bytes=1048576,
        )
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        job_id, job, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertIn(job["status"], ("passed", "rejected"), job)
        root = self.root / "production/agent-evaluation" / job_id / "search"
        key, cached = next(
            (k, v)
            for k, v in tree["evaluations"].items()
            if v["split"] == "val" and v["version"] == tree["seed_version"]
        )
        self.assertEqual(cached["attempt"], 2)
        before = json.loads(
            (root / "evaluations" / key / "attempt-1/report.json").read_text()
        )
        after = json.loads((root / cached["report"]).read_text())
        self.assertEqual(after["summary"].get("regraded_trials"), 1)
        self.assertEqual(
            sum(
                p["task_input"].startswith("VALIDATION_PRIVATE")
                for p in self.judge_requests
            ),
            5,
        )
        old = {t["task_id"]: t for t in before["trials"]}
        for trial in after["trials"]:
            previous = old[trial["task_id"]]
            for field in ("trial_id", "duration_ms", "metrics", "execution_status"):
                self.assertEqual(trial[field], previous[field], field)
            self.assertEqual(trial["evidence"]["trace"], previous["evidence"]["trace"])

    # 回归用例：test_invalid_json_judge_recovers_automatically_without_agent_reexecution。
    # 关键断言：self.assertIn(job["status"], ("passed", "rejected"), job)；此处只验证测试构造的场景。
    def test_summary_conflict_judge_recovers_without_agent_reexecution(self):
        """目标模式中的总结矛盾沿用一次自动补评；原执行身份、轨迹和时延不变。"""
        self.summary_judge_failures = 1
        self.test_invalid_json_judge_recovers_automatically_without_agent_reexecution()

    def test_reason_conflict_recovers_without_agent_reexecution(self):
        """理由冲突耗尽批内修复后，只补评未决题，其他真实执行保持不变。"""
        self.consistency_judge_failures = 1
        self.consistency_issue = "unjustified_requirement"
        self.test_invalid_json_judge_recovers_automatically_without_agent_reexecution()

    def test_judge_errors_and_missing_tokens_recover_per_trial_without_batch_reexecution(self):
        """同批裁判错误与缺遥测并存时，仅缺遥测 trial 重跑真实 Agent。"""
        self.judge_requests = []
        self.consistency_judge_failures = 3
        self.consistency_issue = "unjustified_requirement"
        self.consistency_fail_each_prefix_once = True
        self.consistency_judge_prefixes = (
            "VALIDATION_PRIVATE 甲类",
            "VALIDATION_PRIVATE 乙类",
            "VALIDATION_PRIVATE 丙类",
        )
        self.behavior = "no_change"

        tasks = [
            json.loads(line)
            for line in (self.root / "gepa-tasks.jsonl").read_text().splitlines()
        ]
        val_a = next(task for task in tasks if task["task_id"] == "val-a")
        for suffix, family in (("c", "丙类"), ("d", "丁类")):
            tasks.append(
                dict(
                    val_a,
                    task_id=f"val-{suffix}",
                    group_id=f"val-{suffix}",
                    category=family,
                    task_input=(
                        f"VALIDATION_PRIVATE {family}：生成视频时缺少图片素材，"
                        "仍需完成独立计算。"
                    ),
                )
            )
        (self.root / "gepa-tasks.jsonl").write_text(
            "\n".join(json.dumps(task, ensure_ascii=False) for task in tasks),
            encoding="utf-8",
        )
        result = subprocess.run(
            [
                str(fixture.CLI),
                "dataset",
                "build",
                "--config",
                str(self.root / "gepa-dataset.json"),
                "--output-root",
                str(self.root / "combined-recovery-dataset"),
            ],
            capture_output=True,
            text=True,
            timeout=20,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assessment["dataset"] = "combined-recovery-dataset"
        fixture.write(self.root / "agent-evaluation.json", self.assessment)

        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1, cache_evaluations=True)
        fixture.write(self.root / "gepa.json", config)
        self.config["evaluation_levels"] = ["L1"]
        fixture.write(self.root / "service.json", self.config)
        self.env["GEPA_MISSING_METRIC_ONCE"] = str(self.root / "metric-missing-once")
        self.evaluation.update(checks={}, repetitions=1)
        self.evaluation["variants"][0]["command"][-2] = "gepa_review"
        self.evaluation["variants"][0]["pass_env"].append(
            "GEPA_MISSING_METRIC_ONCE"
        )
        self.evaluation["judge"] = dict(
            enabled=True,
            runtime_config="model.json",
            max_repair_attempts=0,
            prompt="prompts/task-judge-system.txt",
            loop_review_prompt="prompts/task-loop-review.txt",
            consistency_prompt="prompts/task-consistency.txt",
            assessment_mode="objectives",
            max_model_calls=100,
            max_input_bytes=1048576,
        )
        fixture.write(self.root / "evaluation.json", self.evaluation)

        self.start()
        job_id, job, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        search_root = self.root / "production/agent-evaluation" / job_id / "search"
        key, cached = next(
            (key, value)
            for key, value in tree["evaluations"].items()
            if value["split"] == "val"
            and value["version"] == tree["seed_version"]
            and len(value["task_ids"]) == 4
        )
        self.assertEqual(cached["attempt"], 2)
        self.assertTrue(cached["automatic_recovery_used"])
        self.assertTrue(cached["rerun_incomplete_metrics"])
        self.assertEqual(cached["trials_reserved"], 0)
        self.assertEqual(cached["trials_budget_reserved"], 4)

        before = json.loads(
            (search_root / "evaluations" / key / "attempt-1/report.json").read_text()
        )
        after = json.loads((search_root / cached["report"]).read_text())
        old = {trial["task_id"]: trial for trial in before["trials"]}
        new = {trial["task_id"]: trial for trial in after["trials"]}
        self.assertEqual(
            {task_id: trial.get("error_code", "") for task_id, trial in old.items()},
            {
                "val-a": "evaluation_judge_failed",
                "val-b": "evaluation_judge_failed",
                "val-c": "evaluation_judge_failed",
                "val-d": "",
            },
        )
        self.assertIn(
            job["status"],
            ("passed", "rejected"),
            {
                task_id: {
                    "error": trial.get("error_code", ""),
                    "verdict": trial.get("verdict", ""),
                    "execution": trial.get("execution_status", ""),
                    "loop_review": trial.get("evidence", {}).get("loop_review", {}),
                    "assessment": trial.get("evidence", {}).get("assessment", {}),
                }
                for task_id, trial in new.items()
            },
        )
        self.assertEqual(after["summary"].get("reused_trials"), 1)
        self.assertEqual(after["summary"].get("regraded_trials"), 2)
        self.assertEqual(set(old), set(new))
        self.assertEqual(old["val-a"]["error_code"], "evaluation_judge_failed")
        self.assertNotIn("tokens", old["val-a"]["metrics"])
        self.assertNotEqual(new["val-a"]["trial_id"], old["val-a"]["trial_id"])
        self.assertIn("tokens", new["val-a"]["metrics"])
        for task_id in ("val-b", "val-c"):
            self.assertEqual(new[task_id]["trial_id"], old[task_id]["trial_id"])
            self.assertEqual(new[task_id]["evidence"]["trace"], old[task_id]["evidence"]["trace"])
        self.assertEqual(new["val-d"]["trial_id"], old["val-d"]["trial_id"])
        self.assertEqual(
            len({trial["trial_id"] for trial in before["trials"] + after["trials"]}),
            5,
        )
        self.assertEqual(
            tree["trials_used"],
            sum(
                value.get("trials_budget_reserved", 0)
                for value in tree["evaluations"].values()
            ),
        )

    def test_invalid_consistency_reply_recovers_without_agent_reexecution(self):
        """核验器格式错误沿用一次自动恢复，不能当作没有发现矛盾。"""
        self.consistency_judge_failures = 1
        self.consistency_bad_reply = True
        self.test_invalid_json_judge_recovers_automatically_without_agent_reexecution()

    def test_persistent_reason_conflict_stops_after_one_automatic_recovery(self):
        """持续冲突保留未决；不重跑宿主，不无限补评，也不发布候选。"""
        self.consistency_judge_failures = 99
        self.consistency_persistent = True
        self.test_invalid_json_judge_recovers_automatically_without_agent_reexecution()

    def test_invalid_json_judge_recovers_automatically_without_agent_reexecution(self):
        self.judge_requests = []
        self.fail_search_judge, self.search_judge_failures = True, 1
        self.search_judge_invalid_json = True
        if (getattr(self, "summary_judge_failures", 0)
                or getattr(self, "consistency_judge_failures", 0)):
            self.fail_search_judge = False
        self.fail_search_prefix = "VALIDATION_PRIVATE 甲类"
        self.behavior = "no_change"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.config["evaluation_levels"] = ["L1"]
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["checks"] = {}
        self.evaluation["variants"][0]["command"][-2] = "gepa_review"
        self.evaluation["judge"] = dict(
            enabled=True,
            runtime_config="model.json",
            max_repair_attempts=0,
            prompt="prompts/task-judge-system.txt",
            loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100,
            max_input_bytes=1048576,
        )
        if (getattr(self, "summary_judge_failures", 0)
                or getattr(self, "consistency_judge_failures", 0)):
            # 总结/理由矛盾只在目标模式中定义，普通非法 JSON 用例保留原有模式。
            self.evaluation["judge"]["assessment_mode"] = "objectives"
        if getattr(self, "consistency_judge_failures", 0):
            self.evaluation["judge"]["consistency_prompt"] = "prompts/task-consistency.txt"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        job_id, job, _ = self.wait_search()
        if getattr(self, "consistency_persistent", False):
            self.assertEqual(job.get("error"), "gepa_seed_evaluation_inconclusive")
            self.assertFalse(job.get("published", False))
        else:
            self.assertIn(job["status"], ("passed", "rejected"), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        root = self.root / "production/agent-evaluation" / job_id / "search"
        key, cached = next(
            (k, v)
            for k, v in tree["evaluations"].items()
            if v["split"] == "val" and v["version"] == tree["seed_version"]
        )
        self.assertEqual(cached["attempt"], 2)
        before = json.loads(
            (root / "evaluations" / key / "attempt-1/report.json").read_text()
        )
        after = json.loads((root / cached["report"]).read_text())
        self.assertEqual(after["summary"].get("regraded_trials"), 1)
        self.assertEqual(
            sum(
                p["task_input"].startswith("VALIDATION_PRIVATE")
                for p in self.judge_requests
            ),
            3 if getattr(self, "consistency_persistent", False) else 5,
        )
        old = {t["task_id"]: t for t in before["trials"]}
        for trial in after["trials"]:
            previous = old[trial["task_id"]]
            for field in ("trial_id", "duration_ms", "metrics", "execution_status"):
                self.assertEqual(trial[field], previous[field], field)
            self.assertEqual(trial["evidence"]["trace"], previous["evidence"]["trace"])
        if hasattr(self, "consistency_judge_failures"):
            self.assertTrue(cached["automatic_recovery_used"])
            self.assertEqual(before["summary"]["errors"], 1)
            self.assertEqual(after["summary"]["errors"],
                             1 if getattr(self, "consistency_persistent", False) else 0)

    # 回归用例：test_manual_resume_keeps_automatic_judge_recovery。
    # 重点核验字段或诊断：gepa_model_rate_limited、automatic_recovery_used；异常路径不能被当成成功。
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
        self.evaluation["judge"] = dict(
            enabled=True,
            runtime_config="model.json",
            max_repair_attempts=0,
            prompt="prompts/task-judge-system.txt",
            loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100,
            max_input_bytes=1048576,
        )
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

    # 回归用例：test_exact_seed_retry_keeps_good_tasks_after_another_failure。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_exact_seed_retry_keeps_good_tasks_after_another_failure(self):
        self.check_seed_cache_recovery("uncertain", failures=2)

    # 回归用例：test_rate_limited_parent_judge_preserves_the_pending_generation。
    # 重点核验字段或诊断：gepa_model_rate_limited；异常路径不能被当成成功。
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
        self.evaluation["judge"] = dict(
            enabled=True,
            runtime_config="model.json",
            max_repair_attempts=0,
            prompt="prompts/task-judge-system.txt",
            loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100,
            max_input_bytes=1048576,
        )
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        job_id, job, _ = self.wait_search()
        self.assertEqual(job.get("error"), "gepa_model_rate_limited")
        root = self.root / "production/agent-evaluation" / job_id / "search"
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(
            (tree["rounds"], tree["model_calls"], tree["active"]), (1, 0, 1)
        )
        self.assertEqual(len(tree["nodes"]), 2)
        self.assertEqual(tree["nodes"][1]["status"], "pending")
        self.assertFalse(tree.get("holdout_started", False))
        key, cached = next(
            (k, v) for k, v in tree["evaluations"].items() if v["split"] == "train"
        )
        previous = json.loads((root / cached["report"]).read_text())
        self.fail_search_judge = False
        self.call("agent_evaluation.retry", job_id=job_id)
        _, job, _ = self.wait_search()
        self.assertIn(job["status"], ("passed", "rejected"), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        restored = json.loads((root / tree["evaluations"][key]["report"]).read_text())
        self.assertEqual(restored["summary"].get("regraded_trials"), 2)
        self.assertEqual(
            [t["trial_id"] for t in previous["trials"]],
            [t["trial_id"] for t in restored["trials"]],
        )
        self.assertEqual(tree["rounds"], 2)
        self.assertEqual(tree["model_calls"], 2)

    # 回归用例：test_rate_limited_reflection_retries_the_same_generation。
    # 重点核验字段或诊断：gepa_model_rate_limited；异常路径不能被当成成功。
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
        self.assertEqual(
            (before["rounds"], before["model_calls"], before["active"]), (1, 1, 1)
        )
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

    # 回归用例：test_legacy_complete_seed_cache_with_missing_tokens_is_recoverable。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_legacy_complete_seed_cache_with_missing_tokens_is_recoverable(self):
        self.check_seed_cache_recovery("tokens")

    # 测试辅助函数：check_conservative_cost。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def check_conservative_cost(self, fault):
        self.judge_requests, self.seed_judge_tasks = [], []
        # 即使允许一次证据重评，持续未知仍不能按最坏成本补成确定成绩。
        self.seed_fault, self.seed_failures = fault, 100 if fault == "uncertain" else 1
        self.behavior = "no_change"
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            max_rounds=1, max_model_calls=1, unknown_token_policy="worst_case"
        )
        fixture.write(self.root / "gepa.json", config)
        self.evaluation.update(checks={}, repetitions=2)
        self.evaluation["variants"][0]["command"][-2] = "gepa_review"
        if fault == "tokens":
            self.env["GEPA_MISSING_METRIC_ONCE"] = str(
                self.root / "metric-missing-once"
            )
            self.evaluation["variants"][0]["pass_env"].append(
                "GEPA_MISSING_METRIC_ONCE"
            )
        self.evaluation["judge"] = dict(
            enabled=True,
            runtime_config="model.json",
            max_repair_attempts=0,
            prompt="prompts/task-judge-system.txt",
            loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100,
            max_input_bytes=1048576,
        )
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.start()
        job_id, job, _ = self.wait_search()
        if fault != "tokens":
            self.assertEqual(job.get("error"), "gepa_seed_evaluation_inconclusive")
            return
        self.assertIn(job["status"], ("passed", "rejected"), job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        root = self.root / "production/agent-evaluation" / job_id / "search"
        source = next(
            v
            for v in tree["evaluations"].values()
            if v.get("split") == "val" and v["version"] == tree["seed_version"]
        )
        report = json.loads((root / source["report"]).read_text())
        self.assertTrue(any("tokens" not in t["metrics"] for t in report["trials"]))
        objective = config["objective"]
        for task, row in tree["nodes"][0]["validation"].items():
            trials = [t for t in report["trials"] if t["task_id"] == task]
            missing = sum("tokens" not in t["metrics"] for t in trials)
            self.assertEqual(row.get("unknown_token_trials", 0), missing)
            costs = [
                t["metrics"]["tokens"]
                / (objective["token_scale"] + t["metrics"]["tokens"])
                if "tokens" in t["metrics"]
                else 1.0
                for t in trials
            ]
            expected = sum(
                (t["verdict"] == "passed")
                - objective["invalid_loop_weight"]
                * t["evidence"]["loop_review"]["count"]
                / (
                    objective["invalid_loop_scale"]
                    + t["evidence"]["loop_review"]["count"]
                )
                - objective["token_weight"] * cost
                for t, cost in zip(trials, costs)
            ) / len(trials)
            self.assertAlmostEqual(row["fitness"], expected)
            self.assertAlmostEqual(
                row["token_efficiency"], sum(1 - c for c in costs) / len(costs)
            )

    # 回归用例：test_unknown_tokens_can_use_explicit_conservative_cost。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_unknown_tokens_can_use_explicit_conservative_cost(self):
        self.check_conservative_cost("tokens")

    # 回归用例：test_conservative_cost_does_not_accept_uncertain_grading。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_conservative_cost_does_not_accept_uncertain_grading(self):
        self.check_conservative_cost("uncertain")

    # 回归用例：test_exact_seed_retry_can_disable_cache。
    # 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
    def test_exact_seed_retry_can_disable_cache(self):
        self.check_seed_cache_recovery("uncertain", enabled=False)

    # 回归用例：test_round_robin_repairs_mutation_of_an_unselected_component_once。
    # 重点核验字段或诊断：gepa_component_not_authorized、repair_component_selection；异常路径不能被当成成功。
    def test_round_robin_repairs_mutation_of_an_unselected_component_once(self):
        self.configure_merge(
            component_selector="round_robin",
            max_rounds=1,
            max_model_calls=2,
            proposals_per_round=1,
        )
        self.start()
        job_id, job, _ = self.wait_search("L2")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["nodes"][1]["components"], ["action_rules"])
        self.assertNotEqual(tree["nodes"][1]["status"], "error", tree["nodes"][1])
        self.assertEqual(
            tree["nodes"][1]["repair_error"], "gepa_component_not_authorized"
        )
        self.assertEqual(self.requests[-1]["phase"], "repair_component_selection")
        self.assertEqual(self.requests[-1]["selected_components"], ["action_rules"])
        self.assertEqual(len(self.requests[-1]["feedback"]), 1)
        request = next(
            p
            for p in self.requests
            if p.get("selected_components") == ["action_rules"] and "phase" not in p
        )
        self.assertEqual(set(request["components"]), {"action_rules"})
        self.assertIn("optimization_contract", request)

    # 测试辅助函数：run_schedule_component_repair。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def run_schedule_component_repair(
        self, model_calls=2, max_attempts=2, reflection_strategy="single"
    ):
        self.behavior = "schedule_bad_component"
        self.config.update(
            max_level="L2",
            evaluation_levels=["L2"],
            control_learning=dict(
                enabled=True,
                prompt_file="prompts/gepa-reflection.txt",
                max_rules=2,
                max_attempts=max_attempts,
            ),
            instrumentation=dict(
                level="L2", capabilities=["model", "actions", "schedule"]
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_schedule"
        if reflection_strategy == "combee":
            self.evaluation["repetitions"] = 2
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            component_selector="round_robin",
            max_rounds=1,
            max_model_calls=model_calls,
            max_input_bytes=65536 if reflection_strategy == "combee" else 8192,
            max_feedback_trials=4,
            reflection_strategy=reflection_strategy,
        )
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        return job, tree

    # 回归用例：test_component_repair_preserves_bounded_schedule_evidence。
    # 重点核验字段或诊断：gepa_component_not_authorized、repair_component_selection；异常路径不能被当成成功。
    def test_component_repair_preserves_bounded_schedule_evidence(self):
        job, tree = self.run_schedule_component_repair()
        node = tree["nodes"][1]
        self.assertEqual(
            node.get("repair_error"), "gepa_component_not_authorized", node
        )
        repair = self.requests[-1]
        self.assertEqual(repair["phase"], "repair_component_selection")
        self.assertEqual(len(repair.get("feedback", [])), 1, repair)
        self.assertIn(repair["feedback"][0], self.requests[0]["feedback"])
        self.assertNotIn("draft", repair)
        self.assertNotIn("HOLDOUT_PRIVATE", json.dumps(self.requests))
        self.assertNotIn("VALIDATION_PRIVATE", json.dumps(self.requests))
        self.assertLessEqual(
            len(json.dumps(repair, ensure_ascii=False, separators=(",", ":")).encode()),
            8192,
        )
        self.assertEqual(job["status"], "passed", job)
        rules = [
            r
            for r in node["control_coverage"]["rules"].values()
            if r["boundary"] == "schedule"
        ]
        self.assertGreater(sum(r["applied"] for r in rules), 0)

    # 控制只读来源也可能被模型误当成可编辑经验；纠正后仍必须真正在宿主编排生效。
    def test_unknown_experience_component_is_repaired_once(self):
        self.wrong_experience_id = "readonly-control-source-not-an-l1-component"
        job, tree = self.run_schedule_component_repair()
        self.assertEqual(tree["nodes"][1].get("repair_error"), "gepa_experience_not_authorized")
        self.assertEqual(len(self.requests), 2)
        self.assertEqual(self.requests[1]["phase"], "repair_component_selection")
        self.assertNotIn("draft", self.requests[1])
        self.assertEqual(job["status"], "passed", job)
        applied = sum(r["applied"] for r in tree["nodes"][1]["control_coverage"]["rules"].values()
                      if r["boundary"] == "schedule")
        self.assertGreater(applied, 0)
        for system in self.reflection_systems:
            self.assertNotIn("改写后的完整经验文本", system)
            self.assertNotIn("loop_rules 每项为", system)
            self.assertIn("只读来源", system)
            self.assertIn("before_operation", system)
            self.assertNotIn("<!-- gepa:", system)

    # 权限修复不能突破调用预算；未修成的草稿不得进入真实候选执行。
    def test_unknown_experience_component_respects_model_budget(self):
        self.wrong_experience_id = "readonly-control-source-not-an-l1-component"
        _, tree = self.run_schedule_component_repair(model_calls=1)
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(tree["nodes"][1].get("error"), "gepa_experience_not_authorized")
        self.assertNotIn("screening", tree["nodes"][1])

    # 回归用例：test_incremental_schedule_component_violation_is_repaired_once。
    # 关键断言：self.assertEqual(len(self.requests), 2)；此处只验证测试构造的场景。
    def test_incremental_schedule_component_violation_is_repaired_once(self):
        self.incremental_controls = True
        self.incremental_component_violation = "once"
        self.test_component_repair_preserves_bounded_schedule_evidence()
        self.assertEqual(len(self.requests), 2)
        first, repaired = [row["draft"]["control_updates"] for row in self.model_drafts]
        self.assertEqual(
            (first[0]["component"], first[0]["op"]), ("action_rules", "add")
        )
        self.assertEqual(first[0]["value"]["primitive_id"], "operation_budget")
        self.assertEqual(
            [change["component"] for change in repaired], ["schedule_rules"]
        )
        self.assertEqual(self.requests[1]["components"], self.requests[0]["components"])

    # 回归用例：test_incremental_component_repair_cannot_repeat_unauthorized_update。
    # 重点核验字段或诊断：gepa_component_not_authorized、repair_component_selection；异常路径不能被当成成功。
    def test_incremental_component_repair_cannot_repeat_unauthorized_update(self):
        self.incremental_controls = True
        self.incremental_component_violation = "always"
        _, tree = self.run_schedule_component_repair(model_calls=3)
        node = tree["nodes"][1]
        self.assertEqual(
            node.get("repair_error"), "gepa_component_not_authorized", node
        )
        self.assertEqual(
            (node["status"], node["error"]), ("error", "gepa_component_not_authorized")
        )
        self.assertEqual(tree["model_calls"], 2)
        self.assertEqual(len(self.requests), 2)
        self.assertEqual(
            [packet.get("phase") for packet in self.requests],
            [None, "repair_component_selection"],
        )

    # 回归用例：test_incremental_component_repair_respects_model_budget。
    # 重点核验字段或诊断：gepa_component_not_authorized；异常路径不能被当成成功。
    def test_incremental_component_repair_respects_model_budget(self):
        self.incremental_controls = True
        self.incremental_component_violation = "once"
        _, tree = self.run_schedule_component_repair(model_calls=1)
        node = tree["nodes"][1]
        self.assertEqual(
            (node["status"], node["error"]), ("error", "gepa_component_not_authorized")
        )
        self.assertNotIn("repair_error", node)
        self.assertEqual(tree["model_calls"], 1)
        self.assertEqual(len(self.requests), 1)

    # 回归用例：test_incremental_combee_map_component_violation_is_repaired_once。
    # 重点核验字段或诊断：gepa_component_not_authorized、repair_component_selection；异常路径不能被当成成功。
    def test_incremental_combee_map_component_violation_is_repaired_once(self):
        self.incremental_controls = True
        self.incremental_component_violation = "once"
        job, tree = self.run_schedule_component_repair(
            model_calls=3, reflection_strategy="combee"
        )
        node = tree["nodes"][1]
        self.assertEqual(
            node.get("repair_error"), "gepa_component_not_authorized", node
        )
        self.assertEqual(job["status"], "passed", job)
        self.assertEqual(tree["model_calls"], 2)
        self.assertEqual(
            [p.get("phase") for p in self.requests],
            ["combee_map", "repair_component_selection"],
        )
        self.assertEqual(self.requests[1]["components"], self.requests[0]["components"])
        self.assertNotIn("reflection_stages", node)

    # 回归用例：test_incremental_combee_component_repair_respects_group_budget。
    # 重点核验字段或诊断：gepa_model_budget_exhausted；异常路径不能被当成成功。
    def test_incremental_combee_component_repair_respects_group_budget(self):
        self.incremental_controls = True
        self.incremental_component_violation = "once"
        _, tree = self.run_schedule_component_repair(
            model_calls=2, reflection_strategy="combee"
        )
        node = tree["nodes"][1]
        self.assertEqual(
            (node["status"], node["error"]), ("error", "gepa_model_budget_exhausted")
        )
        self.assertNotIn("repair_error", node)
        self.assertEqual(tree["model_calls"], 0)
        self.assertEqual(self.requests, [])

    # 测试辅助函数：exercise_reviewed_control_routing。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def exercise_reviewed_control_routing(
        self, waste_suffix=False, selector="round_robin"
    ):
        import io

        self.behavior = "empty_change"
        self.config["runtime_options"]["aggressive_controls"] = True
        self.config.update(
            max_level="L3",
            evaluation_levels=["L3"],
            control_learning=dict(
                enabled=True,
                prompt_file="prompts/gepa-reflection.txt",
                max_rules=2,
                min_iterations=2,
                min_no_progress=1,
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        # 沿用真实重复 guard；额外循环属于独立 actor，不能被同分支恢复联选混淆。
        # 仅包装本测试的内存源码，不修改宿主文件或其他用例的行为。
        marker = 'for followup in request["task"]["followup_inputs"]:'
        # 路由回归只需少量独立动作；旧 fixture 的 20 条噪声另有专门的截断回归。
        # 只定位该噪声动作之前的循环，不依赖调用参数是否被格式化成多行。
        noise_marker = 'name="independent",'
        extra = """
for routing_iteration in range(1, 5):
    routing_control = evo.invoke("loop.check", dict(run_id=rid, loop_id="routing-loop",
        actor_id="routing-worker", state={"revision": routing_iteration}))
    evo.invoke("loop.commit", dict(run_id=rid, control_id=routing_control["control_id"],
        decision=routing_control["decision"]))
    # 测试宿主也必须遵守实际循环决策；终止后不能继续在已关闭的循环内执行模型。
    if routing_control["decision"] == "stop":
        break
    routing_call = evo.invoke("activity.begin", dict(run_id=rid, kind="model", name="routing-model",
        actor_id="routing-worker", control_id=routing_control["control_id"], input=model_input,
        binding={"pointer": "/messages", "mode": "system_message"}))
    evo.invoke("activity.end", dict(run_id=rid, activity_id=routing_call["activity_id"],
        status="completed", output={"revision": routing_iteration}, actual_input=routing_call["input"],
        executed=True, metrics={"tokens": 1}))
"""
        wrapper = (
            "import sys\nfrom pathlib import Path\n"
            "host = Path(sys.argv.pop(1)); source = host.read_text()\n"
            f"marker = {marker!r}\nassert source.count(marker) == 1\n"
            f"noise_marker = {noise_marker!r}\nassert source.count(noise_marker) == 1\n"
            "noise_start = source.rfind('    for i in range(20):', 0, source.index(noise_marker))\n"
            "assert noise_start >= 0\n"
            "source = source[:noise_start] + source[noise_start:].replace('range(20)', 'range(2)', 1)\n"
            f"exec(compile(source.replace(marker, {extra!r} + marker), str(host), 'exec'), "
            "{'__file__': str(host), '__name__': '__main__'})\n"
        )
        self.evaluation["variants"][0]["command"] = [
            fixture.sys.executable,
            "-B",
            "-c",
            wrapper,
            str(fixture.SOURCE / "tests/agent_evaluation_host.py"),
            "gepa_repeat_guard",
            str(fixture.BUILD / "libtrajectory_evolution_local.so"),
        ]
        self.evaluation["checks"] = {}
        self.evaluation["judge"] = dict(
            enabled=True,
            runtime_config="model.json",
            prompt="prompts/task-judge-system.txt",
            loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100,
            max_input_bytes=1048576,
        )
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        rounds = 1 if selector == "all" else 3
        config.update(
            component_selector=selector,
            max_rounds=rounds,
            max_model_calls=rounds,
            proposals_per_round=1,
            max_input_bytes=32768,
        )
        fixture.write(self.root / "gepa.json", config)
        original_handler = self.server.RequestHandlerClass

        class ReviewedModel(original_handler):
            # 测试辅助函数：do_POST。
            # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
            def do_POST(handler):
                body = handler.rfile.read(int(handler.headers["Content-Length"]))
                packet = json.loads(json.loads(body)["messages"][-1]["content"])
                if "loop_inventory" not in packet:
                    stream = handler.rfile
                    handler.rfile = io.BytesIO(body)
                    try:
                        return original_handler.do_POST(handler)
                    finally:
                        handler.rfile = stream
                reviews = []
                for key, item in packet["loop_inventory"].items():
                    iteration = int(item["iteration"])
                    waste = item["loop_id"] == "routing-loop" and (
                        iteration >= 3 if waste_suffix else iteration == 2
                    )
                    reviews.append(
                        dict(
                            iteration_id=key,
                            decision="waste" if waste else "keep",
                            reason="该轮无新增成果。"
                            if waste
                            else "该轮提供必要结果。",
                            evidence_event_ids=[
                                item.get("anchor_event_id", item["event_ids"][0])
                            ],
                        )
                    )
                judgment = dict(
                    verdict="passed",
                    reason="测试宿主按约定完成任务。",
                    evidence_paths=["/output"],
                    loop_review=dict(status="complete", iteration_reviews=reviews),
                )
                response = json.dumps(
                    dict(
                        choices=[
                            dict(
                                finish_reason="stop",
                                message=dict(
                                    content=json.dumps(judgment, ensure_ascii=False)
                                ),
                            )
                        ]
                    )
                ).encode()
                handler.send_response(200)
                handler.send_header("Content-Type", "application/json")
                handler.send_header("Content-Length", str(len(response)))
                handler.end_headers()
                handler.wfile.write(response)

        self.server.RequestHandlerClass = ReviewedModel
        self.start()
        job_id, _, _ = self.wait_search("L3")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["model_calls"], rounds, tree)
        self.assertEqual(len(self.requests), rounds)
        for node in tree["nodes"][1:]:
            if waste_suffix and node["components"] == ["loop_rules"]:
                # 明确的连续浪费证据允许程序生成受限探针，不再是纯重复草稿。
                self.assertEqual(node["generation_method"], "reviewed_control_probe")
                self.assertTrue(node["control_preflight"]["passed"])
                self.assertEqual(node["status"], "accepted", node)
                self.assertTrue(node["screening"]["applied_changes"], node)
            else:
                self.assertEqual(node["status"], "duplicate", node)
        return tree

    # 回归用例：test_round_robin_reuses_explored_action_when_loop_has_no_reviewed_suffix。
    # 重点核验字段或诊断：executable_control_opportunity、prior_operation_rounds；异常路径不能被当成成功。
    def test_round_robin_reuses_explored_action_when_loop_has_no_reviewed_suffix(self):
        tree = self.exercise_reviewed_control_routing()
        self.assertEqual(tree["nodes"][1]["components"], ["action_rules"])
        node = tree["nodes"][3]
        self.assertEqual(
            node["component_fallback"],
            {
                "from": "loop_rules",
                "to": "action_rules",
                "reason": "executable_control_opportunity",
            },
        )
        self.assertEqual(node["components"], ["action_rules"])
        self.assertGreaterEqual(tree["component_selections"]["action_rules"], 2)
        packet = self.requests[-1]
        self.assertEqual(packet["selected_components"], ["action_rules"])
        self.assert_selected_reflection_view(packet)
        guards = [
            action
            for row in packet["feedback"]
            for run in row["trial"]["evidence"]["runs"]
            for action in run["actions"]
            if action.get("guard_only")
        ]
        self.assertTrue(
            any(
                action["action_control"].get("prior_operation_rounds", 0) > 0
                and action["action_control"]["decision"] == "allow"
                for action in guards
            ),
            guards,
        )

    # 回归用例：test_round_robin_keeps_reviewed_loop_suffix_despite_action_opportunity。
    # 重点核验字段或诊断：reviewed_waste_after；异常路径不能被当成成功。
    def test_round_robin_keeps_reviewed_loop_suffix_despite_action_opportunity(self):
        tree = self.exercise_reviewed_control_routing(waste_suffix=True)
        self.assertEqual(tree["nodes"][3]["components"], ["loop_rules"])
        self.assertNotIn("component_fallback", tree["nodes"][3])
        packet = self.requests[-1]
        self.assertEqual(packet["selected_components"], ["loop_rules"])
        loops = [
            action
            for row in packet["feedback"]
            for run in row["trial"]["evidence"]["runs"]
            for action in run["actions"]
            if action.get("loop_id") == "routing-loop"
            and action["kind"] == "loop.decision"
        ]
        self.assertTrue(loops)
        self.assertTrue(
            all(action["loop_control"]["reviewed_waste_after"] == 2 for action in loops)
        )
        self.assertTrue(
            any(
                action["iteration_assessment"]["decision"] == "waste"
                for action in loops
            )
        )
        self.assertTrue(
            any(
                action["iteration_assessment"]["decision"] == "keep" for action in loops
            )
        )
        self.assert_selected_reflection_view(packet)

    # 回归用例：test_all_components_does_not_reroute_unreviewed_loop_to_actions。
    # 重点核验字段或诊断：reviewed_waste_after；异常路径不能被当成成功。
    def test_all_components_does_not_reroute_unreviewed_loop_to_actions(self):
        tree = self.exercise_reviewed_control_routing(selector="all")
        node = tree["nodes"][1]
        self.assertEqual(
            set(node["components"]),
            {"action_rules", "loop_rules", "experience:test-experience"},
        )
        self.assertNotIn("component_fallback", node)
        self.assertEqual(
            set(self.requests[0]["selected_components"]), set(node["components"])
        )
        self.assert_selected_reflection_view(self.requests[0])
        loops = [
            action
            for row in self.requests[0]["feedback"]
            for run in row["trial"]["evidence"]["runs"]
            for action in run["actions"]
            if action.get("loop_id") == "routing-loop"
            and action["kind"] == "loop.decision"
        ]
        self.assertTrue(loops)
        for action in loops:
            expected = "waste" if int(action["iteration"]) == 2 else "keep"
            self.assertEqual(action["iteration_assessment"]["decision"], expected)
            self.assertNotIn("reviewed_waste_after", action["loop_control"])

    # 测试辅助函数：exercise_applied_experience_routing。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def exercise_applied_experience_routing(
        self, applied, disabled=False, unmatched_first=False, patience=0
    ):
        self.behavior = "empty_change"
        self.judge_requests = []
        self.config.update(
            max_level="L2",
            evaluation_levels=["L2"],
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        if unmatched_first:
            # 两条经验均可被搜索，但排序更靠前的一条与本题无关、没有应用回执。
            # 复现真实批次已使用其他经验，却因第一条未命中而把 L1 轮次让给控制的漏洞。
            memory_path = self.root / "production/by-user/user-a/memory/evolution-memory.pb"
            options = [
                "-I" + str(fixture.SOURCE / "proto"),
                "-I" + str(Path(fixture.PROTOC).resolve().parent.parent / "include"),
                "hybrid_evaluation.proto",
            ]
            decoded = subprocess.run(
                [fixture.PROTOC, "--decode=agentflow.trajectory.evaluation.EvolutionMemoryStore", *options],
                input=memory_path.read_bytes(), capture_output=True, check=True, timeout=10,
            ).stdout.decode()
            entry = decoded[decoded.index("entries {"):]
            entry = (entry.replace("test-experience", "aaa-unmatched-experience")
                     .replace("test-candidate", "aaa-unmatched-candidate")
                     .replace("test-evidence", "aaa-unmatched-evidence"))
            unrelated = {
                "trigger_pattern": "telescope azimuth calibration",
                "observed_problem": "astronomical telescope coordinate drift",
                "proposed_change": "Check telescope azimuth before calibration.",
            }
            entry = "\n".join(
                line.split(":", 1)[0] + ': "' + unrelated[line.strip().split(":", 1)[0]] + '"'
                if line.strip().split(":", 1)[0] in unrelated else line
                for line in entry.splitlines()
            )
            encoded = subprocess.run(
                [fixture.PROTOC, "--encode=agentflow.trajectory.evaluation.EvolutionMemoryStore", *options],
                input=(decoded + "\n" + entry).encode(),
                capture_output=True, check=True, timeout=10,
            ).stdout
            memory_path.write_bytes(encoded)
        # 首个控制槽的父评测不可用，复现控制尚未探索时轮到 L1 的情况。
        # 第二槽正常产出真实注入回执、完整 waste 评审和重复动作 guard。
        search_root = self.root / "production/agent-evaluation"
        unavailable = (
            "request = json.load(sys.stdin)\n"
            f"for checkpoint in Path({str(search_root)!r}).glob('*/search/tree.json'):\n"
            "    tree = json.loads(checkpoint.read_text())\n"
            "    active = tree.get('active')\n"
            "    if active is not None and request['task']['task_input'].startswith('TRAIN_VISIBLE') "
            "and tree['nodes'][int(active)].get('component_slot') == 0:\n"
            "        raise RuntimeError('fixture_first_training_batch_unavailable')\n"
        )
        wrapper = (
            "import sys\nfrom pathlib import Path\n"
            "host = Path(sys.argv.pop(1)); source = host.read_text()\n"
            "marker = 'request = json.load(sys.stdin)'\nassert source.count(marker) == 1\n"
            f"source = source.replace(marker, {unavailable!r})\n"
        )
        if not applied:
            # 宿主没有采用 prepared input，不能凭查询/生成记录声称经验已应用。
            wrapper += (
                "marker = 'actual_input=call[\"input\"],'\n"
                "assert source.count(marker) == 1\n"
                "source = source.replace(marker, 'actual_input=model_input,')\n"
            )
        wrapper += "exec(compile(source, str(host), 'exec'), {'__file__': str(host), '__name__': '__main__'})\n"
        self.evaluation["variants"][0]["command"] = [
            fixture.sys.executable,
            "-B",
            "-c",
            wrapper,
            str(fixture.SOURCE / "tests/agent_evaluation_host.py"),
            "gepa_repeat_guard",
            str(fixture.BUILD / "libtrajectory_evolution_local.so"),
        ]
        self.evaluation["checks"] = {}
        self.evaluation["judge"] = dict(
            enabled=True,
            runtime_config="model.json",
            prompt="prompts/task-judge-system.txt",
            loop_review_prompt="prompts/task-loop-review.txt",
            max_model_calls=100,
            max_input_bytes=1048576,
        )
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            component_selector="round_robin",
            sampling_strategy="independent",
            proposals_per_round=1,
            max_rounds=2,
            max_model_calls=2,
            patience=patience,
            minibatch_size=1,
            acceptance="improvement_or_equal",
        )
        fixture.write(self.root / "gepa.json", config)
        artifact = None
        if disabled:
            self.config["evaluation_levels"] = []
            fixture.write(self.root / "service.json", self.config)
            request = dict(
                memory_path=str(
                    self.root / "production/by-user/user-a/memory/evolution-memory.pb"
                ),
                identity=dict(
                    agent_id="eval-test",
                    user_id="user-a",
                    conversation_id="test",
                    run_id="compile",
                ),
                shadow_options=dict(
                    self.config["shadow_options"],
                    target_scopes=["agent:eval-test/actor:assistant"],
                ),
                options=dict(
                    requested_level="L2",
                    host_capabilities=["context.request", "action.filter"],
                    allowed_users=["user-a"],
                    allowed_conversations=["test"],
                    fallback="strict",
                    experience_template="{{experience}}",
                    max_context_bytes=4096,
                ),
            )
            compiled = subprocess.run(
                [str(fixture.CLI), "runtime", "compile"],
                input=json.dumps(request),
                capture_output=True,
                text=True,
                timeout=20,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            policy = json.loads(compiled.stdout)
            refs = [
                dict(
                    memory_id=item["memory_id"],
                    content_revision=item["content_revision"],
                )
                for item in policy["snapshot"]["experiences"]
            ]
            compiled = subprocess.run(
                [str(fixture.CLI), "runtime", "recompile"],
                input=json.dumps(
                    dict(
                        policy=policy,
                        options=dict(
                            policy["options"],
                            approved_experiences=refs,
                            disabled_injections=refs,
                        ),
                    )
                ),
                capture_output=True,
                text=True,
                timeout=20,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            artifact = dict(
                schema_version=1,
                agent=self.assessment["agent"],
                user_id="user-a",
                level="L2",
                policy=json.loads(compiled.stdout),
            )
            # 已停用经验没有应用回执；仍应允许在其轮次探索恢复，而非永远被控制槽抢走。
            self.model_draft_content = lambda packet, fallback: (
                json.dumps(
                    dict(
                        reason="对照恢复已批准经验。",
                        experience=dict(
                            memory_id="test-experience",
                            content=packet["experiences"][0]["original_content"],
                        ),
                    )
                )
                if packet["selected_components"] == ["experience:test-experience"]
                else fallback
            )
        self.start()
        if artifact is not None:
            self.call("agent_evaluation.submit", artifact=artifact)
        job_id, _, _ = self.wait_search("L2")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(
            tree["nodes"][1]["error"], "gepa_parent_evaluation_inconclusive", tree
        )
        self.assertEqual(tree["nodes"][2]["component_slot"], 1)
        self.assertTrue(tree["training_opportunities"].get("action_rules"), tree)
        return tree

    # 回归用例：test_applied_wasteful_experience_keeps_its_turn_before_unexplored_control。
    # 关键断言：self.assertEqual(node["components"], ["experience:test-experience"])；此处只验证测试构造的场景。
    def test_applied_wasteful_experience_keeps_its_turn_before_unexplored_control(self):
        tree = self.exercise_applied_experience_routing(applied=True)
        node = tree["nodes"][2]
        self.assertEqual(node["components"], ["experience:test-experience"])
        self.assertNotIn("component_fallback", node)
        self.assertEqual(node["generation_method"], "experience_ablation")
        self.assertEqual(tree["component_selections"].get("action_rules", 0), 0)
        self.assertEqual(tree["model_calls"], 0)
        self.assertTrue(node["screening"]["applied_changes"], node)

    def test_unapplied_experience_prefers_another_observed_experience(self):
        """L1 的轮次先选择已有真实回执的其他经验，不被未探索的控制抢走。"""
        tree = self.exercise_applied_experience_routing(applied=True, unmatched_first=True)
        node = tree["nodes"][2]
        self.assertEqual(node["components"], ["experience:test-experience"])
        self.assertEqual(node["component_fallback"], {
            "from": "experience:aaa-unmatched-experience",
            "to": "experience:test-experience",
            "reason": "experience_not_applied_in_training",
        })
        self.assertEqual(node["generation_method"], "experience_ablation")
        self.assertTrue(node["screening"]["applied_changes"])
        self.assertEqual(tree["component_selections"].get("action_rules", 0), 0)
        self.assertEqual(tree["model_calls"], 0)

    def test_inconclusive_parent_does_not_consume_patience(self):
        """patience=1 时，父评测异常仍允许下一批；总轮数上限不被绕开。"""
        tree = self.exercise_applied_experience_routing(applied=True, patience=1)
        self.assertEqual(tree["rounds"], 2)
        self.assertEqual(len(tree["nodes"]), 3)
        self.assertEqual(tree["nodes"][2]["generation_method"], "experience_ablation")
        self.assertTrue(tree["nodes"][2]["screening"]["applied_changes"])
        self.assertEqual(tree["model_calls"], 0)

    # 回归用例：test_unapplied_experience_still_yields_to_unexplored_control。
    # 重点核验字段或诊断：unexplored_control_opportunity；异常路径不能被当成成功。
    def test_unapplied_experience_still_yields_to_unexplored_control(self):
        tree = self.exercise_applied_experience_routing(applied=False)
        node = tree["nodes"][2]
        self.assertEqual(node["components"], ["action_rules"])
        self.assertEqual(
            node["component_fallback"],
            {
                "from": "experience:test-experience",
                "to": "action_rules",
                "reason": "unexplored_control_opportunity",
            },
        )
        self.assertNotIn("generation_method", node)
        self.assertEqual(tree["model_calls"], 1)

    # 回归用例：test_disabled_experience_keeps_its_restore_turn_despite_control_opportunities。
    # 关键断言：self.assertEqual(node["components"], ["experience:test-experience"])；此处只验证测试构造的场景。
    def test_disabled_experience_keeps_its_restore_turn_despite_control_opportunities(
        self,
    ):
        tree = self.exercise_applied_experience_routing(applied=True, disabled=True)
        node = tree["nodes"][2]
        self.assertEqual(node["components"], ["experience:test-experience"])
        self.assertNotIn("component_fallback", node)
        self.assertEqual(tree["model_calls"], 1)
        self.assertIsNone(self.requests[0]["components"]["experience:test-experience"])
        self.assertTrue(self.requests[0]["experiences"][0]["original_content"])
        versions = list(
            (self.root / "production/agent-evaluation").glob("*/search/versions/2.json")
        )
        self.assertEqual(len(versions), 1)
        restored = json.loads(versions[0].read_text())
        self.assertFalse(restored["policy"]["options"].get("disabled_injections"))
        self.assertTrue(node["screening"]["applied_changes"], node)

    # 回归用例：test_round_robin_replaces_control_without_executable_evidence。
    # 重点核验字段或诊断：no_executable_training_evidence；异常路径不能被当成成功。
    def test_round_robin_replaces_control_without_executable_evidence(self):
        self.config.update(
            max_level="L3",
            control_learning=dict(
                enabled=True,
                prompt_file="prompts/gepa-reflection.txt",
                max_rules=2,
                min_iterations=2,
                min_no_progress=1,
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            component_selector="round_robin",
            max_rounds=1,
            max_model_calls=1,
            proposals_per_round=1,
        )
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, _, _ = self.wait_search("L3")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        node = tree["nodes"][1]
        self.assertEqual(
            node["component_fallback"],
            {
                "from": "action_rules",
                "to": "experience:test-experience",
                "reason": "no_executable_training_evidence",
            },
        )
        self.assertEqual(node["components"], ["experience:test-experience"])
        request = self.requests[-1]
        self.assertEqual(request["selected_components"], ["experience:test-experience"])
        self.assertEqual(set(request["components"]), {"experience:test-experience"})
        self.assertNotIn("controls", request)

    # 回归用例：test_failure_replan_gets_generation_opportunity_and_real_application。
    # 重点核验字段或诊断：max_no_progress、gepa_failure_replan、unexplored_control_opportunity；异常路径不能被当成成功。
    def test_failure_replan_gets_generation_opportunity_and_real_application(self):
        self.behavior = "failure_replan"
        self.config["runtime_options"]["aggressive_controls"] = False
        self.config["runtime_options"]["loop_rules"][0]["max_no_progress"] = 3
        self.config.update(
            max_level="L3",
            evaluation_levels=["L3"],
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_failure_replan"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(component_selector="round_robin", max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L3")
        self.assertEqual(job["status"], "passed", job)
        self.assertEqual(job["gate"]["reasons"], [])
        self.assertFalse(job["gate"]["control_coverage"]["level_complete"])
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        node = tree["nodes"][1]
        self.assertEqual(node["components"], ["loop_rules"])
        self.assertTrue(tree["training_opportunities"]["loop_rules"])
        self.assertEqual(
            node["component_fallback"]["reason"], "unexplored_control_opportunity"
        )
        self.assertTrue(node["control_preflight"]["passed"])
        self.assertGreater(sum(node["control_preflight"]["rules"].values()), 0)
        self.assertGreater(
            sum(r["applied"] for r in node["control_coverage"]["rules"].values()), 0
        )
        loops = [
            a["loop_control"]
            for p in self.requests
            for row in p["feedback"]
            for run in row["trial"]["evidence"]["runs"]
            for a in run["actions"]
            if a["kind"] == "loop.decision"
        ]
        self.assertTrue(
            any(
                q["failed_actions"] > 0 and q["repeated_failure_steps"] > 0
                for q in loops
            )
        )

    # 回归用例：test_control_feedback_budget_preserves_structured_loop_evidence。
    # 重点核验字段或诊断：max_no_progress、gepa_failure_replan、max_input_bytes；异常路径不能被当成成功。
    def test_control_feedback_budget_preserves_structured_loop_evidence(self):
        self.behavior = "failure_replan"
        self.config["runtime_options"]["aggressive_controls"] = True
        self.config["runtime_options"]["loop_rules"][0]["max_no_progress"] = 3
        self.config.update(
            max_level="L3",
            evaluation_levels=["L3"],
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_failure_replan"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            component_selector="round_robin",
            max_rounds=1,
            max_model_calls=1,
            max_input_bytes=16384,
        )
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, _, _ = self.wait_search("L3")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        node = tree["nodes"][1]
        packet = next(
            packet
            for packet in self.requests
            if packet["selected_components"] == ["loop_rules"]
        )
        encoded = json.dumps(packet, ensure_ascii=False, separators=(",", ":")).encode()
        self.assertLessEqual(len(encoded), config["max_input_bytes"])
        self.assertTrue(
            any(
                row["trial"].get("control_evidence_preserved")
                for row in packet["feedback"]
            )
        )
        summary = {row["task_id"]: row for row in packet["training_batch_summary"]}
        for row in packet["feedback"]:
            metrics = row["trial"]["metrics"]
            self.assertEqual(metrics["metric_coverage"], dict(
                effective_quality=True, strict_passed=True, invalid_loop_count=True), row)
            self.assertEqual(metrics["quality"], 0, row)
            self.assertFalse(metrics["strict_passed"], row)
            for field in ("quality", "strict_pass_rate", "strict_passed", "invalid_loops",
                          "invalid_loop_count", "metric_coverage"):
                self.assertEqual(metrics[field], summary[row["task_id"]][field], row)
        loops = [
            action["loop_control"]
            for row in packet["feedback"]
            for run in row["trial"]["evidence"]["runs"]
            for action in run["actions"]
            if action["kind"] == "loop.decision"
        ]
        self.assertTrue(
            any(item["repeated_failure_steps"] > 0 for item in loops), loops
        )
        self.assertTrue(node["control_preflight"]["passed"], node)
        self.assertGreater(sum(node["control_preflight"]["rules"].values()), 0)

    # 测试辅助函数：schedule_dataset。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def schedule_dataset(self, dependent_ids, reviewed=True):
        tasks = [
            json.loads(line)
            for line in (self.root / "gepa-tasks.jsonl").read_text().splitlines()
        ]
        for task in tasks:
            prefix = task["task_input"].split("：", 1)[0]
            if task["task_id"] in dependent_ids:
                task["task_input"] = (
                    f"{prefix}：生成视频时缺少图片素材，先准备素材，再生成图像；独立计算继续。"
                )
                if reviewed:
                    task["attributes"] = dict(
                        dependency="prepare_before_image", stop_on_failure=True
                    )
            else:
                task["task_input"] = (
                    f"{prefix}：生成视频时缺少图片素材；准备和图像生成互不依赖。"
                )
        (self.root / "gepa-tasks.jsonl").write_text(
            "\n".join(json.dumps(task, ensure_ascii=False) for task in tasks),
            encoding="utf-8",
        )
        result = subprocess.run(
            [
                str(fixture.CLI),
                "dataset",
                "build",
                "--config",
                str(self.root / "gepa-dataset.json"),
                "--output-root",
                str(self.root / "dependency-dataset"),
            ],
            capture_output=True,
            text=True,
            timeout=20,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assessment["dataset"] = "dependency-dataset"
        fixture.write(self.root / "agent-evaluation.json", self.assessment)

    # 回归用例：test_schedule_component_generates_and_really_orders_skills。
    # 重点核验字段或诊断：schedule_host_mode；异常路径不能被当成成功。
    def test_schedule_component_generates_and_really_orders_skills(self):
        self.schedule_dataset(
            {"train-a", "train-b", "val-a", "val-b", "holdout-a", "holdout-b"}
        )
        self.behavior = "schedule"
        self.config.update(
            max_level="L2",
            evaluation_levels=["L2"],
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
            instrumentation=dict(
                level="L2", capabilities=["model", "actions", "schedule"]
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = getattr(
            self, "schedule_host_mode", "gepa_schedule"
        )
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
        rules = [
            rule
            for rule in node["control_coverage"]["rules"].values()
            if rule["boundary"] == "schedule"
        ]
        self.assertTrue(rules)
        self.assertGreater(sum(rule["applied"] for rule in rules), 0)
        plans = [
            plan
            for packet in self.requests
            for row in packet["feedback"]
            for run in row["trial"]["evidence"]["runs"]
            for plan in run.get("schedule_plans", [])
        ]
        self.assertTrue(plans)
        self.assertNotIn("HOLDOUT_PRIVATE", json.dumps(self.requests))
        self.assertNotIn("VALIDATION_PRIVATE", json.dumps(self.requests))

    # 回归用例：test_schedule_evidence_survives_earlier_single_action_batches。
    # 重点核验字段或诊断：gepa_schedule_late；异常路径不能被当成成功。
    def test_schedule_evidence_survives_earlier_single_action_batches(self):
        self.schedule_host_mode = "gepa_schedule_late"
        self.test_schedule_component_generates_and_really_orders_skills()
        plans = [
            plan
            for packet in self.requests
            for row in packet["feedback"]
            for run in row["trial"]["evidence"]["runs"]
            for plan in run.get("schedule_plans", [])
        ]
        self.assertTrue(any(len(plan.get("actions", [])) > 1 for plan in plans), plans)

    # 回归用例：test_schedule_training_prefers_reviewed_dependency_over_parallel_batch。
    # 关键断言：self.assertEqual(job["status"], "passed", job)；此处只验证测试构造的场景。
    def test_schedule_training_prefers_reviewed_dependency_over_parallel_batch(self):
        self.schedule_dataset({"train-b", "val-b", "holdout-b"})
        self.behavior = "schedule"
        self.config.update(
            max_level="L2",
            evaluation_levels=["L2"],
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
            instrumentation=dict(
                level="L2", capabilities=["model", "actions", "schedule"]
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_schedule"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            component_selector="round_robin",
            minibatch_size=1,
            max_rounds=1,
            max_model_calls=1,
        )
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertEqual(job["status"], "passed", job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["nodes"][1]["tasks"], ["train-b"])
        self.assertEqual(self.requests[0]["feedback"][0]["task_id"], "train-b")
        self.assertTrue(tree["nodes"][1]["control_preflight"]["passed"])
        reports = [
            entry
            for entry in tree["evaluations"].values()
            if entry["split"] == "val" and entry["version"] == job["version"]
        ]
        self.assertEqual(len(reports), 1)
        report = json.loads(
            (
                self.root
                / "production/agent-evaluation"
                / job_id
                / "search"
                / reports[0]["report"]
            ).read_text()
        )
        statuses = {
            trial["task_id"]: [
                event["attributes"]["decision"]["schedule"]["status"]
                for event in trial["evidence"]["trace"]["events"]
                if event.get("attributes", {}).get("type") == "policy.query"
                and event["attributes"]["decision"]["query"].get("boundary")
                == "schedule"
            ]
            for trial in report["trials"]
        }
        self.assertEqual(statuses, {"val-a": ["unchanged"], "val-b": ["scheduled"]})

    # 回归用例：test_schedule_guard_repair_uses_only_observed_training_text。
    # 重点核验字段或诊断：schedule_guard_repair；异常路径不能被当成成功。
    def test_schedule_guard_repair_uses_only_observed_training_text(self):
        self.schedule_dataset({"train-b", "val-b", "holdout-b"})
        self.behavior = "schedule_overstrict"
        self.config.update(
            max_level="L2",
            evaluation_levels=["L2"],
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
            instrumentation=dict(
                level="L2", capabilities=["model", "actions", "schedule"]
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_schedule"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            component_selector="round_robin",
            minibatch_size=1,
            max_rounds=1,
            max_model_calls=1,
        )
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertEqual(job["status"], "passed", job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        node = tree["nodes"][1]
        self.assertTrue(node["control_preflight"]["passed"], node)
        self.assertEqual(node["schedule_guard_repair"][0]["removed_terms"], 1)
        self.assertEqual(tree["model_calls"], 1)
        reports = [
            entry
            for entry in tree["evaluations"].values()
            if entry["split"] == "val" and entry["version"] == job["version"]
        ]
        report = json.loads(
            (
                self.root
                / "production/agent-evaluation"
                / job_id
                / "search"
                / reports[0]["report"]
            ).read_text()
        )
        statuses = {
            trial["task_id"]: [
                event["attributes"]["decision"]["schedule"]["status"]
                for event in trial["evidence"]["trace"]["events"]
                if event.get("attributes", {}).get("type") == "policy.query"
                and event["attributes"]["decision"]["query"].get("boundary")
                == "schedule"
            ]
            for trial in report["trials"]
        }
        self.assertEqual(statuses, {"val-a": ["unchanged"], "val-b": ["scheduled"]})

    # 回归用例：test_incremental_schedule_rule_reaches_success_contract_repair。
    # 关键断言：self.assertEqual(len(self.model_drafts), 1)；此处只验证测试构造的场景。
    def test_incremental_schedule_rule_reaches_success_contract_repair(self):
        self.incremental_controls = True
        self.test_schedule_success_contract_repairs_the_draft_without_another_model_call()
        self.assertEqual(len(self.model_drafts), 1)
        draft = self.model_drafts[0]["draft"]
        self.assertNotIn("controls", draft)
        (update,) = draft["control_updates"]
        self.assertEqual((update["component"], update["op"]), ("schedule_rules", "add"))
        self.assertFalse(update["value"]["require_success"])
        job_id, _, _ = self.wait_search("L2")
        node = self.call("agent_evaluation.tree", job_id=job_id)["nodes"][1]
        self.assertTrue(node["control_preflight"]["passed"], node)
        rules = [
            row
            for row in node["control_coverage"]["rules"].values()
            if row["boundary"] == "schedule"
        ]
        self.assertTrue(rules)
        self.assertGreater(sum(row["applied"] for row in rules), 0)

    # 回归用例：test_incremental_schedule_rule_reaches_observed_guard_repair。
    # 关键断言：self.assertEqual(len(self.model_drafts), 1)；此处只验证测试构造的场景。
    def test_incremental_schedule_rule_reaches_observed_guard_repair(self):
        self.incremental_controls = True
        self.test_schedule_guard_repair_uses_only_observed_training_text()
        self.assertEqual(len(self.model_drafts), 1)
        draft = self.model_drafts[0]["draft"]
        self.assertNotIn("controls", draft)
        (update,) = draft["control_updates"]
        self.assertEqual((update["component"], update["op"]), ("schedule_rules", "add"))
        self.assertIn("生成短视频", update["value"]["task_terms"])

    # 回归用例：test_schedule_training_prefers_failed_case_without_dependency_annotation。
    # 关键断言：self.assertEqual(job["status"], "passed", job)；此处只验证测试构造的场景。
    def test_schedule_training_prefers_failed_case_without_dependency_annotation(self):
        self.schedule_dataset({"train-a", "val-a", "holdout-a"}, reviewed=False)
        self.behavior = "schedule"
        self.config.update(
            max_level="L2",
            evaluation_levels=["L2"],
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
            instrumentation=dict(
                level="L2", capabilities=["model", "actions", "schedule"]
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_schedule"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            component_selector="round_robin",
            minibatch_size=1,
            max_rounds=1,
            max_model_calls=1,
        )
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertEqual(job["status"], "passed", job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["nodes"][1]["tasks"], ["train-a"])
        self.assertEqual(self.requests[0]["feedback"][0]["task_id"], "train-a")

    # 回归用例：test_schedule_reflection_skips_independent_case_without_annotation。
    # 关键断言：self.assertEqual(job["status"], "passed", job)；此处只验证测试构造的场景。
    def test_schedule_reflection_skips_independent_case_without_annotation(self):
        self.schedule_dataset({"train-b", "val-b", "holdout-b"}, reviewed=False)
        self.behavior = "schedule"
        self.config.update(
            max_level="L2",
            evaluation_levels=["L2"],
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
            instrumentation=dict(
                level="L2", capabilities=["model", "actions", "schedule"]
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_schedule"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            component_selector="round_robin",
            minibatch_size=2,
            max_rounds=1,
            max_model_calls=1,
        )
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertEqual(job["status"], "passed", job)
        # 编排证据决定排序，不把独立任务的反例隐藏掉，避免规则错误扩展到整个批次。
        self.assertEqual(
            [row["task_id"] for row in self.requests[0]["feedback"]], ["train-b", "train-a"]
        )

    # 回归用例：test_schedule_reflection_excludes_independent_batch_when_dependency_exists。
    # 关键断言：self.assertEqual(job["status"], "passed", job)；此处只验证测试构造的场景。
    def test_schedule_reflection_excludes_independent_batch_when_dependency_exists(
        self,
    ):
        self.schedule_dataset({"train-b", "val-b", "holdout-b"})
        self.behavior = "schedule"
        self.config.update(
            max_level="L2",
            evaluation_levels=["L2"],
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
            instrumentation=dict(
                level="L2", capabilities=["model", "actions", "schedule"]
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_schedule"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            component_selector="round_robin",
            minibatch_size=2,
            max_rounds=1,
            max_model_calls=1,
        )
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, job, _ = self.wait_search("L2")
        self.assertEqual(job["status"], "passed", job)
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["nodes"][1]["tasks"], ["train-a", "train-b"])
        # 即使有人工依赖标注，仍保留独立样本；有依赖的样本只获得优先展示。
        self.assertEqual(
            [row["task_id"] for row in self.requests[0]["feedback"]], ["train-b", "train-a"]
        )

    # 回归用例：test_control_source_approval_survives_candidate_composition。
    # 关键断言：self.assertEqual(decoded.returncode, 0, decoded.stderr)；此处只验证测试构造的场景。
    def test_control_source_approval_survives_candidate_composition(self):
        path = self.root / "production/by-user/user-a/memory/evolution-memory.pb"
        protoc_args = [
            fixture.PROTOC,
            "-I" + str(fixture.SOURCE / "proto"),
            "-I" + str(Path(fixture.PROTOC).resolve().parent.parent / "include"),
            "hybrid_evaluation.proto",
        ]
        decoded = subprocess.run(
            protoc_args
            + ["--decode=agentflow.trajectory.evaluation.EvolutionMemoryStore"],
            input=path.read_bytes(),
            capture_output=True,
            timeout=10,
        )
        self.assertEqual(decoded.returncode, 0, decoded.stderr)
        control = """
entries { memory_id: "test-control" status: EVOLUTION_MEMORY_STATUS_CANDIDATE
 candidate { schema_version: 1 candidate_id: "control-candidate" generator_version: "scoped-experience-generator-v1"
 target_type: CANDIDATE_TARGET_TYPE_SKILL_ORCHESTRATION target_scope: "agent:eval-test/actor:assistant"
 status: EVOLUTION_CANDIDATE_STATUS_PROPOSED trigger_pattern: "图像技能依赖准备结果"
 observed_problem: "准备尚未完成就启动图像分支" proposed_change: "准备成功后再启动图像分支。"
 support_user_count: 1 support_evidence { evidence_id: "control-evidence"
 source { user_id: "user-a" dialogue_id: "control-run" } } }}
"""
        encoded = subprocess.run(
            protoc_args
            + ["--encode=agentflow.trajectory.evaluation.EvolutionMemoryStore"],
            input=decoded.stdout + control.encode(),
            capture_output=True,
            timeout=10,
        )
        self.assertEqual(encoded.returncode, 0, encoded.stderr)
        path.write_bytes(encoded.stdout)
        self.control_source_id = "test-control"
        self.test_schedule_component_generates_and_really_orders_skills()
        job_id, job, _ = self.wait_search("L2")
        folder = self.root / "production/agent-evaluation" / job_id
        candidate = json.loads((folder / "candidate.json").read_text())
        options = candidate["policy"]["options"]
        self.assertIn(
            "test-control",
            [item["memory_id"] for item in options["approved_experiences"]],
        )
        self.assertTrue(
            any(
                rule["source_experience"]["memory_id"] == "test-control"
                for rule in options["schedule_rules"]
            )
        )
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertNotIn("experience:test-control", tree["available_components"])
        self.assertTrue(
            any(
                any(
                    source["memory_id"] == "test-control"
                    for source in packet.get("controls", {}).get("control_sources", [])
                )
                for packet in self.requests
            )
        )
        self.assertTrue(job["published"], job)

    # 回归用例：test_l3_publication_requires_actual_experience_schedule_and_loop_application。
    # 重点核验字段或诊断：agent_version_id、evaluation_trial_id；异常路径不能被当成成功。
    def test_l3_publication_requires_actual_experience_schedule_and_loop_application(
        self,
    ):
        self.behavior = "full_controls"
        self.config["runtime_options"]["aggressive_controls"] = True
        self.config.update(
            max_level="L3",
            evaluation_levels=["L3"],
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
            instrumentation=dict(
                level="L3", capabilities=["model", "actions", "schedule", "loop"]
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        self.evaluation["variants"][0]["command"][-2] = "gepa_full"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            component_selector="model_choice", max_rounds=1, max_model_calls=1
        )
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
        candidate = json.loads(
            (
                self.root / "production/agent-evaluation" / job_id / "candidate.json"
            ).read_text()
        )
        self.assertTrue(candidate["policy"]["options"]["aggressive_controls"])
        for component in ("schedule_rules", "loop_rules"):
            self.assertTrue(
                any(
                    "source_experience" in rule
                    for rule in candidate["policy"]["options"][component]
                )
            )
        self.assertTrue(status["publications"])
        self.assertNotIn("HOLDOUT_PRIVATE", json.dumps(self.requests))
        self.assertNotIn("VALIDATION_PRIVATE", json.dumps(self.requests))

        # 测试辅助函数：deployed_run。
        # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
        def deployed_run(policy_gate, conversation):
            run = self.call(
                "run.begin",
                conversation_id=conversation,
                input="生成视频时缺少图片素材，核验准备步骤。",
                level="L3",
                capabilities=["model", "actions", "schedule", "loop"],
            )
            self.assertEqual(run.get("agent_version_id"), job["version"], run)
            self.assertEqual(run.get("policy_gate"), policy_gate, run)
            rid = run["run_id"]
            control = self.call(
                "loop.check",
                run_id=rid,
                loop_id="production-worker",
                state={"phase": 1},
            )
            self.call(
                "loop.commit",
                run_id=rid,
                control_id=control["control_id"],
                decision=control["decision"],
            )
            original = {
                "messages": [
                    {
                        "role": "user",
                        "content": "生成视频时缺少图片素材，核验准备步骤。",
                    }
                ]
            }
            model = self.call(
                "activity.begin",
                run_id=rid,
                kind="model",
                name="production-model",
                input=original,
                binding={"pointer": "/messages", "mode": "system_message"},
                control_id=control["control_id"],
            )
            self.assertNotEqual(
                model["input"], original, "published L1 must reach the model input"
            )
            self.call(
                "activity.end",
                run_id=rid,
                activity_id=model["activity_id"],
                status="completed",
                output="check inputs",
                actual_input=model["input"],
                executed=True,
            )
            action = self.call(
                "activity.begin",
                run_id=rid,
                kind="action",
                name="calculate",
                input={"value": 1},
                control_id=control["control_id"],
            )
            self.call(
                "activity.end",
                run_id=rid,
                activity_id=action["activity_id"],
                status="completed",
                output={"success": False},
                executed=True,
            )
            first_failure = self.call(
                "loop.check",
                run_id=rid,
                loop_id="production-worker",
                state={"phase": 2},
            )
            self.assertEqual(first_failure["decision"], "continue", first_failure)
            self.call(
                "loop.commit",
                run_id=rid,
                control_id=first_failure["control_id"],
                decision="continue",
            )
            retry = self.call(
                "activity.begin",
                run_id=rid,
                kind="action",
                name="calculate",
                input={"value": 2},
                control_id=first_failure["control_id"],
            )
            self.call(
                "activity.end",
                run_id=rid,
                activity_id=retry["activity_id"],
                status="completed",
                output={"success": False},
                executed=True,
            )
            recovery = self.call(
                "loop.check",
                run_id=rid,
                loop_id="production-worker",
                state={"phase": 3},
            )
            self.assertEqual(recovery["decision"], "replan", recovery)
            self.call(
                "loop.commit",
                run_id=rid,
                control_id=recovery["control_id"],
                decision="replan",
            )
            model = self.call(
                "activity.begin",
                run_id=rid,
                kind="model",
                name="production-recovery",
                input=original,
                binding={"pointer": "/messages", "mode": "system_message"},
                control_id=recovery["control_id"],
            )
            self.assertTrue(
                any(
                    recovery["context"] in m.get("content", "")
                    for m in model["input"]["messages"]
                )
            )
            self.call(
                "activity.end",
                run_id=rid,
                activity_id=model["activity_id"],
                status="completed",
                output="recover",
                actual_input=model["input"],
                executed=True,
            )
            schedule = self.call(
                "activity.begin",
                run_id=rid,
                kind="schedule",
                name="production-skills",
                input={
                    "actions": [
                        {"action_id": "image", "operation": "image", "arguments": {}},
                        {
                            "action_id": "prepare",
                            "operation": "prepare",
                            "arguments": {},
                        },
                    ]
                },
            )
            view = schedule
            for expected in ("prepare", "image"):
                self.assertEqual(
                    [item["action_id"] for item in view["ready"]], [expected]
                )
                self.call(
                    "schedule.observe",
                    run_id=rid,
                    activity_id=schedule["activity_id"],
                    input={"action_id": expected, "status": "running"},
                )
                view = self.call(
                    "schedule.observe",
                    run_id=rid,
                    activity_id=schedule["activity_id"],
                    input={
                        "action_id": expected,
                        "status": "succeeded",
                        "output": "done",
                    },
                )
            self.assertTrue(view["finished"])
            self.call(
                "activity.end",
                run_id=rid,
                activity_id=schedule["activity_id"],
                status="completed",
                output="done",
            )
            self.call("run.end", run_id=rid, status="completed", output="done")
            fixture.AgentEvaluationTests.tearDown(self)
            traces = list(Path(self.config["data_dir"]).rglob(rid + ".trace.json"))
            self.assertEqual(len(traces), 1)
            trace = json.loads(traces[0].read_text())
            applied = {
                event["attributes"]["receipt"]["boundary"]
                for event in trace["events"]
                if event["kind"] == "advice.applied"
                and event["application"]["status"] == "applied"
            }
            self.assertTrue({"llm", "schedule", "loop"} <= applied, applied)
            self.assertNotIn("evaluation_trial_id", traces[0].read_text())

        # 发布候选还必须在评测宿主之外可用。
        deployed_run("published", "production")
        count = len(self.requests)
        deployment = self.root / "standalone"
        deployment.mkdir()
        bundle = (
            self.root
            / "production/agent-evaluation"
            / job_id
            / "search/published-version.json"
        )
        shutil.copyfile(bundle, deployment / "policy.json")
        self.config = dict(
            schema_version=1,
            agent_id="eval-test",
            actors=["assistant"],
            principals=self.config["principals"],
            port=0,
            max_level="L3",
            auto_evaluate=False,
            data_dir=str(deployment / "data"),
            policy_version_file=str(deployment / "policy.json"),
        )
        fixture.write(self.root / "service.json", self.config)
        for conversation in ("offline-first", "offline-restarted"):
            self.start()
            deployed_run("offline_version", conversation)
        self.assertEqual(
            len(self.requests), count, "deployment must not call the reflection model"
        )
        self.assertFalse(list(deployment.rglob("*memory*.pb")))
        self.assertFalse((deployment / "data/agent-evaluation").exists())

    # 回归用例：test_round_robin_handles_summarized_training_feedback。
    # 重点核验字段或诊断：no_executable_training_evidence；异常路径不能被当成成功。
    def test_round_robin_handles_summarized_training_feedback(self):
        self.behavior = "no_change"
        self.config.update(
            max_level="L3",
            evaluation_levels=["L3"],
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(
            component_selector="round_robin",
            max_rounds=1,
            max_model_calls=1,
            max_input_bytes=8192,
            unknown_token_policy="worst_case",
        )
        fixture.write(self.root / "gepa.json", config)
        tasks = [
            json.loads(line)
            for line in (self.root / "gepa-tasks.jsonl").read_text().splitlines()
        ]
        for task in tasks:
            task["followup_inputs"] = ["再次核验输入。"] * 20
        (self.root / "gepa-tasks.jsonl").write_text(
            "\n".join(json.dumps(task, ensure_ascii=False) for task in tasks),
            encoding="utf-8",
        )
        result = subprocess.run(
            [
                str(fixture.CLI),
                "dataset",
                "build",
                "--config",
                str(self.root / "gepa-dataset.json"),
                "--output-root",
                str(self.root / "gepa-long-dataset"),
            ],
            capture_output=True,
            text=True,
            timeout=20,
        )
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
        # 控制回退后重新构建较小经验输入包；
        # 可以完整保留一行并省略另一行，而不是随意摘要。
        self.assertTrue(
            len(packet["feedback"]) < config["minibatch_size"]
            or any(row["trial"].get("evidence_truncated") for row in packet["feedback"])
        )
        self.assertLessEqual(
            len(json.dumps(packet, ensure_ascii=False, separators=(",", ":")).encode()),
            8192,
        )
        self.assertEqual(
            node["component_fallback"]["reason"], "no_executable_training_evidence"
        )

    # 回归用例：test_untriggerable_rule_is_stopped_before_running_candidate。
    # 重点核验字段或诊断：gepa_failure_replan、no_differential_training_trigger；异常路径不能被当成成功。
    def test_untriggerable_rule_is_stopped_before_running_candidate(self):
        self.behavior = "untriggerable_loop"
        self.config["runtime_options"]["aggressive_controls"] = True
        self.config.update(
            max_level="L3",
            evaluation_levels=["L3"],
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
        )
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

    # 测试辅助函数：run_unused_rule_removal。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def run_unused_rule_removal(self, mixed_experience=False):
        # 用真实 memory revision 编译父规则；它的任务条件在整个 fixture 中从未命中。
        scope = "agent:eval-test/actor:assistant"
        shadow = dict(self.config["shadow_options"], target_scopes=[scope])
        request = dict(
            memory_path=str(
                self.root / "production/by-user/user-a/memory/evolution-memory.pb"
            ),
            identity=dict(
                agent_id="eval-test",
                user_id="user-a",
                conversation_id="removal",
                run_id="seed",
            ),
            options=shadow,
        )
        result = subprocess.run(
            [str(fixture.CLI), "shadow", "compile"],
            input=json.dumps(request),
            capture_output=True,
            text=True,
            timeout=20,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        (source,) = json.loads(result.stdout)["experiences"]
        self.config["runtime_options"]["schedule_rules"] = [
            dict(
                rule_id="unused-learned-order",
                target_scope=scope,
                before_operation="prepare",
                after_operation="image",
                task_terms=["NEVER_MATCHED_RESOURCE_WORKFLOW"],
                require_success=True,
                reason="只适用于另一个明确任务条件。",
                source_experience=dict(
                    memory_id=source["memory_id"],
                    content_revision=source["content_revision"],
                ),
            )
        ]
        self.config.update(
            max_level="L2",
            evaluation_levels=["L2"],
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
            instrumentation=dict(
                level="L2", capabilities=["model", "actions", "schedule"]
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(component_selector="all", max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.behavior = "empty_change"
        draft = dict(
            reason="删除当前训练任务从未使用的规则。",
            control_updates=[dict(component="schedule_rules", op="remove", index=0)],
        )
        if mixed_experience:
            draft["experience"] = dict(
                memory_id="test-experience",
                content="缺少素材时核验甲类任务，并核验乙类任务，独立计算继续。",
            )
        self.model_draft_content = lambda packet, content: json.dumps(
            draft, ensure_ascii=False
        )
        self.start()
        job_id, job, _ = self.wait_search("L2")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(tree["model_calls"], 1)
        self.assertEqual(len(tree["nodes"]), 2, tree)
        self.assertEqual(len(self.requests[0]["components"]["schedule_rules"]), 1)
        return job_id, job, tree

    # 回归用例：test_pure_rule_removal_without_training_effect_never_runs_child。
    # 重点核验字段或诊断：no_differential_training_trigger、pure_removal_no_effect、removed_rule_ids；异常路径不能被当成成功。
    def test_pure_rule_removal_without_training_effect_never_runs_child(self):
        _, _, tree = self.run_unused_rule_removal()
        node = tree["nodes"][1]
        self.assertEqual(node["status"], "untriggerable_controls", node)
        self.assertEqual(node["reason"], "no_differential_training_trigger")
        self.assertFalse(node["control_preflight"]["passed"])
        self.assertTrue(node["control_preflight"]["pure_removal_no_effect"])
        self.assertEqual(
            node["control_preflight"]["removed_rule_ids"], ["unused-learned-order"]
        )
        self.assertEqual(node["control_preflight"]["removal_changes"], {})
        self.assertNotIn("version", node)
        self.assertNotIn("screening", node)
        self.assertTrue(tree["evaluations"])
        self.assertTrue(
            all(
                e["version"] in {tree["seed_version"], tree["baseline_version"]}
                for e in tree["evaluations"].values()
            ),
            "pure deletion started an unnecessary child evaluation",
        )

    # 回归用例：test_mixed_l1_rewrite_and_unused_rule_removal_still_runs_child。
    # 重点核验字段或诊断：pure_removal_no_effect；异常路径不能被当成成功。
    def test_mixed_l1_rewrite_and_unused_rule_removal_still_runs_child(self):
        job_id, _, tree = self.run_unused_rule_removal(mixed_experience=True)
        node = tree["nodes"][1]
        self.assertTrue(node["control_preflight"]["passed"], node)
        self.assertNotIn("pure_removal_no_effect", node["control_preflight"])
        self.assertIn("screening", node)
        self.assertTrue(
            any(
                e["version"] == node["version"] and e["split"] == "train"
                for e in tree["evaluations"].values()
            ),
            "mixed experience rewrite never reached the host",
        )
        self.assertTrue(
            any(
                change.get("llm", 0)
                for change in node["screening"]["applied_changes"].values()
            ),
            "mixed rewrite did not alter any actual model input",
        )
        version = json.loads(
            (
                self.root
                / "production/agent-evaluation"
                / job_id
                / "search/versions/1.json"
            ).read_text()
        )
        self.assertFalse(version["policy"]["options"].get("schedule_rules", []))
        self.assertTrue(version["policy"]["options"]["experience_overrides"])

    # 回归用例：assert_selected_reflection_view。
    # 关键断言：self.assertEqual(set(packet["components"]), selected)；此处只验证测试构造的场景。
    def assert_selected_reflection_view(self, packet):
        selected = set(packet["selected_components"])
        self.assertEqual(set(packet["components"]), selected)
        controls = {
            kind
            for kind in ("action", "loop", "schedule")
            if kind + "_rules" in selected
        }
        if controls:
            self.assertIn("controls", packet)
            for kind in ("action", "loop", "schedule"):
                if kind in controls:
                    self.assertIn(kind + "_primitives", packet["controls"])
                else:
                    self.assertNotIn(kind + "_primitives", packet["controls"])
                    self.assertNotIn("base_" + kind + "_rules", packet["controls"])
        else:
            self.assertNotIn("controls", packet)
            self.assertEqual(
                {
                    "experience:" + source["memory_id"]
                    for source in packet["experiences"]
                },
                selected,
            )

    # 回归用例：test_single_schedule_reflection_hides_action_and_loop_primitives。
    # 关键断言：self.assertEqual(len(self.requests), 1)；此处只验证测试构造的场景。
    def test_single_schedule_reflection_hides_action_and_loop_primitives(self):
        self.test_schedule_component_generates_and_really_orders_skills()
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(self.requests[0]["selected_components"], ["schedule_rules"])
        self.assert_selected_reflection_view(self.requests[0])

    # 回归用例：test_single_experience_reflection_hides_all_control_primitives。
    # 关键断言：self.assertTrue(packets)；此处只验证测试构造的场景。
    def test_single_experience_reflection_hides_all_control_primitives(self):
        self.test_round_robin_replaces_control_without_executable_evidence()
        packets = [
            p
            for p in self.requests
            if p["selected_components"] == ["experience:test-experience"]
        ]
        self.assertTrue(packets)
        for packet in packets:
            self.assert_selected_reflection_view(packet)

    # 回归用例：test_combee_map_reduce_each_receive_only_the_selected_primitives。
    # 关键断言：self.assertEqual(len(packets), 6)；此处只验证测试构造的场景。
    def test_combee_map_reduce_each_receive_only_the_selected_primitives(self):
        # 只检查目标 L2 搜索；默认同时启动的 L1 搜索另有三次 COMBEE 请求。
        self.config["evaluation_levels"] = ["L2"]
        self.test_combee_combines_only_the_selected_components()
        packets = [
            packet
            for packet in self.requests
            if packet.get("phase") in ("combee_map", "combee_reduce")
        ]
        self.assertEqual(len(packets), 6)
        self.assertEqual(len(packets), len(self.requests))
        self.assertEqual([packet["phase"] for packet in packets].count("combee_map"), 4)
        self.assertEqual(
            [packet["phase"] for packet in packets].count("combee_reduce"), 2
        )
        for packet in packets:
            self.assert_selected_reflection_view(packet)
            if packet["selected_components"] == ["action_rules"]:
                self.assertTrue(packet["experiences"])
                self.assertTrue(
                    all(source.get("content") for source in packet["experiences"]),
                    "control-only map/reduce lost the actual experience text",
                )

    # 回归用例：test_coupled_action_loop_reflection_hides_schedule_primitives。
    # 重点核验字段或诊断：gepa_failure_replan；异常路径不能被当成成功。
    def test_coupled_action_loop_reflection_hides_schedule_primitives(self):
        self.behavior = "empty_change"
        self.config["runtime_options"]["aggressive_controls"] = True
        self.config.update(
            max_level="L3",
            evaluation_levels=["L3"],
            control_learning=dict(
                enabled=True, prompt_file="prompts/gepa-reflection.txt", max_rules=2
            ),
            instrumentation=dict(
                level="L3", capabilities=["model", "actions", "loop", "schedule"]
            ),
        )
        fixture.write(self.root / "service.json", self.config)
        # 在既有 host 的同一分支追加真实失败，不伪造轨迹、反馈或策略查询。
        # 只在本测试进程内扩展 fixture，原 host 文件与其他测试均保持不变。
        marker = (
            'if mode in ("gepa_failure_replan", "gepa_full"):\n    control = evo.invoke'
        )
        extra = """if mode in ("gepa_failure_replan", "gepa_full"):
    for retry_index in (2, 3):
        retry = evo.invoke("activity.begin", dict(run_id=rid, kind="action", name="calculate",
            input=action_input, **activity_scope))
        evo.invoke("activity.end", dict(run_id=rid, activity_id=retry["activity_id"], status="completed",
            output={"success": False}, executed=True))
    control = evo.invoke"""
        wrapper = (
            "import sys\nfrom pathlib import Path\n"
            "host = Path(sys.argv.pop(1)); source = host.read_text()\n"
            f"marker = {marker!r}\nassert source.count(marker) == 1\n"
            f"exec(compile(source.replace(marker, {extra!r}), str(host), 'exec'), "
            "{'__file__': str(host), '__name__': '__main__'})\n"
        )
        self.evaluation["variants"][0]["command"] = [
            fixture.sys.executable,
            "-B",
            "-c",
            wrapper,
            str(fixture.SOURCE / "tests/agent_evaluation_host.py"),
            "gepa_failure_replan",
            str(fixture.BUILD / "libtrajectory_evolution_local.so"),
        ]
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(component_selector="round_robin", max_rounds=1, max_model_calls=1)
        fixture.write(self.root / "gepa.json", config)
        self.start()
        job_id, _, _ = self.wait_search("L3")
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["nodes"][1]["components"], ["action_rules", "loop_rules"])
        self.assertEqual(len(self.requests), 1)
        self.assert_selected_reflection_view(self.requests[0])

    # 回归用例：test_new_config_rejects_unknown_strategies_and_invalid_limits。
    # 重点核验字段或诊断：candidate_selection_strategy、proposals_per_round、max_merge_invocations；异常路径不能被当成成功。
    def test_new_config_rejects_unknown_strategies_and_invalid_limits(self):
        config = json.loads((self.root / "gepa.json").read_text())
        for field, value in (
            ("candidate_selection_strategy", "unknown"),
            ("proposals_per_round", 0),
            ("max_merge_invocations", 257),
            ("use_merge", "true"),
            ("epsilon", -1),
            ("frontier_type", "everything"),
            ("component_selector", "host_code"),
            ("reflection_strategy", "unknown"),
            ("duplication_factor", 0),
        ):
            with self.subTest(field=field):
                fixture.write(
                    self.root / "invalid-gepa.json", dict(config, **{field: value})
                )
                result = subprocess.run(
                    [
                        str(fixture.BUILD / "test_gepa_config"),
                        str(self.root / "invalid-gepa.json"),
                    ],
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
                self.assertNotEqual(result.returncode, 0)


    def prepare_external_holdout(self, *, wrong_task=False, incomplete=False, unknown_cost=False):
        tasks = [json.loads(line) for line in (self.root / "gepa-tasks.jsonl").read_text().splitlines()]
        tasks = [task for task in tasks if task["split"] == "holdout"]
        if wrong_task:
            tasks[0]["task_input"] += " DIFFERENT PUBLIC TASK"
        (self.root / "external-tasks.jsonl").write_text("\n".join(json.dumps(task) for task in tasks))
        build = json.loads((self.root / "gepa-dataset.json").read_text())
        build["sources"] = [dict(kind="golden", path=str(self.root / "external-tasks.jsonl"))]
        fixture.write(self.root / "external-dataset.json", build)
        done = subprocess.run([str(fixture.CLI), "dataset", "build", "--config",
            str(self.root / "external-dataset.json"), "--output-root", str(self.root / "external-dataset")],
            capture_output=True, text=True, timeout=20)
        self.assertEqual(done.returncode, 0, done.stderr)
        agent = json.loads(json.dumps(self.assessment["agent"]))
        if isinstance(agent.get("appworld_options"), dict):
            agent["appworld_options"].pop("l1_window_mode", None)
            agent["appworld_options"].pop("l1_observed_phase_experiences", None)
            if not agent["appworld_options"]:
                agent.pop("appworld_options")
        artifact = dict(schema_version=1, agent=agent, user_id="user-a", level="L0", policy=None)
        version = subprocess.run([str(fixture.CLI), "artifact-version"], input=json.dumps(artifact),
            capture_output=True, text=True, timeout=10)
        self.assertEqual(version.returncode, 0, version.stderr)
        source_config = json.loads(json.dumps(self.evaluation))
        source_config.update(splits=["holdout"], max_trials=2,
            artifact_retention=dict(retain_report_pb=False, retain_markdown=False, retain_jsonl=False))
        source_config.setdefault("resources", dict(nice=10, address_space_bytes=1073741824, cpu_seconds=300))
        source_config["variants"][0].update(id="candidate", version=version.stdout.strip(), artifact=artifact)
        fixture.write(self.root / "external-evaluation.json", source_config)
        captured = subprocess.run([str(fixture.CLI), "dataset", "run", str(self.root / "external-dataset"),
            "--config", str(self.root / "external-evaluation.json"), "--output-root", str(self.root / "external-capture")],
            capture_output=True, text=True, env=self.env, timeout=30)
        self.assertIn(captured.returncode, (0, 1), captured.stderr)
        self.external_source = self.root / "external-capture/report.json"
        self.external_bytes = self.external_source.read_bytes()
        report = json.loads(self.external_bytes)
        self.assertFalse((self.external_source.parent / "report.pb").exists())
        fixture.write(self.root / "external-result.json", dict(status="partial" if incomplete else "complete",
            host_version=artifact["agent"]["revision"], source_report=str(self.external_source),
            metrics=dict(tokens_complete=not unknown_cost),
            items=[dict(task_id=row["task_id"], tokens_complete=not unknown_cost,
                        execution_costs_complete=not unknown_cost) for row in report["trials"]]))
        config = json.loads((self.root / "gepa.json").read_text())
        config.update(max_rounds=1, max_model_calls=1,
            holdout_baseline_result_file=str(self.root / "external-result.json"),
            holdout_baseline_dataset=str(self.root / "external-dataset"),
            holdout_baseline_evaluation_file=str(self.root / "external-evaluation.json"))
        fixture.write(self.root / "gepa.json", config)
        return report

    def test_external_holdout_reuses_native_json_source_without_reexecution(self):
        original = self.prepare_external_holdout()
        self.start()
        job_id, job, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["holdout_baseline_cache"]["status"], "hit", tree)
        self.assertEqual(self.external_source.read_bytes(), self.external_bytes)
        entry = next(row for row in tree["evaluations"].values()
                     if row["split"] == "holdout" and row.get("baseline_source"))
        self.assertNotIn("report_hash", entry["baseline_source"])
        self.assertNotIn("manifest_hash", entry["baseline_source"])
        self.assertEqual(entry["trials_reserved"], 0)
        paired = json.loads((self.root / "production/agent-evaluation" / job_id / "search" / entry["report"]).read_text())
        baseline = {row["trial_id"]: row for row in paired["trials"] if row["variant_id"] == "baseline"}
        self.assertEqual(set(baseline), {row["trial_id"] for row in original["trials"]})
        for row in original["trials"]:
            projected = baseline[row["trial_id"]]
            for field in ["metrics", "duration_ms", "checks", "verdict", "execution_status"]:
                self.assertEqual(projected[field], row[field])
            for field in ["assessment", "loop_review", "loaded_version"]:
                if field in row["evidence"]:
                    self.assertEqual(projected["evidence"][field], row["evidence"][field])
            self.assertNotIn("trace", projected["evidence"])
            self.assertEqual(projected["evidence"]["source_reference"]["report"], str(self.external_source))
        self.assertEqual(paired["summary"]["reused_baseline_trials"], 2)

    def test_external_holdout_task_difference_refuses_reuse_without_l0_fallback(self):
        self.prepare_external_holdout(wrong_task=True)
        self.start()
        job_id, job, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["holdout_baseline_cache"]["status"], "invalid", tree)
        self.assertEqual(tree["holdout_baseline_cache"]["reason"], "gepa_external_baseline_task_mismatch", tree)
        self.assertFalse(any(row["split"] == "holdout" for row in tree["evaluations"].values()))
        self.assertEqual(self.external_source.read_bytes(), self.external_bytes)

    def test_external_holdout_pending_source_never_executes_l0(self):
        self.prepare_external_holdout(incomplete=True)
        self.start()
        job_id, job, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["holdout_baseline_cache"]["status"], "pending", tree)
        self.assertFalse(any(row["split"] == "holdout" for row in tree["evaluations"].values()))

    def test_external_holdout_unknown_attempt_cost_cannot_pass_resource_gate(self):
        self.prepare_external_holdout(unknown_cost=True)
        self.assessment["gate"]["max_latency_ratio"] = 1000000
        fixture.write(self.root / "agent-evaluation.json", self.assessment)
        self.start()
        job_id, job, _ = self.wait_search()
        self.assertEqual(job["status"], "rejected", job)
        self.assertIn("historical_resource_cost_incomplete", job["gate"]["reasons"])

    def test_report_only_resources_preserve_core_gain_and_unknown_costs(self):
        self.evaluation["variants"][0]["command"][-2] = "gepa_resource_growth"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.prepare_external_holdout(unknown_cost=True)
        search_config = json.loads((self.root / "gepa.json").read_text())
        search_config.update(max_rounds=3, max_model_calls=3)
        fixture.write(self.root / "gepa.json", search_config)
        self.assessment["gate"].update(resources_report_only=True,
            max_latency_ratio=1, max_token_ratio=1, max_llm_call_ratio=1)
        fixture.write(self.root / "agent-evaluation.json", self.assessment)
        self.start()
        job_id, job, _ = self.wait_search()
        self.assertEqual(job["status"], "passed", {
            "gate_reasons": (job.get("gate") or {}).get("reasons"),
            "report_only": (job.get("gate") or {}).get("resources_report_only"),
        })
        self.assertTrue(job["gate"]["resources_report_only"])
        self.assertGreater(job["gate"]["candidate"]["host_reported_metrics"]["tokens"]["mean"],
                           job["gate"]["baseline"]["host_reported_metrics"]["tokens"]["mean"])
        contracts = [r["optimization_contract"] for r in self.requests if "optimization_contract" in r]
        self.assertTrue(contracts)
        self.assertTrue(all(c.get("resources_report_only") is True for c in contracts))
        self.assertTrue(all(not any(k in c for k in ("max_latency_ratio", "max_token_ratio", "max_llm_call_ratio")) for c in contracts))
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        final = next(r for r in tree["evaluations"].values()
                     if r["split"] == "holdout" and r.get("baseline_source"))
        report = json.loads((self.root / "production/agent-evaluation" / job_id / "search" / final["report"]).read_text())
        self.assertFalse(report["summary"]["resource_costs_complete"])
        self.assertEqual(self.external_source.read_bytes(), self.external_bytes)

    def test_default_resources_still_reject_growth(self):
        self.evaluation["variants"][0]["command"][-2] = "gepa_resource_growth"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.prepare_external_holdout()
        self.assessment["gate"].update(max_latency_ratio=1, max_token_ratio=1, max_llm_call_ratio=1)
        fixture.write(self.root / "agent-evaluation.json", self.assessment)
        self.start()
        _, job, _ = self.wait_search()
        self.assertEqual(job["status"], "rejected")
        self.assertFalse(job["gate"]["passed"])
        self.assertNotIn("resources_report_only", job["gate"])

    def test_report_only_resources_still_reject_failed_core_holdout(self):
        self.evaluation["variants"][0]["command"][-2] = "gepa_holdout_failure"
        fixture.write(self.root / "evaluation.json", self.evaluation)
        self.prepare_external_holdout()
        self.assessment["gate"].update(resources_report_only=True,
            max_latency_ratio=1, max_token_ratio=1, max_llm_call_ratio=1)
        fixture.write(self.root / "agent-evaluation.json", self.assessment)
        self.start()
        _, job, _ = self.wait_search()
        self.assertEqual(job["status"], "rejected")
        self.assertFalse(job["gate"]["passed"])
        self.assertTrue(job["gate"]["resources_report_only"])
        self.assertTrue(any(c in job["gate"]["reasons"] for c in ("quality_below_threshold", "quality_regression", "min_pass_rate_gain")))

    def check_l1_mode_baseline_identity(self, mode, other_options=None):
        self.assessment["agent"]["appworld_options"] = dict(other_options or {}, l1_window_mode=mode)
        fixture.write(self.root / "agent-evaluation.json", self.assessment)
        self.prepare_external_holdout()
        self.start()
        job_id, job, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        source_config = json.loads((self.root / "external-evaluation.json").read_text())
        self.assertEqual(tree["baseline_version"], source_config["variants"][0]["version"])
        self.assertEqual(tree["holdout_baseline_cache"]["status"], "hit", tree["holdout_baseline_cache"])
        self.assertEqual(self.external_source.read_bytes(), self.external_bytes)

    def test_external_l0_v1_mode_has_canonical_identity(self):
        self.check_l1_mode_baseline_identity("observed_phase")

    def test_external_l0_v2_mode_has_canonical_identity(self):
        self.check_l1_mode_baseline_identity("observed_phase_v2")

    def test_external_l0_v3_mode_preserves_other_options(self):
        self.check_l1_mode_baseline_identity("observed_phase_v3", {"keep_option": "fixture"})

    def test_external_l0_observed_filter_has_same_canonical_identity(self):
        self.check_l1_mode_baseline_identity("observed_phase_v3", {
            "l1_observed_phase_experiences": [{"memory_id": "fixture-ref", "content_revision": "fixture-revision"}]})

    def test_external_holdout_artifact_difference_refuses_l0_fallback(self):
        self.prepare_external_holdout()
        path = self.root / "external-evaluation.json"
        config = json.loads(path.read_text())
        config["variants"][0]["artifact"]["agent"]["revision"] = "different-real-agent"
        fixture.write(path, config)
        self.start()
        job_id, job, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["holdout_baseline_cache"]["reason"], "gepa_external_baseline_artifact_mismatch")
        self.assertFalse(any(row["split"] == "holdout" for row in tree["evaluations"].values()))
        self.assertEqual(self.external_source.read_bytes(), self.external_bytes)

    def test_external_holdout_resolves_actual_grading_config_when_source_finishes(self):
        self.prepare_external_holdout()
        fallback = self.root / "external-evaluation.json"
        config = json.loads(fallback.read_text())
        actual = self.external_source.parent.with_name(self.external_source.parent.name + ".regrade.config.json")
        fixture.write(actual, config)
        config["timeout_ms"] += 1
        fixture.write(fallback, config)
        self.start()
        job_id, job, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["holdout_baseline_cache"]["status"], "hit", tree["holdout_baseline_cache"])
        self.assertEqual(tree["holdout_baseline_cache"]["source"]["source_evaluation_file"], str(actual))
        self.assertEqual(self.external_source.read_bytes(), self.external_bytes)

    def regrade_external_holdout(self):
        config = json.loads((self.root / "external-evaluation.json").read_text())
        config["parallelism"] = self.evaluation.get("parallelism", 1) + 1
        path = self.root / "external-review.regrade.config.json"
        fixture.write(path, config)
        original = self.external_source
        completed = subprocess.run([
            str(fixture.CLI), "dataset", "run", str(self.root / "external-dataset"),
            "--config", str(path), "--regrade-report", str(original),
            "--output-root", str(self.root / "external-review")],
            capture_output=True, text=True, env=self.env, timeout=30)
        self.assertIn(completed.returncode, (0, 1), completed.stderr)
        self.external_source = self.root / "external-review/report.json"
        self.external_bytes = self.external_source.read_bytes()
        report = json.loads(self.external_bytes)
        self.assertTrue(report["configuration"]["model_only_regrade"])
        self.assertEqual(report["summary"]["regraded_trials"], 2)
        self.assertEqual(report["summary"]["not_started_trials"], 0)
        result = json.loads((self.root / "external-result.json").read_text())
        result["source_report"] = str(self.external_source)
        fixture.write(self.root / "external-result.json", result)
        return report

    def enable_external_judge(self):
        self.judge_requests = []
        self.evaluation["variants"][0]["command"][-2] = "gepa_review"
        self.evaluation["judge"] = dict(
            enabled=True, runtime_config="model.json", assessment_mode="objectives",
            prompt="prompts/task-judge-system.txt",
            loop_review_prompt="prompts/task-loop-review.txt",
            consistency_prompt="prompts/task-consistency.txt",
            max_model_calls=100, max_input_bytes=1048576)
        fixture.write(self.root / "evaluation.json", self.evaluation)

    def test_external_holdout_model_only_review_parallelism_preserves_agent_execution(self):
        self.enable_external_judge()
        original = self.prepare_external_holdout()
        reviewed = self.regrade_external_holdout()
        for before, after in zip(original["trials"], reviewed["trials"]):
            for field in ["trial_id", "duration_ms", "metrics", "execution_status"]:
                self.assertEqual(before[field], after[field])
        self.start()
        job_id, _, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["holdout_baseline_cache"]["status"], "hit", tree["holdout_baseline_cache"])
        self.assertEqual(self.external_source.read_bytes(), self.external_bytes)
        source = next(e for e in tree["evaluations"].values() if e.get("baseline_source"))
        self.assertEqual(source["trials_reserved"], 0)

    def test_external_holdout_capture_parallelism_difference_still_rejected(self):
        self.prepare_external_holdout()
        path = self.root / "external-evaluation.json"
        config = json.loads(path.read_text())
        config["parallelism"] = self.evaluation.get("parallelism", 1) + 1
        fixture.write(path, config)
        self.start()
        job_id, _, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["holdout_baseline_cache"]["reason"], "gepa_external_baseline_execution_mismatch")
        self.assertFalse(any(e["split"] == "holdout" for e in tree["evaluations"].values()))
        self.assertEqual(self.external_source.read_bytes(), self.external_bytes)

    def ordinary_resumed_external_holdout(self):
        self.enable_external_judge()
        self.evaluation["parallelism"] = 2
        fixture.write(self.root / "evaluation.json", self.evaluation)
        original = self.prepare_external_holdout()
        self.evaluation["parallelism"] = 1
        fixture.write(self.root / "evaluation.json", self.evaluation)
        config = json.loads((self.root / "external-evaluation.json").read_text())
        path = self.root / "external-resume.regrade.config.json"
        fixture.write(path, config)
        completed = subprocess.run([
            str(fixture.CLI), "dataset", "run", str(self.root / "external-dataset"),
            "--config", str(path), "--resume-report", str(self.external_source),
            "--output-root", str(self.root / "external-resume")],
            capture_output=True, text=True, env=self.env, timeout=30)
        self.assertIn(completed.returncode, (0, 1), completed.stderr)
        self.external_source = self.root / "external-resume/report.json"
        self.external_bytes = self.external_source.read_bytes()
        report = json.loads(self.external_bytes)
        self.assertNotIn("execution_source", report["configuration"])
        self.assertEqual(report["summary"]["reused_trials"], 2)
        self.assertEqual(report["summary"]["not_started_trials"], 0)
        for before, after in zip(original["trials"], report["trials"]):
            for field in ["trial_id", "duration_ms", "metrics", "execution_status"]:
                self.assertEqual(before[field], after[field])
        result = json.loads((self.root / "external-result.json").read_text())
        result["source_report"] = str(self.external_source)
        fixture.write(self.root / "external-result.json", result)
        return report

    def test_external_holdout_ordinary_resume_checks_saved_parent_execution(self):
        self.ordinary_resumed_external_holdout()
        self.start()
        job_id, _, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["holdout_baseline_cache"]["status"], "hit", tree["holdout_baseline_cache"])
        self.assertEqual(self.external_source.read_bytes(), self.external_bytes)
        source = next(e for e in tree["evaluations"].values() if e.get("baseline_source"))
        self.assertEqual(source["trials_reserved"], 0)

    def test_external_holdout_ordinary_resume_changed_execution_still_rejected(self):
        report = self.ordinary_resumed_external_holdout()
        report["trials"][0]["duration_ms"] += 1
        fixture.write(self.external_source, report)
        self.external_bytes = self.external_source.read_bytes()
        path = self.external_source.parent / "manifest.json"
        manifest = json.loads(path.read_text())
        manifest["files"]["report.json"] = hashlib.sha256(self.external_bytes).hexdigest()
        fixture.write(path, manifest)
        self.start()
        job_id, _, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["holdout_baseline_cache"]["reason"], "gepa_external_baseline_execution_mismatch")
        self.assertFalse(any(e["split"] == "holdout" for e in tree["evaluations"].values()))

    def test_external_holdout_regrade_wrong_parent_still_rejected(self):
        self.enable_external_judge()
        self.prepare_external_holdout()
        report = self.regrade_external_holdout()
        report["configuration"]["execution_source"]["run_id"] = "evaluation:wrong-parent"
        fixture.write(self.external_source, report)
        self.external_bytes = self.external_source.read_bytes()
        manifest_path = self.external_source.parent / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["files"]["report.json"] = hashlib.sha256(self.external_bytes).hexdigest()
        fixture.write(manifest_path, manifest)
        self.start()
        job_id, _, _ = self.wait_search()
        tree = self.call("agent_evaluation.tree", job_id=job_id)
        self.assertEqual(tree["holdout_baseline_cache"]["reason"], "gepa_external_baseline_execution_mismatch")
        self.assertFalse(any(e["split"] == "holdout" for e in tree["evaluations"].values()))


if __name__ == "__main__":
    unittest.main()
