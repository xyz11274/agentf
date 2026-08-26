#!/usr/bin/env python3
"""sessionlog_hook v2 — 嵌套动作对事件模型，LLM/工具轨迹融合成一条线。

设计要点（对应 v1 代码结构，深度匹配）：
- 8 类事件，全部动作成对（request/response、call/result、start/end、dispatch/end）
- 嵌套结构：session → skill(B 进程) → llm(每轮) → tool(LLM 返回 tool_calls 才出现)
- 工具只在 skill_executor 进程内（skill_executor.py:796-959，LLM 循环内）
- 信息完整不冗余：
    * messages 全量只在 llm.request 存一次（模型看到的一切）
    * response 全量只在 llm.response 存一次（含 logprobs）
    * task_context 归因只在 llm.response.data.meta
    * tool.result / skill.end 不存结果全文（下一轮 llm.request 快照里已有）
- 血缘：source_event_seqs 进程内引用 seq；跨进程配对靠 span_id / skill_name
- 不改原代码：本文件部署到 agent 包 harness/hooks/（配置层），引擎零改动

落盘：trace_output/sessionlogs/{dialogue_id}.jsonl，一行一事件，同步 append（v1 事件频率低）。
"""

import json
import os
import sys
import threading
import time

# ── 配置 ──
_ENABLED = os.environ.get("AF_SESSIONLOG_ENABLED", "0") == "1"
_OUTPUT_ROOT = os.environ.get("AF_SESSIONLOG_DIR", "")
_VERSION = 0

HOOK_NAME = "sessionlog_recorder_v2"
HOOK_EVENTS = [
    "agent.dialogue.start",              # session.start
    "skill_executor.trigger",            # skill.start（SKILL span 已开启后触发）
    "coordinator_v1.before_llm_call",    # llm.request
    "coordinator_v2.before_llm_call",    # llm.request
    "skill_executor.before_llm_call",    # llm.request
    "coordinator_v1.after_llm_call",     # llm.response
    "coordinator_v2.after_llm_call",     # llm.response
    "skill_executor.after_llm_call",     # llm.response
    "skill_executor.before_tool_call",   # tool.call
    "skill_executor.after_tool_call",    # tool.result
    "coordinator.after_skill_dispatch",  # skill.dispatch
    "skill_executor.after_skill_complete",  # skill.end
    "agent.dialogue.end",                # session.end
]
HOOK_ORDER = 100

# ── 进程内状态（hook 单例，每个子进程一份）──
_lock = threading.Lock()
_seq = 0                      # 进程内全局递增 seq
_pid = os.getpid()
_last_llm_request_seq = None      # 最近一次 llm.request 的 seq（llm.response 血缘）
_last_llm_response_seq = None     # 最近一次 llm.response 的 seq（tool.call/skill.dispatch 血缘）
_last_llm_response_span = None    # 最近一次 llm.response 的 span_id（tool.call 的 parent）
_last_llm_response_call = None    # 最近一次 llm.response 的 call_id
_last_tool_call_seq = None        # 最近一次 tool.call 的 seq（tool.result 血缘）
_last_tool_call_ts = None         # 最近一次 tool.call 的 ts（tool.result 耗时）
_last_skill_start_seq = None      # 最近一次 skill.start 的 seq（skill.end 血缘）
_last_skill_start_span = None     # 最近一次 skill.start 的 span_id（skill 内 llm 的 parent）
_last_session_end_source = None   # session.end 血缘

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
        with _writer_lock:
            if not os.path.exists(path) or os.path.getsize(path) == 0:
                with open(path, "a", encoding="utf-8") as f:
                    f.write(json.dumps(header, ensure_ascii=False, separators=(",", ":")) + "\n")
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
    env = {
        "type": event_type,
        "seq": seq,
        "ts": int(time.time() * 1000),
        "pid": _pid,
        "trace_id": _trace_id(),
        "dialogue_id": dialogue_id,
        "session_id": os.environ.get("AF_SESSION_ID", ""),
        "node_id": os.environ.get("AF_NODE_ID", ""),
    }
    if round_i is not None:
        env["round"] = round_i
    if span_id:
        env["span_id"] = span_id
    if parent_span_id:
        env["parent_span_id"] = parent_span_id
    if source_event_seqs:
        env["source_event_seqs"] = source_event_seqs
    env["data"] = data
    if not dialogue_id:
        return seq
    try:
        os.makedirs(_output_dir(), exist_ok=True)
        path = os.path.join(_output_dir(), f"{dialogue_id}.jsonl")
        line = json.dumps(env, ensure_ascii=False, separators=(",", ":"))
        with _writer_lock:
            with open(path, "a", encoding="utf-8") as f:
                f.write(line + "\n")
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
        for k in ("response", "meta"):
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
    if not _ENABLED:
        return
    name = event.name
    payload = event.payload

    dialogue_id = payload.get("dialogue_id") or os.environ.get("AF_DIALOGUE_ID", "")
    if not dialogue_id:
        return

    # ── session.start ──
    if name == "agent.dialogue.start":
        _write_header(dialogue_id, os.environ.get("AF_SESSION_ID", ""))
        # completed_skill：回传边信号（trigger_type=skill_completion 时，AF_COMPLETED_SKILL_NAME 非空
        # = coordinator 是被某 skill 的完成重新触发的，与 skill.end 的 skill_name 匹配成回传边）
        _append(dialogue_id, "session.start", {
            "trigger_type": payload.get("trigger_type", ""),
            "model": payload.get("model", ""),
            "completed_skill": os.environ.get("AF_COMPLETED_SKILL_NAME", ""),
        }, span_id="", source_event_seqs=[])
        return

    # ── skill.start（SKILL_EXECUTOR_TRIGGER：SKILL span 已开启 → current_span_id=SKILL span）──
    if name == "skill_executor.trigger":
        span_id = _current_span_id()
        global _last_skill_start_seq, _last_skill_start_span
        # exec_skill_key：跨进程关联键（coordinator 写 dispatch:{did}:{round}:{i}，本进程经 AF_EXEC_SKILL_KEY 注入）
        _last_skill_start_seq = _append(dialogue_id, "skill.start", {
            "skill_name": payload.get("skill_name", ""),
            "model": payload.get("model", ""),
            "max_rounds": payload.get("max_rounds"),
            "tools_count": payload.get("tools_count"),
            "exec_skill_key": os.environ.get("AF_EXEC_SKILL_KEY", ""),
        }, span_id=span_id, source_event_seqs=[])
        _last_skill_start_span = span_id
        return

    # ── llm.request（BEFORE：ctx.messages 全量，实时落）──
    if name.endswith(".before_llm_call"):
        span_id = _current_span_id()
        msgs = getattr(ctx, "messages", None) or []
        global _last_llm_request_seq
        _last_llm_request_seq = _append(dialogue_id, "llm.request", {
            "model": payload.get("model", ""),
            "messages": msgs,
            "msg_count": len(msgs),
        }, span_id=span_id,
           parent_span_id=_last_skill_start_span if name.startswith("skill_executor") else "",
           source_event_seqs=[],
           round_i=payload.get("round"))
        return

    # ── llm.response（AFTER：从 llm/traces 读 response/meta 全量）──
    if name.endswith(".after_llm_call"):
        call_id = _latest_call_id(dialogue_id)
        llm = _read_llm_call(call_id) if call_id else {}
        span_id = _current_span_id()
        global _last_llm_response_seq, _last_llm_response_span, _last_llm_response_call, _last_session_end_source
        _last_llm_response_seq = _append(dialogue_id, "llm.response", {
            "call_id": call_id,
            "response": llm.get("response"),
            "meta": llm.get("meta"),
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
        global _last_tool_call_seq, _last_tool_call_ts
        _last_tool_call_ts = int(time.time() * 1000)
        _last_tool_call_seq = _append(dialogue_id, "tool.call", {
            "skill_name": payload.get("skill_name", ""),
            "tool_names": payload.get("tool_names", []),
            "round": payload.get("round"),
        }, parent_span_id=_last_llm_response_span or "",
           source_event_seqs=[_last_llm_response_seq] if _last_llm_response_seq else [],
           round_i=payload.get("round"))
        return

    # ── tool.result（AFTER_TOOL_CALL）──
    if name == "skill_executor.after_tool_call":
        dur_ms = None
        if _last_tool_call_ts:
            dur_ms = int(time.time() * 1000) - _last_tool_call_ts
        _append(dialogue_id, "tool.result", {
            "skill_name": payload.get("skill_name", ""),
            "tool_names": payload.get("tool_names", []),
            "results_count": payload.get("results_count"),
            "duration_ms": dur_ms,
            "round": payload.get("round"),
        }, parent_span_id=_last_llm_response_span or "",
           source_event_seqs=[_last_tool_call_seq] if _last_tool_call_seq else [],
           round_i=payload.get("round"))
        return

    # ── skill.dispatch（COORDINATOR_AFTER_SKILL_DISPATCH：coordinator 侧，parent=决策 llm.response）──
    if name == "coordinator.after_skill_dispatch":
        # exec_skill_key：派发边强键（与 skill.start 的 AF_EXEC_SKILL_KEY 精确匹配）。
        # payload 无此字段（coordinator_decide.py:847-851 只给 skill_names/round），
        # 从 DataBus next_skills 读。coordinator_decide.py:250 用 databus.put("next_skills", ...)
        # 写，databus_client.py:98-110 put 自动加 {dialogue_id}/ 前缀 → 实际 key =
        # sys 命名空间 {dialogue_id}/next_skills（框架读 /session/{sid}/sys/{dialogue_id}/next_skills）。
        exec_keys: list[str] = []
        try:
            db = _get_databus()
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
            "skill_names": payload.get("skill_names", []),
            "round": payload.get("round"),
            "exec_skill_keys": exec_keys,
            "exec_skill_key": exec_keys[0] if exec_keys else "",
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
            # exec_skill_key：回传边强键（skill 进程 AF_EXEC_SKILL_KEY 必有，与派发边同源）
            "exec_skill_key": os.environ.get("AF_EXEC_SKILL_KEY", ""),
        }, span_id=_last_skill_start_span or "",
           parent_span_id=_last_skill_start_span or "",
           source_event_seqs=[_last_skill_start_seq] if _last_skill_start_seq else [],
           round_i=payload.get("round"))
        return

    # ── session.end ──
    if name == "agent.dialogue.end":
        _append(dialogue_id, "session.end", {
            "status": payload.get("status", ""),
            "response_text": payload.get("response_text", ""),
        }, source_event_seqs=[_last_session_end_source] if _last_session_end_source else [])
        return
