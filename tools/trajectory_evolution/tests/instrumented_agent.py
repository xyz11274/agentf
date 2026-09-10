"""A normal point-instrumented Agent: no evaluation response or version-file assembly."""
import json
import os
from pathlib import Path
import sys
import subprocess
import time

SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE / "clients/python"))
from evolution_local import LocalEvolution

packet = json.load(sys.stdin)
if "--dispatch" in sys.argv:
    process = subprocess.Popen([sys.executable, __file__, "--http", "--worker"],
                               stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    process.stdin.write(json.dumps(packet).encode())
    process.stdin.close()
    sys.exit(0)
if "--worker" in sys.argv:
    time.sleep(0.15)
task = packet["task"]
assert "group_id" not in task and "expected_behavior" not in task
if "--http" in sys.argv:
    from evolution import Evolution
    agent = Evolution("https://configured-by-deployment.invalid", "configured-by-deployment-credential")
else:
    agent = LocalEvolution(sys.argv[1], "configured-by-deployment.json", "configured-by-deployment-credential")
try:
    if "--exit-before-run" in sys.argv:
        raise SystemExit(9)
    run = agent.point(dict(phase="before", kind="run", input=task["task_input"]))
    for text in [task["task_input"], *task["followup_inputs"]]:
        call = agent.point(dict(phase="before", kind="model", context=run["context"], name="test-provider", input=text))
        # This deterministic test provider replaces a paid model, not the instrumentation.
        answer = 20
        agent.point(dict(phase="after", context=call["context"], executed=True, input=call["input"],
                         output=answer, metrics={"input_tokens": 40, "output_tokens": 2}))
        if "--exit-open-run" in sys.argv:
            raise SystemExit(9)
    call = agent.point(dict(phase="before", kind="action", context=run["context"], name="calculate", input={"value": answer}))
    agent.point(dict(phase="after", context=call["context"], executed=True, output=answer))
    agent.point(dict(phase="after", context=run["context"], output={"answer": answer, "state": {"value": answer}}, business_status="succeeded"))
    print("Normal application stdout; no evaluator-specific JSON envelope.")
    if "--exit-after-run" in sys.argv:
        print(os.environ.get("EVAL_TEST_API_KEY", "private-test-stderr"), file=sys.stderr)
        raise SystemExit(9)
finally:
    if hasattr(agent, "close"):
        agent.close()
    if "--corrupt-capture" in sys.argv:
        config = json.loads(Path(os.environ["TRAJECTORY_EVOLUTION_TRIAL_CONFIG"]).read_text())
        for path in (Path(config["data_dir"]) / "runs").glob("*.json"):
            value = json.loads(path.read_text())
            value["events"][-1]["output"] = {"api_key": os.environ["EVAL_TEST_API_KEY"]}
            path.write_text(json.dumps(value))
