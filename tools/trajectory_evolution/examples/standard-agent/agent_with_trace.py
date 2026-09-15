"""Synchronous standard-trace example; no AgentFlow runtime or hook dependency."""

import json
import sys
import time
import uuid
from pathlib import Path


def run_recorded(agent, task, *, user_id, conversation_id, log_dir,
                 agent_id="report-agent"):
    identity = {
        "run_id": uuid.uuid4().hex,
        "agent_id": agent_id,
        "user_id": user_id,
        "conversation_id": conversation_id,
    }
    events = []
    recording_ok = True

    def emit(kind, **fields):
        nonlocal recording_ok
        event = {
            "event_id": f"event-{len(events) + 1}",
            "producer_id": "main", "actor_id": "assistant",
            "sequence": str(len(events) + 1),
            "timestamp_us": str(time.time_ns() // 1000),
            "kind": kind, **fields,
        }
        try:
            # 当场复制内容，避免业务稍后修改同一个字典，污染历史事件。
            events.append(json.loads(json.dumps(event, ensure_ascii=False,
                                                 allow_nan=False)))
        except Exception:
            recording_ok = False

    emit("run.start", input=task)
    result = None
    execution_status, business_status = "failed", "unknown"
    failure = {}
    try:
        # 这里才是调用你自己的 Agent；第二个返回值由业务判断，不由组件猜测。
        result, business_status = agent(task, emit)
        execution_status = "completed"
        return result
    except BaseException as exc:
        if isinstance(exc, (KeyboardInterrupt, SystemExit)):
            execution_status = "cancelled"
        failure = {"error": {"code": "agent_interrupted_or_failed",
                             "retryability": "unknown"}}
        raise  # 记录异常，但不吞掉或改变原来的异常传播。
    finally:
        emit("run.end", output=result, outcome={
            "execution_status": execution_status,
            "business_status": business_status,
        }, **failure)
        try:
            if not recording_ok:
                raise ValueError("trace_recording_failed")
            root = Path(log_dir)
            root.mkdir(parents=True, exist_ok=True)
            trace = {"schema_version": 1, "trace_kind": "trajectory.v1",
                     "identity": identity, "events": events}
            pending = root / f"{identity['run_id']}.trace.part"
            with pending.open("x", encoding="utf-8") as stream:
                json.dump(trace, stream, ensure_ascii=False, allow_nan=False)
            # 完整落盘后再发布；evaluate 不会扫描尚未写完的 .part 文件。
            pending.replace(root / f"{identity['run_id']}.trace.json")
        except Exception:
            # 采集失败不能覆盖原业务结果/异常，也不打印可能含秘密的异常原文。
            print("WARNING: trajectory_not_saved", file=sys.stderr)


def demo_agent(task, emit):
    # 在你自己的步骤、工具入口直接记录；没有 Hook 引擎或事件总线。
    context = {"execution_id": "main-execution", "step_id": "sum-step"}
    emit("step.start", **context, input=task)
    result, status, action_started = None, "failed", False
    try:
        amounts = task["amounts"]
        emit("action.start", **context, action_id="sum-call",
             operation="sum_amounts", input={"amounts": amounts})
        action_started = True
        result = {"total": sum(amounts)}  # 这里是实际执行工具的位置。
        status = "completed"
        return result, "succeeded"
    except BaseException as exc:
        if isinstance(exc, (KeyboardInterrupt, SystemExit)):
            status = "cancelled"
        raise
    finally:
        if action_started:
            emit("action.end", **context, action_id="sum-call",
                 status=status, output=result)
        emit("step.end", **context, status=status, output=result)


if __name__ == "__main__":
    for amounts in ([12, 18], [7, 9]):
        print(run_recorded(
            demo_agent, {"amounts": amounts}, user_id="demo-user",
            conversation_id="report-conv",  # 同一会话保持不变，每次生成新 run_id。
            log_dir="build/trajectory-evolution/logless-demo/input",
        ))
