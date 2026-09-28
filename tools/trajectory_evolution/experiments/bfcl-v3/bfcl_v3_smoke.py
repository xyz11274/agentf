#!/usr/bin/env python3
"""Small local evaluator smoke test; never part of the experiment output."""
import http.server
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import shutil

ROOT = Path(__file__).resolve().parents[4]
EXPERIMENT = Path(__file__).resolve().parent
DATASET = EXPERIMENT / "datasets/training-reviewed"
DEPLOY = EXPERIMENT / "coldstart-deployment/runtime"


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            request = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
            if request.get("operation") == "point":
                result = {"context": {"run_id": "smoke-run", "kind": request.get("kind"),
                                       "activity_id": "smoke-activity"},
                          "input": request.get("input"), "decision": "allow",
                          "policy_applied": False, "degraded": False}
                if request.get("kind") == "run" and request.get("phase") == "after":
                    result.update(closed=True, ended=True, run_closed=True)
            elif request.get("operation") == "evaluation.next":
                index = int(request.get("index", 0))
                result = {"state": "done"} if index >= 4 else {
                    "state": "input", "index": index,
                    "input": {"messages": [{"role": "user", "content": "smoke"}],
                               "user_id": "bfcl-training", "conversation_id": "smoke",
                               "run_key": "smoke-" + str(index), "turn": index,
                               "initial_state": {"row_id": "multi_turn_base_0"},
                               "constraints": [], "budget": {"timeout_ms": 1000}},
                }
            else:
                # OpenAI-compatible deterministic responses for the nested
                # project Agent.  Exercise the complete coordinator -> skill
                # -> generated-tool path, while keeping the fake payload free
                # of credential-like values.
                messages = request.get("messages") or []
                is_skill = any(str(item.get("content", "")).startswith("## Skill: bfcl-functions")
                               for item in messages if isinstance(item, dict))
                has_tool_result = any(item.get("role") == "tool" for item in messages
                                      if isinstance(item, dict))
                if is_skill and not has_tool_result:
                    message = {"role": "assistant", "tool_calls": [{
                        "id": "bfcl-smoke-call", "type": "function",
                        "function": {"name": "ls", "arguments": '{"a":false}'},
                    }]}
                elif is_skill:
                    message = {"role": "assistant", "content": "BFCL smoke tool completed."}
                elif not has_tool_result:
                    message = {"role": "assistant", "tool_calls": [{
                        "id": "bfcl-smoke-dispatch", "type": "function",
                        "function": {"name": "run_skills", "arguments":
                                     '{"skills":[{"name":"bfcl-functions",'
                                     '"prompt":"Execute the BFCL request."}]}'},
                    }]}
                else:
                    message = {"role": "assistant", "content": "BFCL smoke completed."}
                body = json.dumps({"choices": [{"message": message}],
                                   "usage": {"prompt_tokens": 3, "completion_tokens": 2,
                                             "total_tokens": 5}}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            body = json.dumps({"ok": True, "result": result}).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except Exception:
            self.send_response(500)
            self.end_headers()


def main():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    keeper = os.environ.get("BFCL_SMOKE_DIR")
    context = tempfile.TemporaryDirectory(prefix="bfcl-v3-smoke-") if not keeper else None
    folder_name = keeper or context.name
    Path(folder_name).mkdir(parents=True, exist_ok=True)
    try:
        folder = Path(folder_name)
        # Mirror the deployment-relative resource layout so the native privacy
        # validator sees no absolute private path in the evaluation config.
        tree = folder / "tools/trajectory_evolution/experiments/bfcl-v3"
        tree.mkdir(parents=True)
        shutil.copy2(Path(__file__).with_name("bfcl_v3_host.py"), tree / "bfcl_v3_host.py")
        shutil.copytree(Path(__file__).parent / "datasets/source", tree / "datasets/source")
        config_dir = tree / "coldstart-deployment/runtime"
        config_dir.mkdir(parents=True)
        shutil.copytree(DEPLOY / "prompts", config_dir / "prompts")
        evaluation = json.loads((DEPLOY / "training-001.json").read_text())
        evaluation.update(task_ids=["multi_turn_base_0"], max_trials=1, parallelism=1,
                          checks={}, require_trace=False)
        runtime = json.loads((DEPLOY / "runtime.json").read_text())
        runtime["model"].update(endpoint=f"http://127.0.0.1:{server.server_port}/chat/completions",
                                 model_id="smoke", api_key_env="SMOKE_KEY")
        (config_dir / "runtime.json").write_text(json.dumps(runtime))
        service = json.loads((DEPLOY / "service.json").read_text())
        service.update(runtime_config="runtime.json", data_dir="evolution")
        (config_dir / "service.json").write_text(json.dumps(service))
        variant = evaluation["variants"][0]
        variant["instrumentation_config"] = "service.json"
        variant["command"] = ["python3", "-B", "{config_dir}/../../bfcl_v3_host.py", "--evolution-entry"]
        variant["pass_env"] = ["AF_LLM_BASE_URL", "AF_LLM_API_KEY", "AF_LLM_MODEL",
                                "OPENAI_BASE_URL", "OPENAI_API_KEY", "OPENAI_MODEL",
                                "AF_LLM_TIMEOUT", "AF_LLM_MAX_RETRIES", "BFCL_POINT_TIMEOUT_MS",
                                "BFCL_REPO_ROOT", "BFCL_DEBUG_ERRORS", "BFCL_KEEP_AGENT_TMP",
                                "AGENTFLOW_DAEMON", "PYTHONPATH",
                                "LD_LIBRARY_PATH"]
        (config_dir / "evaluation.json").write_text(json.dumps(evaluation))
        env = os.environ.copy()
        env.update({"AF_LLM_BASE_URL": f"http://127.0.0.1:{server.server_port}",
                    "AF_LLM_API_KEY": "smoke-key-1234567890123456", "AF_LLM_MODEL": "smoke",
                    "OPENAI_BASE_URL": f"http://127.0.0.1:{server.server_port}",
                    "OPENAI_API_KEY": "smoke-key-1234567890123456", "OPENAI_MODEL": "smoke",
                    "EVOLUTION_BFCL_TOKEN": "smoke-token-123456789012345678901234",
                    "BFCL_REPO_ROOT": str(ROOT),
                    "BFCL_DEBUG_ERRORS": "1",
                    "BFCL_KEEP_AGENT_TMP": "1",
                    "TRAJECTORY_UNCONDITIONAL_RESUME": "1"})
        native_command = [str(ROOT / "build/trajectory-evolution/trajectory_evolution"),
                          "dataset", "run", str(DATASET), "--config", str(config_dir / "evaluation.json"),
                          "--output-root", str(folder / "out")]
        if os.environ.get("BFCL_SMOKE_STRACE"):
            native_command = ["strace", "-f", "-o", "/tmp/bfcl-v3-native.strace"] + native_command
        result = subprocess.run(native_command, cwd=ROOT, env=env,
                                text=True, capture_output=True, timeout=180)
        report_path = folder / "out/report.json"
        report = json.loads(report_path.read_text())
        if result.returncode != 0:
            print("NATIVE_STDOUT=" + result.stdout[-4000:])
            print("NATIVE_STDERR=" + result.stderr[-4000:])
            print("REPORT=" + json.dumps(report, ensure_ascii=False)[:16000])
        trial = report["trials"][0]
        tool_calls = float((trial.get("evidence") or {}).get("metrics", {}).get("tool_calls", 0) or 0)
        print(json.dumps({"returncode": result.returncode, "execution_status": trial.get("execution_status"),
                          "error_code": trial.get("error_code"), "tool_calls": tool_calls,
                          "complete": report.get("complete")}, ensure_ascii=False))
        if result.returncode not in (0, 3) or trial.get("execution_status") == "unknown":
            raise SystemExit(1)
        if tool_calls < 1:
            raise SystemExit("route smoke did not execute a generated BFCL tool")
    finally:
        if context:
            context.cleanup()
    server.shutdown()


if __name__ == "__main__":
    main()
