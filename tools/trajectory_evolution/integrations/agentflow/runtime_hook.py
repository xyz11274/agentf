"""L1–L3 薄接入：C++ 决策、宿主执行、实际边界回执；默认关闭。

快照按 dialogue 固定，Loop 状态按 dispatch 实例隔离。配置/查询失败时继续原流程，
只记录固定错误码；不自动晋升候选记忆，不修改既有串并行调度。
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import uuid

_spec = importlib.util.spec_from_file_location(
    "_trajectory_shadow_transport", Path(__file__).resolve().with_name("shadow_hook.py"),
)
_shared = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_shared)

HOOK_NAME = "trajectory_evolution_runtime_v1"
HOOK_ORDER = 200
HOOK_EVENTS = [
    "agent.dialogue.start", "agent.dialogue.end", "agent.dialogue.error",
    "coordinator_v1.before_llm_call", "coordinator_v2.before_llm_call",
    "skill_executor.before_llm_call", "llm.request.prepared",
    "coordinator_v1.after_llm_call", "coordinator_v2.after_llm_call",
    "skill_executor.after_llm_call", "skill_executor.after_skill_complete",
    "coordinator.before_skill_dispatch", "coordinator.after_skill_dispatch",
    "skill_executor.before_tool_call", "skill_executor.tool_result",
    "coordinator_v2.before_tool_call", "coordinator_v2.after_tool_call",
    "loop.before_decision", "loop.after_decision",
]

_pending = {}
_replans = {}
_loop_updates = {}


def _configuration():
    filename = os.environ.get("AF_EVOLUTION_RUNTIME_CONFIG", "")
    if not filename:
        return None
    with open(filename, "rb") as stream:
        raw = stream.read(65537)
    if len(raw) > 65536:
        raise ValueError("configuration_limit")
    config = json.loads(raw)
    if not isinstance(config, dict) or config.get("schema_version") != 1:
        raise ValueError("configuration_invalid")
    if config.get("mode") == "disabled":
        return None
    required = {"schema_version", "mode", "agent_id", "memory_path", "executable",
                "rpc_timeout_ms", "process_timeout_ms", "shadow_options", "options",
                "experience_template_file"}
    if set(config) != required or config["mode"] != "active":
        raise ValueError("configuration_invalid")
    for key in ("rpc_timeout_ms", "process_timeout_ms"):
        if type(config[key]) is not int or not 1 <= config[key] <= 10000:
            raise ValueError("configuration_invalid")
    for key in ("memory_path", "executable", "experience_template_file"):
        if not isinstance(config[key], str) or not Path(config[key]).is_absolute():
            raise ValueError("absolute_path_required")
    if not isinstance(config["options"], dict) or not isinstance(config["shadow_options"], dict):
        raise ValueError("configuration_invalid")
    return config


def _invoke(config, operation, request):
    # exec_skill_key 是调用身份，不是凭据；仅通过结构化协议传入核心。
    # 不把这个宿主专有 KEY 环境变量送进通用密钥扫描，其他凭据仍保留保护。
    environment = {key: value for key, value in os.environ.items() if key != "AF_EXEC_SKILL_KEY"}
    result = _shared._invoke(config, operation, request, command="runtime", env=environment)
    from agentflow_runtime.security import redact_secrets
    safe, _ = redact_secrets(result)
    if result != safe:
        raise ValueError("snapshot_unsafe")
    return result


def _execution(payload):
    execution = os.environ.get("AF_EXEC_SKILL_KEY", "")
    skill = os.environ.get("AF_SKILL_NAME", "") or payload.get("skill_name", "")
    return ("skill:" + skill, execution) if execution else ("coordinator", "coordinator")


def _belongs_to_current(query, execution):
    identity = query["advice_query"]["identity"]
    return (query["execution_id"] == execution
            and identity["run_id"] == os.environ.get("AF_DIALOGUE_ID", "")
            and identity["user_id"] == os.environ.get("AF_USER_ID", "")
            and identity.get("conversation_id", "") == os.environ.get("AF_CLIENT_ID", ""))


def _receipt(config, policy, query, observation):
    try:
        result = _invoke(config, "verify", {
            "policy": policy, "query": query, "observation": observation,
        })
    except Exception:
        result = {
            "identity": query["advice_query"]["identity"], "boundary": query["boundary"],
            "execution_id": query["execution_id"], "call_id": observation.get("call_id", ""),
            "application": {"query_id": query["advice_query"]["query_id"],
                            "decision_id": query["advice_query"]["decision_id"],
                            "policy_id": policy["policy_id"], "policy_revision": policy["policy_id"],
                            "status": "failed", "reason": "receipt_verification_unavailable"},
        }
    _shared._record("evolution.policy.receipt", result)


def _flush(prefix):
    for key in [key for key in _pending if key[:len(prefix)] == prefix]:
        for config, policy, query in _pending.pop(key):
            try:
                _receipt(config, policy, query, {})
            except Exception:
                _shared._record("evolution.policy.error", {
                    "query_id": query["advice_query"]["query_id"],
                    "status": "failed", "reason": "receipt_unavailable",
                })
    for key in [key for key in _replans if key[:len(prefix)] == prefix]:
        config, policy, query = _replans.pop(key)
        try:
            _receipt(config, policy, query, {})
        except Exception:
            pass
    for key in [key for key in _loop_updates if key[:len(prefix)] == prefix]:
        _loop_updates.pop(key)


def handle(event, ctx):
    name, payload = event.name, event.payload
    try:
        # 最终请求和出口回执属于已发起的查询；热关闭配置不能抹掉这些事实。
        if name == "llm.request.prepared" or name.endswith(".after_llm_call") or name in {
            "agent.dialogue.end", "agent.dialogue.error", "skill_executor.after_skill_complete",
        }:
            from agentflow_runtime.trace import current_span_id
            _, current_execution = _execution(payload)
            for pending_key, records in list(_pending.items()):
                if not _belongs_to_current(records[0][2], current_execution):
                    continue
                if name == "llm.request.prepared" or name.endswith(".after_llm_call"):
                    if pending_key[2:] != ("llm", current_span_id()):
                        continue
                records = _pending.pop(pending_key)
                for saved_config, policy, query in records:
                    observation = {"request": payload.get("request", {}),
                                   "transport_allowed": payload.get("transport_allowed", False),
                                   "call_id": payload.get("call_id", "")} if name == "llm.request.prepared" else {}
                    _receipt(saved_config, policy, query, observation)
            if name == "llm.request.prepared" or name.endswith(".after_llm_call"):
                return
            for owner, record in list(_replans.items()):
                if _belongs_to_current(record[2], current_execution):
                    _replans.pop(owner)
                    _receipt(*record, {})
            for owner, update in list(_loop_updates.items()):
                if _belongs_to_current(update[1], current_execution):
                    _loop_updates.pop(owner)
        config = _configuration()
        if config is None:
            return
        from agentflow_runtime.security import register_environment_secrets
        from agentflow_runtime.trace import current_span_id
        register_environment_secrets()
        identity = _shared._identity(config)
        options = config["options"]
        if (identity["user_id"] not in options.get("allowed_users", [])
                or (options.get("allowed_conversations")
                    and identity["conversation_id"] not in options["allowed_conversations"])):
            return
        key = _shared._state_key(identity).replace("/shadow/", "/runtime/")
        scope, execution = _execution(payload)
        owner = (key, execution)
        loop_key = key + "/loop/" + hashlib.sha256(execution.encode()).hexdigest()

        with _shared._namespace(config) as namespace:
            if name in {"agent.dialogue.end", "agent.dialogue.error", "skill_executor.after_skill_complete"}:
                _flush(owner)
                namespace.remove(loop_key)
                if name != "skill_executor.after_skill_complete":
                    namespace.remove(key)
                return
            if name == "agent.dialogue.start":
                if _shared._read_state(namespace, key) is not None:
                    return
                state = {"identity": identity}
                try:
                    with open(config["experience_template_file"], "rb") as stream:
                        template = stream.read(16385)
                    if len(template) > 16384:
                        raise ValueError("configuration_limit")
                    runtime_options = dict(options)
                    runtime_options["experience_template"] = template.decode("utf-8")
                    runtime_options["host_capabilities"] = ["context.request", "action.filter", "loop.decide"]
                    state["policy"] = _invoke(config, "compile", {
                        "memory_path": config["memory_path"], "identity": identity,
                        "shadow_options": config["shadow_options"], "options": runtime_options,
                    })
                except Exception:
                    state["error"] = "policy_compile_failed"
                if namespace.put(key, _shared._encode(state)) is False:
                    raise ValueError("snapshot_publish_failed")
                _shared._record("evolution.policy.snapshot", state)
                return

            state = _shared._read_state(namespace, key)
            if not state or state.get("identity") != identity or state.get("error"):
                raise ValueError("run_not_initialized")
            policy = state["policy"]
            if scope not in policy["snapshot"]["options"]["target_scopes"]:
                return
            if name == "loop.after_decision":
                # 计数绑定宿主最终选择；后续 Hook 拒绝 Stop 时不能残留粘滞停止态。
                update = _loop_updates.pop(owner, None)
                if update:
                    progress, original_query = update
                    previous = original_query.get("previous", {})
                    actual = payload.get("decision", "continue")
                    if actual != progress.get("decision"):
                        progress.update(decision=actual, reason=payload.get("reason", ""),
                                        rule_id="", replans=previous.get("replans", 0))
                    if namespace.put(loop_key, _shared._encode(progress)) is False:
                        raise ValueError("loop_state_publish_failed")
                records = _pending.pop((*owner, "loop"), [])
                for saved_config, saved_policy, query in records:
                    decision = payload.get("decision", "continue")
                    if decision == "replan":
                        _replans[owner] = (saved_config, saved_policy, query)
                    else:
                        _receipt(saved_config, saved_policy, query, {"loop_decision": decision})
                return
            if name in {"coordinator.after_skill_dispatch", "skill_executor.tool_result",
                        "coordinator_v2.after_tool_call"}:
                correlation = payload.get("tool_call_id") or str(payload.get("round"))
                records = _pending.pop((*owner, "actions", correlation), [])
                if name == "coordinator.after_skill_dispatch":
                    prefix = payload.get("tool_call_id", "") + ":"
                    skipped = [prefix + str(item["request_index"]) for item in payload.get("rejected", [])
                               if item.get("stage") == "policy_gate" and "request_index" in item]
                    executed = [prefix + str(item["request_index"]) for item in payload.get("dispatches", [])
                                if "request_index" in item]
                else:
                    results = payload.get("tool_results", [])
                    skipped = [item["tool_call_id"] for item in results if item.get("status") == "skipped"]
                    executed = [item["tool_call_id"] for item in results if item.get("status") != "skipped"]
                for saved_config, saved_policy, query in records:
                    _receipt(saved_config, saved_policy, query, {
                        "skipped_action_ids": skipped, "executed_action_ids": executed,
                    })
                return

            task, feedback, truncated = _shared._query_text(ctx, int(policy["snapshot"]["options"]["max_query_bytes"]))
            query = {
                "advice_query": {"identity": identity, "query_id": "runtime-query:" + uuid.uuid4().hex,
                                 "decision_id": "decision:" + uuid.uuid4().hex,
                                 "target_scope": scope, "task": task, "state": feedback},
                "execution_id": execution,
            }
            if name == "loop.before_decision":
                query.update(boundary="loop", iteration=payload["iteration"],
                             pending_actions=payload.get("pending_actions", False),
                             previous=_shared._read_state(namespace, loop_key) or {})
                # 精确重复状态只是可选规则，不把截断文本当作相同状态证据。
                query["state_fingerprint"] = (
                    hashlib.sha256(_shared._encode([feedback, ctx.artifacts]).encode()).hexdigest()
                    if feedback and not truncated else ""
                )
            elif name.endswith(".before_llm_call"):
                query["boundary"] = "llm"
            else:
                query["boundary"] = "actions"
                if name == "coordinator.before_skill_dispatch":
                    prefix = payload.get("tool_call_id", "")
                    if not prefix:
                        raise ValueError("action_id_missing")
                    query["actions"] = [{"action_id": prefix + ":" + str(i),
                                         "operation": spec["name"], "arguments": spec}
                                        for i, spec in enumerate(payload.get("skills", []), 1)]
                else:
                    query["actions"] = [{"action_id": item["tool_call_id"], "operation": item["tool_name"],
                                         "arguments": json.loads(item.get("arguments_json", "{}"))}
                                        for item in payload.get("tool_calls", [])]
            result = _invoke(config, "query", {"policy": policy, "query": query})
            _shared._record("evolution.policy.query", {
                "identity": identity, "policy_id": policy["policy_id"], "query": query,
                "decision": result, "context_truncated": truncated,
            })
            record = (config, policy, query)
            if query["boundary"] == "loop":
                progress = result.get("progress", {})
                _loop_updates[owner] = (progress, query)
                if result.get("status") == "ready":
                    _pending[(*owner, "loop")] = [record]
                    if result.get("context"):
                        ctx.request_context.append(result["context"])
                    return {"decision": progress["decision"], "reason": progress["reason"],
                            "policy_id": policy["policy_id"],
                            "query_id": query["advice_query"]["query_id"],
                            "state_before": query.get("previous", {}).get("fingerprint", ""),
                            "state_after": progress.get("fingerprint", "")}
            elif query["boundary"] == "llm":
                records = []
                if owner in _replans:
                    records.append(_replans.pop(owner))
                if result.get("context"):
                    ctx.request_context.append(result["context"])
                    records.append(record)
                if records:
                    _pending[(*owner, "llm", current_span_id())] = records
            elif result.get("status") == "ready":
                correlation = payload.get("tool_call_id") or str(payload.get("round"))
                _pending[(*owner, "actions", correlation)] = [record]
                rejected = {item["action_id"]: item["reason"] for item in result.get("actions", [])
                            if item["decision"] == "reject"}
                if name == "coordinator.before_skill_dispatch":
                    prefix = payload["tool_call_id"] + ":"
                    return {"skip_request_indices": {**payload.get("skip_request_indices", {}),
                            **{key[len(prefix):]: value for key, value in rejected.items()}}}
                return {"skip_tool_call_ids": {**payload.get("skip_tool_call_ids", {}), **rejected}}
    except Exception as exc:
        _shared._record("evolution.policy.error", {
            "status": "failed", "application_status": "not_applied", "boundary": name,
            "reason": _shared._error_code(exc).replace("shadow_unavailable", "runtime_unavailable"),
        })
