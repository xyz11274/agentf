# agent_evaluation_host 回归验证：检查包内协议、边界条件与失败处理。
# 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
# 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

"""在隔离评测实例中运行带真实打点的小型测试 Agent。"""

import json
import os
from pathlib import Path
import sys
import time

SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE / "clients/python"))
from evolution_local import LocalEvolution

request = json.load(sys.stdin)
mode, library = sys.argv[1:3]
recovery_budget = mode == "gepa_recovery_budget"
if recovery_budget:
    mode = "gepa_failure_replan"
late_schedule = mode == "gepa_schedule_late"
late_loop = mode == "gepa_loops_late"
if late_schedule or late_loop:
    mode = mode.removesuffix("_late")
if mode == "gepa_schema_validation":
    mode = (
        "gepa_schema_actions"
        if request["task"]["task_input"].startswith("VALIDATION_PRIVATE")
        else "gepa_schema_noop"
    )
if mode == "gepa_schema_training":
    mode = (
        "gepa_schema_actions"
        if request["task"]["task_input"].startswith("TRAIN_VISIBLE")
        else "gepa_schema_noop"
    )
if mode == "slow":
    time.sleep(2)
if mode in ("gepa_merge_retry", "gepa_merge_timeout_retry"):
    timeout_retry = mode == "gepa_merge_timeout_retry"
    mode = "gepa_merge"
    options = (request["artifact"].get("policy") or {}).get("options", {})
    marker = Path(os.environ["GEPA_RUNNER_FAIL_ONCE"])
    generated = any(
        "source_experience" in rule for rule in options.get("action_rules", [])
    )
    if generated and request["task"]["task_id"] == "train-a" and not marker.exists():
        marker.write_text("failed", encoding="utf-8")
        if timeout_retry:
            time.sleep(2)
        else:
            raise RuntimeError("fixture_transient_runner_failure")
artifact = request["artifact"]
if mode == "gepa_merge_pause":
    mode = "gepa_merge"
    options = (artifact.get("policy") or {}).get("options", {})
    if options.get("experience_overrides") and any(
        "source_experience" in r for r in options.get("action_rules", [])
    ):
        Path(os.environ["GEPA_MERGE_READY"]).write_text("ready", encoding="utf-8")
        deadline = time.monotonic() + 30
        while (
            not Path(os.environ["GEPA_MERGE_RELEASE"]).exists()
            and time.monotonic() < deadline
        ):
            time.sleep(0.05)
workspace = Path.cwd()
if mode == "resume":
    with open(os.environ["EVOLUTION_TEST_EXECUTIONS"], "a", encoding="utf-8") as stream:
        stream.write(request["trial_id"] + "\n")
    (workspace / "proof.txt").write_text("verified execution", encoding="utf-8")
version_file = workspace / "version.json"
version_file.write_text(
    json.dumps(
        dict(
            version=request["version"], artifact=artifact, trial_id=request["trial_id"]
        )
    )
)
config = json.loads((SOURCE / "config/service.json").read_text())
config.update(
    agent_id="eval-test",
    data_dir=str(workspace / "state"),
    principals=[
        dict(user_id=artifact["user_id"], token_env="EVALUATION_INSTANCE_TOKEN")
    ],
    evaluation_version_file=str(version_file),
    runtime_config=str(SOURCE / "config/standard-runtime.json"),
    experience_template_file=str(SOURCE / "config/prompts/runtime-experience.txt"),
)
config_file = workspace / "service.json"
config_file.write_text(json.dumps(config))
token = "isolated-evaluation-token-123456789"
os.environ["EVALUATION_INSTANCE_TOKEN"] = token
evo = LocalEvolution(library, config_file, token)
level = artifact["level"]
capabilities = ["model", "actions", "loop"][: int(level[1])]
if mode in ("gepa_schedule", "gepa_full") and int(level[1]) >= 2:
    capabilities.append("schedule")
run = evo.invoke(
    "run.begin",
    dict(input=request["task"]["task_input"], level=level, capabilities=capabilities),
)
rid = run["run_id"]
activity_scope = {}
if mode in (
    "gepa_repeat_guard",
    "gepa_repeat_tool",
    "gepa_schema_wide",
    "gepa_schema_late",
):
    activity_scope["actor_id"] = "fixture-worker"
if mode in (
    "gepa_review",
    "gepa_failure_replan",
    "gepa_full",
    "resume",
    "gepa_repeat_guard",
    "gepa_repeat_tool",
):
    control = evo.invoke(
        "loop.check", dict(run_id=rid, loop_id="reviewed-loop", state={})
    )
    evo.invoke(
        "loop.commit",
        dict(
            run_id=rid, control_id=control["control_id"], decision=control["decision"]
        ),
    )
    activity_scope["control_id"] = control["control_id"]
model_input = {"messages": [{"role": "user", "content": request["task"]["task_input"]}]}
if mode in ("gepa_schema_actions", "gepa_schema_noop"):
    model_input["tools"] = [
        {
            "type": "function",
            "function": {
                "name": "calculate",
                "parameters": {
                    "type": "object",
                    "properties": {"value": {"type": "string"}},
                    "required": ["value"],
                },
            },
        }
    ]
if mode in ("gepa_schema_wide", "gepa_schema_late"):
    model_input["tools"] = [
        {
            "type": "function",
            "function": {
                "name": "calculate",
                "parameters": {
                    "type": "object",
                    "properties": {"zz_value": {"type": "string"}},
                    "required": ["zz_value"],
                },
            },
        }
    ]
call = evo.invoke(
    "activity.begin",
    dict(
        run_id=rid,
        kind="model",
        name="fixture-model",
        input=model_input,
        binding={"pointer": "/messages", "mode": "system_message"},
        **activity_scope,
    ),
)
result = 0 if mode == "bad_candidate" and level != "L0" else 20
if mode in ("memory_bad", "memory_good"):
    injected = call["input"] != model_input
    result = 20 if injected == (mode == "memory_good") else 0
if mode in (
    "gepa",
    "gepa_holdout_failure",
    "gepa_merge",
    "gepa_merge_regression",
    "gepa_review",
):
    # 只有打点实际生成的请求才能影响该测试宿主。
    text = json.dumps(call["input"], ensure_ascii=False)
    family = "甲类" if "甲类" in request["task"]["task_input"] else "乙类"
    result = 20 if "核验" + family in text else 0
    if (
        mode == "gepa_holdout_failure"
        and "HOLDOUT_PRIVATE" in request["task"]["task_input"]
    ):
        result = 0
if mode in ("gepa_actions", "gepa_schema_actions", "gepa_loops", "gepa_failure_replan"):
    result = 0
missing_metric = False
if (
    mode == "gepa_review"
    and level != "L0"
    and "VALIDATION_PRIVATE 甲类" in request["task"]["task_input"]
):
    marker = os.environ.get("GEPA_MISSING_METRIC_ONCE")
    if marker and not Path(marker).exists():
        Path(marker).touch()
        missing_metric = True
evo.invoke(
    "activity.end",
    dict(
        run_id=rid,
        activity_id=call["activity_id"],
        status="completed",
        output=result,
        actual_input=call["input"],
        executed=True,
        **(
            {"metrics": {"tokens": 100}}
            if mode.startswith("gepa") and not missing_metric
            else {}
        ),
    ),
)
action_input = {"value": "" if mode == "gepa_schema_actions" else result}
if mode == "gepa_repeat_target":
    action_input = {"output": "artifact.txt", "attempt": 1}
if mode in ("gepa_schema_wide", "gepa_schema_late"):
    action_input = {f"field_{i:02}": "present" for i in range(40)}
    action_input["zz_value"] = "present"
action = evo.invoke(
    "activity.begin",
    dict(
        run_id=rid,
        kind="action",
        name="calculate",
        input=action_input,
        **activity_scope,
    ),
)
blocked = (
    mode
    in ("gepa_actions", "gepa_schema_actions", "gepa_merge", "gepa_merge_regression")
    and action["decision"] == "reject"
)
if blocked and (mode in ("gepa_actions", "gepa_schema_actions") or family == "乙类"):
    result = 20
if mode == "gepa_merge_regression" and blocked and "核验甲类" in text:
    result = 0
evo.invoke(
    "activity.end",
    dict(
        run_id=rid,
        activity_id=action["activity_id"],
        status="cancelled" if blocked else "completed",
        output={"success": False, "error": {"code": "FixtureUnavailable"}}
        if recovery_budget
        else {"success": False}
        if mode
        in (
            "gepa_failure_replan",
            "gepa_full",
            "gepa_repeat_guard",
            "gepa_repeat_tool",
            "gepa_repeat_target",
        )
        else result,
        executed=not blocked,
    ),
)
if mode == "gepa_repeat_target":
    for index in (2, 3):
        retry = evo.invoke(
            "activity.begin",
            dict(
                run_id=rid,
                kind="action",
                name="calculate",
                input={"output": "artifact.txt", "attempt": index},
                stream_id=f"redispatched-{index}",
            ),
        )
        blocked = retry["decision"] == "reject"
        result = 20 if blocked else 0
        evo.invoke(
            "activity.end",
            dict(
                run_id=rid,
                activity_id=retry["activity_id"],
                status="cancelled" if blocked else "completed",
                output={"success": False},
                executed=not blocked,
            ),
        )
if mode in ("gepa_repeat_guard", "gepa_repeat_tool", "gepa_schema_late"):
    if mode != "gepa_schema_late":
        retry = evo.invoke(
            "activity.begin",
            dict(
                run_id=rid,
                kind="action",
                name="calculate",
                input={"attempt": 2},
                **activity_scope,
            ),
        )
        evo.invoke(
            "activity.end",
            dict(
                run_id=rid,
                activity_id=retry["activity_id"],
                status="completed",
                output={"success": False},
                executed=True,
            ),
        )
    # 把可操作证据放在展示预算之后，使用包实际产生的事件，
    # 不使用手工拼写的反思包。
    for i in range(20):
        noise = evo.invoke(
            "activity.begin",
            dict(
                run_id=rid,
                kind="action",
                name="independent",
                input={"index": i},
                **activity_scope,
            ),
        )
        evo.invoke(
            "activity.end",
            dict(
                run_id=rid,
                activity_id=noise["activity_id"],
                status="completed",
                output={"index": i},
                executed=True,
            ),
        )
    guard = mode == "gepa_repeat_guard"
    final_input = (
        {**action_input, "zz_value": ""}
        if mode == "gepa_schema_late"
        else {"attempt": 3}
    )
    final = evo.invoke(
        "activity.begin",
        dict(
            run_id=rid,
            kind="action",
            name="calculate",
            input=final_input,
            extensions={"guard_only": guard},
            **activity_scope,
        ),
    )
    blocked = final["decision"] == "reject"
    result = 20 if blocked else 0
    evo.invoke(
        "activity.end",
        dict(
            run_id=rid,
            activity_id=final["activity_id"],
            status="cancelled" if guard or blocked else "completed",
            executed=not (guard or blocked),
            output={"guard_decision": final["decision"]}
            if guard
            else {"success": blocked},
        ),
    )
if mode in ("gepa_failure_replan", "gepa_full"):
    control = evo.invoke(
        "loop.check",
        dict(run_id=rid, loop_id="reviewed-loop", state={"new_error_id": "different"}),
    )
    evo.invoke(
        "loop.commit",
        dict(
            run_id=rid, control_id=control["control_id"], decision=control["decision"]
        ),
    )
    if control["decision"] == "continue":
        retry = evo.invoke(
            "activity.begin",
            dict(
                run_id=rid,
                kind="action",
                name="calculate",
                input=action_input,
                control_id=control["control_id"],
            ),
        )
        evo.invoke(
            "activity.end",
            dict(
                run_id=rid,
                activity_id=retry["activity_id"],
                status="completed",
                output={"success": False, "error": {"code": "FixtureUnavailable"}}
                if recovery_budget
                else {"success": False},
                executed=True,
            ),
        )
        control = evo.invoke(
            "loop.check",
            dict(
                run_id=rid,
                loop_id="reviewed-loop",
                state={"new_error_id": "different-again"},
            ),
        )
        evo.invoke(
            "loop.commit",
            dict(
                run_id=rid,
                control_id=control["control_id"],
                decision=control["decision"],
            ),
        )
    if recovery_budget and control["decision"] == "stop":
        result = 20
    else:
        recovery = evo.invoke(
            "activity.begin",
            dict(
                run_id=rid,
                kind="model",
                name="recovery-model",
                input=model_input,
                binding={"pointer": "/messages", "mode": "system_message"},
                control_id=control["control_id"],
            ),
        )
        if (
            not recovery_budget
            and control["decision"] == "replan"
            and any(
                control["context"] in m.get("content", "")
                for m in recovery["input"]["messages"]
            )
        ):
            result = 20
        evo.invoke(
            "activity.end",
            dict(
                run_id=rid,
                activity_id=recovery["activity_id"],
                status="completed",
                output=result,
                actual_input=recovery["input"],
                executed=True,
                metrics={"tokens": 100},
            ),
        )
if late_loop:
    for i in range(20):
        noise = evo.invoke(
            "activity.begin",
            dict(run_id=rid, kind="action", name=f"unrelated-{i}", input={}),
        )
        evo.invoke(
            "activity.end",
            dict(
                run_id=rid,
                activity_id=noise["activity_id"],
                status="failed",
                output="An independent operation failed.",
                executed=True,
            ),
        )
if mode == "gepa_loops":
    for iteration in range(4):
        control = evo.invoke(
            "loop.check",
            dict(run_id=rid, loop_id="bounded-calculation", state={"value": result}),
        )
        evo.invoke(
            "loop.commit",
            dict(
                run_id=rid,
                control_id=control["control_id"],
                decision=control["decision"],
            ),
        )
        if control["decision"] == "stop":
            result = 20
            break
if mode in ("gepa_schedule", "gepa_full"):
    if late_schedule:
        for i in range(9):
            noise = evo.invoke(
                "activity.begin",
                dict(
                    run_id=rid,
                    kind="schedule",
                    name=f"single-{i}",
                    input=dict(
                        actions=[
                            dict(
                                action_id="single",
                                operation="independent",
                                arguments={},
                            )
                        ]
                    ),
                ),
            )
            for status in ("running", "succeeded"):
                evo.invoke(
                    "schedule.observe",
                    dict(
                        run_id=rid,
                        activity_id=noise["activity_id"],
                        input=dict(action_id="single", status=status, output="done"),
                    ),
                )
            evo.invoke(
                "activity.end",
                dict(
                    run_id=rid,
                    activity_id=noise["activity_id"],
                    status="completed",
                    output="done",
                ),
            )
    requires_material = "准备和图像生成互不依赖" not in request["task"]["task_input"]
    prepared = evo.invoke(
        "activity.begin",
        dict(
            run_id=rid,
            kind="schedule",
            name="skills",
            input=dict(
                actions=[
                    dict(action_id="image", operation="image", arguments={}),
                    dict(action_id="prepare", operation="prepare", arguments={}),
                ]
            ),
        ),
    )
    view, result, material_ready = prepared, 0, False
    while not view["finished"]:
        ready = view["ready"]
        assert ready, view
        for item in ready:
            view = evo.invoke(
                "schedule.observe",
                dict(
                    run_id=rid,
                    activity_id=prepared["activity_id"],
                    input=dict(action_id=item["action_id"], status="running"),
                ),
            )
        for item in ready:
            if item["operation"] == "image":
                result = 20 if not requires_material or material_ready else 0
            else:
                material_ready = True
            view = evo.invoke(
                "schedule.observe",
                dict(
                    run_id=rid,
                    activity_id=prepared["activity_id"],
                    input=dict(
                        action_id=item["action_id"], status="succeeded", output="done"
                    ),
                ),
            )
    evo.invoke(
        "activity.end",
        dict(
            run_id=rid,
            activity_id=prepared["activity_id"],
            status="completed",
            output=result,
        ),
    )
for followup in request["task"]["followup_inputs"]:
    activity = evo.invoke(
        "activity.begin",
        dict(
            run_id=rid,
            kind="model",
            name="fixture-followup",
            input={"messages": [{"role": "user", "content": followup}]},
            binding={"pointer": "/messages", "mode": "system_message"},
        ),
    )
    evo.invoke(
        "activity.end",
        dict(
            run_id=rid,
            activity_id=activity["activity_id"],
            status="completed",
            output=result,
            actual_input=activity["input"],
            executed=True,
        ),
    )
evo.invoke("run.end", dict(run_id=rid, status="completed", output=str(result)))
evo.drain()
deadline = time.monotonic() + 3
paths = []
while not paths and time.monotonic() < deadline:
    paths = list((workspace / "state/by-user").rglob("*.trace.json"))
    if not paths:
        time.sleep(0.02)
evo.close()
if len(paths) != 1:
    raise RuntimeError("fixture_trace_missing")
trace = json.loads(paths[0].read_text())
print(
    json.dumps(
        dict(
            schema_version=1,
            trial_id=request["trial_id"],
            execution_status="completed",
            loaded_version="wrong-version"
            if mode == "wrong_version"
            else run["agent_version_id"],
            output=str(result),
            state={"value": result},
            trace=trace,
            metrics=dict(
                steps=3,
                tool_calls=1,
                llm_calls=1 + len(request["task"]["followup_inputs"]),
                tokens=100,
            ),
        )
    )
)
