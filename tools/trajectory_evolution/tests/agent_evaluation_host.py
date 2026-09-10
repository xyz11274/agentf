"""Run a small instrumented Agent in an isolated evaluation instance."""
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
if mode == "slow":
    time.sleep(2)
artifact = request["artifact"]
if mode == "gepa_merge_pause":
    mode = "gepa_merge"
    options = (artifact.get("policy") or {}).get("options", {})
    if options.get("experience_overrides") and any("source_experience" in r for r in options.get("action_rules", [])):
        Path(os.environ["GEPA_MERGE_READY"]).write_text("ready", encoding="utf-8")
        deadline = time.monotonic() + 30
        while not Path(os.environ["GEPA_MERGE_RELEASE"]).exists() and time.monotonic() < deadline:
            time.sleep(.05)
workspace = Path.cwd()
version_file = workspace / "version.json"
version_file.write_text(json.dumps(dict(version=request["version"], artifact=artifact, trial_id=request["trial_id"])))
config = json.loads((SOURCE / "config/service.json").read_text())
config.update(agent_id="eval-test", data_dir=str(workspace / "state"),
              principals=[dict(user_id=artifact["user_id"], token_env="EVALUATION_INSTANCE_TOKEN")],
              evaluation_version_file=str(version_file),
              runtime_config=str(SOURCE / "config/standard-runtime.json"),
              experience_template_file=str(SOURCE / "config/prompts/runtime-experience.txt"))
config_file = workspace / "service.json"
config_file.write_text(json.dumps(config))
token = "isolated-evaluation-token-123456789"
os.environ["EVALUATION_INSTANCE_TOKEN"] = token
evo = LocalEvolution(library, config_file, token)
level = artifact["level"]
capabilities = ["model", "actions", "loop"][:int(level[1])]
run = evo.invoke("run.begin", dict(input=request["task"]["task_input"], level=level, capabilities=capabilities))
rid = run["run_id"]
model_input = {"messages": [{"role": "user", "content": request["task"]["task_input"]}]}
call = evo.invoke("activity.begin", dict(run_id=rid, kind="model", name="fixture-model", input=model_input,
    binding={"pointer": "/messages", "mode": "system_message"}))
result = 0 if mode == "bad_candidate" and level != "L0" else 20
if mode in ("memory_bad", "memory_good"):
    injected = call["input"] != model_input
    result = 20 if injected == (mode == "memory_good") else 0
if mode in ("gepa", "gepa_holdout_failure", "gepa_merge", "gepa_merge_regression"):
    # Only the request actually produced by the point can affect this fixture.
    text = json.dumps(call["input"], ensure_ascii=False)
    family = "甲类" if "甲类" in request["task"]["task_input"] else "乙类"
    result = 20 if "核验" + family in text else 0
    if mode == "gepa_holdout_failure" and "HOLDOUT_PRIVATE" in request["task"]["task_input"]:
        result = 0
if mode in ("gepa_actions", "gepa_loops"):
    result = 0
evo.invoke("activity.end", dict(run_id=rid, activity_id=call["activity_id"], status="completed",
                               output=result, actual_input=call["input"], executed=True,
                               **({"metrics": {"tokens": 100}} if mode.startswith("gepa") else {})))
action = evo.invoke("activity.begin", dict(run_id=rid, kind="action", name="calculate", input={"value": result}))
blocked = mode in ("gepa_actions", "gepa_merge", "gepa_merge_regression") and action["decision"] == "reject"
if blocked and (mode == "gepa_actions" or family == "乙类"):
    result = 20
if mode == "gepa_merge_regression" and blocked and "核验甲类" in text:
    result = 0
evo.invoke("activity.end", dict(run_id=rid, activity_id=action["activity_id"], status="cancelled" if blocked else "completed",
                               output=result, executed=not blocked))
if mode == "gepa_loops":
    for iteration in range(4):
        control = evo.invoke("loop.check", dict(run_id=rid, loop_id="bounded-calculation", state={"iteration": iteration}))
        evo.invoke("loop.commit", dict(run_id=rid, control_id=control["control_id"], decision=control["decision"]))
        if control["decision"] == "stop":
            result = 20
            break
for followup in request["task"]["followup_inputs"]:
    activity = evo.invoke("activity.begin", dict(run_id=rid, kind="model", name="fixture-followup",
        input={"messages": [{"role": "user", "content": followup}]}, binding={"pointer": "/messages", "mode": "system_message"}))
    evo.invoke("activity.end", dict(run_id=rid, activity_id=activity["activity_id"], status="completed",
                                   output=result, actual_input=activity["input"], executed=True))
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
print(json.dumps(dict(schema_version=1, trial_id=request["trial_id"], execution_status="completed",
    loaded_version="wrong-version" if mode == "wrong_version" else run["agent_version_id"],
    output=str(result), state={"value": result}, trace=trace,
    metrics=dict(steps=3, tool_calls=1, llm_calls=1 + len(request["task"]["followup_inputs"]), tokens=100))))
