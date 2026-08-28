#!/usr/bin/env python3
"""sessionlog_hook v2 — 嵌套动作对事件模型，LLM/工具轨迹融合成一条线。

设计要点（对应 v1 代码结构，深度匹配）：
- 15 类 Harness 事件，区分 dialogue 起止、Coordinator 激活、循环决策及各层动作对；
  第 16 类 client.delivery 由 Bridge 在最终响应边界直接追加
- 嵌套结构：session → skill(B 进程) → llm(每轮) → tool(LLM 返回 tool_calls 才出现)
- 工具只在 skill_executor 进程内（skill_executor.py:796-959，LLM 循环内）
- 信息完整不冗余：
    * llm.request 保存 Harness 有效输入，llm.transport.request 保存实际序列化请求
    * llm.response 自包含 request、Provider 原始响应和所有 after Hook 后的最终响应
    * 三段 LLM 事件直接保存 skill_name/exec_skill_key/batch_id；完整调用上下文同时保留在 meta
    * tool.result 同步保存完整结果，关闭下一轮 llm.request 前崩溃的丢失窗口
- 血缘：source_event_seqs 进程内引用 seq；跨进程配对优先使用 exec_skill_key / batch_id
- Hook 观测异常由引擎隔离，不改变工具结果或业务控制流

落盘：trace_output/sessionlogs/{dialogue_id}.jsonl，一行一事件；进程内锁配合
POSIX flock 同步 append，并隔离进程中断留下的半行。
"""

import json
import os
import sys
import threading
import time

try:
    import fcntl
except ImportError:  # pragma: no cover - harness runtime is POSIX
    fcntl = None

# ── 配置 ──
_ENABLED = os.environ.get("AF_SESSIONLOG_ENABLED", "0") == "1"
_OUTPUT_ROOT = os.environ.get("AF_SESSIONLOG_DIR", "")
_VERSION = 1

HOOK_NAME = "sessionlog_recorder_v2"
HOOK_EVENTS = [
    "agent.dialogue.start",              # session.start（整个 dialogue 一次）
    "coordinator.trigger",                # coordinator.activation（每次 v1 激活）
    "coordinator.tool_loop_retry",        # loop.control（Coordinator 纠错）
    "skill_executor.trigger",            # skill.start（SKILL span 已开启后触发）
    "skill_executor.recovery_decision",   # loop.control（Skill 语义恢复）
    "coordinator_v1.before_llm_call",    # llm.request
    "coordinator_v2.before_llm_call",    # llm.request
    "skill_executor.before_llm_call",    # llm.request
    "llm.request.prepared",              # llm.transport.request（精确 HTTP body）
    "coordinator.llm_result",            # llm.response（Hook 后最终值）
    "coordinator.batch_result",          # batch.result（框架聚合终态）
    "coordinator.messages_snapshot",     # messages.snapshot（append-only）
    "skill_executor.llm_result",         # llm.response（Hook 后最终值）
    "skill_executor.before_tool_call",   # tool.call
    "skill_executor.tool_result",         # tool.result（最终有效结果）
    "artifact.storage_result",            # artifact.persist（内容身份 + 存储终态）
    "coordinator.after_skill_dispatch",  # skill.dispatch
    "skill_executor.after_skill_complete",  # skill.end
    "agent.dialogue.error",              # session.end（未处理异常）
    "agent.dialogue.end",                # session.end
]
HOOK_ORDER = 100

# ── 进程内状态（hook 单例，每个子进程一份）──
_lock = threading.Lock()
_seq = 0                      # 进程内全局递增 seq
_pid = os.getpid()
_process_started_us = time.time_ns() // 1000
_producer_id = f"python:{_pid}:{_process_started_us}"
_last_llm_request_seq = None      # 最近一次 llm.request 的 seq（llm.response 血缘）
_last_llm_response_seq = None     # 最近一次 llm.response 的 seq（tool.call/skill.dispatch 血缘）
_last_llm_response_span = None    # 最近一次 llm.response 的 span_id（tool.call 的 parent）
_last_llm_response_call = None    # 最近一次 llm.response 的 call_id
_last_tool_call_seq = None        # 最近一次 tool.call 的 seq（tool.result 血缘）
_last_tool_call_ts = None         # 最近一次 tool.call 的 ts（tool.result 耗时）
_last_tool_result_seq = None      # 最近一次 tool.result 的 seq（artifact.persist 血缘）
_last_skill_start_seq = None      # 最近一次 skill.start 的 seq（skill.end 血缘）
_last_skill_start_span = None     # 最近一次 skill.start 的 span_id（skill 内 llm 的 parent）
_last_session_end_source = None   # session.end 血缘
_last_coordinator_activation_seq = None  # 当前 Coordinator 激活（batch.result 血缘）

# ── DataBus 连接（复用 recorder 同款底层客户端）──
_databus = None
_databus_lock = threading.Lock()


def _get_databus():
    global _databus
    with _databus_lock:
        if _databus is not None:
            return _databus
        try:
            from agentflow_databus import DataBusClient as RealDataBusClient
            uds = os.environ.get("AF_DATABUS_UDS_FILE", "")
            sid = os.environ.get("AF_SESSION_ID", "")
            if not uds:
                return None
            _databus = RealDataBusClient(uds, session_id=sid, timeout_seconds=10.0)
        except Exception as exc:
            print(f"[sessionlog] databus init failed: {exc}", file=sys.stderr, flush=True)
            _databus = None
        return _databus


# ── 落盘（同步 append，照 span_writer 模式）──
_writer_lock = threading.Lock()
_created_header = False


def _write_jsonl_line(path: str, line: str, *, only_if_empty: bool = False) -> bool:
    """Append one isolated JSONL record under a process-shared file lock."""
    record = (line + "\n").encode("utf-8")
    with _writer_lock:
        with open(path, "a+b", buffering=0) as f:
            if fcntl is not None:
                fcntl.flock(f.fileno(), fcntl.LOCK_EX)
            try:
                f.seek(0, os.SEEK_END)
                size = f.tell()
                if only_if_empty and size:
                    return False
                if size:
                    f.seek(-1, os.SEEK_END)
                    if f.read(1) != b"\n":
                        f.write(b"\n")
                view = memoryview(record)
                while view:
                    written = f.write(view)
                    if not written:
                        raise OSError("short write while appending sessionlog")
                    view = view[written:]
                return True
            finally:
                if fcntl is not None:
                    fcntl.flock(f.fileno(), fcntl.LOCK_UN)


def _output_dir() -> str:
    if _OUTPUT_ROOT:
        return _OUTPUT_ROOT
    base = os.environ.get("AF_TRACE_OUTPUT_DIR", "")
    if base:
        return os.path.join(base, "sessionlogs")
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "trace_output", "sessionlogs")


def _write_header(dialogue_id: str, session_id: str) -> None:
    """头行：version + 会话身份（借鉴 DSH HeaderLine）。"""
    global _created_header
    if _created_header or not dialogue_id:
        return
    d = _output_dir()
    try:
        os.makedirs(d, exist_ok=True)
        path = os.path.join(d, f"{dialogue_id}.jsonl")
        header = {
            "type": "session.header",
            "version": _VERSION,
            "dialogue_id": dialogue_id,
            "session_id": session_id,
            "created_at": int(time.time() * 1000),
            "agent": os.environ.get("AF_AGENT_PACKAGE_DIR", "").split("/")[-1] or "unknown",
            "model": os.environ.get("AF_LLM_MODEL", ""),
        }
        _write_jsonl_line(
            path,
            json.dumps(header, ensure_ascii=False, separators=(",", ":")),
            only_if_empty=True,
        )
        _created_header = True
    except Exception as exc:
        print(f"[sessionlog] header failed: {exc}", file=sys.stderr, flush=True)


def _trace_id() -> str:
    """进程级 trace_id：AF_TRACE_ID 注入为空时 fallback py_local_{pid}_{ts}（与 emit.py 同款，进程内稳定）。"""
    tid = os.environ.get("AF_TRACE_ID", "")
    if tid:
        return tid
    # 与 agentflow_runtime.trace 的 fallback 一致：首次生成后缓存复用
    global _fallback_trace
    if _fallback_trace is None:
        _fallback_trace = f"py_local_{_pid}_{int(time.time() * 1000)}"
    return _fallback_trace


_fallback_trace: str | None = None


def _append(dialogue_id: str, event_type: str, data: dict,
            span_id: str = "", parent_span_id: str = "",
            source_event_seqs: list | None = None, round_i: int | None = None) -> int:
    """写一个事件，返回其 seq。同步落盘。"""
    global _seq
    with _lock:
        _seq += 1
        seq = _seq
    timestamp_us = time.time_ns() // 1000
    event_id = f"{_producer_id}:{seq}"
    source_event_ids = [
        f"{_producer_id}:{source_seq}"
        for source_seq in (source_event_seqs or [])
    ]
    trace_id = _trace_id()
    semantic_kind, _, lifecycle_phase = event_type.rpartition(".")
    env = {
        "schema_version": _VERSION,
        "event_id": event_id,
        "idempotency_key": event_id,
        "type": event_type,
        "semantic_kind": semantic_kind or event_type,
        "lifecycle_phase": lifecycle_phase or event_type,
        "seq": seq,
        "producer_sequence": seq,
        "timestamp_us": timestamp_us,
        "ts": timestamp_us // 1000,
        "producer_id": _producer_id,
        "pid": _pid,
        "trace_id": trace_id,
        "run_id": dialogue_id,
        "dialogue_id": dialogue_id,
        "session_id": os.environ.get("AF_SESSION_ID", ""),
        "task_id": os.environ.get("AF_SELF_TASK_ID", ""),
        "node_id": os.environ.get("AF_NODE_ID", ""),
        "primary_parent_event_id": source_event_ids[0] if source_event_ids else "",
        "causal_parent_event_ids": source_event_ids,
        "attributes": {},
        "redaction": {"applied": False, "policy": "none"},
    }
    if round_i is not None:
        env["round"] = round_i
    if span_id:
        env["span_id"] = span_id
    if parent_span_id:
        env["parent_span_id"] = parent_span_id
    if source_event_seqs:
        env["source_event_seqs"] = source_event_seqs
        env["source_event_ids"] = source_event_ids
    env["data"] = data
    if not dialogue_id:
        return seq
    try:
        os.makedirs(_output_dir(), exist_ok=True)
        path = os.path.join(_output_dir(), f"{dialogue_id}.jsonl")
        line = json.dumps(env, ensure_ascii=False, separators=(",", ":"))
        _write_jsonl_line(path, line)
    except Exception as exc:
        print(f"[sessionlog] append failed: {exc}", file=sys.stderr, flush=True)
    return seq


def _current_span_id() -> str:
    try:
        from agentflow_runtime.trace import current_span_id
        return current_span_id()
    except Exception:
        return ""


# ── DataBus 读 llm/traces（AFTER_LLM_CALL 时）──

def _latest_call_id(dialogue_id: str):
    db = _get_databus()
    if not db:
        return None
    try:
        raw = db.llm().get(f"traces/index/{dialogue_id}")
        if raw is None:
            return None
        if isinstance(raw, bytes):
            raw = raw.decode("utf-8", errors="replace")
        if isinstance(raw, str):
            idx = json.loads(raw)
        else:
            idx = raw
        if isinstance(idx, list) and idx:
            return idx[-1]
    except Exception as exc:
        print(f"[sessionlog] read index failed: {exc}", file=sys.stderr, flush=True)
    return None


def _read_llm_call(call_id: str) -> dict:
    out = {}
    db = _get_databus()
    if not db:
        return out
    try:
        ns = db.llm()
        for k in ("request", "provider_response", "response", "meta"):
            try:
                v = ns.get(f"traces/{call_id}/{k}")
                if v is not None:
                    if isinstance(v, bytes):
                        v = v.decode("utf-8", errors="replace")
                    if isinstance(v, str):
                        try:
                            v = json.loads(v)
                        except Exception:
                            pass
                    out[k] = v
            except Exception:
                pass
    except Exception as exc:
        print(f"[sessionlog] read llm/traces failed: {exc}", file=sys.stderr, flush=True)
    return out


# ── hook 入口 ──

def handle(event, ctx) -> None:
    global _last_coordinator_activation_seq
    global _last_skill_start_seq, _last_skill_start_span
    global _last_llm_request_seq
    global _last_llm_response_seq, _last_llm_response_span
    global _last_llm_response_call, _last_session_end_source
    global _last_tool_call_seq, _last_tool_call_ts
    global _last_tool_result_seq

    if not _ENABLED:
        return
    name = event.name
    payload = event.payload

    dialogue_id = payload.get("dialogue_id") or os.environ.get("AF_DIALOGUE_ID", "")
    if not dialogue_id:
        return

    # ── session.start：整个 dialogue 只出现一次 ──
    if name == "agent.dialogue.start":
        _write_header(dialogue_id, os.environ.get("AF_SESSION_ID", ""))
        _append(dialogue_id, "session.start", {
            "input": payload.get("input", ""),
            "trigger_type": payload.get("trigger_type", ""),
            "coordinator_version": payload.get("coordinator_version", ""),
            "code_revision": payload.get("code_revision", ""),
            "agent_definition_sha256": payload.get("agent_definition_sha256", ""),
            "model": payload.get("model", ""),
            "max_iterations": payload.get("max_iterations"),
            "max_completion_tokens": payload.get("max_completion_tokens"),
        }, span_id="", source_event_seqs=[])
        return

    # ── coordinator.activation：每次事件驱动的 Coordinator 执行 ──
    if name == "coordinator.trigger":
        _write_header(dialogue_id, os.environ.get("AF_SESSION_ID", ""))
        completed_batch_key = payload.get("completed_batch_key", "")
        batch_id = (
            completed_batch_key.removeprefix("completion_batch:")
            if completed_batch_key else ""
        )
        _last_coordinator_activation_seq = _append(dialogue_id, "coordinator.activation", {
            "trigger_type": payload.get("trigger_type", ""),
            "model": payload.get("model", ""),
            "coordinator_version": payload.get("coordinator_version", ""),
            "code_revision": payload.get("code_revision", ""),
            "agent_definition_sha256": payload.get("agent_definition_sha256", ""),
            "max_iterations": payload.get("max_iterations"),
            "max_completion_tokens": payload.get("max_completion_tokens"),
            "completed_skill": payload.get("completed_skill", ""),
            "completed_batch_key": completed_batch_key,
            "batch_id": batch_id,
            "activation_index": payload.get("activation_index"),
            "skill_completion_index": payload.get("skill_completion_index"),
        }, span_id="", source_event_seqs=[])
        return

    # ── batch.result：框架 ALL-join 的完整终态（含未进入 Python 的失败分支）──
    if name == "coordinator.batch_result":
        _append(dialogue_id, "batch.result", {
            "completed_batch_key": payload.get("completed_batch_key", ""),
            "batch_id": payload.get("batch_id", ""),
            "status": payload.get("status", ""),
            "successful_count": payload.get("successful_count", 0),
            "total_count": payload.get("total_count", 0),
            "results": payload.get("results", []),
        }, source_event_seqs=(
            [_last_coordinator_activation_seq]
            if _last_coordinator_activation_seq else []
        ))
        return

    # ── messages.snapshot：兼容缓存之外的 append-only 消息历史 ──
    if name == "coordinator.messages_snapshot":
        messages = payload.get("messages") or []
        _append(dialogue_id, "messages.snapshot", {
            "messages": messages,
            "message_count": payload.get("message_count", len(messages)),
        }, source_event_seqs=(
            [_last_llm_response_seq] if _last_llm_response_seq else []
        ))
        return

    # ── loop.control：明确记录继续/停止的原因与预算 ──
    if name in {
        "coordinator.tool_loop_retry",
        "skill_executor.recovery_decision",
    }:
        _append(dialogue_id, "loop.control", {
            "scope": (
                "coordinator"
                if name == "coordinator.tool_loop_retry"
                else "skill"
            ),
            "source_event_name": name,
            "action": payload.get("action", ""),
            "reason": payload.get("reason", ""),
            "stop_reason": payload.get("stop_reason", ""),
            "round": payload.get("round"),
            "next_round": payload.get("next_round"),
            "tool_loop_round": payload.get("tool_loop_round"),
            "skill_name": payload.get("skill_name", ""),
            "recovery_attempt": payload.get("recovery_attempt"),
            "max_recovery_attempts": payload.get("max_recovery_attempts"),
            "previous_completion_budget": payload.get("previous_completion_budget"),
            "completion_budget": payload.get("completion_budget"),
            "finish_reason": payload.get("finish_reason", ""),
            "tool_calls": payload.get("tool_calls", []),
        }, parent_span_id=_last_llm_response_span or "",
           source_event_seqs=[_last_llm_response_seq] if _last_llm_response_seq else [],
           round_i=payload.get("round"))
        return

    # ── skill.start（SKILL_EXECUTOR_TRIGGER：SKILL span 已开启 → current_span_id=SKILL span）──
    if name == "skill_executor.trigger":
        span_id = _current_span_id()
        # exec_skill_key：跨进程关联键（coordinator 写 dispatch:{did}:{round}:{i}，本进程经 AF_EXEC_SKILL_KEY 注入）
        _last_skill_start_seq = _append(dialogue_id, "skill.start", {
            "skill_name": payload.get("skill_name", ""),
            "skill_prompt": payload.get("skill_prompt", ""),
            "load_skills": payload.get("load_skills", []),
            "effective_load_skills": payload.get("effective_load_skills", []),
            "force": payload.get("force", False),
            "request_index": payload.get("request_index"),
            "model": payload.get("model", ""),
            "skill_doc_digests": payload.get("skill_doc_digests", {}),
            "system_prompt_sha256": payload.get("system_prompt_sha256", ""),
            "task_instruction_sha256": payload.get("task_instruction_sha256", ""),
            "tool_definition_digests": payload.get("tool_definition_digests", []),
            "max_rounds": payload.get("max_rounds"),
            "tools_count": payload.get("tools_count"),
            "tool_names": payload.get("tool_names", []),
            "exec_skill_key": (
                payload.get("exec_skill_key") or os.environ.get("AF_EXEC_SKILL_KEY", "")
            ),
            "batch_id": payload.get("batch_id") or os.environ.get("AF_BATCH_ID", ""),
        }, span_id=span_id, source_event_seqs=[])
        _last_skill_start_span = span_id
        return

    # ── llm.request（BEFORE：ctx.messages 全量，实时落）──
    if name.endswith(".before_llm_call"):
        span_id = _current_span_id()
        msgs = getattr(ctx, "messages", None) or []
        task_type = (
            "skill" if name.startswith("skill_executor")
            else "coordinator" if name.startswith("coordinator")
            else ""
        )
        _last_llm_request_seq = _append(dialogue_id, "llm.request", {
            "model": payload.get("model", ""),
            "messages": msgs,
            "msg_count": len(msgs),
            "request_stage": "hook_observation",
            "skill_name": (
                payload.get("skill_name", "")
                or ("coordinator" if task_type == "coordinator" else "")
            ),
            "exec_skill_key": payload.get("exec_skill_key", "") or os.environ.get(
                "AF_EXEC_SKILL_KEY", ""
            ),
            "batch_id": payload.get("batch_id", "") or os.environ.get(
                "AF_BATCH_ID", ""
            ),
            "task_type": task_type,
            "activation_index": payload.get("activation_index"),
            "decision_index": payload.get("decision_index"),
            "skill_completion_index": payload.get("skill_completion_index"),
            "internal_tool_correction_index": payload.get(
                "internal_tool_correction_index"
            ),
        }, span_id=span_id,
           parent_span_id=_last_skill_start_span if name.startswith("skill_executor") else "",
           source_event_seqs=[],
           round_i=payload.get("round"))
        return

    # ── llm.transport.request：序列化完成、HTTP 尚未开始的精确请求 ──
    if name == "llm.request.prepared":
        request = payload.get("request")
        if not isinstance(request, dict):
            request = {}
        meta = payload.get("meta")
        if not isinstance(meta, dict):
            meta = {}
        prior_request_seq = _last_llm_request_seq
        span_id = _current_span_id()
        task_type = meta.get("task_type", "")
        _last_llm_request_seq = _append(dialogue_id, "llm.transport.request", {
            "call_id": payload.get("call_id", ""),
            "model": payload.get("model", ""),
            "request": request,
            "msg_count": len(request.get("messages") or []),
            "request_stage": "transport_prepared",
            "skill_name": (
                meta.get("skill_name", "")
                or ("coordinator" if task_type == "coordinator" else "")
            ),
            "exec_skill_key": meta.get("exec_skill_key", ""),
            "batch_id": meta.get("batch_id", ""),
            "task_type": task_type,
            "meta": meta,
        }, span_id=span_id,
           parent_span_id=(
               _last_skill_start_span
               if meta.get("task_type") == "skill"
               else ""
           ),
           source_event_seqs=[prior_request_seq] if prior_request_seq else [],
           round_i=meta.get("round"))
        return

    # ── llm.response（所有 after Hook 完成后的最终有效响应）──
    if name in ("coordinator.llm_result", "skill_executor.llm_result"):
        call_id = payload.get("call_id")
        llm = _read_llm_call(call_id) if call_id else {}
        response = payload.get("response")
        if not isinstance(response, dict):
            response = llm.get("response")
        meta = llm.get("meta")
        if not isinstance(meta, dict):
            meta = {}
        task_type = meta.get("task_type", "")
        span_id = _current_span_id()
        _last_llm_response_seq = _append(dialogue_id, "llm.response", {
            "call_id": call_id,
            "request": llm.get("request"),
            "provider_response": llm.get("provider_response"),
            "response": response,
            "skill_name": (
                payload.get("skill_name", "")
                or meta.get("skill_name", "")
                or ("coordinator" if task_type == "coordinator" else "")
            ),
            "exec_skill_key": meta.get("exec_skill_key", "") or os.environ.get(
                "AF_EXEC_SKILL_KEY", ""
            ),
            "batch_id": meta.get("batch_id", "") or os.environ.get(
                "AF_BATCH_ID", ""
            ),
            "task_type": task_type,
            "meta": meta,
            "activation_index": payload.get("activation_index"),
            "decision_index": payload.get("decision_index"),
            "skill_completion_index": payload.get("skill_completion_index"),
            "internal_tool_correction_index": payload.get(
                "internal_tool_correction_index"
            ),
        }, span_id=span_id,
           parent_span_id=_last_skill_start_span if name.startswith("skill_executor") else "",
           source_event_seqs=[_last_llm_request_seq] if _last_llm_request_seq else [],
           round_i=payload.get("round"))
        _last_llm_response_span = span_id
        _last_llm_response_call = call_id
        _last_session_end_source = _last_llm_response_seq
        return

    # ── tool.call（BEFORE_TOOL_CALL：skill 内、本轮 llm.response 之后）──
    if name == "skill_executor.before_tool_call":
        _last_tool_call_ts = int(time.time() * 1000)
        _last_tool_call_seq = _append(dialogue_id, "tool.call", {
            "skill_name": payload.get("skill_name", ""),
            "tool_names": payload.get("tool_names", []),
            "tool_calls": payload.get("tool_calls", []),
            "round": payload.get("round"),
        }, parent_span_id=_last_llm_response_span or "",
           source_event_seqs=[_last_llm_response_seq] if _last_llm_response_seq else [],
           round_i=payload.get("round"))
        return

    # ── tool.result（最终结果观测事件，不可干预）──
    if name == "skill_executor.tool_result":
        dur_ms = None
        if _last_tool_call_ts:
            dur_ms = int(time.time() * 1000) - _last_tool_call_ts
        _last_tool_result_seq = _append(dialogue_id, "tool.result", {
            "skill_name": payload.get("skill_name", ""),
            "tool_names": payload.get("tool_names", []),
            "results_count": payload.get("results_count"),
            "tool_results": payload.get("tool_results", []),
            "duration_ms": dur_ms,
            "round": payload.get("round"),
        }, parent_span_id=_last_llm_response_span or "",
           source_event_seqs=[_last_tool_call_seq] if _last_tool_call_seq else [],
           round_i=payload.get("round"))
        return

    # ── artifact.persist（文件内容身份与 Bridge 存储终态）──
    if name == "artifact.storage_result":
        source_seq = _last_tool_result_seq or _last_llm_response_seq
        _append(dialogue_id, "artifact.persist", {
            "status": payload.get("status", ""),
            "skill_name": payload.get("skill_name", ""),
            "exec_skill_key": payload.get("exec_skill_key", ""),
            "batch_id": os.environ.get("AF_BATCH_ID", ""),
            "agent_name": payload.get("agent_name", ""),
            "artifact_session_id": payload.get("artifact_session_id", ""),
            "attempted_count": payload.get("attempted_count", 0),
            "uploaded_count": payload.get("uploaded_count", 0),
            "returned_artifact_ids": payload.get("returned_artifact_ids", []),
            "entries": payload.get("entries", []),
            "error_type": payload.get("error_type", ""),
            "error": payload.get("error", ""),
        }, parent_span_id=_last_llm_response_span or "",
           source_event_seqs=[source_seq] if source_seq else [])
        return

    # ── skill.dispatch（COORDINATOR_AFTER_SKILL_DISPATCH：coordinator 侧，parent=决策 llm.response）──
    if name == "coordinator.after_skill_dispatch":
        # 新 Coordinator 直接提供 exec_skill_keys；旧调用方成功分发时再从
        # DataBus next_skills 回读，失败状态绝不能读取可能残留的旧值。
        status = payload.get("status", "dispatched")
        exec_keys = list(payload.get("exec_skill_keys") or [])
        try:
            db = (
                _get_databus()
                if not exec_keys and status in ("dispatched", "partially_dispatched")
                else None
            )
            if db:
                raw = None
                read_tries = []
                try:
                    raw = db.sys().get(f"{dialogue_id}/next_skills", None)
                    read_tries.append(f"key={dialogue_id}/next_skills -> {raw!r}"[:120])
                except Exception as e:
                    read_tries.append(f"key={dialogue_id}/next_skills ERR {e}"[:120])
                if raw is None:
                    try:
                        raw = db.sys().get("next_skills", None)
                        read_tries.append(f"key=next_skills -> {raw!r}"[:120])
                    except Exception as e:
                        read_tries.append(f"key=next_skills ERR {e}"[:120])
                if raw is None:
                    # 再试一次 put_global 风格的全局 key
                    try:
                        raw = db.get_global("next_skills", None) if hasattr(db, "get_global") else None
                        read_tries.append(f"global next_skills -> {raw!r}"[:120])
                    except Exception as e:
                        read_tries.append(f"global ERR {e}"[:120])
                print(f"[sessionlog] next_skills reads: {'; '.join(read_tries)}", file=sys.stderr, flush=True)
                if isinstance(raw, bytes):
                    raw = raw.decode("utf-8", errors="replace")
                if isinstance(raw, str) and raw:
                    print(f"[sessionlog] next_skills raw type={type(raw).__name__} len={len(raw)} repr={raw[:300]!r}", file=sys.stderr, flush=True)
                    import json as _json
                    try:
                        raw = _json.loads(raw)
                    except Exception as e:
                        print(f"[sessionlog] next_skills JSON parse failed: {e}", file=sys.stderr, flush=True)
                # raw 可能是 list（UDS 已反序列化）或 dict
                if isinstance(raw, list):
                    exec_keys = [s.get("exec_skill_key", "") for s in raw if isinstance(s, dict)]
                elif isinstance(raw, dict):
                    exec_keys = [raw.get("exec_skill_key", "")] if raw.get("exec_skill_key") else []
                else:
                    print(f"[sessionlog] next_skills raw type={type(raw).__name__}", file=sys.stderr, flush=True)
        except Exception as exc:
            print(f"[sessionlog] read next_skills failed: {exc}", file=sys.stderr, flush=True)
        print(f"[sessionlog] dispatch exec_keys={exec_keys}", file=sys.stderr, flush=True)
        _append(dialogue_id, "skill.dispatch", {
            "status": status,
            "requested_skill_names": (
                payload.get("requested_skill_names") or payload.get("skill_names", [])
            ),
            "skill_names": payload.get("skill_names", []),
            "round": payload.get("round"),
            "batch_id": payload.get("batch_id", ""),
            "exec_skill_keys": exec_keys,
            "exec_skill_key": exec_keys[0] if exec_keys else "",
            "dispatches": payload.get("dispatches", []),
            "rejected": payload.get("rejected", []),
            "warnings": payload.get("warnings", []),
        }, parent_span_id=_last_llm_response_span or "",
           source_event_seqs=[_last_llm_response_seq] if _last_llm_response_seq else [],
           round_i=payload.get("round"))
        return

    # ── skill.end（AFTER_SKILL_COMPLETE：skill 进程内，parent=skill.start）──
    if name == "skill_executor.after_skill_complete":
        _append(dialogue_id, "skill.end", {
            "skill_name": payload.get("skill_name", ""),
            "status": payload.get("status", ""),
            "round": payload.get("round"),
            "skill_result": payload.get("skill_result", ""),
            "error": payload.get("error", ""),
            # exec_skill_key：回传边强键（skill 进程 AF_EXEC_SKILL_KEY 必有，与派发边同源）
            "exec_skill_key": os.environ.get("AF_EXEC_SKILL_KEY", ""),
            "batch_id": os.environ.get("AF_BATCH_ID", ""),
            "artifacts": list(getattr(ctx, "artifacts", None) or []),
        }, span_id=_last_skill_start_span or "",
           source_event_seqs=[_last_skill_start_seq] if _last_skill_start_seq else [],
           round_i=payload.get("round"))
        return

    # ── session.end（Coordinator 未处理异常；业务异常仍由框架接管）──
    if name == "agent.dialogue.error":
        _append(dialogue_id, "session.end", {
            "status": payload.get("status", "unhandled_exception"),
            "response_text": "",
            "error_type": payload.get("error_type", ""),
            "error": payload.get("error", ""),
            "elapsed_s": payload.get("elapsed_s"),
            "terminal_source": name,
        }, source_event_seqs=[_last_session_end_source] if _last_session_end_source else [])
        return

    # ── session.end ──
    if name == "agent.dialogue.end":
        _append(dialogue_id, "session.end", {
            "status": payload.get("status", ""),
            "response_text": payload.get("response_text", ""),
        }, source_event_seqs=[_last_session_end_source] if _last_session_end_source else [])
        return
