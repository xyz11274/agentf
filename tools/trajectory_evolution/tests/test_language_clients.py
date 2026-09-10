"""Run the same L0-L3 contract against native language clients and the real C++ server."""
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

SOURCE = Path(__file__).resolve().parents[1]
BUILD = Path(sys.argv[1]).resolve()
ROOT = Path(tempfile.mkdtemp(prefix="language-clients.", dir=BUILD))
TOKEN = "fixture-principal-alpha-00000001"
ENV = dict(os.environ, INTEGRATION_TEST_A=TOKEN, INTEGRATION_TEST_B="fixture-principal-bravo-00000002", PYTHONDONTWRITEBYTECODE="1")


def read_line(process, timeout=12):
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        if not selector.select(timeout):
            raise AssertionError("native client or service did not respond within the test deadline")
    line = process.stdout.readline()
    if not line:
        raise AssertionError("native client or service exited early")
    return line


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


def scenario(language, command, url, schema=None, response_schema=None):
    env = dict(ENV, EVOLUTION_URL=url, EVOLUTION_TEST_TOKEN=TOKEN)
    with (ROOT / (language + ".stderr")).open("w", encoding="utf-8") as errors:
        client = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors, text=True, encoding="utf-8", env=env, bufsize=1)
        def call(operation, fields, request_id=None, status=200):
            request = dict(fields, operation=operation, request_id=request_id or uuid.uuid4().hex)
            if schema:
                schema.validate(request)
            client.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
            client.stdin.flush()
            reply = json.loads(read_line(client))
            if status != 200:
                assert reply["ok"] is False and reply["status"] == status, (language, operation, reply)
                return reply
            assert reply["ok"] is True, (language, operation, reply)
            if response_schema:
                response_schema.validate(reply)
            return reply["result"]
        try:
            run_ids = []
            for level in range(4):
                conversation = "多语言会话-" + language if level < 3 else "会" * 38
                begin = dict(conversation_id=conversation, input="视频生成缺少素材", level="L" + str(level), capabilities=["model", "actions", "loop"])
                rid = uuid.uuid4().hex
                run = call("task.begin", begin, rid)
                assert call("task.begin", begin, rid) == run
                call("task.begin", dict(begin, input="不允许使用同一 ID 偷换内容"), rid, 409)
                run_ids.append(run["run_id"])
                common = {"run_id": run["run_id"]}
                calls = 0
                decisions = []
                for iteration in range(3 if level == 3 else 1):
                    step = call("step.begin", dict(common, state={"result": "缺少素材"}))
                    decisions.append(step["decision"])
                    fields = dict(common, step_id=step["step_id"])
                    if step["decision"] != "stop":
                        model = call("model.prepare", dict(fields, request={"system": "原始中文指令"}))
                        assert (model["request"]["system"] != "原始中文指令") == (level > 0)
                        call("model.observe", dict(fields, call_id=model["call_id"], request=model["request"], output={"next": "tools"}, status="completed", transport_allowed=True))
                        plans = call("actions.prepare", dict(fields, actions=[
                            {"id": "success", "operation": "lookup", "arguments": {"query": "资料"}},
                            {"id": "failure", "operation": "lookup", "arguments": {"query": "超时分支"}},
                            {"id": "blocked", "operation": "video", "arguments": {}},
                        ]))["actions"]
                        assert [p["id"] for p in plans] == ["success", "failure", "blocked"]
                        assert plans[2]["decision"] == ("reject" if level >= 2 else "allow")
                        # The same transport must preserve original call IDs despite reverse completion.
                        for plan in reversed(plans):
                            executed = plan["decision"] == "allow"
                            outcome = "failed" if plan["id"] == "failure" else ("completed" if executed else "cancelled")
                            call("action.observe", dict(fields, call_id=plan["call_id"], output={"host_id": plan["id"]}, status=outcome, executed=executed))
                        call("state.record", dict(fields, before={"result": "缺少素材"}, after={"result": "缺少素材"}))
                        calls += 1
                    call("step.end", dict(fields, status="completed", decision=step["decision"]))
                if level == 3:
                    assert decisions == ["continue", "replan", "stop"], (language, decisions)
                    assert calls == 2
                end = call("task.end", dict(common, status="completed", business_status="partial", output="独立任务已完成，素材不足的分支未完成"))
                assert end["capture_complete"] is True
                status = call("task.status", common)
                assert status["closed"] and status["capture_complete"] and status["observed_boundaries"] == {"model": True, "actions": True, "loop": True}
            # Every native client uses the same cross-process context, not a language-specific Agent wrapper.
            delegated = call("task.begin", dict(conversation_id="delegated-" + language, input="并发委派", level="L0", capabilities=[]))
            common = {"run_id": delegated["run_id"]}
            step = call("step.begin", dict(common, state="dispatch"))
            plans = call("actions.prepare", dict(common, step_id=step["step_id"], actions=[
                {"id": name, "operation": "delegate", "arguments": {},
                 "delegate": {"actor_id": "worker", "execution_id": name, "completion": "delivery"}}
                for name in ["child-a", "child-b"]]))["actions"]
            for plan in plans:
                call("action.observe", dict(common, step_id=step["step_id"], call_id=plan["call_id"], executed=True, output="delivered"))
            step = call("step.begin", dict(common, state="sender continues"))
            call("task.end", dict(common, output="too soon"), status=409)
            for plan in reversed(plans):
                context = plan["context"]
                call("step.begin", dict(context, state="received"))
                call("execution.end", dict(common, execution_id=context["execution_id"], output=plan["id"], status="completed"))
                assert call("execution.status", dict(common, execution_id=context["execution_id"]))["ended"]
            assert call("task.end", dict(common, output="done"))["capture_complete"]
            # The neutral profile has no required role, execution tree or serial step.
            neutral = call("run.begin", dict(input="视频生成缺少素材", level="L3", capabilities=["model", "actions", "loop"],
                extensions={"client_language": language}))
            common = {"run_id": neutral["run_id"]}
            assert call("contract.describe", {})["protocol"] == "evolution.boundary.v1"
            control = call("loop.check", dict(common, loop_id="callback-loop", state="ready"))
            call("loop.commit", dict(common, control_id=control["control_id"], decision=control["decision"]))
            model = call("activity.begin", dict(common, kind="model", name="native-provider", input="原有中文指令",
                actor_id="dynamic-" + language, control_id=control["control_id"], binding={"pointer": "", "mode": "text"}))
            action = call("activity.begin", dict(common, kind="action", name="lookup", input={"query": "并行"}))
            call("activity.end", dict(common, activity_id=action["activity_id"], status="failed", executed=True, output="分支业务失败"))
            assert "先补充图片" in model["input"]
            call("activity.end", dict(common, activity_id=model["activity_id"], status="completed", executed=True,
                actual_input=model["input"], output="另一个分支继续成功"))
            call("event.record", dict(common, type="domain.metric", data={"score": 0.9}))
            assert call("run.end", dict(common, status="completed", business_status="partial", output="完成"))["capture_complete"]
            assert call("run.status", common)["closed"]
            # Existing host IDs suffice; no context map, callback adapter or merge code.
            reference = {"run": "keyed-" + language}
            keyed = call("point", dict(phase="before", kind="run", ref=reference, input="视频生成缺少素材",
                         level="L3", capabilities=["model", "actions", "loop"]))
            decisions = []
            for i in range(3):
                control = call("point", dict(phase="guard", kind="loop", ref=dict(reference, key=f"round-{i}"), state="same"))
                decisions.append(control["decision"])
                if control["decision"] != "stop":
                    model_ref = dict(reference, key=f"model-{i}")
                    prepared = call("point", dict(phase="before", kind="model", ref=model_ref, input="先检查素材",
                                    binding={"pointer": "", "mode": "text"}))
                    call("point", dict(phase="after", kind="model", ref=model_ref, actual_input=prepared["input"],
                         executed=True, output="need image"))
            assert decisions == ["continue", "replan", "stop"], (language, decisions)
            closed = call("point", dict(phase="after", kind="run", ref=reference, output="done"))
            assert closed["closed"] and closed["capture_complete"], (language, closed)
            # Explicit privacy failure is never treated as a successful observation.
            private = call("task.begin", dict(conversation_id="privacy-" + language, input="安全数据", level="L0", capabilities=[]))
            fields = {"run_id": private["run_id"]}
            step = call("step.begin", dict(fields, state={}))
            model = call("model.prepare", dict(fields, step_id=step["step_id"], request={"system": "safe"}))
            call("model.observe", dict(fields, step_id=step["step_id"], call_id=model["call_id"], request=model["request"], output={"api_key": "fixture-must-never-be-written"}, status="completed", transport_allowed=True), status=400)
            call("capture.gap", dict(fields, reason="privacy_rejected"))
            closed = call("task.abort", dict(fields, status="unknown"))
            assert closed["capture_complete"] is False and closed["evaluation_status"] == "quarantined"
            return {"language": language, "levels": ["L0", "L1", "L2", "L3"], "run_ids": run_ids,
                "reverse_completion": True, "isolated_branch_failure": True, "privacy_quarantine": True,
                "delegated_background_context": True, "neutral_boundary_protocol": True, "keyed_points": True}
        finally:
            client.stdin.close()
            stop(client)


def main():
    subprocess.run([str(BUILD / "test_integration_service"), "--fixture", str(ROOT)], check=True, env=ENV, capture_output=True)
    config_path = ROOT / "service.json"
    config = json.loads(config_path.read_text(encoding="utf-8"))
    config["auto_evaluate"] = False  # Model evaluation has its separate end-to-end HTTP fixture test.
    config_path.write_text(json.dumps(config, ensure_ascii=False), encoding="utf-8")
    schema, response_schema = None, None
    try:
        import jsonschema
        schema = jsonschema.Draft202012Validator(json.loads((SOURCE / "api/invoke-v1.schema.json").read_text(encoding="utf-8")))
        document = json.loads((SOURCE / "api/openapi.json").read_text(encoding="utf-8"))
        response_schema = jsonschema.Draft202012Validator({"$ref": "#/components/schemas/InvokeResponse", "components": document["components"]})
    except ImportError:
        pass
    probes = {"python": [sys.executable, "-B", str(SOURCE / "tests/clients/probe.py")],
        "c": [str(BUILD / "test_client_c")], "cpp": [str(BUILD / "test_client_cpp")]}
    node = shutil.which("node")
    if node:
        probes["javascript"] = [node, str(SOURCE / "tests/clients/probe.mjs")]
    if os.environ.get("EVOLUTION_LANGUAGE_PROBES"):
        probes.update(json.loads(os.environ["EVOLUTION_LANGUAGE_PROBES"]))
    with (ROOT / "service.stderr").open("w", encoding="utf-8") as errors:
        server = subprocess.Popen([str(BUILD / "trajectory_evolution"), "serve", "--config", str(config_path)], stdout=subprocess.PIPE, stderr=errors, text=True, encoding="utf-8", env=ENV)
        try:
            port = re.search(r"listening=127\.0\.0\.1:(\d+)", read_line(server)).group(1)
            results = [scenario(name, command, "http://127.0.0.1:" + port, schema, response_schema) for name, command in probes.items()]
            # task.end persists the recovery state first; the background worker
            # publishes trace files even when model/rule evaluation is disabled.
            for _ in range(100):
                published = list((ROOT / "data/by-user").glob("*/traces/**/*.trace.json"))
                quarantined = list((ROOT / "data/by-user").glob("*/quarantine/**/*.trace.json"))
                if len(published) == len(results) * 7 and len(quarantined) == len(results):
                    break
                time.sleep(0.05)
            assert len(published) == len(results) * 7 and len(quarantined) == len(results), "background trace publication did not finish"
        finally:
            stop(server)
    traces = list((ROOT / "data/by-user").glob("*/traces/**/*.trace.json"))
    assert len(traces) == len(results) * 7
    for trace in traces:
        result = subprocess.run([str(BUILD / "trajectory_evolution"), "doctor", str(trace), "--level", "L0"], capture_output=True, text=True, env=ENV)
        assert result.returncode == 0, result.stderr
    for file in ROOT.rglob("*"):
        if file.is_file():
            content = file.read_bytes()
            assert TOKEN.encode() not in content and b"fixture-must-never-be-written" not in content, str(file)
    summary = {"clients": results, "trace_count": len(traces), "schema_validated": schema is not None,
        "scope": "Native transport plus L0-L3 lifecycle; model provider is not called in this suite.", "output": str(ROOT)}
    (ROOT / "test-summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False))


if __name__ == "__main__":
    main()
