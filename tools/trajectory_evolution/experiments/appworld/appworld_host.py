"""Real AgentFlow decisions executed once in the official AppWorld sandbox.

Each native task owns an independent AppWorld and private AgentFlow daemon.
Only public instruction, profile, API docs and observations reach the model.
The official final database tests run after interaction, never during planning.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import sys
import time
import uuid

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
HOST_VERSION = "appworld-agentflow-v3"
REDACTION = "<redacted:credential_field>"
_SECRET_FIELDS = {"authorization", "apikey", "password", "passwd", "accesstoken",
                  "refreshtoken", "clientsecret", "privatekey", "cookie", "secret", "credentials"}
_NATIVE_CREDENTIAL_PATTERNS = [
    (r"\bsk-[A-Za-z0-9_-]{16,}\b", 0), (r"\bark-[A-Za-z0-9_-]{16,}\b", 0),
    (r"\bAKIA[A-Z0-9]{16}\b", 0), (r"\bAIza[A-Za-z0-9_-]{20,}\b", 0),
    (r"\bgh[pousr]_[A-Za-z0-9_]{20,}\b", 0), (r"\bgithub_pat_[A-Za-z0-9_]{20,}\b", 0),
    (r"\bxox[a-z]-[A-Za-z0-9-]{16,}\b", 0),
    (r"\bBearer\s+[A-Za-z0-9._~+/-]{16,}=*\b", re.I),
    (r"(api[_ -]?key|access[_ -]?token|secret|password)\s*[:=]\s*[\"']?[A-Za-z0-9._~+/-]{12,}", re.I),
    (r"([?&](api[_-]?key|access[_-]?token|token|signature|sig|key)=)[^&\s\"']{8,}", re.I),
    (r"\beyJ[A-Za-z0-9_-]{16,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\b", 0),
    (r"-----BEGIN [A-Z ]*PRIVATE KEY-----", 0),
]


def native_contains_secret(text, depth=0):
    if depth > 16:
        return True
    for name, secret in os.environ.items():
        if len(secret) >= 8 and any(part in name.upper() for part in
                                  ["KEY", "TOKEN", "SECRET", "PASSWORD", "CREDENTIAL", "AUTH"]):
            if secret in text:
                return True
    try:
        decoded = json.loads(text)
    except (ValueError, TypeError):
        decoded = None
    if isinstance(decoded, (dict, list)):
        pending = [decoded]
        while pending:
            item = pending.pop()
            if isinstance(item, dict):
                pending.extend(item.keys())
                pending.extend(item.values())
            elif isinstance(item, list):
                pending.extend(item)
            elif isinstance(item, str) and native_contains_secret(item, depth + 1):
                return True
        return False
    if isinstance(decoded, str):
        return native_contains_secret(decoded, depth + 1)
    inspected = text.replace(REDACTION, "")
    return any(re.search(pattern, inspected, flags) for pattern, flags in _NATIVE_CREDENTIAL_PATTERNS)


def native_view(value):
    """Redact the persisted view, preserving actual model/environment values."""
    if isinstance(value, dict):
        return {(f"redacted_{key}" if re.sub(r"[-_]", "", str(key)).lower() in _SECRET_FIELDS else key):
                REDACTION if re.sub(r"[-_]", "", str(key)).lower() in _SECRET_FIELDS
                else native_view(item) for key, item in value.items()}
    if isinstance(value, list):
        return [native_view(item) for item in value]
    if not isinstance(value, str):
        return value
    for name, secret in os.environ.items():
        if len(secret) >= 8 and any(part in name.upper() for part in
                                  ["KEY", "TOKEN", "SECRET", "PASSWORD", "CREDENTIAL", "AUTH"]):
            value = value.replace(secret, REDACTION)
    quoted = re.compile(r"(?i)((?:[\"']?(?:password|passwd|access[_-]?token|refresh[_-]?token|api[_-]?key|secret|authorization)[\"']?)\s*[:=]\s*)([\"'])([^\"']*)\2")
    value = quoted.sub(lambda match: match[1] + match[2] + REDACTION + match[2], value)
    patterns = [r"\b(?:sk-|ark-)[A-Za-z0-9_-]{16,}\b",
                r"\beyJ[A-Za-z0-9_-]{16,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\b",
                r"(?i)\bBearer\s+[A-Za-z0-9._~+/-]{16,}=*\b",
                r"(?i)(?:api[_ -]?key|access[_ -]?token|secret|password)\s*[:=]\s*[A-Za-z0-9._~+/-]{12,}"]
    for pattern in patterns:
        value = re.sub(pattern, REDACTION, value)
    return REDACTION if native_contains_secret(value) else value


def real_clock():
    return time.clock_gettime(time.CLOCK_MONOTONIC)
os.environ.setdefault("APPWORLD_ROOT", str(HERE / "runtime/world"))
for directory in [HERE, HERE / "runtime", HERE.parent / "bfcl-v3",
                  ROOT / "tools/trajectory_evolution/clients/python"]:
    sys.path.insert(0, str(directory))

from appworld_agent_runtime import AgentRunner, COORDINATOR_INSTRUCTION
from appworld_skill import read_reply
from evolution import Evolution
from evolution_entry import evaluate


def public_value(value):
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, dict):
        return {str(k): public_value(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [public_value(v) for v in value]
    if hasattr(value, "model_dump"):
        return public_value(value.model_dump(mode="json"))
    if hasattr(value, "isoformat"):
        return value.isoformat()
    raise TypeError("appworld_public_value_unsupported")


def public_context(world):
    task = world.task
    supervisor = {name: getattr(task.supervisor, name) for name in
                  ["first_name", "last_name", "email", "phone_number"]}
    docs = task.api_docs
    # Discover all other public schemas through the official api_docs APIs.
    bootstrap_docs = {name: public_value(docs[name]) for name in
                      ["api_docs", "supervisor"] if name in docs}
    return {"instruction": task.instruction, "supervisor": supervisor,
            "datetime": public_value(task.datetime),
            "app_descriptions": public_value(task.app_descriptions),
            "bootstrap_api_docs": bootstrap_docs}


def tracker_metrics(tracker):
    """Read the original tracker; do not infer success from completion alone."""
    if hasattr(tracker, "to_dict"):
        raw = tracker.to_dict()
    elif hasattr(tracker, "model_dump"):
        raw = tracker.model_dump(mode="json")
    else:
        raw = {}
    passes = raw.get("passes", getattr(tracker, "passes", None))
    failures = raw.get("failures", getattr(tracker, "failures", None))
    if isinstance(passes, list) and isinstance(failures, list):
        passed, failed = len(passes), len(failures)
    else:
        passed = raw.get("num_passed", getattr(tracker, "num_passed", None))
        failed = raw.get("num_failed", getattr(tracker, "num_failed", None))
    total = raw.get("num_tests", getattr(tracker, "num_tests", None))
    success = raw.get("success", getattr(tracker, "success", None))
    if (not isinstance(passed, int) or not isinstance(failed, int)
            or not isinstance(total, int) or total <= 0 or not isinstance(success, bool)):
        raise RuntimeError("appworld_tracker_schema_unknown")
    return {"appworld_reward": float(success), "strict_passed": int(success),
            "quality_score": passed / total, "official_tests_total": total,
            "official_tests_passed": passed, "official_tests_failed": failed}


def reply_diagnostic(runner):
    """Preserve only structural failure evidence before private logs are removed."""
    models, stable_errors = [], set()
    budget_notice = False
    expected = getattr(runner.client, "_appworld_expected_dialogue", None)
    for path in (runner.root / "sessionlogs").rglob("*.jsonl"):
        for line in path.read_text(encoding="utf-8").splitlines():
            try:
                event = json.loads(line)
            except ValueError:
                continue
            data = event.get("data") or {}
            if not expected or expected not in [event.get("dialogue_id"),
                    (data.get("meta") or {}).get("dialogue_id")]:
                continue
            for code in re.findall(r"\bappworld_reply_[a-z0-9_]+\b", line):
                stable_errors.add(code)
            budget_notice |= any(signal in line.lower() for signal in
                ["sdk_call_budget_exceeded", "tool_call_budget_exceeded",
                 "tool execution exceeded remaining node budget"])
            if event.get("type") != "llm.response":
                continue
            response = data.get("provider_response") or data.get("response") or {}
            choices = response.get("choices") or []
            message = choices[0].get("message", {}) if choices else {}
            models.append({"timestamp_us": event.get("timestamp_us", 0),
                "skill": data.get("skill_name"),
                "finish_reason": choices[0].get("finish_reason") if choices else None,
                "http_status": (data.get("meta") or {}).get("http_status"),
                "tool_names": [call.get("function", {}).get("name")
                    for call in message.get("tool_calls", [])],
                "legacy_function_call": bool(message.get("function_call")),
                "has_text": bool(message.get("content"))})
    models.sort(key=lambda item: item.pop("timestamp_us", 0))
    state = json.loads(Path(runner.meta["state_path"]).read_text())
    module = Path(runner.meta["module_path"]).read_text()
    nonce = re.search(r"_REQUEST_ID = ['\"]([a-f0-9]+)['\"]", module)
    turn = re.search(r"_TURN = ([0-9]+)", module)
    return {"last_model_responses": models[-4:], "stable_reply_errors": sorted(stable_errors),
        "rpc": getattr(runner.client, "_appworld_rpc_diagnostic", {}),
        "state_nonce_matches_host": state.get("request_id") == runner.meta.get("request_id"),
        "module_nonce_matches_state": bool(nonce) and nonce[1] == state.get("request_id"),
        "module_turn_matches_state": bool(turn) and int(turn[1]) == state.get("turn"),
        "sdk_budget_stop_observed": budget_notice}


def natural_model_stop(diagnostic):
    """Only a proven current-turn normal provider stop reaches final grading."""
    models = diagnostic.get("last_model_responses") or []
    if not models:
        return False
    last = models[-1]
    return (diagnostic.get("rpc", {}).get("matched_terminal") is True
            and last.get("http_status") in (200, "200")
            and last.get("finish_reason") == "stop"
            and not last.get("tool_names") and not last.get("legacy_function_call")
            and not diagnostic.get("stable_reply_errors")
            and diagnostic.get("sdk_budget_stop_observed") is False
            and all(diagnostic.get(key) is True for key in
                ["state_nonce_matches_host", "module_nonce_matches_state", "module_turn_matches_state"]))


class Trial:
    def __init__(self, task):
        self.task = task
        trial_url = os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_URL", "")
        self.evo = Evolution(trial_url, os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_TOKEN", ""),
                             timeout_ms=5000, retries=0) if trial_url else None
        self.run_context = None
        self.calls = self.covered_calls = self.known_tokens = 0
        self.tokens_complete = True
        self.applied = {"L1": 0, "L2": 0, "L3": 0}
        self.degraded_points = 0
        self.level = "L0"
        trial_config = os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_CONFIG", "")
        version_path = Path(trial_config).with_name("version.json") if trial_config else None
        if version_path and version_path.is_file():
            self.level = str(json.loads(version_path.read_text()).get("artifact", {}).get("level", "L0"))

    def point(self, fields):
        try:
            if self.evo is None:
                raise RuntimeError("appworld_standalone_no_native_boundary")
            safe = native_view(fields)
            result = self.evo.point(safe, timeout_ms=1000)
            return result
        except Exception:
            self.degraded_points += 1
            return {"context": fields.get("context", {}), "input": fields.get("input"),
                    "decision": "allow", "degraded": True}

    def account(self, usage):
        calls = int(usage.get("llm_calls", 0) or 0)
        self.calls += calls
        self.covered_calls += int(usage.get("token_covered_calls", 0) or 0)
        self.known_tokens += int(usage.get("total_tokens", 0) or 0)
        if calls:
            self.tokens_complete &= bool(usage.get("tokens_complete"))

    def receipts(self):
        try:
            status = self.evo.invoke("task.status", {"run_id": self.run_context.get("run_id")})
            applied = int((status.get("receipts") or {}).get("applied", 0) or 0)
            if self.level.upper() in self.applied:
                self.applied[self.level.upper()] = applied
        except Exception:
            pass

    def costs(self, started):
        return {"tokens": self.known_tokens if self.tokens_complete else None,
                "known_agent_tokens": self.known_tokens, "tokens_complete": self.tokens_complete,
                "model_calls": self.calls, "token_covered_calls": self.covered_calls,
                "latency_ms": round((real_clock() - started) * 1000, 3),
                "l1_applied": self.applied["L1"], "l2_applied": self.applied["L2"],
                "l3_applied": self.applied["L3"], "level": self.level,
                "degraded_boundary_points": self.degraded_points}

    def receive(self):
        started = real_clock()
        raw_id = str((self.task.get("initial_state") or {})["source_task_id"])
        if not re.fullmatch(r"[a-z0-9]+_[0-9]+", raw_id):
            raise RuntimeError("appworld_task_id_invalid")
        # Public instruction is required by the native learning summary. It
        # lives in specs.json, outside the separate hidden ground_truth files.
        specs = json.loads((Path(os.environ["APPWORLD_ROOT"]) /
                            "data/tasks" / raw_id / "specs.json").read_text())
        begin = self.point({"phase": "before", "kind": "run", "level": self.level,
            "capabilities": ["model", "actions", "loop"],
            "conversation_id": self.task.get("conversation_id"),
            "input": {"task_id": self.task.get("task_id", "appworld-" + raw_id),
                      "benchmark": "appworld", "instruction": specs["instruction"],
                      "native_view_redacted": True}})
        self.run_context = begin.get("context", {})
        runner = world = None
        history = []
        reason = "max_steps"
        tool_errors = repeated_code = decisions = 0
        previous_code = None
        try:
            from appworld import AppWorld
            AgentRunner._load_env_file(os.environ)
            for name in ["HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy"]:
                os.environ.pop(name, None)
            experiment = "agentflow-" + uuid.uuid4().hex
            evidence_root = Path(os.environ.get("APPWORLD_EVIDENCE_ROOT",
                str(ROOT / "trace_output/appworld/training/official-evidence")))
            evidence = evidence_root / experiment
            evidence.mkdir(parents=True, exist_ok=False)
            official_link = Path(os.environ["APPWORLD_ROOT"]) / "experiments/outputs" / experiment
            official_link.parent.mkdir(parents=True, exist_ok=True)
            official_link.symlink_to(evidence, target_is_directory=True)
            world = AppWorld(task_id=raw_id, experiment_name=experiment,
                             raise_on_unsafe_syntax=True, null_patch_unsafe_execution=True,
                             timeout_seconds=int(os.environ.get("APPWORLD_CODE_TIMEOUT", "30")))
            context = public_context(world)
            policy = ("Complete the user's AppWorld task by submitting Python code. "
                      "The public `apis` object is already instantiated in the execution "
                      "namespace. Do not import apis: it is an existing object, not an "
                      "importable module. Python variables persist across code blocks. "
                      "Call apis.<app>.<api>(...) and print useful results. Discover "
                      "available APIs and their exact parameters through apis.api_docs "
                      "using the public documentation below. For authenticated APIs, "
                      "consult the documented login API and obtain the supervisor's "
                      "account credentials using the documented supervisor API; keep "
                      "the returned access token in a variable and supply it where the "
                      "API documentation requires it. An API error is an observation "
                      "to diagnose before repeating that code. No hidden evaluation or "
                      "host filesystem access. Submit apis.supervisor.complete_task() "
                      "when the requested work is done.\n\n" +
                      json.dumps(context, ensure_ascii=False))
            runner = AgentRunner(self.level)
            for turn in range(int(os.environ.get("APPWORLD_MAX_STEPS", "30"))):
                loop = self.point({"phase": "before", "kind": "loop", "name": "appworld-interaction",
                    "context": self.run_context, "input": {"step": turn, "continue": True}})
                stop = loop.get("decision") in {"stop", "deny", "skip", "reject"}
                if isinstance(loop.get("input"), dict) and loop["input"].get("continue") is False:
                    stop = True
                self.point({"phase": "after", "kind": "loop",
                    "context": loop.get("context", self.run_context), "executed": True,
                    "actual_input": loop.get("input"), "decision": "stop" if stop else "continue",
                    "output": {"stopped": stop}})
                if stop:
                    reason = "policy_stop"
                    break
                model_input = {"system": policy, "messages": history, "turn": turn,
                               "native_view_redacted": True}
                model = self.point({"phase": "before", "kind": "model", "name": "appworld-agentflow",
                    "context": loop.get("context", self.run_context), "input": model_input,
                    "binding": {"pointer": "/system", "mode": "text"}})
                # Strategies may change only the native /system binding. The
                # real public history retains the official simulator values.
                native_prepared = model.get("input", model_input)
                prepared = {**model_input, "native_view_redacted": True}
                if isinstance(native_prepared, dict) and isinstance(native_prepared.get("system"), str):
                    observed_system = native_prepared["system"]
                    original_system_view = native_view(policy)
                    if observed_system != original_system_view:
                        if observed_system.startswith(original_system_view):
                            prepared["system"] = policy + observed_system[len(original_system_view):]
                        elif REDACTION not in observed_system:
                            prepared["system"] = observed_system
                if not isinstance(prepared, dict) or not isinstance(prepared.get("system"), str):
                    prepared = model_input
                if model.get("decision") in {"stop", "deny", "skip", "reject"}:
                    raise RuntimeError("policy_model_rejected")
                runner._partial_usage = {}
                accounted = False
                try:
                    runner.configure(policy=prepared["system"], tools_schema=[],
                                     messages=prepared.get("messages", history), turn=turn)
                    _, result = runner.ask([
                        {"role": "system", "content": COORDINATOR_INSTRUCTION},
                        {"role": "user", "content": "[AppWorld request]\nDispatch appworld-office once. "
                         "Read its complete current public request and use appworld_submit_code once. "
                         "Do not execute AppWorld APIs in this coordinator."}],
                        user_id="appworld-training", dialogue_id="appworld-" + raw_id, turn=turn)
                    usage = result.get("usage") or {}
                    self.account(usage)
                    accounted = True
                    code = read_reply(runner.meta)["code"]
                except Exception as reply_error:
                    if not accounted:
                        self.account(runner._partial_usage)
                    diagnostic = {}
                    try:
                        diagnostic = reply_diagnostic(runner)
                        (evidence / "reply-diagnostic.json").write_text(
                            json.dumps(native_view(diagnostic), ensure_ascii=False))
                    except (OSError, ValueError, TypeError):
                        pass
                    self.point({"phase": "after", "kind": "model",
                        "context": model.get("context", self.run_context), "executed": False,
                        "status": "failed",
                        "actual_input": prepared, "output": {"error_code": "agent_reply_invalid"},
                        "error": {"code": "agent_reply_invalid", "retryability": "unknown"}})
                    raise
                decisions += 1
                self.point({"phase": "after", "kind": "model",
                    "context": model.get("context", self.run_context), "executed": True,
                    "actual_input": prepared, "output": {"code": code},
                    "metrics": {"input_tokens": usage.get("prompt_tokens"),
                                "output_tokens": usage.get("completion_tokens")}
                    if usage.get("tokens_complete") else {}})
                action = self.point({"phase": "before", "kind": "action", "name": "appworld.execute",
                    "context": loop.get("context", self.run_context),
                    "input": {"code": code, "native_view_redacted": True}})
                selected = action.get("input")
                original_view = native_view(code)
                if isinstance(selected, dict) and isinstance(selected.get("code"), str) and selected["code"] != original_view:
                    if REDACTION in selected["code"]:
                        raise RuntimeError("policy_action_redaction_invalid")
                    code = selected["code"]
                if action.get("decision") in {"stop", "deny", "skip", "reject"}:
                    reason = "policy_action_rejected"
                    self.point({"phase": "after", "kind": "action",
                        "context": action.get("context", self.run_context), "executed": False,
                        "status": "failed",
                        "actual_input": {"code": code}, "output": {"error_code": reason},
                        "error": {"code": reason, "retryability": "non_retryable"}})
                    break
                observation = world.execute(code)
                if not isinstance(observation, str):
                    raise RuntimeError("appworld_execute_result_invalid")
                failed = observation.lstrip().startswith(("Execution failed", "Traceback"))
                tool_errors += int(failed)
                repeated_code += int(code == previous_code)
                previous_code = code
                self.point({"phase": "after", "kind": "action",
                    "context": action.get("context", self.run_context), "executed": True,
                    "status": "failed" if failed else "completed",
                    "actual_input": {"code": code, "native_view_redacted": True},
                    "output": {"observation": observation, "native_view_redacted": True},
                    **({"error": {"code": "appworld_code_error", "retryability": "unknown"}} if failed else {})})
                history.extend([{"role": "assistant", "content": code},
                                {"role": "user", "content": observation}])
                if world.task_completed():
                    reason = "task_completed"
                    break
            # Grade only now. Never feed this tracker or its hidden tests back to the model.
            tracker = world.evaluate()
            metrics = tracker_metrics(tracker)
            self.receipts()
            output = {**metrics, **self.costs(started), "termination_reason": reason,
                "task_completed": bool(world.task_completed()), "agent_decisions": decisions,
                "tool_error_count": tool_errors, "repeated_code_count": repeated_code,
                "official_experiment_name": experiment, "response": "",
                "official_evidence_dir": str(evidence),
                "evaluation_status": "completed", "error_code": ""}
            train_ids = set((HERE / "datasets/train_ids.txt").read_text().splitlines())
            if raw_id in train_ids:
                output["training_feedback"] = {"source": "official_training_tests",
                    "failures": [{"requirement": item.get("requirement"),
                                  "label": item.get("label")} for item in tracker.failures]}
            self.point({"phase": "after", "kind": "run", "context": self.run_context,
                        "output": output, "business_status": "succeeded"
                        if metrics["appworld_reward"] == 1 else "failed"})
            return output
        except Exception as error:
            self.receipts()
            code = str(error)
            safe_code = code if re.fullmatch(r"(?:appworld|agent|policy)_[a-z0-9_]+", code) else "appworld_host_error"
            output = {**self.costs(started), "appworld_reward": None, "quality_score": None,
                "strict_passed": None, "evaluation_status": "errored", "error_code": safe_code,
                "error_type": type(error).__name__, "agent_decisions": decisions,
                "tool_error_count": tool_errors, "repeated_code_count": repeated_code,
                "response": ""}
            if "experiment" in locals() and "evidence" in locals():
                output.update(official_experiment_name=experiment,
                              official_evidence_dir=str(evidence))
            self.point({"phase": "after", "kind": "run", "context": self.run_context,
                        "output": output, "business_status": "failed",
                        "status": "failed",
                        "error": {"code": safe_code, "retryability": "unknown"}})
            return output
        finally:
            if world is not None:
                world.close()
            if runner is not None:
                runner.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-entry", action="store_true")
    parser.add_argument("--task-id")
    args = parser.parse_args()
    if args.run_entry:
        if not os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_URL"):
            raise RuntimeError("native_evolution_boundary_required")
        if not evaluate(lambda task: Trial(task).receive()):
            raise RuntimeError("appworld_evaluation_entry_not_enabled")
        return 0
    if args.task_id:
        task = {"task_id": "appworld-" + args.task_id,
                "initial_state": {"source_task_id": args.task_id}}
        print(json.dumps(Trial(task).receive(), ensure_ascii=False, indent=2))
        return 0
    parser.error("choose --run-entry or --task-id")


if __name__ == "__main__":
    raise SystemExit(main())
