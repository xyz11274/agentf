"""Opt-in host regression: real v1 functions + collector, fixture bus/model/tools.

Run from an AgentFlow checkout; not a dependency of the standalone component.
The two workers run in different processes and each uses only existing IDs.
"""
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import re
import selectors
import subprocess
import sys
import tempfile
import time
from unittest.mock import MagicMock, patch

SOURCE = Path(__file__).resolve().parents[1]
REPO = SOURCE.parents[1]
sys.path[:0] = [str(SOURCE / "clients/python"), str(REPO / "packages/agentflow-runtime/src")]


def response(content="", calls=None):
    message = {"role": "assistant", "content": content}
    if calls:
        message["tool_calls"] = calls
    return {"choices": [{"message": message, "finish_reason": "tool_calls" if calls else "stop"}],
            "usage": {"prompt_tokens": 10, "completion_tokens": 5}}


def tool(name, arguments, identity):
    return {"id": identity, "type": "function", "function": {"name": name, "arguments": json.dumps(arguments)}}


class FixtureBus:
    def __init__(self, folder):
        self.folder = folder
        folder.mkdir(exist_ok=True)
        self.worker = os.environ.get("AF_EXEC_SKILL_KEY", "")

    def _path(self, name):
        return self.folder / hashlib.sha256(name.encode()).hexdigest()

    def _read(self, name):
        path = self._path(name)
        return path.read_text() if path.exists() else None

    def _write(self, name, value):
        path = self._path(name)
        temporary = path.with_suffix(f".{os.getpid()}.part")
        temporary.write_text(value)
        temporary.replace(path)
        return True

    def is_connected(self):
        return True

    def get(self, name):
        return self._read("sys:" + self.worker + ":" + name) or self._read("sys::" + name)

    def put(self, name, value):
        return self._write("sys:" + self.worker + ":" + name, value)

    def get_global(self, name):
        return self._read("global:" + name)

    def put_global(self, name, value):
        return self._write("global:" + name, value)

    def delete(self, name):
        self._path("sys:" + self.worker + ":" + name).unlink(missing_ok=True)
        return True

    def response_push(self, *args, **kwargs):
        return True


def node(folder, mode):
    from agentflow_runtime import coordinator_decide as cd, skill_executor as se, llm_call
    from evolution_configured import points
    module = se if mode.startswith("worker") else cd
    bus = FixtureBus(folder / "bus")
    recorder = MagicMock()
    recorder.is_connected = False
    captures, executions = [], []
    def model(messages, *args, **kwargs):
        captures.append(json.loads(json.dumps(messages)))
        if mode == "dispatch":
            return response(calls=[tool("run_skills", {"skills": [
                {"name": "worker-a", "prompt": "视频生成缺少图片素材，并行检索 A"},
                {"name": "worker-b", "prompt": "视频生成缺少图片素材，独立尝试 B"}], "reason": "parallel"}, "dispatch-call")])
        if mode.startswith("worker") and len(captures) == 1:
            return response(calls=[tool("bash", {"command": "echo slow"}, "call-slow"),
                                   tool("bash", {"command": "echo failure"}, "call-failure")]
                            if mode == "worker-a" else [tool("video", {}, "call-video")])
        return response("完成，独立任务结果已汇总。")
    recorder.trace_call.side_effect = model
    recorder.record_effective_response.side_effect = lambda _, raw: raw
    registry = MagicMock()
    registry.get_tools.return_value = [
        {"type": "function", "function": {"name": name, "parameters": {
            "type": "object", "properties": {"command": {"type": "string"}}}}} for name in ("bash", "video")]
    def execute(name, arguments):
        started = time.monotonic()
        time.sleep(0.12 if arguments.get("command") == "echo slow" else 0.02)
        executions.append({"name": name, "arguments": arguments, "start": started, "end": time.monotonic()})
        return json.dumps({"error": "fixture network failure"} if arguments.get("command") == "echo failure"
                          else {"stdout": "lookup complete", "exit_code": 0})
    registry.execute.side_effect = execute
    cfg = MagicMock()
    for method, value in {"coordinator_max_iterations": 6, "max_completion_tokens": 2000,
                          "node_timeout_sec": 30, "skill_max_iterations": 4, "skill_max_completion_tokens": 2000,
                          "node_execution_timeout": 30, "protect_last_rounds": 0, "skill_max_retries": 0,
                          "skill_llm_recovery_attempts": 0, "fold_dir": "", "hard_compression_model": "",
                          "fold_compression_enabled": False, "hard_compression_enabled": False}.items():
        getattr(cfg, method).return_value = value
    with contextlib.ExitStack() as stack:
        replacements = {"DataBusClient": lambda: bus, "_load_dotenv": lambda: None,
                        "load_hooks_from_dir": lambda *args: None, "hook_trigger": lambda _, data, **kwargs: data,
                        "emit_event": lambda *args, **kwargs: None, "AgentConfig": cfg,
                        "parse_agent_md": lambda *args: ({}, "fixture agent"),
                        "compress_messages": lambda messages, *args, **kwargs: (messages, "skip"),
                        "build_round_reminder": lambda **kwargs: None,
                        "resolve_llm_config": lambda *args, **kwargs: {"model": "fixture", "base_url": "http://127.0.0.1:1",
                                                                      "api_key": "fixture-model-credential-never-captured"},
                        "get_recorder": lambda: recorder}
        if module is cd:
            replacements.update(_init_skills=lambda: None, _validate_run_skills_args=lambda *args: "")
        else:
            replacements.update(_inject_venv_env=lambda: None, _resolve_skill_dir=lambda _: folder,
                                load_skills_from_list=lambda _: {}, _LocalToolRegistry=lambda *args: registry,
                                extract_structured_artifacts=lambda *args: [], extract_all_artifacts=lambda *args, **kwargs: [])
        for name, value in replacements.items():
            stack.enter_context(patch.object(module, name, value))
        stack.enter_context(patch.object(llm_call, "get_recorder", lambda: recorder))
        output = io.StringIO()
        if mode in ("stop", "worker-stop"):
            # Host handling of an explicitly committed stop is tested independently
            # from the policy engine's repeated-state tests.
            stack.enter_context(patch.object(points, "allow_loop", return_value=False))
        with contextlib.redirect_stdout(output):
            module.execute()
    (folder / (mode + ".result.json")).write_text(json.dumps({"models": captures, "tools": executions,
                                                             "output": output.getvalue()}, ensure_ascii=False))


def main(build):
    root = Path(tempfile.mkdtemp(prefix="v1-points.", dir=build))
    subprocess.run([str(build / "test_integration_service"), "--fixture", str(root)], check=True, capture_output=True)
    path = root / "service.json"
    config = json.loads(path.read_text())
    config.update(auto_evaluate=False, instrumentation={"level": "L3", "capabilities": ["model", "actions", "loop"],
        "binding": {"pointer": "/messages", "mode": "system_message"},
        "output_metrics": {"input_tokens": "/usage/prompt_tokens", "output_tokens": "/usage/completion_tokens"}},
        http_persistence={"mode": "buffered", "max_buffer_bytes": 67108864})
    config["runtime_options"]["loop_rules"][0]["max_no_progress"] = 6
    path.write_text(json.dumps(config))
    env = {**os.environ, "INTEGRATION_TEST_A": "fixture-principal-alpha-00000001",
           "INTEGRATION_TEST_B": "fixture-principal-bravo-00000002", "AF_SKIP_DOTENV": "1",
           "AF_COORDINATOR_VERSION": "v1", "AF_USER_ID": "user-a", "AF_CLIENT_ID": "v1-conversation",
           "AF_DIALOGUE_ID": "v1-process-test", "AF_SESSION_ID": "0", "AF_COMPLETED_SKILL_NAME": "",
           "AF_COMPLETED_BATCH_KEY": "", "AF_EXEC_SKILL_KEY": "", "AF_DATABUS_UDS_FILE": "",
           "AF_TRACE_OUTPUT_DIR": str(root / "host-traces"), "AF_INPUT": "视频生成缺少图片素材，请并发执行并汇总。"}
    for name in ("TRAJECTORY_EVOLUTION_TRIAL_URL", "TRAJECTORY_EVOLUTION_TRIAL_TOKEN"):
        env.pop(name, None)
    server = subprocess.Popen([str(build / "trajectory_evolution"), "serve", "--config", str(path)], env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(server.stdout, selectors.EVENT_READ)
            assert selector.select(15), "collector startup timeout"
        port = re.search(r"listening=127.0.0.1:(\d+)", server.stdout.readline()).group(1)
        client_config = root / "client.json"
        client_config.write_text(json.dumps({"url": "http://127.0.0.1:" + port, "principals": config["principals"],
                                            "point_timeout_ms": 1000,
                                            "privacy": {"public_env_names": ["AF_EXEC_SKILL_KEY", "AF_COMPLETED_BATCH_KEY"]}}))
        env["EVOLUTION_POINT_CONFIG"] = str(client_config)
        def launch(mode, extra=None):
            return subprocess.Popen([sys.executable, "-B", __file__, "--node", str(root), mode],
                                    env={**env, **(extra or {})}, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        def finish(process):
            out, err = process.communicate(timeout=30)
            assert process.returncode == 0, (out, err)
        finish(launch("dispatch"))
        bus = FixtureBus(root / "bus")
        specs = json.loads(bus.get("next_skills"))
        assert len(specs) == 2
        assert all(set(spec) == {"name", "exec_skill_key", "batch_id", "force"} for spec in specs)
        workers = [launch(mode, {"AF_EXEC_SKILL_KEY": spec["exec_skill_key"]})
                   for mode, spec in zip(("worker-a", "worker-b"), specs)]
        for worker in reversed(workers):
            finish(worker)
        finish(launch("complete", {"AF_COMPLETED_SKILL_NAME": "worker-a", "AF_SKILL_SUCCESS": "true",
                                   "AF_SKILL_RESULT": "两个工作单元均已返回，检索中有一次网络错误。"}))
        traces = root / "data/by-user/user-a/traces/v1-conversation"
        for _ in range(100):
            paths = list(traces.glob("*.trace.json"))
            if paths:
                break
            time.sleep(0.02)
        assert len(paths) == 1, list(root.rglob("*.trace.json"))
        trace = json.loads(paths[0].read_text())
        metrics = trace["attributes"]["metrics"]
        assert metrics["llm_calls"] == 6, metrics
        assert metrics["tool_calls"] == 2, metrics
        assert metrics["tokens"] == 90, metrics
        assert trace["events"][-1]["kind"] == "run.end"
        for mode in ("dispatch", "worker-a", "worker-b", "complete"):
            result = json.loads((root / (mode + ".result.json")).read_text())
            assert "先补充图片" in json.dumps(result["models"], ensure_ascii=False), mode
            if mode == "worker-a":
                assert len(result["tools"]) == 2
                assert max(call["start"] for call in result["tools"]) < min(call["end"] for call in result["tools"])
                second = result["models"][1]
                assert [item["tool_call_id"] for item in second if item.get("role") == "tool"] == ["call-slow", "call-failure"]
            if mode == "worker-b":
                assert result["tools"] == []
                assert "rejected" in json.dumps(result["models"][1])
        doctor = subprocess.run([str(build / "trajectory_evolution"), "doctor", str(paths[0]), "--level", "L0"], capture_output=True, text=True)
        assert doctor.returncode == 0, doctor.stderr
        for artifact in (root / "data").rglob("*"):
            if artifact.is_file():
                assert b"fixture-model-credential-never-captured" not in artifact.read_bytes()
                assert env["INTEGRATION_TEST_A"].encode() not in artifact.read_bytes()
        # Stop handling uses the existing terminal paths, without an extra host
        # controller. Engine-generated stop/replan is covered by InstrumentationTests.
        for mode in ("stop", "worker-stop"):
            finish(launch(mode, {"EVOLUTION_POINT_CONFIG": "", "AF_DIALOGUE_ID": mode,
                                 "AF_EXEC_SKILL_KEY": specs[0]["exec_skill_key"] if mode == "worker-stop" else ""}))
            stopped = json.loads((root / (mode + ".result.json")).read_text())
            assert stopped["models"] == [] and stopped["tools"] == []
            assert json.loads(stopped["output"].splitlines()[-1])["status"] == "loop_control_stop"
        unavailable = root / "unavailable-client.json"
        unavailable.write_text(json.dumps({"url": "http://127.0.0.1:1", "principals": config["principals"],
                                            "point_timeout_ms": 10}))
        finish(launch("offline", {"EVOLUTION_POINT_CONFIG": str(unavailable), "AF_DIALOGUE_ID": "offline"}))
        offline = json.loads((root / "offline.result.json").read_text())
        assert len(offline["models"]) == 1 and "先补充图片" not in json.dumps(offline["models"], ensure_ascii=False)
        assert "完成" in offline["output"]
        (root / "test-summary.json").write_text(json.dumps({"status": "passed", "metrics": metrics,
            "trace": str(paths[0]), "parallel_workers": 2, "stop_exits": 2, "collector_unavailable_preserved": True,
            "model_transport": "fixture", "databus_transport": "fixture"}, indent=2))
        print(root / "test-summary.json")
    finally:
        server.terminate()
        try:
            server.wait(timeout=8)
        except subprocess.TimeoutExpired:
            server.kill(); server.wait(timeout=3)


if __name__ == "__main__":
    if sys.argv[1] == "--node":
        node(Path(sys.argv[2]), sys.argv[3])
    else:
        main(Path(sys.argv[1]).resolve())
