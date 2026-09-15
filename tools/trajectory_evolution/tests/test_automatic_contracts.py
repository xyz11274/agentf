"""Generic points discover wire metadata without host adapters or tool lists."""
import asyncio
import json
import os
from pathlib import Path
import re
import selectors
import subprocess
import sys
import tempfile
import time
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[1]
BUILD = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(SOURCE / "clients/python"))
from evolution import Evolution
from evolution_local import LocalEvolution
from evolution_configured import ConfiguredPoints, _LocalPoints

TOKEN = "fixture-principal-alpha-00000001"
os.environ.update(INTEGRATION_TEST_A=TOKEN, INTEGRATION_TEST_B="fixture-principal-bravo-00000002")


def scenario(client):
    sequence = 0
    run = "automatic-contracts"

    def point(phase, kind, key=None, parent=None, **fields):
        ref = {"run": run}
        if key is not None:
            ref["key"] = key
        if parent is not None:
            ref["parent"] = parent
        result = client.invoke("point", dict(phase=phase, kind=kind, ref=ref, **fields))
        assert not result.get("degraded"), result
        return result

    def model(payload, branch, usage=None):
        nonlocal sequence
        sequence += 1
        key = f"model-{sequence}"
        prepared = point("before", "model", key, branch, input=payload)
        point("after", "model", key, input=prepared["input"], actual_input=prepared["input"],
              executed=True, output={"usage": usage or {"prompt_tokens": 10, "completion_tokens": 2}})
        return prepared

    def action(arguments, branch, operation="calculate", allow=True, apply=True):
        nonlocal sequence
        sequence += 1
        key = f"action-{sequence}"
        prepared = point("before", "action", key, branch, name=operation, input=arguments, apply_policy=apply)
        assert prepared["decision"] == ("allow" if allow else "reject"), prepared
        if not allow:
            assert "tool_required_arguments_missing" in prepared["reason"], prepared
        point("after", "action", key, executed=allow, output="executed" if allow else "rejected")

    point("before", "run", level="L2", input="视频生成缺少图片素材")
    for branch in ("left", "right", "ambiguous", "optional", "anthropic", "responses", "google", "legacy"):
        point("before", "custom", branch, input=branch)
    schema = {"type": "object", "properties": {"value": {"type": ["string", "null"]}}, "required": ["value"]}
    payload = {"messages": [{"role": "user", "content": "calculate"}], "tools": [
        {"type": "function", "function": {"name": "calculate", "parameters": schema}}]}
    prepared = model(payload, "left")
    assert "先补充图片" in json.dumps(prepared["input"], ensure_ascii=False), prepared
    action({}, "left", allow=False)
    action({"value": ""}, "left")  # JSON Schema required means present, not nonempty.
    action({"value": None}, "left")
    action({}, "left", operation="unseen")
    action({}, "left", apply=False)

    other = {"messages": [], "tools": [{"type": "function", "function": {
        "name": "calculate", "parameters": {"type": "object", "required": ["other"]}}}]}
    # Same name, different concurrent branch: no shared whitelist or last-writer win.
    model(other, "right")
    action({"other": 1}, "right")
    action({"value": 1}, "left")
    action({"value": 1}, "right", allow=False)
    model(payload, "ambiguous")
    model(other, "ambiguous")
    action({}, "ambiguous")

    optional = {"messages": [], "tools": [{"type": "function", "function": {
        "name": "calculate", "parameters": {"type": "object", "properties": {"value": {"type": "string"}}}}}]}
    model(optional, "optional")
    action({}, "optional")
    model({"messages": [], "tools": [{"name": "calculate", "input_schema": schema}]}, "anthropic",
          {"input_tokens": 3, "output_tokens": 2, "cache_read_input_tokens": 5, "cache_creation_input_tokens": 2})
    action({}, "anthropic", allow=False)
    model({"input": "calculate", "tools": [{"type": "function", "name": "calculate", "parameters": schema}]}, "responses")
    action({}, "responses", allow=False)
    model({"contents": [], "systemInstruction": {"parts": [{"text": "calculate"}]}, "tools": [
        {"functionDeclarations": [{"name": "calculate", "parameters": schema}]}]}, "google")
    action({}, "google", allow=False)
    model({"messages": [], "functions": [{"name": "calculate", "parameters": schema}]}, "legacy")
    action({}, "legacy", allow=False)

    # A tool-free Messages request may be OpenAI or Anthropic. Do not add roles.
    for content in ("视频生成缺少图片素材", [{"type": "text", "text": "视频生成缺少图片素材"}]):
        prepared = model({"messages": [{"role": "user", "content": content}]}, None)
        assert len(prepared["input"]["messages"]) == 1 and "system" not in prepared["input"], prepared
        assert prepared["input"]["messages"][0]["role"] == "user", prepared
        assert "先补充图片" in json.dumps(prepared["input"], ensure_ascii=False), prepared

    # Explicit bindings/metrics remain overrides; no migration of custom formats.
    prepared = point("before", "model", "explicit", input={"body": "original"}, binding={"pointer": "/body", "mode": "text"})
    point("after", "model", "explicit", actual_input=prepared["input"], executed=True,
          output={"usage": {"prompt_tokens": 90, "completion_tokens": 2}}, metrics={"input_tokens": 10})
    for branch in ("left", "right", "ambiguous", "optional", "anthropic", "responses", "google", "legacy"):
        point("after", "custom", branch, output="done")
    closed = point("after", "run", output="done")
    assert closed["closed"] and closed["capture_complete"], closed
    assert closed["metrics"]["llm_calls"] == 12, closed["metrics"]
    assert closed["metrics"]["input_tokens"] == 120 and closed["metrics"]["output_tokens"] == 24, closed["metrics"]

    run = "isolated-next-run"
    point("before", "run", level="L2", input="no inherited contracts")
    action({}, None)
    point("after", "run", output="done")
    run = "observe-only"
    point("before", "run", level="L0", input="baseline preserves calls")
    model(payload, None)
    action({}, None)
    assert point("after", "run", output="done")["capture_complete"]

    run = "invalid-contract-metadata"
    point("before", "run", level="L2", input="unsupported metadata stays unknown")
    malformed = {"messages": [], "tools": [{"type": "function", "function": {
        "name": "calculate", "parameters": {"required": "not-an-array"}}}]}
    model(malformed, None)
    action({}, None)
    duplicate = {"messages": [], "tools": payload["tools"] + other["tools"]}
    model(duplicate, None)
    action({}, None)
    # Only actual_input may establish the tool contract, not the prepared input.
    prepared = point("before", "model", "changed-wire", input=payload)
    point("after", "model", "changed-wire", executed=True, actual_input={"messages": []}, output={
        "usage": {"prompt_tokens": "unknown", "completion_tokens": -1}})
    action({}, None)
    closed = point("after", "run", output="done")
    assert closed["capture_complete"] and "tokens" not in closed["metrics"], closed

    run = "provider-usage"
    point("before", "run", level="L1", input="independent thinking usage")
    prepared = point("before", "model", "usage", input="a plain prompt")
    point("after", "model", "usage", executed=True, actual_input=prepared["input"], output={
        "usageMetadata": {"promptTokenCount": 10, "candidatesTokenCount": 2,
                          "thoughtsTokenCount": 3, "totalTokenCount": 15}})
    closed = point("after", "run", output="done")
    assert closed["metrics"]["input_tokens"] == 10 and closed["metrics"]["output_tokens"] == 5, closed
    assert closed["metrics"]["tokens"] == 15, closed

    run = "bounded-tool-catalog"
    point("before", "run", level="L2", input="changing tool sets stay bounded")
    for start in (0, 33):
        tools = [{"type": "function", "function": {"name": f"op{i}", "parameters": schema}}
                 for i in range(start, start + 33)]
        model({"messages": [], "tools": tools}, None)
    action({}, None, operation="op33")
    assert point("after", "run", output="done")["capture_complete"]


async def configured_async():
    observed = []
    async def execute(value):
        observed.append(value)
        return value
    with patch.dict(os.environ, {"EVOLUTION_POINT_CONFIG": "", "TRAJECTORY_EVOLUTION_TRIAL_CONFIG": ""}):
        client = ConfiguredPoints()
        payload = {"original": True}
        assert await client.acall({"input": payload}, execute) is payload
        assert await client.aallow_loop({"state": "same"})
    forwarded = []
    def point(fields, **options):
        forwarded.append(fields)
        return {"input": fields.get("input"), "decision": "allow"}
    real = _LocalPoints("http://127.0.0.1:1", TOKEN)
    client = ConfiguredPoints()
    with patch.object(client, "_client", return_value=real), patch.object(real, "point", side_effect=point):
        fields = {"ref": {"run": "async"}, "kind": "action", "input": {"value": 1}}
        assert await client.acall(fields, execute) == {"value": 1}
        assert "key" not in fields["ref"]
        assert forwarded[0]["ref"] == forwarded[1]["ref"]
        assert forwarded[1]["executed"]
    assert len(observed) == 2


def main():
    with tempfile.TemporaryDirectory(prefix="automatic-contracts.", dir=BUILD) as root:
        for transport in ("native", "http"):
            folder = Path(root) / transport
            subprocess.run([str(BUILD / "test_integration_service"), "--fixture", str(folder)], check=True, capture_output=True)
            path = folder / "service.json"
            config = json.loads(path.read_text())
            config.update(auto_evaluate=False, instrumentation={"level": "L2", "capabilities": ["model", "actions"]})
            config["runtime_options"]["action_rules"] = []
            path.write_text(json.dumps(config))
            server = None
            if transport == "native":
                client = LocalEvolution(BUILD / "libtrajectory_evolution_local.so", path, TOKEN, contention_budget_us=10000)
            else:
                server = subprocess.Popen([str(BUILD / "trajectory_evolution"), "serve", "--config", str(path)],
                                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
                with selectors.DefaultSelector() as selector:
                    selector.register(server.stdout, selectors.EVENT_READ)
                    assert selector.select(15), "service startup timeout"
                port = re.search(r"listening=127.0.0.1:(\d+)", server.stdout.readline()).group(1)
                client = Evolution("http://127.0.0.1:" + port, TOKEN, retries=0)
            try:
                scenario(client)
                if transport == "native":
                    client.drain()
                for _ in range(100):
                    paths = list((folder / "data").rglob("*.trace.json"))
                    if len(paths) >= 6:
                        break
                    time.sleep(0.02)
                traces = [json.loads(p.read_text()) for p in paths]
                assert len(traces) == 6, paths
                for trace_path in paths:
                    result = subprocess.run(
                        [str(BUILD / "trajectory_evolution"), "doctor", str(trace_path), "--level", "L0"],
                        capture_output=True, text=True, timeout=15)
                    assert result.returncode == 0, result.stderr
                    assert TOKEN not in trace_path.read_text()
                checks = [e["attributes"]["contract_check"] for t in traces for e in t["events"]
                          if "contract_check" in e.get("attributes", {})]
                assert any(c["reason"] == "tool_definition_ambiguous" for c in checks), checks
                assert any(c["reason"] == "tool_definition_capacity" for c in checks), checks
                assert any(c["enforced"] for c in checks), checks
            finally:
                if server:
                    server.terminate()
                    try:
                        server.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait(timeout=3)
                else:
                    client.close()
        asyncio.run(configured_async())
    print("Automatic contracts: native/HTTP, formats, optional fields, conflicts, isolation, usage and configured async passed")


if __name__ == "__main__":
    main()
