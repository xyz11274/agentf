# 兼容接入入口：将已有日志或打点数据转入组件公开协议。
# 宿主适配留在本层，通用评测、记忆与策略逻辑不依赖具体 Agent 框架。

"""Trajectory Evolution Shadow 接入：只读上下文，复用 C++ 查询，不干预执行。

挂载到 agent 包 harness/hooks；未设置 AF_EVOLUTION_SHADOW_CONFIG 时零 I/O。
一个 dialogue.start 初始化一次快照；v1 后续进程从 dialogue 身份对应的 DataBus
状态读取。禁止在首次决策或重触发时临时编译，避免中途开启或状态丢失后偷换版本。
"""

from __future__ import annotations

import hashlib
from contextlib import contextmanager
import json
import os
from pathlib import Path
import socket
import subprocess
import threading
import uuid

HOOK_NAME = "trajectory_evolution_shadow_v1"
HOOK_EVENTS = [
    "agent.dialogue.start",
    "coordinator_v1.before_llm_call",
    "coordinator_v2.before_llm_call",
    "agent.dialogue.end",
    "agent.dialogue.error",
]
# SessionLog 先记录 Run/LLM 边界，影子事件再引用对应边界。
HOOK_ORDER = 200

_lock = threading.Lock()


# 编码跨接口载荷，保持 JSON 类型和 UTF-8 内容有效。
def _encode(value) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


# 读取兼容集成配置，缺失或非法配置按入口约定处理。
def _configuration():
    path = os.environ.get("AF_EVOLUTION_SHADOW_CONFIG", "")
    if not path:
        return None
    with open(path, "rb") as stream:
        raw = stream.read(65537)
    if len(raw) > 65536:
        raise ValueError("configuration_limit")
    config = json.loads(raw)
    if not isinstance(config, dict) or config.get("schema_version") != 1:
        raise ValueError("configuration_invalid")
    if config.get("mode") == "disabled":
        return None
    required = {
        "schema_version",
        "mode",
        "agent_id",
        "memory_path",
        "executable",
        "query_scope",
        "rpc_timeout_ms",
        "process_timeout_ms",
        "options",
    }
    if set(config) != required or config["mode"] != "shadow":
        raise ValueError("configuration_invalid")
    for field in ("rpc_timeout_ms", "process_timeout_ms"):
        if type(config[field]) is not int or not 1 <= config[field] <= 10000:
            raise ValueError("configuration_invalid")
    for field in ("agent_id", "query_scope", "memory_path", "executable"):
        if not isinstance(config[field], str) or not config[field]:
            raise ValueError("configuration_invalid")
    for field in ("memory_path", "executable"):
        if not Path(config[field]).is_absolute():
            raise ValueError("absolute_path_required")
    if not isinstance(config["options"], dict):
        raise ValueError("configuration_invalid")
    return config


# 构造当前应用、用户、会话和运行身份，用于隔离采集与策略。
def _identity(config):
    identity = {
        "agent_id": config["agent_id"],
        "user_id": os.environ.get("AF_USER_ID", ""),
        "conversation_id": os.environ.get("AF_CLIENT_ID", ""),
        "run_id": os.environ.get("AF_DIALOGUE_ID", ""),
    }
    for field, value in identity.items():
        if (
            not isinstance(value, str)
            or len(value.encode("utf-8")) > 256
            or any(ord(char) < 32 or ord(char) == 127 for char in value)
            or (field != "conversation_id" and not value)
        ):
            raise ValueError("identity_invalid")
    package = os.environ.get("AF_AGENT_PACKAGE_DIR", "")
    if package and Path(package).name != identity["agent_id"]:
        raise ValueError("agent_binding_mismatch")
    return identity


@contextmanager
# 确定当前集成使用的存储命名空间，避免不同应用共享状态键。
def _namespace(config):
    # 仅接入层依赖 DataBus；核心 C++ 不认识 AgentFlow。
    from agentflow_databus import DataBusClient

    uds = os.environ.get("AF_DATABUS_UDS_FILE", "")
    sid = os.environ.get("AF_SESSION_ID", "")
    if not uds or not sid:
        raise ValueError("databus_unavailable")

    class _BoundedClient(DataBusClient):
        # 建立兼容入口所需连接，不将凭据放进业务载荷。
        def _connect(self):
            # SDK 在 RPC 阶段才设置超时；本接入也限制 UDS 建连等待。
            with self._lock:
                connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                try:
                    connection.settimeout(self._timeout_seconds)
                    connection.connect(self._socket_path)
                except Exception:
                    connection.close()
                    raise
                self._socket = connection
                self._connected = True

    with _BoundedClient(
        uds,
        session_id=sid,
        timeout_seconds=config["rpc_timeout_ms"] / 1000,
    ) as client:
        yield client.sys()


# 按运行身份构造状态键，避免仅按技能名称保存并发上下文。
def _state_key(identity):
    # 完整身份参与 key，不以共享 session 槽位或并行完成顺序作为身份。
    # 保留宿主的 dialogue 前缀，异常退出也可由原有清理逻辑回收。
    return (
        identity["run_id"]
        + "/_trajectory_evolution/shadow/"
        + hashlib.sha256(
            _encode(identity).encode("utf-8"),
        ).hexdigest()
    )


# 读取当前运行的已有状态，缺失时按协议进入初始化或降级路径。
def _read_state(namespace, key):
    raw = namespace.get(key)
    if raw is None:
        return None
    if isinstance(raw, bytes):
        raw = raw.decode("utf-8")
    if not isinstance(raw, str) or len(raw.encode("utf-8")) > 1024 * 1024:
        raise ValueError("snapshot_state_invalid")
    state = json.loads(raw)
    if not isinstance(state, dict):
        raise ValueError("snapshot_state_invalid")
    return state


# 执行底层协议传输，复用请求身份与总期限，不能扩展为业务回调重试。
def _invoke(config, operation, request, *, command="shadow", env=None):
    raw = _encode(request).encode("utf-8")
    if len(raw) > 2 * 1024 * 1024:
        raise ValueError("request_limit")
    completed = subprocess.run(
        [config["executable"], command, operation],
        input=raw,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        timeout=config["process_timeout_ms"] / 1000,
        check=False,
        env=env,
    )
    if completed.returncode != 0 or len(completed.stdout) > 1024 * 1024:
        raise ValueError("component_failed")
    output = json.loads(completed.stdout)
    if not isinstance(output, dict):
        raise ValueError("component_response_invalid")
    return output


# 记录已确认事实和回执，保留因果关系及执行归属。
def _record(name, payload):
    # 轨迹写入也不能阻止原始 LLM 调用。异常诊断不输出原文或配置路径。
    try:
        from agentflow_runtime.hooks import trigger
        from agentflow_runtime.observability import emit_event
        from agentflow_runtime.security import redact_secrets

        safe, _ = redact_secrets(payload)
        trigger(name, dict(safe))
        emit_event(name, safe)
    except Exception:
        pass


# 从异常提取允许公开的固定错误码，不回显任意异常文本。
def _error_code(exc):
    if isinstance(exc, subprocess.TimeoutExpired):
        return "component_timeout"
    if isinstance(exc, FileNotFoundError):
        return "component_or_config_unavailable"
    # 仅允许固定错误码，绝不把子进程 stderr、JSON 原文或路径放入轨迹。
    known = {
        "configuration_limit",
        "configuration_invalid",
        "absolute_path_required",
        "identity_invalid",
        "agent_binding_mismatch",
        "databus_unavailable",
        "snapshot_state_invalid",
        "snapshot_identity_mismatch",
        "run_not_initialized",
        "request_limit",
        "component_failed",
        "component_response_invalid",
        "snapshot_publish_failed",
        "snapshot_unsafe",
        "snapshot_limit",
        "component_timeout",
        "component_or_config_unavailable",
        "shadow_unavailable",
    }
    return (
        str(exc)
        if isinstance(exc, ValueError) and str(exc) in known
        else "shadow_unavailable"
    )


# 提取当前任务的检索文本，避免重复包含全部对话历史。
def _query_text(ctx, budget):
    # 不复制完整历史或 system prompt；取最近任务和最近三条工具反馈。
    task, state, truncated = "", [], False
    for message in reversed(getattr(ctx, "messages", []) or []):
        if not isinstance(message, dict):
            continue
        role = message.get("role")
        if (role != "user" or task) and (role != "tool" or len(state) >= 3):
            continue
        content = message.get("content", "")
        if isinstance(content, list):
            pieces, remaining = [], budget
            for item in content:
                if isinstance(item, dict) and isinstance(item.get("text"), str):
                    text = item["text"]
                    pieces.append(text[:remaining])
                    truncated |= len(text) > remaining
                    remaining = max(0, remaining - len(pieces[-1]) - 1)
                if not remaining:
                    truncated = True
                    break
            content = "\n".join(pieces)
        if not isinstance(content, str):
            continue
        truncated |= len(content) > budget
        if not task and role == "user":
            task = content[:budget]
        if role == "tool" and len(state) < 3:
            state.append(content[:budget])
        if task and len(state) == 3:
            break
    from agentflow_runtime.security import redact_secrets

    task, _ = redact_secrets(task)
    feedback = "\n".join(reversed(state))
    truncated |= len(feedback) > budget
    feedback, _ = redact_secrets(feedback[:budget])
    clip = lambda text, limit: text.encode("utf-8")[: max(0, limit)].decode(
        "utf-8", errors="ignore"
    )
    # 预留 query/decision/identity 协议开销；截断明确进入观察记录。
    allowance = max(0, budget - 2048)
    safe_task = clip(task, allowance // 2)
    safe_state = clip(feedback, allowance - len(safe_task.encode("utf-8")))
    return (
        safe_task,
        safe_state,
        truncated or safe_task != task or safe_state != feedback,
    )


# 处理当前兼容集成事件，读取必要身份后调用通用组件接口。
def handle(event, ctx) -> None:
    # 不写 ctx、不返回 payload 补丁，也不修改环境或业务 DataBus key。
    terminal = event.name in {"agent.dialogue.end", "agent.dialogue.error"}
    query_id = "shadow-query:" + uuid.uuid4().hex
    decision_id = ""
    try:
        config = _configuration()
        if config is None:
            return
        from agentflow_runtime.security import register_environment_secrets

        register_environment_secrets()
        identity = _identity(config)
        key = _state_key(identity)
        with _namespace(config) as namespace:
            if terminal:
                namespace.remove(key)
                return
            if event.name == "agent.dialogue.start":
                # 宿主契约要求每个 Run 只有一个 start；重复通知不会覆盖已有快照。
                with _lock:
                    existing = _read_state(namespace, key)
                    if existing is not None:
                        return
                    state = {"identity": identity, "query_scope": config["query_scope"]}
                    try:
                        snapshot = _invoke(
                            config,
                            "compile",
                            {
                                "identity": identity,
                                "options": config["options"],
                                "memory_path": config["memory_path"],
                            },
                        )
                        from agentflow_runtime.security import redact_secrets

                        safe, _ = redact_secrets(snapshot)
                        if safe != snapshot:
                            raise ValueError("snapshot_unsafe")
                        if snapshot.get("identity") != identity or not snapshot.get(
                            "snapshot_id"
                        ):
                            raise ValueError("snapshot_identity_mismatch")
                        if (
                            len(_encode(snapshot).encode("utf-8"))
                            > config["options"]["max_snapshot_bytes"]
                        ):
                            raise ValueError("snapshot_limit")
                        state["snapshot"] = snapshot
                    except Exception as exc:
                        state["error_code"] = _error_code(exc)
                    # 初始化失败也固定为本轮失败状态，避免每轮重新编译/启动子进程。
                    if namespace.put(key, _encode(state)) is False:
                        raise ValueError("snapshot_publish_failed")
                    _record(
                        "evolution.shadow.snapshot",
                        {
                            "dialogue_id": identity["run_id"],
                            "identity": identity,
                            "mode": "shadow",
                            "application_status": "not_applied",
                            "status": "error" if state.get("error_code") else "ready",
                            "reason": state.get("error_code", "run_snapshot_frozen"),
                            "snapshot": state.get("snapshot", {}),
                        },
                    )
                return

            from agentflow_runtime.trace import current_span_id

            decision_id = current_span_id() or "decision:" + uuid.uuid4().hex
            state = _read_state(namespace, key)
            if state is None:
                raise ValueError("run_not_initialized")
            if state.get("identity") != identity:
                raise ValueError("snapshot_identity_mismatch")
            if state.get("error_code"):
                raise ValueError(state["error_code"])
            snapshot = state["snapshot"]
            task, feedback, truncated = _query_text(
                ctx, int(snapshot["options"]["max_query_bytes"])
            )
            query = {
                "identity": identity,
                "query_id": query_id,
                "decision_id": decision_id,
                "target_scope": state["query_scope"],
                "task": task,
                "state": feedback,
            }
            result = _invoke(config, "query", {"snapshot": snapshot, "query": query})
            if (
                result.get("identity") != identity
                or result.get("query_id") != query_id
                or result.get("decision_id") != decision_id
                or result.get("mode") != "shadow"
                or result.get("application_status") != "not_applied"
            ):
                raise ValueError("component_response_invalid")
            result.update(
                {
                    "dialogue_id": identity["run_id"],
                    "context_truncated": truncated,
                    "round": event.payload.get("round"),
                    "activation_index": event.payload.get("activation_index"),
                    "decision_index": event.payload.get("decision_index"),
                }
            )
            _record("evolution.shadow.query", result)
    except Exception as exc:
        if not terminal:
            _record(
                "evolution.shadow.query"
                if event.name != "agent.dialogue.start"
                else "evolution.shadow.snapshot",
                {
                    "dialogue_id": os.environ.get("AF_DIALOGUE_ID", ""),
                    "query_id": query_id,
                    "decision_id": decision_id,
                    "mode": "shadow",
                    "application_status": "not_applied",
                    "status": "error",
                    "reason": _error_code(exc),
                },
            )
