"""Real native boundary + SDK with a nonblocking submitter, not a model fixture."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

SOURCE = Path(__file__).resolve().parents[1]
BUILD = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(SOURCE / "clients/python"))
from evolution_local import LocalEvolution

os.environ["INTEGRATION_TEST_A"] = "fixture-principal-alpha-00000001"
os.environ["INTEGRATION_TEST_B"] = "fixture-principal-bravo-00000002"
ROOT = Path(tempfile.mkdtemp(prefix="dispatch-client.", dir=BUILD))
subprocess.run([str(BUILD / "test_integration_service"), "--fixture", str(ROOT)],
               check=True, capture_output=True, timeout=20)
config = json.loads((ROOT / "service.json").read_text())
config["auto_evaluate"] = False
config["runtime_options"]["loop_rules"] = []
config["runtime_options"]["schedule_rules"] = [dict(
    rule_id="prepare-first", target_scope="agent:my-agent/actor:assistant",
    before_operation="prepare", after_operation="image", require_success=True,
    reason="Prepare before generating an image")]
(ROOT / "service.json").write_text(json.dumps(config))
BINDING = dict(items="/jobs", operation="/name", receipt="/ticket", accepted="/accepted",
               accepted_index="/index", accepted_id="/id", results="/results", result_id="/id",
               result_success="/ok", result_operation="/name", result_status="/status", result_output="/output")


def scenario(c, run, level="L2", failure=False, text=False, reject=False):
    captured = c.point(dict(phase="before", kind="run", ref=dict(run=run), input="test",
                            level=level, capabilities=["schedule"]))
    assert not captured.get("degraded"), captured
    calls, waves = [], []

    def submit(payload):
        calls.append([item["name"] for item in payload["jobs"]])
        ticket = "ticket-" + str(len(calls))
        ack = dict(ticket=ticket, accepted=[] if reject else [
            dict(index=i + 1, id=ticket + ":" + str(i + 1)) for i in range(len(payload["jobs"]))])
        waves.append(dict(ticket=ticket, results=[
            dict(id=item["id"], name=payload["jobs"][i]["name"], ok=not (failure and payload["jobs"][i]["name"] == "prepare"))
            for i, item in enumerate(ack["accepted"])]))
        return json.dumps(ack) if text else ack

    fmt = "json_text" if text else "json"
    output = c.dispatch(dict(name="submit", ref=dict(run=run, key="dispatch"), binding=BINDING,
                             input=dict(jobs=[dict(name="image"), dict(name="prepare"), dict(name="report")], reason="unchanged")),
                        submit, result_format=fmt)
    ack = json.loads(output) if text else output
    assert isinstance(ack, dict), "JSON text must not be encoded twice"
    if reject:
        assert len(calls) == 1 and not ack["accepted"], calls
    else:
        assert calls == ([["image", "prepare", "report"]] if level == "L0" else [["prepare", "report"]]), calls
        first = waves[0]
        first["results"].reverse()  # Out-of-order branch arrival must preserve logical ordering.
        result = c.resume_dispatch(dict(ref=dict(run=run, key="complete-1"), input=first), submit, result_format=fmt)
        if level == "L2" and not failure:
            assert result["pending"] and calls == [["prepare", "report"], ["image"]], (result, calls)
            duplicate = c.resume_dispatch(dict(ref=dict(run=run, key="duplicate-complete"), input=first), submit, result_format=fmt)
            assert duplicate["pending"] and len(calls) == 2, "duplicate completion redispatched a skill"
            result = c.resume_dispatch(dict(ref=dict(run=run, key="complete-2"), input=waves[1]), submit, result_format=fmt)
        assert not result["pending"] and result["completion"]["ticket"] == ack["ticket"], result
        assert [entry["name"] for entry in result["completion"]["results"]] == ["image", "prepare", "report"]
        if failure:
            assert result["completion"]["results"][0]["status"] == "skipped"
            assert result["completion"]["results"][2]["ok"]
    closed = c.point(dict(phase="after", kind="run", ref=dict(run=run), output="done"))
    assert closed.get("closed") and closed.get("capture_complete"), closed


c = LocalEvolution(BUILD / "libtrajectory_evolution_local.so", ROOT / "service.json",
                   os.environ["INTEGRATION_TEST_A"], background=False)
try:
    scenario(c, "ordered")
    scenario(c, "unchanged", level="L0")
    scenario(c, "isolated-failure", failure=True)
    scenario(c, "json-text", text=True)
    scenario(c, "no-submission", reject=True, text=True)
    assert c.metrics()["degraded_calls"] == 0, c.metrics()
finally:
    c.close()
print("Dispatch SDK: ordering, L0, failure isolation, duplicate delivery and JSON text passed")

# A new host capability must produce a freshly validated policy, without
# regenerating memories or silently retaining the old immutable policy ID.
compile_request = dict(
    memory_path=str(ROOT / "data/by-user/user-a/memory/evolution-memory.pb"),
    identity=dict(run_id="compile", agent_id="my-agent", user_id="user-a", conversation_id="compile"),
    shadow_options=dict(config["shadow_options"], target_scopes=["agent:my-agent/actor:assistant"]),
    options=dict(requested_level="L1", allowed_users=["user-a"], fallback="strict",
                 host_capabilities=["context.request"], max_context_bytes=4096,
                 experience_template="{{experience}}"))
compiled = subprocess.run([str(BUILD / "trajectory_evolution"), "runtime", "compile"],
                          input=json.dumps(compile_request), text=True, capture_output=True, timeout=20)
assert compiled.returncode == 0, compiled.stdout
original = json.loads(compiled.stdout)
options = dict(original["options"], requested_level="L2", host_capabilities=["context.request", "schedule.dispatch"])
request = dict(policy=original, options=options)
recompiled = subprocess.run([str(BUILD / "trajectory_evolution"), "runtime", "recompile"],
                            input=json.dumps(request), text=True, capture_output=True, timeout=20)
assert recompiled.returncode == 0, recompiled.stdout
updated = json.loads(recompiled.stdout)
assert updated["snapshot"] == original["snapshot"]
assert updated["policy_id"] != original["policy_id"] and updated["effective_level"] == "L2"
assert "schedule.dispatch" in updated["options"]["host_capabilities"]
for invalid in (dict(request, memory_path="unexpected.pb"),
                dict(request, policy=dict(original, policy_id="runtime-v2:invalid")),
                dict(request, options=dict(options, host_capabilities=[]))):
    rejected = subprocess.run([str(BUILD / "trajectory_evolution"), "runtime", "recompile"],
                              input=json.dumps(invalid), text=True, capture_output=True, timeout=20)
    assert rejected.returncode != 0, "Invalid recompile request was accepted"
print("Policy recompile: unchanged memory, new capability, integrity and strict capability checks passed")
