# test_dispatch_client 回归验证：检查包内协议、边界条件与失败处理。
# 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
# 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

"""使用真实原生边界和 SDK 验证非阻塞派发协议，不以模型桩替代打点。"""

import json
import os
from pathlib import Path
import re
import selectors
import subprocess
import sys
import tempfile
import time

SOURCE = Path(__file__).resolve().parents[1]
BUILD = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(SOURCE / "clients/python"))
from evolution_local import LocalEvolution
from evolution import Evolution

os.environ["INTEGRATION_TEST_A"] = "fixture-principal-alpha-00000001"
os.environ["INTEGRATION_TEST_B"] = "fixture-principal-bravo-00000002"
ROOT = Path(tempfile.mkdtemp(prefix="dispatch-client.", dir=BUILD))
subprocess.run(
    [str(BUILD / "test_integration_service"), "--fixture", str(ROOT)],
    check=True,
    capture_output=True,
    timeout=20,
)
config = json.loads((ROOT / "service.json").read_text())
config["auto_evaluate"] = False
config["runtime_options"]["loop_rules"] = []
config["runtime_options"]["schedule_rules"] = [
    dict(
        rule_id="prepare-first",
        target_scope="agent:my-agent/actor:assistant",
        before_operation="prepare",
        after_operation="image",
        require_success=True,
        reason="Prepare before generating an image",
    )
]
config["runtime_options"]["action_rules"] += [
    dict(
        rule_id="image-needs-source",
        target_scope="agent:my-agent/actor:assistant",
        operation="image",
        required_arguments=["source"],
        reason="source is missing",
    ),
    dict(
        rule_id="one-attempt-per-target",
        target_scope="agent:my-agent/actor:assistant",
        operation="retry",
        max_attempts=1,
        attempt_key_arguments=["target"],
        reason="target already attempted",
    ),
]
(ROOT / "service.json").write_text(json.dumps(config))
BINDING = dict(
    items="/jobs",
    operation="/name",
    receipt="/ticket",
    accepted="/accepted",
    accepted_index="/index",
    accepted_id="/id",
    results="/results",
    result_id="/id",
    result_success="/ok",
    result_operation="/name",
    result_status="/status",
    result_output="/output",
)


# 测试辅助函数：scenario。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def scenario(c, run, level="L2", failure=False, text=False, reject=False):
    captured = c.point(
        dict(
            phase="before",
            kind="run",
            ref=dict(run=run),
            input="test",
            level=level,
            capabilities=["schedule"],
        )
    )
    assert not captured.get("degraded"), captured
    calls, waves = [], []

    # 校验并提交后台评测作业，返回可查询身份，不在主 Agent 热路径等待整场实验。
    def submit(payload):
        calls.append([item["name"] for item in payload["jobs"]])
        ticket = "ticket-" + str(len(calls))
        ack = dict(
            ticket=ticket,
            accepted=[]
            if reject
            else [
                dict(index=i + 1, id=ticket + ":" + str(i + 1))
                for i in range(len(payload["jobs"]))
            ],
        )
        waves.append(
            dict(
                ticket=ticket,
                results=[
                    dict(
                        id=item["id"],
                        name=payload["jobs"][i]["name"],
                        ok=not (failure and payload["jobs"][i]["name"] == "prepare"),
                    )
                    for i, item in enumerate(ack["accepted"])
                ],
            )
        )
        return json.dumps(ack) if text else ack

    fmt = "json_text" if text else "json"
    output = c.dispatch(
        dict(
            name="submit",
            ref=dict(run=run, key="dispatch"),
            binding=BINDING,
            input=dict(
                jobs=[dict(name="image"), dict(name="prepare"), dict(name="report")],
                reason="unchanged",
            ),
        ),
        submit,
        result_format=fmt,
    )
    ack = json.loads(output) if text else output
    assert isinstance(ack, dict), "JSON text must not be encoded twice"
    if reject:
        assert len(calls) == 1 and not ack["accepted"], calls
    else:
        assert calls == (
            [["image", "prepare", "report"]]
            if level == "L0"
            else [["prepare", "report"]]
        ), calls
        first = waves[0]
        first["results"].reverse()  # 分支乱序到达仍必须保留逻辑顺序。
        result = c.resume_dispatch(
            dict(ref=dict(run=run, key="complete-1"), input=first),
            submit,
            result_format=fmt,
        )
        if level == "L2" and not failure:
            assert result["pending"] and calls == [["prepare", "report"], ["image"]], (
                result,
                calls,
            )
            duplicate = c.resume_dispatch(
                dict(ref=dict(run=run, key="duplicate-complete"), input=first),
                submit,
                result_format=fmt,
            )
            assert duplicate["pending"] and len(calls) == 2, (
                "duplicate completion redispatched a skill"
            )
            result = c.resume_dispatch(
                dict(ref=dict(run=run, key="complete-2"), input=waves[1]),
                submit,
                result_format=fmt,
            )
        assert (
            not result["pending"] and result["completion"]["ticket"] == ack["ticket"]
        ), result
        assert [entry["name"] for entry in result["completion"]["results"]] == [
            "image",
            "prepare",
            "report",
        ]
        if failure:
            assert result["completion"]["results"][0]["status"] == "skipped"
            assert result["completion"]["results"][2]["ok"]
    closed = c.point(dict(phase="after", kind="run", ref=dict(run=run), output="done"))
    assert closed.get("closed") and closed.get("capture_complete"), closed


GUARD_TRACES = []


# 测试辅助函数：guard_scenarios。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def guard_scenarios(c, prefix, **options):
    """不提供宿主守卫回调，验证同一本地和 HTTP 派发协议保护每个波次。"""
    cases = {
        "later-wave": (
            [dict(name="image"), dict(name="prepare"), dict(name="report")],
            [["prepare", "report"]],
            ["cancelled", "completed", "completed"],
        ),
        "partial-first-wave": (
            [
                dict(name="image"),
                dict(name="video"),
                dict(name="report"),
                dict(
                    name="dependent", after=[dict(action_id="1", require_success=True)]
                ),
            ],
            [["report"]],
            ["cancelled", "cancelled", "completed", "skipped"],
        ),
        "all-rejected": (
            [
                dict(name="image"),
                dict(name="video"),
                dict(
                    name="dependent", after=[dict(action_id="1", require_success=True)]
                ),
            ],
            [],
            ["cancelled", "cancelled", "skipped"],
        ),
        "retry-target": (
            [
                dict(name="retry", target="A"),
                dict(name="report"),
                dict(name="retry", target="A", after=[dict(action_id="1")]),
                dict(name="retry", target="B", after=[dict(action_id="3")]),
            ],
            [["retry", "report"], ["retry"]],
            ["failed", "completed", "cancelled", "completed"],
        ),
        "L0": (
            [dict(name="video"), dict(name="image")],
            [["video", "image"]],
            ["completed", "completed"],
        ),
        "schedule-only": (
            [dict(name="video"), dict(name="image")],
            [["video", "image"]],
            ["completed", "completed"],
        ),
        "policy-disabled": (
            [dict(name="video"), dict(name="image")],
            [["video", "image"]],
            ["completed", "completed"],
        ),
    }
    for case, (jobs, expected_calls, expected_statuses) in cases.items():
        run = prefix + "-" + case
        caps = (
            ["schedule"]
            if case == "schedule-only"
            else ["model", "actions", "schedule", "loop"]
        )
        level = "L0" if case == "L0" else "L2" if case == "schedule-only" else "L3"
        started = c.point(
            dict(
                phase="before",
                kind="run",
                ref=dict(run=run),
                input="dispatch test",
                level=level,
                capabilities=caps,
            ),
            **options,
        )
        assert not started.get("degraded"), started
        if "loop" in caps:
            loop = c.point(
                dict(
                    phase="guard",
                    kind="loop",
                    ref=dict(run=run, key="loop"),
                    loop_id="planner",
                    actor_id="assistant",
                    state="ready",
                ),
                **options,
            )
            assert not loop.get("degraded"), loop
        for i, job in enumerate(jobs, 1):
            job.update(id=str(i), after=job.get("after", []))
        binding = dict(BINDING, action_id="/id", after="/after")
        calls, completions = [], []

        # 校验并提交后台评测作业，返回可查询身份，不在主 Agent 热路径等待整场实验。
        def submit(payload):
            assert payload["jobs"], (
                "a rejected wave must not call the host with an empty batch"
            )
            calls.append([job["name"] for job in payload["jobs"]])
            ticket = run + ":ticket:" + str(len(calls))
            accepted = [
                dict(index=i, id=ticket + ":" + str(i))
                for i in range(1, len(payload["jobs"]) + 1)
            ]
            results = []
            for job, item in zip(payload["jobs"], accepted):
                ok = not (case == "retry-target" and job["id"] == "1")
                results.append(
                    dict(
                        id=item["id"],
                        name=job["name"],
                        ok=ok,
                        status="completed" if ok else "failed",
                        output=dict(value=job["id"]),
                    )
                )
            completions.append(dict(ticket=ticket, results=list(reversed(results))))
            return json.dumps(dict(ticket=ticket, accepted=accepted))

        # 提交经过组件编排的当前派发波次，业务回调只负责实际提交，不等待其他分支完成。
        def dispatch(actual):
            return c.dispatch(
                dict(
                    name="submit",
                    actor_id="assistant",
                    ref=dict(run=run, key="plan", parent="envelope"),
                    binding=binding,
                    input=actual,
                    apply_policy=case != "policy-disabled",
                ),
                submit,
                result_format="json_text",
                **options,
            )

        # 模拟真实异步宿主：提交信封早于后续波次返回，
        # 策略取消也不能被当作业务调用失败。
        ack = json.loads(
            c.call(
                dict(
                    kind="action",
                    name="submit",
                    actor_id="assistant",
                    ref=dict(run=run, key="envelope"),
                    input=dict(jobs=jobs),
                ),
                dispatch,
                result_format="json_text",
                **options,
            )
        )
        if not expected_calls:
            assert (
                ack["status"] == "cancelled"
                and ack["reason"] == "all_dispatch_actions_rejected"
            ), ack
            assert ack["accepted"] == [] and len(ack["results"]) == len(jobs), ack
            completion = ack
        else:
            result = None
            completed = 0
            while completed < len(completions):
                payload = completions[completed]
                result = c.resume_dispatch(
                    dict(
                        ref=dict(run=run, key="complete-" + str(completed)),
                        input=payload,
                    ),
                    submit,
                    result_format="json_text",
                    **options,
                )
                assert not result.get("degraded"), result
                if result.get("pending"):
                    count = len(calls)
                    duplicate = c.resume_dispatch(
                        dict(
                            ref=dict(run=run, key="duplicate-" + str(completed)),
                            input=payload,
                        ),
                        submit,
                        result_format="json_text",
                        **options,
                    )
                    assert duplicate.get("pending") and len(calls) == count, (
                        "claim replay repeated a callback"
                    )
                completed += 1
            assert result and not result.get("pending"), result
            completion = result["completion"]
        assert calls == expected_calls, (case, calls)
        assert [row["name"] for row in completion["results"]] == [
            job["name"] for job in jobs
        ], completion
        assert [row["status"] for row in completion["results"]] == expected_statuses, (
            completion
        )
        for row in completion["results"]:
            if row["status"] in ("cancelled", "skipped"):
                assert row["executed"] is False and row["ok"] is False, row
            if row["status"] == "cancelled":
                assert (
                    row["output"]["status"] == "rejected" and row["output"]["reason"]
                ), row
        if case == "retry-target":
            probe = c.point(
                dict(
                    phase="guard",
                    kind="action",
                    name="retry",
                    actor_id="assistant",
                    ref=dict(run=run, key="budget-probe"),
                    input=dict(target="A"),
                ),
                **options,
            )
            assert probe.get("decision") == "reject" and not probe.get("degraded"), (
                probe
            )
        if "loop" in caps:
            loop = c.point(
                dict(
                    phase="guard",
                    kind="loop",
                    ref=dict(run=run, key="loop-after"),
                    loop_id="planner",
                    actor_id="assistant",
                    state="finished",
                ),
                **options,
            )
            assert not loop.get("degraded"), loop
        ended = c.point(
            dict(phase="after", kind="run", ref=dict(run=run), output="done"), **options
        )
        assert ended.get("closed") and ended.get("capture_complete"), ended
        GUARD_TRACES.append((started["context"]["run_id"], case))


c = LocalEvolution(
    BUILD / "libtrajectory_evolution_local.so",
    ROOT / "service.json",
    os.environ["INTEGRATION_TEST_A"],
    background=False,
)
try:
    scenario(c, "ordered")
    scenario(c, "unchanged", level="L0")
    scenario(c, "isolated-failure", failure=True)
    scenario(c, "json-text", text=True)
    scenario(c, "no-submission", reject=True, text=True)
    guard_scenarios(c, "native")
    assert c.metrics()["degraded_calls"] == 0, c.metrics()
finally:
    c.close()
print(
    "Dispatch SDK: ordering, L0, failure isolation, duplicate delivery and JSON text passed"
)

with (ROOT / "http.stderr").open("w") as errors:
    server = subprocess.Popen(
        [
            str(BUILD / "trajectory_evolution"),
            "serve",
            "--config",
            str(ROOT / "service.json"),
        ],
        stdout=subprocess.PIPE,
        stderr=errors,
        text=True,
    )
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(server.stdout, selectors.EVENT_READ)
            assert selector.select(15), "HTTP service startup timeout"
        port = re.search(r"listening=127.0.0.1:(\d+)", server.stdout.readline()).group(
            1
        )
        http = Evolution("http://127.0.0.1:" + port, os.environ["INTEGRATION_TEST_A"])
        guard_scenarios(http, "http", timeout_ms=5000)
    finally:
        server.terminate()
        try:
            server.wait(timeout=8)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait(timeout=3)

for run_id, case in GUARD_TRACES:
    path = ROOT / "data/by-user/user-a/traces" / run_id / (run_id + ".trace.json")
    for _ in range(100):
        if path.exists():
            break
        time.sleep(0.01)
    events = json.loads(path.read_text())["events"]
    guards = [
        event
        for event in events
        if event["kind"] == "step.start"
        and event.get("attributes", {}).get("extensions", {}).get("guard_only")
    ]
    if case in ("L0", "schedule-only", "policy-disabled"):
        assert not guards, (
            case,
            "undeclared action permission or policy bypass was ignored",
        )
    else:
        assert guards and all(
            event["actor_id"] == "assistant" and event.get("loop_id") == "planner"
            for event in guards
        ), (case, guards)
    cancelled = [
        event
        for event in events
        if event.get("attributes", {}).get("type") == "schedule.cancelled"
    ]
    assert all(event["attributes"]["executed"] is False for event in cancelled)
    if case == "all-rejected":
        envelopes = {
            event["action_id"]
            for event in events
            if event["kind"] == "action.start" and event.get("operation") == "submit"
        }
        envelope = [
            event
            for event in events
            if event["kind"] == "action.end" and event.get("action_id") in envelopes
        ]
        assert envelope and not any(
            event.get("attributes", {}).get("observed_failure") for event in envelope
        ), envelope
        assert not any(
            event["kind"] == "action.start"
            and event.get("attributes", {}).get("type") == "schedule.action"
            for event in events
        ), "cancelled work was falsely recorded as running"
    if case in ("later-wave", "retry-target"):
        assert not any(
            event["kind"] == "advice.applied"
            and event.get("attributes", {}).get("receipt", {}).get("boundary")
            == "schedule"
            and event.get("application", {}).get("status") == "applied"
            for event in events
        ), "unexecuted cancellation was falsely counted as schedule application"
    queries = [
        event["attributes"]["decision"]["query"]
        for event in events
        if event.get("attributes", {}).get("type") == "policy.query"
    ]
    loops = [query for query in queries if query.get("boundary") == "loop"]
    if case not in ("L0", "schedule-only"):
        assert loops and int(loops[-1].get("failed_actions", 0)) == (
            1 if case == "retry-target" else 0
        ), (case, loops)
    if case == "retry-target":
        retries = [
            action
            for query in queries
            if query.get("boundary") == "actions"
            for action in query.get("actions", [])
            if action.get("operation") == "retry"
            and action.get("arguments", {}).get("target") == "A"
        ]
        assert int(retries[-1]["target_failures"]["one-attempt-per-target"]) == 1, (
            retries
        )
print(
    "Native + HTTP: later-wave guards, empty ACK, cancellation, capability isolation and trace attribution passed"
)

# 宿主能力改变后必须生成重新验证的策略，
# 不要求重建记忆，也不能悄悄保留旧的不可变策略身份。
compile_request = dict(
    memory_path=str(ROOT / "data/by-user/user-a/memory/evolution-memory.pb"),
    identity=dict(
        run_id="compile",
        agent_id="my-agent",
        user_id="user-a",
        conversation_id="compile",
    ),
    shadow_options=dict(
        config["shadow_options"], target_scopes=["agent:my-agent/actor:assistant"]
    ),
    options=dict(
        requested_level="L1",
        allowed_users=["user-a"],
        fallback="strict",
        host_capabilities=["context.request"],
        max_context_bytes=4096,
        experience_template="{{experience}}",
    ),
)
compiled = subprocess.run(
    [str(BUILD / "trajectory_evolution"), "runtime", "compile"],
    input=json.dumps(compile_request),
    text=True,
    capture_output=True,
    timeout=20,
)
assert compiled.returncode == 0, compiled.stdout
original = json.loads(compiled.stdout)
options = dict(
    original["options"],
    requested_level="L2",
    host_capabilities=["context.request", "schedule.dispatch"],
)
request = dict(policy=original, options=options)
recompiled = subprocess.run(
    [str(BUILD / "trajectory_evolution"), "runtime", "recompile"],
    input=json.dumps(request),
    text=True,
    capture_output=True,
    timeout=20,
)
assert recompiled.returncode == 0, recompiled.stdout
updated = json.loads(recompiled.stdout)
assert updated["snapshot"] == original["snapshot"]
assert (
    updated["policy_id"] != original["policy_id"] and updated["effective_level"] == "L2"
)
assert "schedule.dispatch" in updated["options"]["host_capabilities"]
for invalid in (
    dict(request, memory_path="unexpected.pb"),
    dict(request, policy=dict(original, policy_id="runtime-v2:invalid")),
    dict(request, options=dict(options, host_capabilities=[])),
):
    rejected = subprocess.run(
        [str(BUILD / "trajectory_evolution"), "runtime", "recompile"],
        input=json.dumps(invalid),
        text=True,
        capture_output=True,
        timeout=20,
    )
    assert rejected.returncode != 0, "Invalid recompile request was accepted"
print(
    "Policy recompile: unchanged memory, new capability, integrity and strict capability checks passed"
)
