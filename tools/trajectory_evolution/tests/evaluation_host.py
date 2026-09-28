# evaluation_host 回归验证：检查包内协议、边界条件与失败处理。
# 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
# 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

"""任务评测器的受控测试宿主，不调用付费模型提供方。"""

import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import time
import zlib


request = json.load(sys.stdin)
mode = sys.argv[1] if len(sys.argv) > 1 else "good"
task = request["task"]
assert "expected_behavior" not in task and "split" not in task and "checks" not in task
if mode == "sleep":
    time.sleep(20)
if mode == "child":
    child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
    Path(sys.argv[2]).write_text(str(child.pid))
    time.sleep(20)
if mode == "detached_child":
    first = os.fork()
    if first == 0:
        os.setsid()
        if os.fork() != 0:
            os._exit(0)
        Path(sys.argv[2]).write_text(str(os.getpid()))
        directory = Path.cwd()
        time.sleep(1)
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "late-write.txt").write_text("must not survive timeout")
        time.sleep(30)
        os._exit(0)
    os.waitpid(first, 0)
    time.sleep(20)
if mode == "nonzero":
    print("host failed", file=sys.stderr)
    sys.exit(9)
if mode == "oversize":
    print("x" * 100000)
    sys.exit(0)
if mode == "bad_json":
    print("this is not json")
    sys.exit(0)
if mode == "order":
    time.sleep(float(task["initial_state"].get("delay", 0)))

value = int(task["initial_state"].get("value", 20))
response = dict(
    schema_version=1,
    trial_id=request["trial_id"],
    execution_status="completed",
    output="任务已完成。",
    state={
        "value": value if mode != "wrong" else -1,
        "task_id": task["task_id"],
        "turns": len(task["followup_inputs"]) + 1,
    },
    metrics={"steps": 2, "tool_calls": 1, "loop_count": 2},
)
if mode == "wrong_id":
    response["trial_id"] = "some-other-trial"
if mode == "failed":
    response["execution_status"] = "failed"
if mode == "environment":
    response["execution_status"] = "environment_error"
if mode == "leak":
    response["output"] = os.environ["EVAL_TEST_API_KEY"]
if mode == "env":
    response["state"]["unexpected_env"] = "EVAL_UNRELATED_SECRET" in os.environ
if mode == "repeated_evidence":
    response["state"]["details"] = task["initial_state"]["details"]
if mode == "repeated_leak":
    response["state"]["details"] = [os.environ["EVAL_TEST_API_KEY"] * 30] * 3
if mode == "budget":
    response["metrics"]["steps"] = 999
if mode == "no_metrics":
    del response["metrics"]
if mode == "invalid_png_fixture":
    directory = Path("output/run/assets")
    directory.mkdir(parents=True)
    (directory / "broken.png").write_text("NOT_AN_IMAGE")
    Path("report.json").write_text(json.dumps({"is_valid_png": False}))
if mode in ("artifact", "artifact_leak", "symlink", "missing"):
    path = Path("report.json")
    if mode == "symlink":
        path.symlink_to(sys.argv[2])
    elif mode != "missing":
        path.write_text(
            json.dumps(
                {
                    "total": value
                    if mode != "artifact_leak"
                    else os.environ["EVAL_TEST_API_KEY"]
                }
            )
        )
    response["artifacts"] = [{"path": "report.json", "media_type": "application/json"}]
if mode.startswith("png"):
    directory = Path("output/run/assets")
    directory.mkdir(parents=True)

    # 测试辅助函数：chunk。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def chunk(kind, data):
        return (
            struct.pack(">I", len(data))
            + kind
            + data
            + struct.pack(">I", zlib.crc32(kind + data))
        )

    width = 100000 if mode == "png_huge" else 2
    height = 100000 if mode == "png_huge" else 1
    data = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(b"\0" + bytes((255, 0, 0, 255)) * 2))
        + chunk(b"IEND", b"")
    )
    if mode == "png_truncated":
        data = data[:40]
    if mode == "png_no_end":
        data = data[:-12]
    if mode == "png_corrupt":
        data = data[:41] + b"\xff\xff" + data[43:]
    for index in range(33 if mode == "png_many" else 1):
        path = directory / f"image{index}.png"
        if mode == "png_symlink":
            path.symlink_to(sys.argv[2])
        else:
            path.write_bytes(data)
    if mode == "png_parent_symlink":
        Path("output/link").symlink_to("run", target_is_directory=True)
    if mode == "png_declared":
        response["artifacts"] = [
            {"path": "output/run/assets/image0.png", "media_type": "image/png"}
        ]
if mode in (
    "trace",
    "trace_wrong_id",
    "trace_open",
    "loop_trace",
    "loop_trace_unordered",
    "loop_trace_join",
    "loop_trace_join_unordered",
    "repeated_evidence",
):
    events = [
        dict(
            event_id="start",
            kind="run.start",
            producer_id="host",
            actor_id="agent",
            sequence=1,
            timestamp_us="1",
            input=task["task_input"],
        ),
        dict(
            event_id="end",
            kind="run.end",
            producer_id="host",
            actor_id="agent",
            sequence=2,
            timestamp_us="2",
            caused_by=["start"],
            output=response["output"],
            outcome={"execution_status": "completed", "business_status": "unknown"},
        ),
    ]
    response["trace"] = dict(
        schema_version=1,
        trace_kind="trajectory.v1",
        identity={
            "run_id": request["trial_id"],
            "agent_id": "test-host",
            "user_id": "test-user",
            "conversation_id": request["trial_id"],
        },
        events=events,
    )
    if mode in (
        "loop_trace",
        "loop_trace_unordered",
        "loop_trace_join",
        "loop_trace_join_unordered",
        "repeated_evidence",
    ):
        events.pop()
        completed = []
        for branch in ("left", "right"):
            previous = "start"
            sequence = 0
            for iteration in (1, 2):
                step = f"{branch}-{iteration}"
                for kind in ("step.start", "action.start", "action.end", "step.end"):
                    sequence += 1
                    event_id = f"{step}-{kind}"
                    event = dict(
                        event_id=event_id,
                        kind=kind,
                        producer_id=branch,
                        actor_id="agent",
                        execution_id=branch,
                        loop_id=branch,
                        iteration=iteration,
                        step_id=step,
                        sequence=sequence,
                        timestamp_us=str(len(events) + 1),
                        caused_by=[previous],
                    )
                    if kind.startswith("action."):
                        event.update(
                            action_id=step + "-call",
                            operation="calculate",
                            input={"value": value},
                        )
                    if kind.endswith(".end"):
                        event.update(status="completed", output=value)
                    if mode == "repeated_evidence":
                        if kind in ("step.start", "action.start", "action.end"):
                            event["input"] = {
                                "details": task["initial_state"]["details"]
                            }
                        if kind.endswith(".end"):
                            event["output"] = {
                                "value": value,
                                "text": task["initial_state"]["details"]["copies"][0],
                            }
                    events.append(event)
                    previous = event_id
            completed.append(previous)
        events.append(
            dict(
                event_id="end",
                kind="run.end",
                producer_id="host",
                actor_id="agent",
                sequence=2,
                timestamp_us=str(len(events) + 1),
                caused_by=completed,
                output=response["output"],
                outcome={"execution_status": "completed", "business_status": "unknown"},
            )
        )
    if mode in ("loop_trace_join", "loop_trace_join_unordered"):
        by_id = {e["event_id"]: e for e in events}
        by_id["right-2-step.start"]["caused_by"] = ["left-1-step.start"]
        by_id["left-1-step.end"]["caused_by"].append("right-2-step.end")
        # 已汇聚子分支与无关分支都在父节点期间运行，
        # 仅靠时间戳无法判断父节点实际看到了哪个结果。
        order = [
            "start",
            "left-1-step.start",
            "left-1-action.start",
            "left-1-action.end",
        ]
        order += [
            f"right-{i}-{kind}"
            for i in (1, 2)
            for kind in ("step.start", "action.start", "action.end", "step.end")
        ]
        order += ["left-1-step.end"]
        order += [
            f"left-2-{kind}"
            for kind in ("step.start", "action.start", "action.end", "step.end")
        ]
        order += ["end"]
        events[:] = [by_id[event_id] for event_id in order]
        for i, event in enumerate(events, 1):
            event["timestamp_us"] = str(i)
    if mode in ("loop_trace_unordered", "loop_trace_join_unordered"):
        events.reverse()
    if mode == "trace_wrong_id":
        response["trace"]["identity"]["run_id"] = "wrong"
    if mode == "trace_open":
        response["trace"]["events"].pop()
print(json.dumps(response, ensure_ascii=False))
