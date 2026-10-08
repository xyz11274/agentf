"""Private production AgentFlow routes for the AppWorld experiment only."""
from __future__ import annotations
import inspect, json, os, re, shutil, socket, subprocess, sys, textwrap, time
from pathlib import Path
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
BFCL = ROOT / "tools/trajectory_evolution/experiments/bfcl-v3"
for directory in [HERE, BFCL, ROOT / "benchmark", ROOT / "packages/agentflow-runtime/src", ROOT / "tools/trajectory_evolution/clients/python"]:
    sys.path.insert(0, str(directory))
from bfcl_v3_host import AgentRunner as BaselineAgentRunner, AGENT_DAEMON


def _clock():
    # AppWorld legitimately freezes global time APIs to the simulated date.
    # The operating-system monotonic clock remains real for RPC timeouts.
    return time.clock_gettime(time.CLOCK_MONOTONIC)


def wait_turn_response(client, deadline):
    """Accept only the current run's official dialogue_id notification."""
    expected = getattr(client, "_appworld_expected_dialogue", None)
    if not isinstance(expected, str) or not expected:
        raise RuntimeError("appworld_rpc_identity_missing")
    texts, finals, errors = [], [], []
    ignored = 0

    def absorb(messages):
        nonlocal ignored
        terminal = False
        for message in messages:
            metadata = message.get("metadata") or {}
            if not isinstance(metadata, dict) or metadata.get("dialogue_id") != expected:
                ignored += 1
                continue
            content = message.get("content", "")
            if message.get("final"):
                terminal = True
                if content:
                    finals.append(content)
            if message.get("error"):
                terminal = True
                errors.append(content or "agent response error")
            if content:
                texts.append(content)
        return terminal

    def result(timed_out):
        client._appworld_rpc_diagnostic = {"ignored_notifications": ignored,
                                          "matched_terminal": not timed_out}
        return {"texts": texts, "final_text": "\n".join(finals or texts),
                "error": "\n".join(errors), "timed_out": timed_out}

    if client.sock is not None:
        client.sock.settimeout(5)
    while _clock() < deadline:
        queued = list(client.pushed)
        client.pushed.clear()
        if absorb(queued):
            return result(False)
        try:
            line = client._recv_line()
        except socket.timeout:
            continue
        except OSError:
            return result(True)
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("method") != "agent.response":
            continue
        params = event.get("params")
        if isinstance(params, str):
            try:
                params = json.loads(params)
            except json.JSONDecodeError:
                continue
        if isinstance(params, dict) and absorb([params]):
            return result(False)
    return result(True)

_ask_source = textwrap.dedent(inspect.getsource(BaselineAgentRunner.ask))
_ask_source = _ask_source.replace("from run_eval import send_turn, wait_turn_response", "from run_eval import send_turn")
_ask_source = _ask_source.replace("time.time() + 900", "_clock() + int(os.environ.get('APPWORLD_TURN_TIMEOUT', '180'))")
_ask_source = _ask_source.replace("    send_turn(self.client,", "    self.client._appworld_expected_dialogue = run_key\n    send_turn(self.client,", 1)
exec(compile(_ask_source, "<appworld-private-rpc-ask>", "exec"), globals())
_ask_real_clock = ask
COORDINATOR_INSTRUCTION = (
    "You are the AgentFlow coordinator for AppWorld cross-application tasks. For "
    "each message marked [AppWorld request], dispatch run_skills exactly once to "
    "appworld-office. Its skill document contains the complete official public "
    "task instruction, API documentation and previous environment observations. "
    "The skill must call appworld_submit_code once. Do not execute AppWorld code "
    "yourself. Stop immediately after its structured code is saved; no redispatch "
    "or extra summary is needed."
)


def _remove(path: Path) -> None:
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(path)

def _prepare_private_routes(package: Path) -> None:
    """Finish one real AgentFlow decision as soon as its reply is committed.

    Stock runtime functions, DataBus outputs and hook terminals remain in use.
    Only this disposable package receives the two terminal branches; the shared
    runtime and business package are never changed.
    """
    source_dir = ROOT / "packages/agentflow-runtime/src/agentflow_runtime"
    modules = package / "harness"
    skill = (source_dir / "skill_executor.py").read_text(encoding="utf-8")
    context_needle = '''    messages = [
        {"role": "system", "content": system_prompt},
        {"role": "user",   "content": task_instruction},
    ]'''
    if skill.count(context_needle) != 1:
        raise RuntimeError("private_context_insertion_unavailable")
    skill = skill.replace(context_needle, context_needle + '''
    from appworld_skill import compose_skill_messages
    messages = compose_skill_messages(messages, skill_name=skill_name)
''', 1)
    skill_needle = "                if _timed_out_tool_call_ids:\n                    _finish_node_timeout(round_i + 1)"
    skill_terminal = '''                from appworld_skill import current_reply as _appworld_current_reply
                if skill_name == "appworld-office" and _appworld_current_reply() is not None:
                    output = {"skill_result": "AppWorld code saved.",
                              "status": "completed", "skill_name": skill_name,
                              "rounds_used": round_i + 1, "artifacts_json": "[]"}
                    emit_event(hook_events.SKILL_EXECUTOR_COMPLETED, {
                        "round": round_i + 1, "total_rounds": round_i + 1,
                        "skill_name": skill_name, "status": "completed",
                        "response_length": len(output["skill_result"]),
                        "artifacts_count": 0, "elapsed_s": round(time.time() - _turn_start, 1),
                    }, duration_ms=(time.monotonic() - _script_t0) * 1000)
                    if databus.is_connected():
                        databus.put("skill_result:skill-runner", json.dumps(output, ensure_ascii=True))
                    print(json.dumps(output, ensure_ascii=False))
                    _emit_skill_end_once(AgentContext(messages=messages, artifacts=[]))
                    _skill_span.set_status(SpanStatus.OK)
                    return
'''
    coordinator = (source_dir / "coordinator_decide.py").read_text(encoding="utf-8")
    coordinator_needle = "        with emit_span(\n            SpanKind.LLM, hook_events.SPAN_COORDINATOR_LLM_ROUND,"
    coordinator_terminal = '''        from appworld_skill import current_reply as _appworld_current_reply
        if _appworld_current_reply() is not None:
            response_text = "AppWorld code saved."
            _final_ctx = AgentContext(messages=messages)
            _hook_payload = hook_trigger(hook_mounts.COORDINATOR_BEFORE_FINAL_RESPONSE,
                {"round": current_round, "response_text": response_text, "has_tool_call": False},
                can_intervene=True, ctx=_final_ctx)
            response_text = _hook_payload.get("response_text", response_text)
            _emit_dialogue_end_once("normal", response_text, _final_ctx, can_intervene=True)
            print(json.dumps({"response": response_text}, ensure_ascii=False))
            hook_trigger(hook_mounts.COORDINATOR_AFTER_FINAL_RESPONSE,
                {"round": current_round, "response_length": len(response_text),
                 "elapsed_s": round(time.time() - _t0, 1)}, ctx=_final_ctx)
            if databus.is_connected():
                put_e2e_response(databus, response_text, log_timing=True)
            save_session_messages(recorder, messages)
            return
'''
    if skill.count(skill_needle) != 1 or coordinator.count(coordinator_needle) != 1:
        raise RuntimeError("private_terminal_insertion_unavailable")
    skill = skill.replace(skill_needle, skill_terminal + skill_needle, 1)
    coordinator = coordinator.replace(coordinator_needle, coordinator_terminal + coordinator_needle, 1)
    for kind, source in [("skill_executor", skill), ("coordinator_decide", coordinator)]:
        target = modules / ("_appworld_" + kind + ".py")
        compile(source, str(target), "exec")
        target.write_text(source, encoding="utf-8")
        entry = '''from pathlib import Path
import importlib.util
import sys
package = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(package / "harness"))
from appworld_skill import install_private_reply_transport, install_native_observer
install_private_reply_transport()
install_native_observer()
name = "agentflow_runtime.PRIVATE_KIND"
spec = importlib.util.spec_from_file_location(name, package / "harness" / "_appworld_PRIVATE_KIND.py")
runtime = importlib.util.module_from_spec(spec)
sys.modules[name] = runtime
spec.loader.exec_module(runtime)
import agentflow_runtime
setattr(agentflow_runtime, "PRIVATE_KIND", runtime)
runtime.AGENT_PACKAGE_DIR = str(package)
runtime.AGENT_MD_PATH = str(package / "agent.md")
runtime.NODES_DIR = str(package / "nodes")
'''.replace("PRIVATE_KIND", kind)
        if kind == "skill_executor":
            entry += "from business_sim.tool_scope import BusinessToolRegistry\nruntime._LocalToolRegistry = BusinessToolRegistry\n"
        entry += "if __name__ == '__main__':\n    runtime.execute()\n"
        (package / "nodes" / (kind + ".py")).write_text(entry, encoding="utf-8")

class AgentRunner(BaselineAgentRunner):
    """Reuse production RPC, private package isolation and exact usage counting."""

    def __init__(self, level: str = "L0"):
        self.runtime_level = str(level).upper()
        self._accounted_calls = 0
        self._accounted_covered = 0
        self._accounted_usage = {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0}
        self._last_usage_complete = False
        self._partial_usage = {}
        super().__init__()

    def _usage_coverage(self) -> tuple[int, int, dict]:
        calls = covered = 0
        requests = 0
        totals = {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0}
        for path in (self.root / "sessionlogs").rglob("*.jsonl"):
            try:
                for line in path.read_text(encoding="utf-8").splitlines():
                    event = json.loads(line)
                    if event.get("type") == "llm.request":
                        requests += 1
                    if event.get("type") != "llm.response":
                        continue
                    calls += 1
                    data = event.get("data") or {}
                    usage = data.get("usage")
                    if not isinstance(usage, dict):
                        for key in ["response", "provider_response"]:
                            response = data.get(key)
                            if isinstance(response, dict) and isinstance(response.get("usage"), dict):
                                usage = response["usage"]
                                break
                    if isinstance(usage, dict) and all(
                        isinstance(usage.get(k), int) and not isinstance(usage[k], bool)
                        and usage[k] >= 0 for k in ["prompt_tokens", "completion_tokens"]
                    ):
                        covered += 1
                        totals["prompt_tokens"] += usage["prompt_tokens"]
                        totals["completion_tokens"] += usage["completion_tokens"]
                        total = usage.get("total_tokens")
                        totals["total_tokens"] += total if isinstance(total, int) and total >= 0 \
                            else usage["prompt_tokens"] + usage["completion_tokens"]
            except (OSError, ValueError, UnicodeError):
                continue
        return max(calls, requests), covered, totals

    def ask(self, *args, **kwargs):
        try:
            text, result = _ask_real_clock(self, *args, **kwargs)
        finally:
            calls, covered, totals = self._usage_coverage()
            delta_calls = max(0, calls - self._accounted_calls)
            delta_covered = max(0, covered - self._accounted_covered)
            self._accounted_calls, self._accounted_covered = calls, covered
            self._last_usage_complete = delta_calls > 0 and delta_calls == delta_covered
            self._partial_usage = {key: max(0, totals[key] - self._accounted_usage[key]) for key in totals}
            self._accounted_usage = totals
            self._partial_usage.update(llm_calls=delta_calls,
                tokens_complete=self._last_usage_complete, token_covered_calls=delta_covered)
        result["usage"].update(self._partial_usage)
        return text, result

    def _overlay_agent_package(self) -> None:
        super()._overlay_agent_package()
        from appworld_skill import prepare_skill

        hooks = self.agent_package / "harness" / "hooks"
        if hooks.is_symlink():
            source_hooks = hooks.resolve()
            hooks.unlink()
            shutil.copytree(source_hooks, hooks, symlinks=False)
        skills = self.agent_package / "harness" / "skills"
        for old in list(skills.iterdir()):
            _remove(old)
        self.meta = prepare_skill(self.root, skills / "appworld-office")
        (self.agent_package / "skills.json").write_text(json.dumps({
            "appworld-office": "./harness/skills/appworld-office"}), encoding="utf-8")
        model = os.environ.get("AF_LLM_MODEL") or os.environ.get("OPENAI_MODEL", "deepseek-v4-flash")
        (self.agent_package / "agent.md").write_text(
            "---\nname: AppWorld AgentFlow coordinator\nmodel: " + model +
            "\nskills:\n  - name: appworld-office\n---\n" + COORDINATOR_INSTRUCTION + "\n",
            encoding="utf-8")
        _prepare_private_routes(self.agent_package)

    def _start(self) -> None:
        from run_eval import RpcClient, check_daemon

        if not AGENT_DAEMON.is_file():
            raise RuntimeError("agentflowd_not_built")
        config = json.loads((self.agent_package / "daemon.json").read_text())
        hooks = self.agent_package / "harness" / "hooks"
        hooks.mkdir(parents=True, exist_ok=True)
        integration = ROOT / "tools/trajectory_evolution/integrations/agentflow"
        for name in ["runtime_hook.py", "shadow_hook.py"]:
            shutil.copy2(integration / name, hooks / name)
        runtime_config = self.root / "evolution-runtime.json"
        runtime_config.write_text(json.dumps({
            # The real trial's native boundary below owns strategy injection.
            # This unrelated empty private memory must add no per-level cost.
            "schema_version": 1, "mode": "disabled",
            "agent_id": self.agent_package.name,
            "memory_path": str(self.root / "evolution-memory"),
            "executable": os.environ.get("TRAJECTORY_EVOLUTION_EXECUTABLE",
                str(ROOT / "build/evolution-no-hooks-install/bin/trajectory_evolution")),
            "rpc_timeout_ms": 1000, "process_timeout_ms": 5000,
            "shadow_options": {"max_policy_entries": 64, "max_advice_items": 3,
                "max_advice_bytes": 4096, "max_query_bytes": 32768,
                "max_memory_bytes": 16777216, "max_snapshot_bytes": 262144,
                "max_match_terms": 128, "min_matched_terms": 1},
            "options": {"target_scopes": ["coordinator", "skill:appworld-office"],
                "allowed_users": ["business-training"], "allowed_conversations": [],
                "max_context_bytes": 4096},
            "experience_template_file": str(ROOT / "tools/trajectory_evolution/config/prompts/runtime-experience.txt"),
        }), encoding="utf-8")
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        config["server"].update(listen=f"127.0.0.1:{port}",
            uds_biz_path=str(self.root / "biz.sock"), uds_ctrl_path=str(self.root / "ctrl.sock"))
        config["daemon"]["pid_file"] = str(self.root / "daemon.pid")
        config["trace"] = {"output_dir": str(self.root / "traces")}
        config_path = self.root / "daemon.json"
        config_path.write_text(json.dumps(config), encoding="utf-8")
        env = os.environ.copy()
        self._load_env_file(env)
        # The private observer joins this exact run through a separate
        # transport. The nested native entry variables remain removed below.
        env['APPWORLD_CAPTURE_URL'] = env.get('TRAJECTORY_EVOLUTION_TRIAL_URL', '')
        env['APPWORLD_CAPTURE_TOKEN'] = env.get('TRAJECTORY_EVOLUTION_TRIAL_TOKEN', '')
        # The nested observer needs the selected policy level to keep L0
        # baseline accounting separate from strategy application telemetry.
        # This is an execution-mode flag only; it does not alter the Agent
        # prompt or the AppWorld world.
        env['APPWORLD_POLICY_LEVEL'] = self.runtime_level
        # This outer benchmark host owns the one native run and strategy
        # boundaries. A coordinator must not enter the same trial a second
        # time when the production runtime notices inherited trial variables.
        for key in list(env):
            if key.startswith("TRAJECTORY_EVOLUTION_TRIAL_") or key == "TRAJECTORY_EVOLUTION_EVALUATION_ENTRY":
                env.pop(key, None)
        for key in ["HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy"]:
            env.pop(key, None)
        env.update(self.meta["env"])
        env.update({
            "AF_SKIP_DOTENV": "1", "AF_COORDINATOR_VERSION": "v1",
            "AF_SESSIONLOG_ENABLED": "1", "AF_SESSIONLOG_DIR": str(self.root / "sessionlogs"),
            "AF_TRACE_OUTPUT_DIR": str(self.root / "traces"),
            "BUSINESS_EXECUTION_MODE": "simulation",
            "AF_EVOLUTION_RUNTIME_CONFIG": str(runtime_config),
            "PYTHONPATH": os.pathsep.join([str(ROOT / "benchmark"),
                str(ROOT / "packages/agentflow-runtime/src"),
                str(ROOT / "tools/trajectory_evolution/clients/python"),
                str(self.agent_package / "harness"), str(HERE), env.get("PYTHONPATH", "")]),
        })
        self._log = (self.root / "daemon.log").open("w", encoding="utf-8")
        self.proc = subprocess.Popen(
            [str(AGENT_DAEMON), "--config", str(config_path), "--package", str(self.agent_package)],
            cwd=ROOT, env=env, stdout=self._log, stderr=subprocess.STDOUT, start_new_session=True)
        deadline = _clock() + 15
        while _clock() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("agentflowd_exited")
            client = None
            try:
                client = RpcClient("127.0.0.1", port, timeout=0.5)
                check_daemon(client, "business-simulator")
                self.client = client
                return
            except (OSError, RuntimeError, ValueError):
                if client:
                    client.close()
                time.sleep(0.1)
        raise RuntimeError("agentflowd_start_timeout")

    def configure(self, *, policy: str, tools_schema: list[dict], messages: list[dict],
                  turn: int, native_context: dict | None = None,
                  native_replan_context: str | None = None,
                  l1_window_mode: str | None = None,
                  observed_phase_experiences: list[dict] | None = None,
                  auth_parameter_experiences: list[dict] | None = None,
                  auth_fallback_experiences: list[dict] | None = None,
                  auth_parameter_contracts: list | None = None,
                  auth_structured_missing: bool = False,
                  shape_error_auth_experiences: dict | None = None,
                  saved_result_experiences: dict | None = None,
                  saved_result_contracts: list | None = None) -> None:
        from appworld_skill import set_request, _locked, _atomic_text
        if l1_window_mode is not None:
            set_request(self.meta, policy, tools_schema, messages, turn,
                        l1_window_mode=l1_window_mode,
                        native_run_id=(native_context or {}).get('run_id'),
                        observed_phase_experiences=observed_phase_experiences,
                        auth_parameter_experiences=auth_parameter_experiences,
                        auth_fallback_experiences=auth_fallback_experiences,
                        auth_parameter_contracts=auth_parameter_contracts,
                        auth_structured_missing=auth_structured_missing,
                        **({'shape_error_auth_experiences': shape_error_auth_experiences}
                           if shape_error_auth_experiences is not None else {}),
                        **({'saved_result_experiences': saved_result_experiences,
                            'saved_result_contracts': saved_result_contracts}
                           if saved_result_experiences is not None else {}))
        else:
            set_request(self.meta, policy, tools_schema, messages, turn,
                        **({'auth_parameter_experiences': auth_parameter_experiences,
                            'auth_fallback_experiences': auth_fallback_experiences,
                            'auth_parameter_contracts': auth_parameter_contracts,
                            **({'auth_structured_missing': True} if auth_structured_missing is True else {})}
                           if auth_parameter_experiences is not None else {}),
                        **({'shape_error_auth_experiences': shape_error_auth_experiences}
                           if shape_error_auth_experiences is not None else {}),
                        **({'saved_result_experiences': saved_result_experiences,
                            'saved_result_contracts': saved_result_contracts}
                           if saved_result_experiences is not None else {}))
        if native_context is not None:
            with _locked(self.meta):
                path = Path(self.meta['state_path'])
                state = json.loads(path.read_text())
                state['native_context'] = native_context
                if isinstance(native_replan_context, str) and native_replan_context.strip():
                    state['native_replan_context'] = native_replan_context
                state['framework_capture'] = {'calls': 0, 'transport_attempts': 0, 'failures': 0}
                _atomic_text(path, json.dumps(state, ensure_ascii=False))
        if self.proc is None:
            self._start()
