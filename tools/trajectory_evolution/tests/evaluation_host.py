"""Controlled host for task evaluator tests; never calls a paid provider."""

import json
import os
from pathlib import Path
import subprocess
import sys
import time


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
response = dict(schema_version=1, trial_id=request["trial_id"], execution_status="completed",
                output="任务已完成。", state={"value": value if mode != "wrong" else -1,
                                         "task_id": task["task_id"], "turns": len(task["followup_inputs"]) + 1},
                metrics={"steps": 2, "tool_calls": 1, "loop_count": 2})
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
if mode == "budget":
    response["metrics"]["steps"] = 999
if mode == "no_metrics":
    del response["metrics"]
if mode in ("artifact", "artifact_leak", "symlink", "missing"):
    path = Path("report.json")
    if mode == "symlink":
        path.symlink_to(sys.argv[2])
    elif mode != "missing":
        path.write_text(json.dumps({"total": value if mode != "artifact_leak" else os.environ["EVAL_TEST_API_KEY"]}))
    response["artifacts"] = [{"path": "report.json", "media_type": "application/json"}]
if mode in ("trace", "trace_wrong_id", "trace_open"):
    events = [dict(event_id="start", kind="run.start", producer_id="host", actor_id="agent",
                   sequence=1, timestamp_us="1", input=task["task_input"]),
              dict(event_id="end", kind="run.end", producer_id="host", actor_id="agent",
                   sequence=2, timestamp_us="2", caused_by=["start"], output=response["output"],
                   outcome={"execution_status": "completed", "business_status": "unknown"})]
    response["trace"] = dict(schema_version=1, trace_kind="trajectory.v1",
                             identity={"run_id": request["trial_id"], "agent_id": "test-host",
                                       "user_id": "test-user", "conversation_id": request["trial_id"]},
                             events=events)
    if mode == "trace_wrong_id":
        response["trace"]["identity"]["run_id"] = "wrong"
    if mode == "trace_open":
        response["trace"]["events"].pop()
print(json.dumps(response, ensure_ascii=False))
