#!/usr/bin/env python3
"""BFCL v3 isolated host used by the native trajectory evaluator.

The host owns the benchmark fixture and strict checker.  AgentFlow sees only
instrumentation at the model/action boundary; raw fixture credentials and
function state never enter persisted task attributes or trace payloads.
"""

from __future__ import annotations

import argparse
import ast
import json
import os
from pathlib import Path
import re
import signal
import shutil
import socket
import subprocess
import sys
import time
import uuid
import urllib.error
import urllib.request
from zipfile import ZipFile

HERE = Path(__file__).resolve().parent
# Native smoke copies this entry into a temporary relative tree.  The formal
# evaluator runs from the repository tree; the explicit override keeps both
# paths on the same AgentFlow runtime without copying the repository or its
# credentials into the smoke tree.
ROOT = Path(os.environ.get("BFCL_REPO_ROOT", str(HERE.parents[3]))).resolve()
SOURCE_ZIP = HERE / "datasets/source/bfcl_v3_splits.zip"
BASELINE = Path("/root/lai-code/baselines/gorilla/berkeley-function-call-leaderboard")
if BASELINE.is_dir():
    sys.path.insert(0, str(BASELINE))
sys.path.insert(0, str(ROOT / "tools/trajectory_evolution/clients/python"))
sys.path.insert(0, str(ROOT / "benchmark"))

try:
    Path("/tmp/bfcl-v3-host-start.code").write_text("started", encoding="utf-8")
except Exception:
    pass

from evolution import Evolution  # noqa: E402
from evolution_entry import evaluate  # noqa: E402


# BFCL intentionally uses the repository's production AgentFlow package as the
# baseline.  The benchmark fixture is isolated below, while the agent is the
# same coordinator -> skill-runner deployment used by business acceptance.
AGENT_PACKAGE = ROOT / "examples/agent/business-simulator"
AGENT_DAEMON = Path(os.environ.get("AGENTFLOW_DAEMON", str(ROOT / "build/agentflowd")))
sys.path.insert(0, str(AGENT_PACKAGE / "harness"))


class AgentRunner:
    """Run the current project Agent through its public JSON-RPC boundary.

    One runner is created per native evaluation process.  A runner keeps one
    dialogue history and uses a private temporary daemon/config, so BFCL trials
    never write business-simulator artifacts or share state with another trial.
    """

    def __init__(self):
        import tempfile
        self._tmp = tempfile.TemporaryDirectory(prefix="bfcl-agent-")
        self.root = Path(self._tmp.name)
        self.agent_package = self.root / "agent-package"
        self._overlay_agent_package()
        self.proc = None
        self.client = None
        self._log = None
        self._bfcl_meta = None
        self._row_id = ""
        self.messages: list[dict] = []
        self.runtime_state = None
        self.row_file = None
        self.call_log = None
        self._usage_total = {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0}
        self._llm_calls_total = 0

    def _overlay_agent_package(self) -> None:
        """Create a tiny symlinked package overlay without copying business data."""
        self.agent_package.mkdir(parents=True, exist_ok=True)
        for child in AGENT_PACKAGE.iterdir():
            destination = self.agent_package / child.name
            if child.name == "agent.md":
                continue
            if child.name in ("coordinator_workflow.json", "dummy_pre_skill_workflow.json",
                              "skill_workflow.json"):
                # These files are rewritten below. A symlink would mutate the
                # shared business package and race with every parallel trial.
                shutil.copy2(child, destination)
                continue
            if child.name == "outputs":
                # The business package contains a large, mutable artifact tree.
                # BFCL only needs the code package, so give this daemon an empty
                # private destination instead of linking the business records.
                destination.mkdir(parents=True, exist_ok=True)
                continue
            if child.name == "data":
                # Seed data is small and may be read by a defensive fallback;
                # copying it prevents a BFCL trial from writing business state.
                shutil.copytree(child, destination, symlinks=True)
                continue
            if child.name == "nodes":
                # The generated node entrypoints set AGENT_PACKAGE_DIR from
                # ``__file__``.  Symlinking them resolves back to the business
                # package and silently bypasses this private agent.md.
                shutil.copytree(child, destination, symlinks=False)
                continue
            if child.name == "harness":
                # Keep the harness code lightweight through symlinks, while
                # making harness/skills a real directory so dynamic BFCL
                # scripts can be refreshed without escaping skill resolution.
                destination.mkdir(parents=True, exist_ok=True)
                for harness_child in child.iterdir():
                    target = destination / harness_child.name
                    if harness_child.name == "skills":
                        shutil.copytree(harness_child, target, symlinks=True)
                    else:
                        try:
                            target.symlink_to(harness_child, target_is_directory=harness_child.is_dir())
                        except FileExistsError:
                            pass
                continue
            try:
                destination.symlink_to(child, target_is_directory=child.is_dir())
            except FileExistsError:
                pass
        # Workflows shipped by business-simulator contain a repository-relative
        # working_dir.  If left unchanged, agentflowd starts the original
        # business node scripts even though --package points at this overlay.
        # Rewrite only the private copies; the production package is untouched.
        for workflow_name in ("coordinator_workflow.json", "dummy_pre_skill_workflow.json",
                              "skill_workflow.json"):
            workflow = self.agent_package / workflow_name
            if not workflow.is_file():
                continue
            try:
                document = json.loads(workflow.read_text(encoding="utf-8"))
                for node in document.get("nodes", []):
                    if isinstance(node, dict) and "working_dir" in node:
                        node["working_dir"] = str(self.agent_package)
                workflow.write_text(json.dumps(document, ensure_ascii=False), encoding="utf-8")
            except (OSError, ValueError, TypeError) as error:
                raise RuntimeError("bfcl_private_workflow_invalid:" + workflow_name) from error
        source = (AGENT_PACKAGE / "agent.md").read_text(encoding="utf-8")
        # The business package pins its production model in agent.md.  BFCL
        # runs must use the model selected by the experiment environment so a
        # provider switch cannot silently leave the private daemon on the old
        # endpoint/model pair.  The original business package is untouched.
        selected_model = os.environ.get("AF_LLM_MODEL") or os.environ.get("OPENAI_MODEL")
        if selected_model:
            source = re.sub(r"(?m)^model:\s*.*$", f"model: {selected_model}", source, count=1)
        if "- name: bfcl-functions" not in source:
            source = source.replace("  - name: web-search\n", "  - name: bfcl-functions\n  - name: web-search\n", 1)
        marker = ("当用户输入包含 `[BFCL available functions` 标记时，必须只派发 `bfcl-functions`，\n"
                  "并把当前轮的函数 schema 和原始请求完整交给它；不要把这一轮路由到 web-search 或其他业务 skill。\n")
        if "[BFCL available functions" not in source:
            source = source.replace("\n## 场景路由\n", "\n" + marker + "\n## 场景路由\n", 1)
        (self.agent_package / "agent.md").write_text(source, encoding="utf-8")

    def _close_process(self) -> None:
        """Stop only the private AgentFlow daemon, retaining the trial workspace."""
        if self.client is not None:
            try:
                self.client.close()
            except Exception:
                pass
            self.client = None
        if self.proc is not None:
            try:
                if self.proc.poll() is None:
                    os.killpg(self.proc.pid, signal.SIGTERM)
                    self.proc.wait(timeout=8)
            except (OSError, subprocess.TimeoutExpired):
                try:
                    os.killpg(self.proc.pid, signal.SIGKILL)
                    self.proc.wait(timeout=4)
                except (OSError, subprocess.TimeoutExpired):
                    try:
                        self.proc.kill()
                    except Exception:
                        pass
            self.proc = None
        if self._log is not None:
            try:
                self._log.close()
            except Exception:
                pass
            self._log = None

    def _prepare_row(self, row: dict, *, live_model_name: str) -> None:
        """Materialise the private BFCL skill before the daemon is started."""
        row_id = str(row.get("id", ""))
        if not row_id:
            raise RuntimeError("bfcl_row_id_missing")
        if self.proc is not None and self._row_id != row_id:
            self._close_process()
        if self._row_id == row_id and self._bfcl_meta is not None:
            if self.proc is None:
                self._start()
            return
        from bfcl_skill import prepare_row
        workspace = self.root / "bfcl-runtime"
        if workspace.exists():
            shutil.rmtree(workspace, ignore_errors=True)
        self._bfcl_meta = prepare_row(workspace, row, live_model_name=live_model_name)
        from bfcl_skill import sync_generated_skill
        sync_generated_skill(
            self.agent_package / "harness" / "skills" / "bfcl-functions",
            self._bfcl_meta["scripts_dir"],
            row=row, turn=0,
        )
        self.runtime_state = Path(self._bfcl_meta["state_path"])
        self.row_file = Path(self._bfcl_meta["row_path"])
        self.call_log = Path(self._bfcl_meta["call_log"])
        self._row_id = row_id
        self._start()

    @staticmethod
    def _load_env_file(env: dict) -> None:
        dotenv = AGENT_PACKAGE / ".env"
        if not dotenv.is_file():
            return
        for line in dotenv.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            key, value = key.strip(), value.strip().strip("\"'")
            if key.startswith(("AF_LLM_", "OPENAI_", "BUSINESS_", "WEBSEARCH_")):
                env.setdefault(key, value)

    def _start(self) -> None:
        import json as _json
        import socket as _socket
        import time as _time
        from run_eval import RpcClient, check_daemon

        if not AGENT_DAEMON.is_file():
            raise RuntimeError("agentflowd_not_built")
        config = _json.loads((self.agent_package / "daemon.json").read_text(encoding="utf-8"))
        # Load the same native runtime hook used by production agents.  The
        # business package normally ships only session logging; BFCL's private
        # overlay therefore materialises the hook modules and a per-trial
        # policy config before the daemon loads harness hooks.
        hook_dir = self.agent_package / "harness" / "hooks"
        hook_dir.mkdir(parents=True, exist_ok=True)
        integration_dir = ROOT / "tools/trajectory_evolution/integrations/agentflow"
        for hook_name in ("runtime_hook.py", "shadow_hook.py"):
            source = integration_dir / hook_name
            if source.is_file():
                shutil.copy2(source, hook_dir / hook_name)
        evolution_exe = Path(os.environ.get(
            "TRAJECTORY_EVOLUTION_EXECUTABLE",
            str(ROOT / "build/evolution-no-hooks-install/bin/trajectory_evolution"),
        ))
        runtime_config_path = self.root / "evolution-runtime.json"
        runtime_config_path.write_text(_json.dumps({
            "schema_version": 1,
            "mode": "active",
            # agentflowd binds AF_AGENT_PACKAGE_DIR to the private overlay
            # (whose basename is ``agent-package``).  Keep the runtime
            # identity bound to that package so the native hook cannot accept
            # another daemon's DataBus events.
            "agent_id": self.agent_package.name,
            "memory_path": str(self.root / "evolution-memory"),
            "executable": str(evolution_exe),
            "rpc_timeout_ms": 1000,
            "process_timeout_ms": 5000,
            "shadow_options": {"max_policy_entries": 64, "max_advice_items": 3,
                                "max_advice_bytes": 4096, "max_query_bytes": 32768,
                                "max_memory_bytes": 16777216, "max_snapshot_bytes": 262144,
                                "max_match_terms": 128, "min_matched_terms": 1},
            # The hook receives coordinator events with scope ``coordinator``
            # and the generated BFCL skill events with scope
            # ``skill:bfcl-functions``.  The public RPC uses this fixed user
            # identity; omitting it makes the hook silently return before
            # querying the native policy service.
            "options": {"target_scopes": ["coordinator", "skill:bfcl-functions"],
                        "allowed_users": ["bfcl-agent"],
                        "allowed_conversations": [],
                        "max_context_bytes": 4096},
            "experience_template_file": str(ROOT / "tools/trajectory_evolution/config/prompts/runtime-experience.txt"),
        }, ensure_ascii=False), encoding="utf-8")
        with _socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        config["server"].update(
            listen=f"127.0.0.1:{port}",
            uds_biz_path=str(self.root / "biz.sock"),
            uds_ctrl_path=str(self.root / "ctrl.sock"),
        )
        config["daemon"]["pid_file"] = str(self.root / "daemon.pid")
        config["trace"] = {"output_dir": str(self.root / "traces")}
        config_path = self.root / "daemon.json"
        config_path.write_text(_json.dumps(config), encoding="utf-8")
        env = os.environ.copy()
        self._load_env_file(env)
        env.update({
            "AF_SKIP_DOTENV": "1",
            "AF_COORDINATOR_VERSION": "v1",
            # Session logs stay in the private trial directory and are used
            # only to recover exact AgentFlow token accounting for the BFCL
            # run.end metrics.  They never enter trace_output.
            "AF_SESSIONLOG_ENABLED": "1",
            "AF_SESSIONLOG_DIR": str(self.root / "sessionlogs"),
            "AF_TRACE_OUTPUT_DIR": str(self.root / "traces"),
            "BFCL_RUNTIME_STATE": str(self._bfcl_meta["state_path"]),
            "BFCL_RUNTIME_SKILL_DIR": str(self._bfcl_meta["scripts_dir"]),
            "BFCL_BASELINE_ROOT": str(BASELINE),
            "BFCL_BASELINE_PATH": str(BASELINE),
            "PYTHONPATH": os.pathsep.join([
                str(ROOT / "benchmark"),
                str(ROOT / "packages/agentflow-runtime/src"),
                str(ROOT / "tools/trajectory_evolution/clients/python"),
                str(AGENT_PACKAGE / "harness"),
                str(BASELINE),
                env.get("PYTHONPATH", ""),
            ]),
            # Keep the agent's own tools in an isolated in-memory/scratch mode.
            "BUSINESS_EXECUTION_MODE": "simulation",
            "AF_EVOLUTION_RUNTIME_CONFIG": str(runtime_config_path),
        })
        log = (self.root / "daemon.log").open("w", encoding="utf-8")
        self.proc = subprocess.Popen(
            [str(AGENT_DAEMON), "--config", str(config_path), "--package", str(self.agent_package)],
            cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        self._log = log
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("agentflowd_exited")
            try:
                client = RpcClient("127.0.0.1", port, timeout=0.5)
                check_daemon(client, "business-simulator")
                self.client = client
                return
            except (OSError, RuntimeError, ValueError):
                try:
                    client.close()
                except Exception:
                    pass
                time.sleep(0.1)
        raise RuntimeError("agentflowd_start_timeout")

    def set_bfcl_context(self, row: dict, *, turn: int, available_names: list[str],
                         live_model_name: str) -> None:
        """Publish only private per-trial state consumed by bfcl-functions."""
        self._prepare_row(row, live_model_name=live_model_name)
        from bfcl_skill import set_turn
        set_turn(self._bfcl_meta["state_path"], turn=int(turn),
                 live_model_name=live_model_name)
        from bfcl_skill import sync_generated_skill
        sync_generated_skill(
            self.agent_package / "harness" / "skills" / "bfcl-functions",
            self._bfcl_meta["scripts_dir"],
            row=row, turn=int(turn),
        )

    def ask(self, messages: list[dict], *, user_id: str, dialogue_id: str,
            turn: int = 0) -> tuple[str, dict]:
        from run_eval import send_turn, wait_turn_response

        self.messages = [*messages]
        payload = json.dumps({"stream": False, "messages": self.messages}, ensure_ascii=False)
        # conversation_id is the benchmark row.  Reusing one conversation for
        # all rows would make the daemon retain another row's history.
        dialogue_key = re.sub(r"[^A-Za-z0-9-]", "-", str(dialogue_id))[:96]
        run_key = f"{dialogue_key}-{turn}"
        send_turn(self.client, "business-simulator", user_id, dialogue_key, run_key, payload)
        result = wait_turn_response(self.client, time.time() + 900)
        if result.get("error") or result.get("timed_out"):
            # Keep only a stable provider failure class.  Trial.receive maps
            # this to a task-scoped external failure so it cannot become a
            # BFCL capability failure or enter learning memory.
            detail = str(result.get("error") or "agent_turn_failed").lower()
            if result.get("timed_out") or "timeout" in detail:
                code = "external_model_timeout"
            elif "429" in detail or "capacity" in detail or "rate" in detail:
                code = "external_model_http_429"
            else:
                code = "external_model_failed"
            raise RuntimeError(code)
        cumulative = {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0}
        llm_calls = 0
        # AgentFlow's RPC response intentionally contains no provider usage;
        # read the private session log emitted by the same daemon to preserve
        # token metrics without copying requests or credentials to artifacts.
        for path in (self.root / "sessionlogs").rglob("*.jsonl"):
            try:
                for line in path.read_text(encoding="utf-8").splitlines():
                    event = json.loads(line)
                    if event.get("type") == "llm.response":
                        llm_calls += 1
                    data = event.get("data") or {}
                    value = data.get("usage") or {}
                    if not isinstance(value, dict):
                        value = {}
                    if not value:
                        # Provider usage is nested under llm.response in the
                        # public session schema.  Search only the response
                        # payload, never persist it or expose it to traces.
                        stack = [data.get("response"), data.get("provider_response")]
                        while stack and not value:
                            item = stack.pop()
                            if isinstance(item, dict):
                                candidate = item.get("usage")
                                if isinstance(candidate, dict):
                                    value = candidate
                                    break
                                stack.extend(item.values())
                            elif isinstance(item, list):
                                stack.extend(item)
                    for key in cumulative:
                        number = value.get(key)
                        if isinstance(number, (int, float)):
                            cumulative[key] += int(number)
            except (OSError, ValueError, UnicodeError):
                continue
        usage = {key: max(0, cumulative[key] - self._usage_total[key]) for key in cumulative}
        call_delta = max(0, llm_calls - self._llm_calls_total)
        self._usage_total = cumulative
        self._llm_calls_total = llm_calls
        result["usage"] = {**usage, "llm_calls": call_delta}
        return str(result.get("final_text") or ""), result

    def close(self) -> None:
        self._close_process()
        if os.environ.get("BFCL_KEEP_AGENT_TMP") == "1":
            try:
                snapshot = Path("/tmp/bfcl-agent-last")
                if snapshot.exists():
                    shutil.rmtree(snapshot)
                snapshot.mkdir()
                for name in ("daemon.log", "sessionlogs", "traces"):
                    item = self.root / name
                    if item.is_file():
                        shutil.copy2(item, snapshot / name)
                    elif item.is_dir():
                        shutil.copytree(item, snapshot / name)
            except Exception:
                pass
        self._tmp.cleanup()


_SECRET = re.compile(
    r"(?i)(api[_ -]?key|access[_ -]?token|password|secret|authorization)\s*[:=]?\s*"
    r"(['\"]?)[A-Za-z0-9._~+/-]{6,}\2"
)
_CODE = re.compile(r"(?i)(access\s+(?:code|token)\s*[:=]?\s*)[A-Za-z0-9_.-]+")


def _safe_text(value: object, limit: int = 4096) -> str:
    text = value if isinstance(value, str) else json.dumps(value, ensure_ascii=False, default=str)
    text = _CODE.sub(r"\1<REDACTED>", text)
    text = _SECRET.sub(r"\1=<REDACTED>", text)
    text = re.sub(r"\b(?:sk|ark)-[A-Za-z0-9_-]{16,}\b", "<REDACTED>", text)
    return text[:limit]


def _safe_obj(value: object, limit: int = 4096) -> object:
    if isinstance(value, str):
        return _safe_text(value, limit)
    if isinstance(value, dict):
        return {str(k): _safe_obj(v, limit) for k, v in list(value.items())[:64]}
    if isinstance(value, list):
        return [_safe_obj(v, limit) for v in value[:64]]
    return value


def _debug_exception(path_name: str, phase: str, error: BaseException) -> None:
    """Keep one compact local diagnostic outside experiment artifacts.

    The native evaluator intentionally keeps child stderr out of reports.  The
    exception class/message is needed to distinguish a fixture/runtime defect
    from a genuine BFCL mismatch; no prompts, schemas, or credentials are
    written here.
    """
    try:
        path = Path("/tmp") / path_name
        with path.open("a", encoding="utf-8") as handle:
            handle.write(f"{phase}:{type(error).__name__}:{str(error)[:240]}\n")
    except Exception:
        pass


class Source:
    def __init__(self, path: Path):
        self.path = path
        self.rows: dict[str, dict] = {}
        with ZipFile(path) as archive:
            for member in ("splits/multi_turn_train.jsonl", "splits/multi_turn_val.jsonl"):
                for line in archive.read(member).splitlines():
                    if line.strip():
                        row = json.loads(line)
                        self.rows[str(row["id"])] = row

    def get(self, row_id: str) -> dict:
        try:
            return self.rows[row_id]
        except KeyError:
            raise RuntimeError("bfcl_row_not_found") from None


def _call_name(value: str) -> str:
    try:
        tree = ast.parse(value, mode="eval")
        node = tree.body
        if isinstance(node, ast.Call):
            node = node.func
        if isinstance(node, ast.Name):
            return node.id
        if isinstance(node, ast.Attribute):
            return node.attr
    except (SyntaxError, ValueError):
        pass
    return ""


def _parse_call_expression(value: str) -> list[str]:
    """Parse only Python call expressions; never eval model text."""
    text = value.strip()
    if text.startswith("```"):
        text = re.sub(r"^```(?:python|json)?\s*|\s*```$", "", text, flags=re.I | re.S).strip()
    candidates = [text]
    if "[" in text and "]" in text:
        candidates.append(text[text.find("["):text.rfind("]") + 1])
    for candidate in candidates:
        try:
            tree = ast.parse(candidate, mode="eval")
        except (SyntaxError, ValueError):
            continue
        value_node = tree.body
        class _JsonLiteralNames(ast.NodeTransformer):
            def visit_Name(self, node):  # noqa: N802
                literals = {"true": True, "false": False, "null": None}
                if node.id in literals:
                    return ast.copy_location(ast.Constant(value=literals[node.id]), node)
                return node
        value_node = _JsonLiteralNames().visit(value_node)
        ast.fix_missing_locations(value_node)
        nodes = list(value_node.elts) if isinstance(value_node, (ast.List, ast.Tuple)) else [value_node]
        calls = []
        for node in nodes:
            if not isinstance(node, ast.Call) or not _call_name(ast.unparse(node)):
                calls = []
                break
            calls.append(ast.unparse(node))
        if calls or (isinstance(value_node, (ast.List, ast.Tuple)) and not value_node.elts):
            return calls
    return []


def _tool_calls(message: dict) -> list[str]:
    calls = []
    for item in message.get("tool_calls") or []:
        function = item.get("function") or {}
        name = function.get("name")
        raw_args = function.get("arguments", "{}")
        try:
            args = json.loads(raw_args) if isinstance(raw_args, str) else raw_args
        except (TypeError, ValueError):
            continue
        if isinstance(name, str) and isinstance(args, dict):
            calls.append(f"{name}({', '.join(f'{k}={v!r}' for k, v in args.items())})")
    function = message.get("function_call") or {}
    if function.get("name") and not calls:
        try:
            args = json.loads(function.get("arguments", "{}"))
        except (TypeError, ValueError):
            args = {}
        if isinstance(args, dict):
            calls.append(f"{function['name']}({', '.join(f'{k}={v!r}' for k, v in args.items())})")
    return calls


def decode_message(message: dict) -> tuple[list[str], str]:
    calls = _tool_calls(message)
    content = message.get("content")
    text = content if isinstance(content, str) else ""
    if not calls and text:
        calls = _parse_call_expression(text)
    return calls, text


def _endpoint() -> str:
    value = os.environ.get("AF_LLM_BASE_URL") or os.environ.get("OPENAI_BASE_URL") or ""
    value = value.rstrip("/")
    if value.endswith("/chat/completions"):
        return value
    return value + "/chat/completions"


def _model() -> str:
    return os.environ.get("AF_LLM_MODEL") or os.environ.get("OPENAI_MODEL") or ""


def request_model(payload: dict) -> tuple[dict, dict]:
    endpoint, model = _endpoint(), _model()
    key = os.environ.get("AF_LLM_API_KEY") or os.environ.get("OPENAI_API_KEY")
    if not endpoint or not model or not key:
        raise RuntimeError("model_not_configured")
    body = dict(payload, model=model, temperature=0)
    timeout = max(1.0, min(float(os.environ.get("AF_LLM_TIMEOUT", "120")), 300.0))
    retries = max(0, min(int(os.environ.get("AF_LLM_MAX_RETRIES", "3")), 5))
    last = "model_call_failed"
    for attempt in range(retries + 1):
        request = urllib.request.Request(endpoint, data=json.dumps(body, ensure_ascii=False).encode(),
                                          headers={"Content-Type": "application/json", "Authorization": "Bearer " + key})
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                result = json.loads(response.read(8 * 1024 * 1024).decode("utf-8"))
            choice = (result.get("choices") or [{}])[0]
            message = choice.get("message") or {}
            return message, result.get("usage") or {}
        except urllib.error.HTTPError as error:
            last = "model_http_" + str(error.code)
            if error.code not in (408, 409, 429, 500, 502, 503, 504):
                break
        except (urllib.error.URLError, TimeoutError, ValueError, OSError):
            last = "model_transport_failed"
        if attempt < retries:
            time.sleep(min(2 ** attempt, 8))
    # A few OpenAI-compatible endpoints reject legacy ``functions``. Retry once
    # with the modern ``tools`` shape without duplicating a successful request.
    if "functions" in body and last.startswith("model_http_4"):
        modern = dict(body)
        functions = modern.pop("functions")
        modern["tools"] = [{"type": "function", "function": item} for item in functions]
        request = urllib.request.Request(endpoint, data=json.dumps(modern, ensure_ascii=False).encode(),
                                          headers={"Content-Type": "application/json", "Authorization": "Bearer " + key})
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                result = json.loads(response.read(8 * 1024 * 1024).decode("utf-8"))
            return ((result.get("choices") or [{}])[0].get("message") or {}, result.get("usage") or {})
        except Exception:
            pass
    raise RuntimeError(last)


def _available_functions(row: dict, turn: int) -> list[dict]:
    current = {item.get("name"): item for item in row.get("functions", []) if item.get("name")}
    for key, groups in (row.get("missed_functions") or {}).items():
        try:
            available_at = int(key)
        except (TypeError, ValueError):
            continue
        if turn >= available_at:
            for group in groups:
                for item in group:
                    if item.get("name"):
                        current[item["name"]] = item
    return list(current.values())


def _function_f1(predicted: list[list[str]], expected: list[list[str]]) -> float:
    p = [_call_name(call) for turn in predicted for call in turn if _call_name(call)]
    g = [_call_name(call) for turn in expected for call in turn if _call_name(call)]
    if not p and not g:
        return 1.0
    if not p or not g:
        return 0.0
    left, right = list(p), list(g)
    hit = 0
    for name in list(left):
        if name in right:
            hit += 1
            left.remove(name)
            right.remove(name)
    precision, recall = hit / len(p), hit / len(g)
    return 2 * precision * recall / (precision + recall) if precision + recall else 0.0


def _logged_calls(path: Path, turn: int, row_id: str = "") -> list[str]:
    """Read compact calls emitted by the business Agent's BFCL skill."""
    return [item["call"] for item in _logged_call_records(path, turn, row_id)
            if isinstance(item.get("call"), str) and item["call"]]


def _logged_call_records(path: Path, turn: int, row_id: str = "") -> list[dict]:
    """Read private call plus result observations for the next BFCL turn."""
    if not path.is_file():
        return []
    result = []
    try:
        for line in path.read_text(encoding="utf-8").splitlines():
            item = json.loads(line)
            if (int(item.get("turn", -1)) == turn
                    and (not row_id or str(item.get("row_id", row_id)) == row_id)
                    and item.get("status") == "completed"):
                call = item.get("call")
                if isinstance(call, str) and call:
                    result.append({"call": call, "result": str(item.get("result", ""))[:4096],
                                   "status": str(item.get("status", "completed"))})
    except (OSError, ValueError, TypeError):
        return result
    return result


class Trial:
    def __init__(self, source: Source, row: dict, level: str, agent: AgentRunner):
        self.source, self.row, self.level, self.agent = source, row, level, agent
        self.evo = Evolution(os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_URL", ""),
                             os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_TOKEN", ""),
                             timeout_ms=5000, retries=0)
        self.run_context = None
        self.messages: list[dict] = []
        self.calls: list[list[str]] = []
        self.turn_outputs: list[str] = []
        self.tokens = 0
        self.model_calls = 0
        self.invalid_loops = 0
        self.external_error_code = ""
        self.policy_applied = {"L1": 0, "L2": 0, "L3": 0}
        self._last_call_keys: set[str] = set()
        self.live_model_name = "bfcl-live-" + uuid.uuid4().hex[:12]
        self.started_ns = time.monotonic_ns()
        self.intent_hits = 0
        self.intent_observations = 0
        # The official BFCL system message asks a raw model to emit function
        # expressions directly.  Passing it to the production coordinator
        # competes with the AgentFlow routing contract and causes the model to
        # bypass ``bfcl-functions``.  The official checker still owns that
        # message/ground truth; the coordinator only needs a compact directive
        # to dispatch the generated BFCL skill, exactly like business turns.
        self.messages.append({"role": "system", "content": (
            "You are the AgentFlow coordinator. For every user message marked "
            "[BFCL available functions], call run_skills exactly once with "
            "the bfcl-functions skill and pass the complete current request "
            "and schemas in its prompt. Do not call benchmark functions "
            "directly, do not answer with a function expression, and do not "
            "dispatch any other skill. After the skill result, summarize it "
            "briefly and stop."
        )})

    def _point(self, fields: dict) -> dict:
        try:
            result = self.evo.point(fields, timeout_ms=min(5000, int(os.environ.get("BFCL_POINT_TIMEOUT_MS", "1000"))))
            if result.get("policy_applied") is True:
                level = result.get("application_level") or result.get("policy_level") \
                    or result.get("effective_level") or result.get("level") or fields.get("level")
                if isinstance(level, str) and level.upper() in self.policy_applied:
                    self.policy_applied[level.upper()] += 1
            if result.get("degraded"):
                try:
                    Path("/tmp/bfcl-v3-point-debug.log").open("a", encoding="utf-8").write(
                        str(fields.get("kind", "unknown")) + ":" + str(result.get("reason", "degraded")) + "\n")
                except Exception:
                    pass
            return result
        except Exception:
            try:
                Path("/tmp/bfcl-v3-point-debug.log").open("a", encoding="utf-8").write(
                    str(fields.get("kind", "unknown")) + ":exception\n")
            except Exception:
                pass
            return {"context": fields.get("context", {}), "input": fields.get("input"),
                    "decision": "allow", "policy_applied": False, "degraded": True}

    def _receipt_counts(self, run_context: dict | None) -> dict:
        """Read authoritative applied receipts after the run is closed.

        ``point`` responses contain the action decision, while the verified
        application status is emitted by the boundary service when the host
        reports the actual result.  The status endpoint exposes only counts;
        all receipts in one trial share that trial's effective level, so
        assigning only ``applied`` receipts to that level preserves the
        distinction between a real application and a policy query/degrade.
        """
        run_id = run_context.get("run_id") if isinstance(run_context, dict) else None
        if not isinstance(run_id, str) or not run_id:
            return {}
        try:
            status = self.evo.invoke("task.status", {"run_id": run_id})
        except Exception:
            return {}
        receipts = status.get("receipts") if isinstance(status, dict) else None
        if not isinstance(receipts, dict):
            return {}
        applied = receipts.get("applied", 0)
        if isinstance(applied, bool) or not isinstance(applied, (int, float)) or applied < 0:
            return {}
        return {"applied": int(applied)}

    def receive(self, task: dict) -> dict:
        turn = int(task.get("turn", len(self.calls)))
        raw_turn = self.row["turns"][turn] if turn < len(self.row["turns"]) else []
        user_messages = [m for m in raw_turn if m.get("role") == "user" and m.get("content")]
        functions = _available_functions(self.row, turn)
        public_input = {
            "function_names": [f["name"] for f in functions],
            "message_count": len(self.messages),
            "message_chars": sum(len(str(m.get("content", ""))) for m in self.messages),
            "turn": turn,
        }
        if self.run_context is None:
            run = self._point({"phase": "before", "kind": "run", "input": public_input,
                               "level": self.level, "capabilities": ["model", "actions", "loop"],
                               "conversation_id": task.get("conversation_id")})
            self.run_context = run.get("context", {})
        if user_messages:
            for message in user_messages:
                self.messages.append({"role": "user", "content": message.get("content", "")})
        else:
            # In BFCL multi-turn rows an empty ``miss_func`` turn is a schema
            # refresh: newly available functions are added while the previous
            # user request remains active.  It is still a model turn.  Keep
            # the continuation explicit without exposing checker output or
            # ground truth to the agent.
            previous = ""
            for prior in reversed(self.messages):
                if prior.get("role") == "user" and prior.get("content"):
                    previous = str(prior["content"])
                    break
            refreshed = ", ".join(str(item["name"]) for item in functions)
            self.messages.append({
                "role": "user",
                "content": (
                    "[BFCL function refresh] Continue the previous request using "
                    "the currently available functions. Newly available schemas: "
                    + refreshed + "\nPrevious request:\n" + previous
                ),
            })
        skipped = False
        message = {"content": ""}
        usage = {}
        start = time.monotonic()
        if skipped:
            self.calls.append([])
            self.turn_outputs.append("")
        else:
            # The current AgentFlow coordinator receives the same BFCL system
            # instruction/schema and conversation history through its public
            # RPC endpoint.  This keeps the measured baseline on the project
            # agent instead of bypassing coordinator/skill-runner with a raw
            # model HTTP request.
            # The business Agent's public RPC has no OpenAI ``functions``
            # parameter.  Carry the row's currently available schemas in the
            # user turn, preserving the same BFCL instruction while letting the
            # coordinator decide through its normal path.
            schema_hint = json.dumps(functions, ensure_ascii=False, separators=(",", ":"))
            self.messages[-1]["content"] = (
                "[BFCL available functions; use only these schemas]\n" + schema_hint +
                "\n[BFCL request]\n" + self.messages[-1]["content"]
            )
            model_input = {"messages": self.messages, "functions": functions,
                           "function_call": "auto"}
            self.agent.set_bfcl_context(
                self.row, turn=turn,
                available_names=[f["name"] for f in functions],
                live_model_name=self.live_model_name,
            )
            model_point = self._point({"phase": "before", "kind": "model", "context": self.run_context,
                                       "name": "bfcl-model", "input": public_input})
            try:
                text, response = self.agent.ask(
                    self.messages, user_id="bfcl-agent", dialogue_id=str(self.row["id"]),
                    turn=turn,
                )
                message, usage = {"content": text}, response.get("usage") or {}
                self.model_calls += int(usage.get("llm_calls", 1) or 1)
            except Exception as error:
                message, usage = {"content": ""}, {}
                error_code = str(error)[:64]
                if error_code.startswith("external_model_"):
                    self.external_error_code = error_code
                else:
                    self.invalid_loops += 1
                self.calls.append([])
                if os.environ.get("BFCL_DEBUG_ERRORS") == "1":
                    try:
                        Path("/tmp/bfcl-v3-agent-error.code").write_text(
                            type(error).__name__ + ":" + str(error)[:240], encoding="utf-8")
                    except Exception:
                        pass
                self.turn_outputs.append("")
                self._point({"phase": "after", "kind": "model", "context": model_point.get("context", self.run_context),
                             "executed": False, "input": public_input, "output": {"error_code": error_code}})
            else:
                calls, content = decode_message(message)
                skill_records = _logged_call_records(self.agent.call_log, turn, str(self.row["id"]))
                skill_calls = [item["call"] for item in skill_records]
                calls_from_skill = bool(skill_records)
                if calls_from_skill:
                    calls = skill_calls
                known = {f["name"] for f in functions}
                valid_calls = []
                for call in calls:
                    name = _call_name(call)
                    if name not in known:
                        self.invalid_loops += 1
                    else:
                        valid_calls.append(call)
                calls = valid_calls
                self.turn_outputs.append(_safe_text(content))
                # Preserve the model turn in the benchmark conversation.  The
                # next BFCL turn must see the prior assistant/tool exchange.
                self.messages.append({"role": "assistant", "content": content})
                self._point({"phase": "after", "kind": "model", "context": model_point.get("context", self.run_context),
                             "executed": True, "input": public_input, "output": _safe_text(content),
                             "metrics": {"input_tokens": usage.get("prompt_tokens", usage.get("input_tokens", 0)),
                                         "output_tokens": usage.get("completion_tokens", usage.get("output_tokens", 0))}})
                self.calls.append(calls)
                for call in calls:
                    key = f"{turn}:{call}"
                    if key in self._last_call_keys:
                        self.invalid_loops += 1
                    self._last_call_keys.add(key)
                    action = self._point({"phase": "before", "kind": "action", "context": self.run_context,
                                          "name": _call_name(call), "input": {"call": _safe_text(call, 1024)}})
                    if calls_from_skill:
                        result, executed = "agentflow_skill_executed", True
                    else:
                        try:
                            from bfcl_eval.eval_checker.multi_turn_eval.multi_turn_utils import execute_multi_turn_func_call
                            result, _ = execute_multi_turn_func_call([call], self.row["initial_config"],
                                self.row["involved_classes"], self.live_model_name, self.row["id"],
                                long_context="long_context" in self.row.get("test_category", ""), is_evaL_run=True)
                            result = result[0] if result else ""
                            executed = True
                        except Exception as error:
                            _debug_exception("bfcl-v3-function-errors.log", f"{self.row.get('id','')}:turn{turn}:{call}", error)
                            result, executed = "execution_failed", False
                            self.invalid_loops += 1
                    action_end = {"phase": "after", "kind": "action",
                                  "context": action.get("context", self.run_context),
                                  "executed": executed,
                                  "actual_input": {"call": _safe_text(call, 1024)},
                                  "output": _safe_text(result, 512)}
                    if not executed:
                        action_end["error"] = {"code": "action_failed", "retryability": "unknown"}
                    self._point(action_end)
                    if not calls_from_skill:
                        self.messages.append({"role": "tool", "content": _safe_text(result, 512)})
                # The coordinator response already contains the skill's
                # compact result. Do not append a synthetic ``role=tool``
                # message: it has no matching assistant tool_call id and can
                # make the next provider request invalid or ambiguous. The
                # private call journal remains authoritative for BFCL state
                # and checking.
        self.tokens += int(usage.get("total_tokens", 0) or 0)
        final = turn == len(self.row["turns"]) - 1
        output = {"response": _safe_text(self.turn_outputs[-1] if self.turn_outputs else ""),
                  "turn": turn, "bfcl_valid": None, "strict_passed": None}
        if final:
            output.update(self.finish())
        final_run_context = self.run_context
        self._point({"phase": "after", "kind": "run", "context": self.run_context,
                     "output": _safe_obj(output),
                     "business_status": "failed" if self.external_error_code else "succeeded"})
        if final:
            receipts = self._receipt_counts(final_run_context)
            applied = receipts.get("applied", 0)
            if applied:
                level = str(self.level).upper()
                if level in self.policy_applied:
                    output[level.lower() + "_applied"] = applied
        self.run_context = None
        return output

    def finish(self) -> dict:
        if self.external_error_code:
            return {"bfcl_valid": None, "strict_passed": None,
                    "error_code": self.external_error_code,
                    "invalid_loop_count": None,
                    "latency_ms": round((time.monotonic_ns() - self.started_ns) / 1e6, 3),
                    "model_calls": self.model_calls, "tokens": self.tokens,
                    "level": self.level,
                    "l1_applied": self.policy_applied.get("L1", 0),
                    "l2_applied": self.policy_applied.get("L2", 0),
                    "l3_applied": self.policy_applied.get("L3", 0)}
        expected = self.row["ground_truth"]
        # BFCL's checker expects turns -> response steps -> call expressions.
        # The AgentFlow response is one step per turn, so retain that middle
        # list even when the model emits several parallel calls.
        decoded = [[turn] for turn in self.calls]
        valid = False
        error_code = ""
        try:
            from bfcl_eval.eval_checker.multi_turn_eval.multi_turn_checker import (
                multi_turn_checker, multi_turn_irrelevance_checker)
            check = multi_turn_checker(decoded, expected, self.row, self.row.get("test_category", ""),
                                       self.live_model_name + "-strict")
            irrelevant = multi_turn_irrelevance_checker(decoded, expected)
            valid = bool(check.get("valid")) and bool(irrelevant.get("valid"))
            if not valid:
                error_code = str(check.get("error_type") or irrelevant.get("error_type") or "bfcl_mismatch")[:96]
        except Exception as error:
            _debug_exception("bfcl-v3-checker-errors.log", str(self.row.get("id", "")), error)
            error_code = "bfcl_checker_failed"
        predicted_names = [[_call_name(call) for call in turn if _call_name(call)]
                           for turn in self.calls]
        expected_names = [[_call_name(call) for call in turn if _call_name(call)]
                          for turn in expected]
        pairs = zip(predicted_names, expected_names)
        intent_hits = sum(1 for predicted, gold in pairs if predicted == gold)
        intent_rate = intent_hits / len(expected_names) if expected_names else None
        elapsed_ms = (time.monotonic_ns() - self.started_ns) / 1e6
        return {"bfcl_valid": valid, "strict_passed": int(valid), "error_code": error_code,
                "function_f1": _function_f1(self.calls, expected),
                "intent_f1": _function_f1(self.calls, expected),
                "intent_recognition_rate": intent_rate,
                "invalid_loop_count": self.invalid_loops,
                "latency_ms": round(elapsed_ms, 3),
                "model_calls": self.model_calls,
                "tokens": self.tokens,
                "level": self.level,
                "l1_applied": self.policy_applied.get("L1", 0),
                "l2_applied": self.policy_applied.get("L2", 0),
                "l3_applied": self.policy_applied.get("L3", 0)}


def run_entry() -> int:
    source = Source(SOURCE_ZIP)
    current: Trial | None = None
    current_row_id = ""
    agent = AgentRunner()

    def receive(task: dict):
        nonlocal current, current_row_id
        state = task.get("initial_state") or {}
        row_id = str(state.get("row_id", ""))
        row = source.get(row_id)
        artifact_level = "L0"
        version_file = Path(os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_CONFIG", "")).with_name("version.json")
        if version_file.is_file():
            try:
                artifact_level = str(json.loads(version_file.read_text()).get("artifact", {}).get("level", "L0"))
            except Exception:
                pass
        if current is None or current_row_id != row_id:
            current = Trial(source, row, artifact_level, agent)
            current_row_id = row_id
        return current.receive(task)

    try:
        if not evaluate(receive):
            raise RuntimeError("bfcl_evaluation_entry_not_enabled")
        return 0
    finally:
        agent.close()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--evolution-entry", action="store_true")
    args = parser.parse_args(argv)
    if not args.evolution_entry:
        parser.error("BFCL host is only an isolated evolution entry")
    return run_entry()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except SystemExit as error:
        if error.code in (0, None):
            raise
        try:
            Path("/tmp/bfcl-v3-host-error.code").write_text(
                "SystemExit:" + str(error.code)[:160], encoding="utf-8")
        except Exception:
            pass
        raise
    except BaseException as error:
        # The native evaluator intentionally redacts child stderr.  Keep one
        # fixed-code diagnostic outside experiment artifacts for local runner
        # debugging; never write exception text, prompts or credentials.
        try:
            Path("/tmp/bfcl-v3-host-error.code").write_text(
                type(error).__name__ + ":" + str(error)[:160], encoding="utf-8")
        except Exception:
            pass
        raise
